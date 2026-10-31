/*
 * d2sgpu-test: GPU step 3b check from user space on the d2s.
 *
 * Opens D2SGPU, maps a job buffer and a result buffer into this process,
 * writes a Mali Write Value job with the CPU, runs it, and checks the value
 * the GPU wrote through the process's own mapping. Then a job that writes
 * to an unmapped GPU address (must fault, GPU must recover) and one more
 * job; then 8 jobs queued asynchronously with one buffer wait, and a GPU
 * write into an imported IOSurface read back through the surface's own
 * mapping. Every line also goes to the kernel log ("D2S GPU UC: <process>:"),
 * so the result is in the log ring even with no console.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <IOSurface/IOSurfaceRef.h>
#include "d2sgpu.h"

static io_connect_t conn = MACH_PORT_NULL;

static void
say(const char *fmt, ...)
{
	char line[150];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if ((size_t)n >= sizeof(line)) {
		n = sizeof(line) - 1;
	}
	printf("%s\n", line);
	if (conn != MACH_PORT_NULL) {
		d2sgpu_log(conn, line, (size_t)n);
	}
}

/* genxml v7 Write Value job (64 bytes): header, then payload */
static void
write_value_job(volatile uint32_t *j, uint64_t dst, uint32_t value)
{
	for (int i = 0; i < 16; i++) {
		j[i] = 0;
	}
	j[4] = 1u | 2u << 1 | 1u << 16;	/* is64b, type Write Value, index 1 */
	j[8] = (uint32_t)dst;
	j[9] = (uint32_t)(dst >> 32);
	j[10] = 6;			/* Immediate 32 */
	j[12] = value;
}

/* 32-bit BGRA surface */
static IOSurfaceRef
make_surface(int w, int h)
{
	int bpe = 4, fmt = 0x42475241;	/* 'BGRA' */
	const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement,
		               kIOSurfacePixelFormat };
	CFNumberRef vals[] = {
		CFNumberCreate(NULL, kCFNumberIntType, &w), CFNumberCreate(NULL, kCFNumberIntType, &h),
		CFNumberCreate(NULL, kCFNumberIntType, &bpe), CFNumberCreate(NULL, kCFNumberIntType, &fmt),
	};
	CFDictionaryRef props = CFDictionaryCreate(NULL, keys, (const void **)vals, 4,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef s = IOSurfaceCreate(props);
	CFRelease(props);
	for (int i = 0; i < 4; i++) {
		CFRelease(vals[i]);
	}
	return s;
}

static int
run_test(void)
{
	conn = d2sgpu_open();
	if (conn == MACH_PORT_NULL) {
		printf("d2sgpu-test: cannot open D2SGPU\n");
		return 1;
	}
	struct d2sgpu_info info;
	if (d2sgpu_info(conn, &info) != KERN_SUCCESS) {
		say("USER TEST FAIL: GET_INFO");
		return 1;
	}
	say("GPU id %#x shaders %#llx js %#x, my AS%u", info.gpu_id,
	    (unsigned long long)info.shader_present, info.js_present, info.as);

	struct d2sgpu_buf job = { 0 }, res = { 0 };
	kern_return_t k1 = d2sgpu_buf_create(conn, 4096, D2SGPU_BUF_GPU_WRITE, &job);
	kern_return_t k2 = d2sgpu_buf_create(conn, 64 * 1024, D2SGPU_BUF_GPU_WRITE, &res);
	if (k1 != KERN_SUCCESS || k2 != KERN_SUCCESS || !job.cpu || !res.cpu) {
		say("USER TEST FAIL: buffers %#x %#x", k1, k2);
		return 1;
	}
	say("job buf %u gpu %#llx cpu %p; result buf %u gpu %#llx size %#llx cpu %p",
	    job.handle, (unsigned long long)job.gpu_va, job.cpu, res.handle,
	    (unsigned long long)res.gpu_va, (unsigned long long)res.size, res.cpu);

	volatile uint32_t *j = job.cpu, *r = res.cpu;
	struct d2sgpu_result out;

	/* 1. the GPU writes into the last page of the result buffer */
	r[60 * 1024 / 4] = 0x44444444u;
	write_value_job(j, res.gpu_va + 60 * 1024, 0xD25C0001u);
	d2sgpu_submit(conn, 0, job.gpu_va, 100000, &out);
	int ok1 = out.rc == D2SGPU_JOB_DONE && r[60 * 1024 / 4] == 0xD25C0001u;
	say("job 1: rc %llu in %llu us, result %#x, job status word %#x",
	    (unsigned long long)out.rc, (unsigned long long)out.us, r[60 * 1024 / 4], j[0]);

	/* 2. a write to an unmapped GPU address must fault */
	uint64_t hole = res.gpu_va + res.size;	/* the guard page */
	write_value_job(j, hole, 0xBAD);
	d2sgpu_submit(conn, 0, job.gpu_va, 100000, &out);
	int ok2 = out.rc == D2SGPU_JOB_MMU_FAULT && out.reset_ok &&
	    (out.fault_addr & ~0xfffull) == hole;
	say("job 2 (fault): rc %llu fault %#llx @ %#llx reset %llu",
	    (unsigned long long)out.rc, (unsigned long long)out.fault_status,
	    (unsigned long long)out.fault_addr, (unsigned long long)out.reset_ok);

	/* 3. and the next job works */
	write_value_job(j, res.gpu_va, 0xD25C0003u);
	d2sgpu_submit(conn, 0, job.gpu_va, 100000, &out);
	int ok3 = out.rc == D2SGPU_JOB_DONE && r[0] == 0xD25C0003u;
	say("job 3: rc %llu result %#x", (unsigned long long)out.rc, r[0]);

	/* 4. asynchronous: 8 jobs queued back to back, then one buffer wait */
	uint32_t both[2] = { job.handle, res.handle };
	uint64_t seq[8] = { 0 };
	int queued = 1;
	for (int i = 0; i < 8; i++) {
		write_value_job(j + 16 * (i + 1), res.gpu_va + 0x200 + 4 * (uint64_t)i, 0xD25C0100u + (uint32_t)i);
		r[0x200 / 4 + i] = 0;
	}
	for (int i = 0; i < 8 && queued; i++) {
		queued = d2sgpu_submit_async(conn, 1, job.gpu_va + 64 * (uint64_t)(i + 1), both, 2,
		    &seq[i]) == KERN_SUCCESS;
	}
	uint64_t wrc = queued ? d2sgpu_buf_wait(conn, &res, 1000000) : 99;
	struct d2sgpu_wait_result w = { 0 };
	d2sgpu_wait(conn, seq[7], 0, &w);
	int good = 0;
	for (int i = 0; i < 8; i++) {
		good += r[0x200 / 4 + i] == 0xD25C0100u + (uint32_t)i;
	}
	int ok4 = queued && wrc == D2SGPU_JOB_DONE && w.rc == D2SGPU_JOB_DONE && good == 8;
	say("job 4 (async x8): seq %llu..%llu, buf wait rc %llu, last rc %llu, %d/8 results",
	    (unsigned long long)seq[0], (unsigned long long)seq[7], (unsigned long long)wrc,
	    (unsigned long long)w.rc, good);

	/* 5. the GPU writes a pixel into an IOSurface; the CPU reads it back
	 * through the IOSurface's own mapping */
	int ok5 = 0;
	IOSurfaceRef surf = make_surface(64, 64);
	if (surf == NULL) {
		say("job 5 (IOSurface): IOSurfaceCreate failed");
	} else {
		struct d2sgpu_buf sb = { 0 };
		uint32_t sid = IOSurfaceGetID(surf);
		size_t bpr = IOSurfaceGetBytesPerRow(surf);
		kern_return_t kr = d2sgpu_import_surface(conn, sid, D2SGPU_BUF_GPU_WRITE, &sb);
		if (kr != KERN_SUCCESS) {
			say("job 5 (IOSurface): import of surface %u failed %#x", sid, kr);
		} else {
			uint64_t off = 10 * bpr + 40 * 4;	/* pixel (40, 10) */
			write_value_job(j, sb.gpu_va + off, 0xD25C5555u);
			uint32_t hs[2] = { job.handle, sb.handle };
			uint64_t s5 = 0;
			d2sgpu_submit_async(conn, 1, job.gpu_va, hs, 2, &s5);
			uint64_t src = d2sgpu_buf_wait(conn, &sb, 1000000);
			IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
			uint32_t px = *(volatile uint32_t *)((uint8_t *)IOSurfaceGetBaseAddress(surf) + off);
			IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
			ok5 = src == D2SGPU_JOB_DONE && px == 0xD25C5555u;
			say("job 5 (IOSurface %u, %zu bytes/row): gpu %#llx size %#llx, rc %llu, pixel %#x",
			    sid, bpr, (unsigned long long)sb.gpu_va, (unsigned long long)sb.size,
			    (unsigned long long)src, px);
			d2sgpu_buf_destroy(conn, &sb);
		}
		CFRelease(surf);
	}

	d2sgpu_buf_destroy(conn, &job);
	d2sgpu_buf_destroy(conn, &res);
	int all = ok1 && ok2 && ok3 && ok4 && ok5;
	say("USER TEST %s (map+job %s, fault %s, after-fault %s, async %s, IOSurface %s)",
	    all ? "OK" : "FAIL", ok1 ? "ok" : "FAIL", ok2 ? "ok" : "FAIL", ok3 ? "ok" : "FAIL",
	    ok4 ? "ok" : "FAIL", ok5 ? "ok" : "FAIL");
	IOServiceClose(conn);
	conn = MACH_PORT_NULL;
	return all ? 0 : 1;
}

#ifdef D2SGPU_DYLIB
/*
 * Library build (d2sgpu-test.dylib), inserted into backboardd by the
 * d2senv boot-arg: run the test once on a thread of its own, a few
 * seconds after load, so backboardd's start-up is not held up. In any
 * other process that inherits the variable it does nothing.
 */
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static void *
test_thread(void *arg)
{
	(void)arg;
	sleep(5);
	run_test();
	return NULL;
}

__attribute__((constructor)) static void
d2sgpu_test_load(void)
{
	const char *name = getprogname();
	if (name == NULL || strcmp(name, "backboardd") != 0) {
		return;
	}
	pthread_t t;
	if (pthread_create(&t, NULL, test_thread, NULL) == 0) {
		pthread_detach(t);
	}
}
#else
int
main(void)
{
	return run_test();
}
#endif
