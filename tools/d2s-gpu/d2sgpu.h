/*
 * User-space side of the d2s Mali-G76 interface (kernel: xnu patch 0132,
 * iokit/Kernel/arm/D2SGPU.cpp). Small wrappers over IOKit calls; the iOS
 * SDK has IOKit.framework but not IOKitLib.h, so the few functions used
 * are declared here.
 */
#ifndef D2SGPU_H
#define D2SGPU_H

#include <stdint.h>
#include <stddef.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;

CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type,
    io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallMethod(mach_port_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, const void *inputStruct, size_t inputStructCnt,
    uint64_t *output, uint32_t *outputCnt, void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType,
    task_port_t intoTask, mach_vm_address_t *atAddress, mach_vm_size_t *ofSize,
    uint32_t options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType,
    task_port_t fromTask, mach_vm_address_t atAddress);

#define D2SGPU_MAP_ANYWHERE	1u	/* kIOMapAnywhere */

enum {
	D2SGPU_GET_INFO = 0,
	D2SGPU_BUF_CREATE = 1,
	D2SGPU_BUF_DESTROY = 2,
	D2SGPU_SUBMIT = 3,
	D2SGPU_LOG = 4,
	D2SGPU_SUBMIT_ASYNC = 5,
	D2SGPU_WAIT = 6,
	D2SGPU_BUF_WAIT = 7,
	D2SGPU_IMPORT_SURFACE = 8,
};

#define D2SGPU_BUF_GPU_WRITE	1u
#define D2SGPU_BUF_GPU_EXEC	2u

#define D2SGPU_JOB_DONE		0
#define D2SGPU_JOB_ERROR	1
#define D2SGPU_JOB_MMU_FAULT	2
#define D2SGPU_JOB_TIMEOUT	3
#define D2SGPU_JOB_INVALID	4

struct d2sgpu_info {			/* GET_INFO, version 1 */
	uint32_t version;
	uint32_t gpu_id;
	uint32_t mmu_features;
	uint32_t as_present;
	uint32_t js_present;
	uint32_t tiler_present;
	uint32_t l2_present;
	uint32_t as;
	uint64_t shader_present;
};

struct d2sgpu_buf {
	uint32_t handle;
	uint64_t gpu_va;
	uint64_t size;
	void *cpu;			/* this process's mapping */
};

struct d2sgpu_result {
	uint64_t rc, us, js_status, fault_status, fault_addr, reset_ok;
};

static inline io_connect_t
d2sgpu_open(void)
{
	io_connect_t conn = MACH_PORT_NULL;
	io_service_t s = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("D2SGPU"));
	if (s == MACH_PORT_NULL) {
		return MACH_PORT_NULL;
	}
	if (IOServiceOpen(s, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
		conn = MACH_PORT_NULL;
	}
	IOObjectRelease(s);
	return conn;
}

static inline kern_return_t
d2sgpu_info(io_connect_t c, struct d2sgpu_info *info)
{
	size_t n = sizeof(*info);
	return IOConnectCallMethod(c, D2SGPU_GET_INFO, NULL, 0, NULL, 0, NULL, NULL, info, &n);
}

static inline kern_return_t
d2sgpu_buf_create(io_connect_t c, uint64_t size, uint32_t flags, struct d2sgpu_buf *b)
{
	uint64_t in[2] = { size, flags }, out[3] = { 0 };
	uint32_t nout = 3;
	kern_return_t kr = IOConnectCallMethod(c, D2SGPU_BUF_CREATE, in, 2, NULL, 0, out, &nout,
	    NULL, NULL);
	if (kr != KERN_SUCCESS) {
		return kr;
	}
	b->handle = (uint32_t)out[0];
	b->gpu_va = out[1];
	b->size = out[2];
	mach_vm_address_t addr = 0;
	mach_vm_size_t len = 0;
	kr = IOConnectMapMemory64(c, b->handle, mach_task_self(), &addr, &len, D2SGPU_MAP_ANYWHERE);
	b->cpu = (void *)(uintptr_t)addr;
	return kr;
}

