/*
 * d2sdrm: what Mesa's panfrost driver needs from libdrm and the Linux
 * panfrost kernel driver, implemented over the d2s D2SGPU IOKit user client
 * (xnu patches 0132-0135, tools/d2s-gpu/d2sgpu.h). Mesa is built against
 * libdrm's real xf86drm.h (tools/d2s-gl/libdrm); d2sdrm.c provides the
 * functions behind it.
 */
#ifndef D2SDRM_H
#define D2SDRM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the GPU: a "DRM fd" for panfrost_create_screen, or -1. */
int d2sdrm_open(void);

/* A descriptor that drmPrimeFDToHandle turns into a buffer holding the
 * IOSurface's own pages (the IOSurface must stay alive meanwhile); -1 on
 * error. Close it with close() after the import. */
int d2sdrm_surface_fd(int drm_fd, uint32_t iosurface_id);

/* Used by Mesa (tools/d2s-gl/mesa.patch) in place of mmap/munmap of
 * panfrost buffers: buffers are mapped once, when created or imported. */
void *d2sdrm_mmap(int drm_fd, off_t offset, size_t size);
int d2sdrm_munmap(void *addr, size_t size);
uint64_t d2sdrm_bo_size(int drm_fd, uint32_t handle);

/* One line into the kernel log ("D2S GPU UC: <process>: ..."). */
void d2sdrm_log(int drm_fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif
