/*
 * d2sdrm: libdrm and the Linux panfrost kernel interface for Mesa's
 * panfrost driver, implemented over D2SGPU (see d2sdrm.h).
 *
 * One device per process. "File descriptors" are real descriptors on
 * /dev/null so they are unique and close() works; nothing is read from
 * them.
 *
 * Buffers: CREATE_BO makes a D2SGPU buffer and maps it into the process at
 * once; MMAP_BO hands out handle << 32 as the "offset", which d2sdrm_mmap
 * turns back into that mapping. Imported IOSurfaces are mapped on first use.
 *
 * Caches: we report panfrost interface 1.5, so Mesa treats mappings as
 * write-combined and does no cache maintenance. Mappings are cached; the
 * kernel writes back the buffers a job lists before it runs (SUBMIT_ASYNC)
 * and drops them from the caches when WAIT_BO is done (BUF_WAIT).
 *
 * Fences: a syncobj holds a D2SGPU sequence number. Jobs of all processes
 * run in submission order, so a job's in-syncs are always met by the time
 * it starts and are not waited for here.
 *
 * The tiler heap (PANFROST_BO_HEAP, grown on GPU faults on Linux) is
 * allocated in full.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

#include <xf86drm.h>
#include <drm-uapi/panfrost_drm.h>
#include "d2sgpu.h"
#include "d2sdrm.h"

#define MAX_HANDLES	1025		/* D2SGPU handles are 1..1024 */
#define MAX_SYNCOBJS	4096
#define MAX_SURFACE_FDS	64

enum { BO_FREE, BO_OWN, BO_SURFACE };

struct bo {
	int kind;
	struct d2sgpu_buf buf;
	uint32_t create_flags;
};

struct syncobj {
	int used;
	int signaled;		/* signalled with no job attached */
	uint64_t seq;		/* 0: none attached */
};

/*
 * The buffers of recent jobs. Mesa waits on syncobjs, not on buffers, so
 * nothing else would drop stale CPU cache lines of what a job wrote: the
 * first wait that sees a job finished flushes its buffers (BUF_FLUSH).
 */
#define NJOBS		64
#define JOB_BOS		256
static struct {
	uint64_t seq;
	uint32_t n, flushed;
	uint32_t h[JOB_BOS];
} jobs[NJOBS];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t conn = MACH_PORT_NULL;
static int dev_fd = -1;
static struct d2sgpu_info info;
static struct bo bos[MAX_HANDLES];
static struct syncobj syncobjs[MAX_SYNCOBJS];
static struct { int fd; uint32_t id; } surface_fds[MAX_SURFACE_FDS];

static int
fail(int err)
{
	errno = err;
	return -1;
}

void
d2sdrm_log(int fd, const char *fmt, ...)
{
	char line[150];
	va_list ap;
	(void)fd;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0 || conn == MACH_PORT_NULL) {
		return;
	}
	d2sgpu_log(conn, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

int
d2sdrm_open(void)
{
	pthread_mutex_lock(&lock);
	if (dev_fd < 0) {
		conn = d2sgpu_open();
		if (conn != MACH_PORT_NULL && d2sgpu_info(conn, &info) == KERN_SUCCESS &&
		    info.version >= 2) {
			dev_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
		}
	}
	int fd = dev_fd;
	pthread_mutex_unlock(&lock);
	return fd;
}

static int
is_dev(int fd)
{
	return fd >= 0 && fd == dev_fd && conn != MACH_PORT_NULL;
}

/* ---- time ------------------------------------------------------------ */

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Flush the buffers of every recorded job up to done_seq not flushed yet. */
static void
after_done(uint64_t done_seq)
{
	pthread_mutex_lock(&lock);
	for (int i = 0; i < NJOBS; i++) {
		if (jobs[i].seq != 0 && jobs[i].seq <= done_seq && !jobs[i].flushed) {
			for (uint32_t k = 0; k < jobs[i].n; k++) {
				uint64_t h = jobs[i].h[k];
				IOConnectCallMethod(conn, 10 /* BUF_FLUSH */, &h, 1, NULL, 0, NULL, NULL,
				    NULL, NULL);
			}
			jobs[i].flushed = 1;
		}
	}
	pthread_mutex_unlock(&lock);
}

/*
 * Microseconds left until the absolute CLOCK_MONOTONIC time abs_ns, capped at
 * the 5 s a D2SGPU wait takes. Mesa passes "forever" as OS_TIMEOUT_INFINITE,
 * all ones, which is -1 here: a negative deadline means forever.
 */
static uint32_t
left_us(int64_t abs_ns)
{
	if (abs_ns < 0) {
		return 5000000;
	}
	int64_t left = abs_ns - now_ns();
	return left <= 0 ? 0 : left / 1000 > 5000000 ? 5000000 : (uint32_t)(left / 1000);
}

/* Wait for seq until abs_ns (see left_us); D2SGPU waits are capped at 5 s,
 * so loop. Returns the job's rc or TIMEOUT. */
static uint64_t
wait_seq(uint64_t seq, int64_t abs_ns)
{
	for (;;) {
		uint32_t us = left_us(abs_ns);
		struct d2sgpu_wait_result w = { 0 };
		if (d2sgpu_wait(conn, seq, us, &w) != KERN_SUCCESS) {
			return D2SGPU_JOB_INVALID;
		}
		if (w.rc != D2SGPU_JOB_TIMEOUT) {
			after_done(w.done_seq);
			if (w.rc != D2SGPU_JOB_DONE) {
				d2sdrm_log(dev_fd, "d2sdrm: job %llu rc %llu js status %#llx fault %#llx @ %#llx",
				    (unsigned long long)seq, (unsigned long long)w.rc,
				    (unsigned long long)w.js_status, (unsigned long long)w.fault_status,
				    (unsigned long long)w.fault_addr);
			}
			return w.rc;
		}
		if (us < 5000000) {
			return w.rc;
		}
	}
}

/* ---- buffers --------------------------------------------------------- */

static struct bo *
bo_get(uint32_t h)
{
	return h >= 1 && h < MAX_HANDLES && bos[h].kind != BO_FREE ? &bos[h] : NULL;
}

static int
create_bo(struct drm_panfrost_create_bo *a)
{
	struct d2sgpu_buf b = { 0 };
	uint32_t flags = D2SGPU_BUF_GPU_WRITE;
	if (!(a->flags & PANFROST_BO_NOEXEC)) {
		flags |= D2SGPU_BUF_GPU_EXEC;
	}
	if (d2sgpu_buf_create(conn, a->size, flags, &b) != KERN_SUCCESS || b.cpu == NULL) {
		d2sdrm_log(dev_fd, "d2sdrm: CREATE_BO %u bytes flags %#x failed", a->size, a->flags);
		return fail(ENOMEM);
	}
	if (b.handle >= MAX_HANDLES) {
		d2sgpu_buf_destroy(conn, &b);
		return fail(ENOMEM);
	}
	bos[b.handle] = (struct bo){ BO_OWN, b, a->flags };
	a->handle = b.handle;
	a->offset = b.gpu_va;
	return 0;
}

static int
close_bo(uint32_t h)
{
	struct bo *bo = bo_get(h);
	if (bo == NULL) {
		return fail(EINVAL);
	}
	d2sgpu_buf_destroy(conn, &bo->buf);	/* unmaps our view too */
	memset(bo, 0, sizeof(*bo));
	return 0;
}

void *
d2sdrm_mmap(int fd, off_t offset, size_t size)
{
	pthread_mutex_lock(&lock);
	struct bo *bo = is_dev(fd) ? bo_get((uint32_t)((uint64_t)offset >> 32)) : NULL;
	void *p = MAP_FAILED;
	if (bo != NULL && size <= bo->buf.size) {
		if (bo->buf.cpu == NULL) {	/* imported surface: map on first use */
			mach_vm_address_t addr = 0;
			mach_vm_size_t len = 0;
			if (IOConnectMapMemory64(conn, bo->buf.handle, mach_task_self(), &addr, &len,
			    D2SGPU_MAP_ANYWHERE) == KERN_SUCCESS) {
				bo->buf.cpu = (void *)(uintptr_t)addr;
			}
		}
		if (bo->buf.cpu != NULL) {
			p = bo->buf.cpu;
		}
	}
	pthread_mutex_unlock(&lock);
	if (p == MAP_FAILED) {
		errno = EINVAL;
	}
	return p;
}

int
d2sdrm_munmap(void *addr, size_t size)
{
	(void)addr;
	(void)size;
	return 0;		/* the mapping lives as long as the buffer */
}

int
d2sdrm_surface_fd(int drm_fd, uint32_t iosurface_id)
{
	if (!is_dev(drm_fd)) {
		return -1;
	}
	int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_SURFACE_FDS; i++) {
		if (surface_fds[i].fd <= 0) {
			surface_fds[i].fd = fd;
			surface_fds[i].id = iosurface_id;
			pthread_mutex_unlock(&lock);
			return fd;
		}
	}
	pthread_mutex_unlock(&lock);
	close(fd);
	return -1;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	pthread_mutex_lock(&lock);
	uint32_t id = 0;
	for (int i = 0; i < MAX_SURFACE_FDS; i++) {
		if (surface_fds[i].fd == prime_fd && prime_fd > 0) {
			id = surface_fds[i].id;
			surface_fds[i].fd = 0;	/* one import per descriptor */
			break;
		}
	}
	int ret = fail(EINVAL);
	struct d2sgpu_buf b = { 0 };
	if (is_dev(fd) && id != 0 &&
	    d2sgpu_import_surface(conn, id, D2SGPU_BUF_GPU_WRITE, &b) == KERN_SUCCESS &&
	    b.handle < MAX_HANDLES) {
		bos[b.handle] = (struct bo){ BO_SURFACE, b, PANFROST_BO_NOEXEC };
		*handle = b.handle;
		ret = 0;
	}
	pthread_mutex_unlock(&lock);
	return ret;
}

/* Size of a buffer (Mesa asks this for imports instead of seeking a dma-buf
 * fd; see patch-mesa.py); 0 if there is no such buffer. */
uint64_t
d2sdrm_bo_size(int fd, uint32_t handle)
{
	pthread_mutex_lock(&lock);
	struct bo *bo = is_dev(fd) ? bo_get(handle) : NULL;
	uint64_t size = bo != NULL ? bo->buf.size : 0;
	pthread_mutex_unlock(&lock);
	return size;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	return fail(ENOSYS);	/* no exports to other processes yet */
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	pthread_mutex_lock(&lock);
	int ret = is_dev(fd) ? close_bo(handle) : fail(EBADF);
	pthread_mutex_unlock(&lock);
	return ret;
}

/* ---- GET_PARAM ------------------------------------------------------- */

static uint32_t
reg(uint32_t off)
{
	uint32_t v = 0;
	size_t n = sizeof(v);
	IOConnectCallMethod(conn, 9 /* READ_REGS */, NULL, 0, &off, sizeof(off), NULL, NULL, &v, &n);
	return v;
}

static int
get_param(struct drm_panfrost_get_param *a)
{
	uint32_t p = a->param;
	uint64_t v;
	switch (p) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID: v = info.gpu_id >> 16; break;
	case DRM_PANFROST_PARAM_GPU_REVISION: v = info.gpu_id & 0xffff; break;
	case DRM_PANFROST_PARAM_SHADER_PRESENT: v = info.shader_present; break;
	case DRM_PANFROST_PARAM_TILER_PRESENT: v = info.tiler_present; break;
	case DRM_PANFROST_PARAM_L2_PRESENT: v = info.l2_present; break;
	case DRM_PANFROST_PARAM_STACK_PRESENT: v = reg(0xe00); break;
	case DRM_PANFROST_PARAM_AS_PRESENT: v = info.as_present; break;
	case DRM_PANFROST_PARAM_JS_PRESENT: v = info.js_present; break;
	case DRM_PANFROST_PARAM_L2_FEATURES: v = reg(0x004); break;
	case DRM_PANFROST_PARAM_CORE_FEATURES: v = reg(0x008); break;
	case DRM_PANFROST_PARAM_TILER_FEATURES: v = reg(0x00c); break;
	case DRM_PANFROST_PARAM_MEM_FEATURES: v = reg(0x010); break;
	case DRM_PANFROST_PARAM_MMU_FEATURES: v = info.mmu_features; break;
	case DRM_PANFROST_PARAM_MAX_THREADS: v = reg(0x0a0); break;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ: v = reg(0x0a4); break;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ: v = reg(0x0a8); break;
	case DRM_PANFROST_PARAM_THREAD_FEATURES: v = reg(0x0ac); break;
	case DRM_PANFROST_PARAM_COHERENCY_FEATURES: v = reg(0x300); break;
	case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC: v = reg(0x310); break;
	case DRM_PANFROST_PARAM_NR_CORE_GROUPS: v = (uint64_t)__builtin_popcount(info.l2_present); break;
	case DRM_PANFROST_PARAM_AFBC_FEATURES: v = 0; break;	/* no such register before v10 */
	case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP: v = (uint64_t)reg(0x09c) << 32 | reg(0x098); break;
	case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY: {
		uint64_t f = 0;
		size_t n = sizeof(f);
		v = sysctlbyname("hw.tbfrequency", &f, &n, NULL, 0) == 0 ? f : 26000000;
		break;
	}
	case DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES: v = 1u << PANFROST_JM_CTX_PRIORITY_MEDIUM; break;
	case DRM_PANFROST_PARAM_SELECTED_COHERENCY: v = DRM_PANFROST_GPU_COHERENCY_NONE; break;
	default:
		if (p >= DRM_PANFROST_PARAM_TEXTURE_FEATURES0 && p <= DRM_PANFROST_PARAM_TEXTURE_FEATURES3) {
			v = reg(0x0b0 + 4 * (p - DRM_PANFROST_PARAM_TEXTURE_FEATURES0));
		} else if (p >= DRM_PANFROST_PARAM_JS_FEATURES0 && p <= DRM_PANFROST_PARAM_JS_FEATURES15) {
			v = reg(0x0c0 + 4 * (p - DRM_PANFROST_PARAM_JS_FEATURES0));
		} else {
			return fail(EINVAL);
		}
	}
	a->value = v;
	return 0;
}

/* ---- jobs and syncobjs ----------------------------------------------- */

static struct syncobj *
sync_get(uint32_t h)
{
	return h >= 1 && h < MAX_SYNCOBJS && syncobjs[h].used ? &syncobjs[h] : NULL;
}

static int
submit(struct drm_panfrost_submit *a)
{
	const uint32_t *handles = (const uint32_t *)(uintptr_t)a->bo_handles;
	int js = (a->requirements & PANFROST_JD_REQ_FS) ? 0 : 1;
	uint64_t seq = 0;
	if (a->bo_handle_count > 256) {
		return fail(EINVAL);
	}
	if (d2sgpu_submit_async(conn, js, a->jc, handles, a->bo_handle_count, &seq) != KERN_SUCCESS) {
		d2sdrm_log(dev_fd, "d2sdrm: SUBMIT jc %#llx js %d failed", (unsigned long long)a->jc, js);
		return fail(EINVAL);
	}
	struct syncobj *s = a->out_sync ? sync_get(a->out_sync) : NULL;
	if (s != NULL) {
		s->seq = seq;
		s->signaled = 0;
	}
	int slot = (int)(seq % NJOBS);
	jobs[slot].seq = seq;
	jobs[slot].n = a->bo_handle_count < JOB_BOS ? a->bo_handle_count : JOB_BOS;
	jobs[slot].flushed = 0;
	memcpy(jobs[slot].h, handles, jobs[slot].n * sizeof(uint32_t));
	return 0;
}

/* 0 when signalled, -ETIME when not by the deadline, -EINVAL if a syncobj
 * has nothing to wait for (and WAIT_FOR_SUBMIT is not set). */
static int
sync_wait(const uint32_t *handles, uint32_t count, int64_t abs_ns, uint32_t flags,
    uint32_t *first)
{
	int all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	for (uint32_t i = 0; i < count; i++) {
		pthread_mutex_lock(&lock);
		struct syncobj *s = sync_get(handles[i]);
		int signaled = s != NULL && s->signaled;
		uint64_t seq = s != NULL ? s->seq : 0;
		pthread_mutex_unlock(&lock);
		if (s == NULL) {
			return -EINVAL;
		}
		if (!signaled && seq == 0) {
			if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) {
				return -EINVAL;
			}
			if (all) {
				return -ETIME;	/* nothing will ever be submitted for it here */
			}
			continue;
		}
		if (signaled || wait_seq(seq, abs_ns) != D2SGPU_JOB_TIMEOUT) {
			if (!all) {
				if (first != NULL) {
					*first = i;
				}
				return 0;
			}
		} else if (all) {
			return -ETIME;
		}
	}
	return all ? 0 : -ETIME;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	if (!is_dev(fd)) {
		return -EBADF;
	}
	pthread_mutex_lock(&lock);
	for (uint32_t h = 1; h < MAX_SYNCOBJS; h++) {
		if (!syncobjs[h].used) {
			syncobjs[h] = (struct syncobj){ 1, (flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0, 0 };
			*handle = h;
			pthread_mutex_unlock(&lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&lock);
	return -ENOMEM;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct syncobj *s = sync_get(handle);
	if (s != NULL) {
		memset(s, 0, sizeof(*s));
	}
	pthread_mutex_unlock(&lock);
	return s != NULL ? 0 : -EINVAL;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	int r = sync_wait(handles, num_handles, timeout_nsec, flags, first_signaled);
	if (r) {
		errno = -r;
	}
	return r;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)points;		/* binary use only */
	return drmSyncobjWait(fd, handles, num_handles, timeout_nsec, flags, first_signaled);
}

static int
sync_set(const uint32_t *handles, uint32_t count, int signaled)
{
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = sync_get(handles[i]);
		if (s == NULL) {
			pthread_mutex_unlock(&lock);
			return -EINVAL;
		}
		s->signaled = signaled;
		s->seq = 0;
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	return sync_set(handles, handle_count, 0);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	return sync_set(handles, handle_count, 1);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags)
{
	(void)fd;
	(void)dst_point;
	(void)src_point;
	(void)flags;
	pthread_mutex_lock(&lock);
	struct syncobj *d = sync_get(dst_handle), *s = sync_get(src_handle);
	if (d != NULL && s != NULL) {
		d->signaled = s->signaled;
		d->seq = s->seq;
	}
	pthread_mutex_unlock(&lock);
	return d != NULL && s != NULL ? 0 : -EINVAL;
}

/*
 * Sync files. panfrost makes every fence by exporting its context syncobj as
 * a sync file and importing that into a new syncobj (pan_fence.c). Here a
 * sync file is a placeholder descriptor on /dev/null that records a snapshot
 * of the syncobj (signalled, or the job's sequence number; the numbers come
 * from one queue for all processes). Descriptor numbers are reused after
 * close(), which d2sdrm does not see, so a new export with the same number
 * replaces the old record; the table is a ring otherwise.
 */
#define MAX_SYNC_FILES	256
static struct {
	int fd;			/* 0: free */
	int signaled;
	uint64_t seq;
} sync_files[MAX_SYNC_FILES];
static unsigned sync_file_next;

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	*sync_file_fd = -1;
	if (!is_dev(fd)) {
		return -EBADF;
	}
	int sf = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (sf < 0) {
		return -errno;
	}
	pthread_mutex_lock(&lock);
	struct syncobj *s = sync_get(handle);
	if (s == NULL || (!s->signaled && s->seq == 0)) {
		pthread_mutex_unlock(&lock);
		close(sf);
		return -EINVAL;		/* no fence to export, as on Linux */
	}
	unsigned slot = MAX_SYNC_FILES;
	for (unsigned i = 0; i < MAX_SYNC_FILES; i++) {
		if (sync_files[i].fd == sf) {
			slot = i;	/* a stale record for a reused number */
			break;
		}
	}
	if (slot == MAX_SYNC_FILES) {
		slot = sync_file_next++ % MAX_SYNC_FILES;
	}
	sync_files[slot].fd = sf;
	sync_files[slot].signaled = s->signaled;
	sync_files[slot].seq = s->seq;
	pthread_mutex_unlock(&lock);
	*sync_file_fd = sf;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	if (!is_dev(fd) || sync_file_fd <= 0) {
		return -EINVAL;
	}
	int ret = -EINVAL;
	pthread_mutex_lock(&lock);
	struct syncobj *s = sync_get(handle);
	for (unsigned i = 0; s != NULL && i < MAX_SYNC_FILES; i++) {
		if (sync_files[i].fd == sync_file_fd) {
			s->signaled = sync_files[i].signaled;
			s->seq = sync_files[i].seq;
			ret = 0;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return ret;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd;
	(void)obj_fd;
	(void)handle;
	return -ENOSYS;
}

/* ---- the ioctl switch ------------------------------------------------ */

static int
wait_bo(struct drm_panfrost_wait_bo *a)
{
	struct bo *bo = bo_get(a->handle);
	if (bo == NULL) {
		return fail(EINVAL);
	}
	struct d2sgpu_buf b = bo->buf;
	pthread_mutex_unlock(&lock);
	uint64_t rc;
	uint32_t us;
	do {	/* absolute deadline, like the kernel's; negative = forever */
		us = left_us(a->timeout_ns);
		rc = d2sgpu_buf_wait(conn, &b, us);
	} while (rc == D2SGPU_JOB_TIMEOUT && us == 5000000);
	pthread_mutex_lock(&lock);
	return rc == D2SGPU_JOB_TIMEOUT ? fail(ETIMEDOUT) : 0;
}

static int
sync_bo(struct drm_panfrost_sync_bo *a)
{
	const struct drm_panfrost_bo_sync_op *ops = (const void *)(uintptr_t)a->ops;
	for (uint32_t i = 0; i < a->op_count; i++) {
		uint64_t h = ops[i].handle;
		IOConnectCallMethod(conn, 10 /* BUF_FLUSH */, &h, 1, NULL, 0, NULL, NULL, NULL, NULL);
	}
	return 0;
}

static int
ioctl_locked(unsigned long req, void *arg)
{
	switch (req) {
	case DRM_IOCTL_PANFROST_GET_PARAM:
		return get_param(arg);
	case DRM_IOCTL_PANFROST_CREATE_BO:
		return create_bo(arg);
	case DRM_IOCTL_PANFROST_MMAP_BO: {
		struct drm_panfrost_mmap_bo *a = arg;
		if (bo_get(a->handle) == NULL) {
			return fail(EINVAL);
		}
		a->offset = (uint64_t)a->handle << 32;
		return 0;
	}
	case DRM_IOCTL_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *a = arg;
		struct bo *bo = bo_get(a->handle);
		if (bo == NULL) {
			return fail(EINVAL);
		}
		a->offset = bo->buf.gpu_va;
		return 0;
	}
	case DRM_IOCTL_PANFROST_QUERY_BO_INFO: {
		struct drm_panfrost_query_bo_info *a = arg;
		struct bo *bo = bo_get(a->handle);
		if (bo == NULL) {
			return fail(EINVAL);
		}
		a->extra_flags = bo->kind == BO_SURFACE ? DRM_PANFROST_BO_IS_IMPORTED : 0;
		a->create_flags = bo->create_flags;
		return 0;
	}
	case DRM_IOCTL_PANFROST_WAIT_BO:
		return wait_bo(arg);
	case DRM_IOCTL_PANFROST_SYNC_BO:
		return sync_bo(arg);
	case DRM_IOCTL_PANFROST_SUBMIT:
		return submit(arg);
	case DRM_IOCTL_PANFROST_MADVISE:
		((struct drm_panfrost_madvise *)arg)->retained = 1;	/* never evicted */
		return 0;
	case DRM_IOCTL_PANFROST_SET_LABEL_BO:
		return 0;
	case DRM_IOCTL_PANFROST_JM_CTX_CREATE:
		((struct drm_panfrost_jm_ctx_create *)arg)->handle = 1;	/* one queue anyway */
		return 0;
	case DRM_IOCTL_PANFROST_JM_CTX_DESTROY:
		return 0;
	case DRM_IOCTL_GEM_CLOSE:
		return close_bo(((struct drm_gem_close *)arg)->handle);
	case DRM_IOCTL_SYNCOBJ_CREATE: {
		struct drm_syncobj_create *a = arg;
		pthread_mutex_unlock(&lock);
		int r = drmSyncobjCreate(dev_fd, a->flags, &a->handle);
		pthread_mutex_lock(&lock);
		return r ? fail(-r) : 0;
	}
	case DRM_IOCTL_SYNCOBJ_DESTROY: {
		pthread_mutex_unlock(&lock);
		int r = drmSyncobjDestroy(dev_fd, ((struct drm_syncobj_destroy *)arg)->handle);
		pthread_mutex_lock(&lock);
		return r ? fail(-r) : 0;
	}
	case DRM_IOCTL_SYNCOBJ_WAIT: {
		struct drm_syncobj_wait *a = arg;
		pthread_mutex_unlock(&lock);
		int r = sync_wait((const uint32_t *)(uintptr_t)a->handles, a->count_handles,
		    a->timeout_nsec, a->flags, &a->first_signaled);
		pthread_mutex_lock(&lock);
		return r ? fail(-r) : 0;
	}
	case DRM_IOCTL_SYNCOBJ_RESET:
	case DRM_IOCTL_SYNCOBJ_SIGNAL: {
		struct drm_syncobj_array *a = arg;
		pthread_mutex_unlock(&lock);
		int r = sync_set((const uint32_t *)(uintptr_t)a->handles, a->count_handles,
		    req == DRM_IOCTL_SYNCOBJ_SIGNAL);
		pthread_mutex_lock(&lock);
		return r ? fail(-r) : 0;
	}
	default:
		d2sdrm_log(dev_fd, "d2sdrm: unsupported ioctl %#lx", req);
		return fail(ENOTTY);
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	if (!is_dev(fd)) {
		return fail(EBADF);
	}
	pthread_mutex_lock(&lock);
	int r = ioctl_locked(request, arg);
	int e = errno;
	pthread_mutex_unlock(&lock);
	errno = e;
	return r;
}

/* ---- device info ----------------------------------------------------- */

drmVersionPtr
drmGetVersion(int fd)
{
	if (!is_dev(fd)) {
		return NULL;
	}
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (v == NULL) {
		return NULL;
	}
	v->version_major = 1;
	v->version_minor = 5;	/* 1.6 would turn on WB mmap + user cache ops */
	v->name = strdup("panfrost");
	v->name_len = 8;
	v->date = strdup("20261031");
	v->date_len = 8;
	v->desc = strdup("d2s Mali-G76 over D2SGPU");
	v->desc_len = (int)strlen(v->desc);
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v != NULL) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)name;
	(void)busid;
	(void)type;
	return d2sdrm_open();
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	*device = NULL;
	return -ENODEV;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL) {
		*device = NULL;
	}
}