static inline kern_return_t
d2sgpu_buf_destroy(io_connect_t c, struct d2sgpu_buf *b)
{
	uint64_t h = b->handle;
	if (b->cpu != NULL) {
		IOConnectUnmapMemory64(c, b->handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)b->cpu);
		b->cpu = NULL;
	}
	return IOConnectCallMethod(c, D2SGPU_BUF_DESTROY, &h, 1, NULL, 0, NULL, NULL, NULL, NULL);
}

static inline kern_return_t
d2sgpu_submit(io_connect_t c, int js, uint64_t head, uint32_t timeout_us, struct d2sgpu_result *r)
{
	uint64_t in[3] = { (uint64_t)js, head, timeout_us };
	uint32_t nout = 6;
	return IOConnectCallMethod(c, D2SGPU_SUBMIT, in, 3, NULL, 0, &r->rc, &nout, NULL, NULL);
}

static inline void
d2sgpu_log(io_connect_t c, const char *text, size_t len)
{
	IOConnectCallMethod(c, D2SGPU_LOG, NULL, 0, text, len, NULL, NULL, NULL, NULL);
}

/* Queue a job chain; the listed buffers are written back from the CPU
 * caches first and remember this job for d2sgpu_buf_wait. */
static inline kern_return_t
d2sgpu_submit_async(io_connect_t c, int js, uint64_t head, const uint32_t *handles,
    uint32_t n, uint64_t *seq)
{
	uint64_t in[2] = { (uint64_t)js, head };
	uint32_t nout = 1;
	return IOConnectCallMethod(c, D2SGPU_SUBMIT_ASYNC, in, 2, handles, n * sizeof(uint32_t),
	           seq, &nout, NULL, NULL);
}

struct d2sgpu_wait_result {
	uint64_t rc, js_status, fault_status, fault_addr, done_seq;
};

/* rc D2SGPU_JOB_TIMEOUT: not finished within timeout_us (0 = just look) */
static inline kern_return_t
d2sgpu_wait(io_connect_t c, uint64_t seq, uint32_t timeout_us, struct d2sgpu_wait_result *r)
{
	uint64_t in[2] = { seq, timeout_us };
	uint32_t nout = 5;
	return IOConnectCallMethod(c, D2SGPU_WAIT, in, 2, NULL, 0, &r->rc, &nout, NULL, NULL);
}

/* Wait for the buffer's last job, then make the GPU's writes visible to the
 * CPU. Returns the job's rc, or D2SGPU_JOB_TIMEOUT. */
static inline uint64_t
d2sgpu_buf_wait(io_connect_t c, const struct d2sgpu_buf *b, uint32_t timeout_us)
{
	uint64_t in[2] = { b->handle, timeout_us }, rc = D2SGPU_JOB_INVALID;
	uint32_t nout = 1;
	IOConnectCallMethod(c, D2SGPU_BUF_WAIT, in, 2, NULL, 0, &rc, &nout, NULL, NULL);
	return rc;
}

/* Map an IOSurface's own pages into the GPU (no CPU mapping is made here:
 * the process uses the IOSurface's). */
static inline kern_return_t
d2sgpu_import_surface(io_connect_t c, uint32_t surface_id, uint32_t flags, struct d2sgpu_buf *b)
{
	uint64_t in[2] = { surface_id, flags }, out[3] = { 0 };
	uint32_t nout = 3;
	kern_return_t kr = IOConnectCallMethod(c, D2SGPU_IMPORT_SURFACE, in, 2, NULL, 0, out, &nout,
	    NULL, NULL);
	b->handle = (uint32_t)out[0];
	b->gpu_va = out[1];
	b->size = out[2];
	b->cpu = NULL;
	return kr;
}

#endif
