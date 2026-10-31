#!/usr/bin/env python3
"""Apply the d2s changes to a Mesa source tree (checked against mesa-26.2.6).

  patch-mesa.py MESA_DIR

meson.build: look for libdrm on Darwin too (finds tools/d2s-gl/libdrm).

panfrost buffers are mapped once by d2sdrm (tools/d2s-gl/d2sdrm.c), when
they are created or imported, so on Apple platforms:
  - pan_kmod_bo_mmap() asks d2sdrm_mmap() for that mapping instead of
    mmap()ing the "DRM fd" (there is no such file to map)
  - panfrost_bo_munmap() leaves it alone (d2sdrm_munmap() is a no-op; the
    mapping goes when the buffer is closed)
Each replacement must match exactly once, so a Mesa update that moves the
code fails here instead of building something different. Idempotent.
"""
import sys
from pathlib import Path

DECL = ('#if defined(__APPLE__)\n'
        'void *d2sdrm_mmap(int drm_fd, off_t offset, size_t size);\n'
        'int d2sdrm_munmap(void *addr, size_t size);\n'
        '#endif\n')

EDITS = [
    # Darwin has no kernel DRM, so Mesa never looks for libdrm there; look
    # anyway (optional there), so tools/d2s-gl/libdrm is found and
    # HAVE_LIBDRM is set.
    ('meson.build',
     "with_gallium_drisw_kms = false\nif system_has_kms_drm\n",
     "with_gallium_drisw_kms = false\nif system_has_kms_drm or host_machine.system() == 'darwin'\n"),
    ('src/panfrost/lib/kmod/pan_kmod.h',
     '   host_addr =\n      os_mmap(host_addr, bo->size, prot, flags, bo->dev->fd, mmap_offset);\n',
     '#if defined(__APPLE__)\n'
     '   (void)prot;\n   (void)flags;\n'
     '   host_addr = d2sdrm_mmap(bo->dev->fd, mmap_offset, bo->size);\n'
     '#else\n'
     '   host_addr =\n      os_mmap(host_addr, bo->size, prot, flags, bo->dev->fd, mmap_offset);\n'
     '#endif\n'),
    ('src/panfrost/lib/kmod/pan_kmod.h',
     'static inline void *\npan_kmod_bo_mmap(',
     DECL + '\nstatic inline void *\npan_kmod_bo_mmap('),
    # Performance-counter sessions are Linux-only (eventfd, timerfd, an RT
    # scheduler) and d2sdrm has no counters: no session on Apple platforms.
    ('src/panfrost/lib/kmod/panfrost_kmod.c',
     '   int ret = sched_setscheduler(0, SCHED_FIFO, &rt_params);\n',
     '#if defined(__APPLE__)\n'
     '   (void)rt_params;\n   int ret = 0;\n'
     '#else\n'
     '   int ret = sched_setscheduler(0, SCHED_FIFO, &rt_params);\n'
     '#endif\n'),
    ('src/panfrost/lib/kmod/panfrost_kmod.c',
     '   struct panfrost_kmod_perf_session *session;\n\n'
     '   session = pan_kmod_dev_alloc(dev, sizeof(*session));\n',
     '   struct panfrost_kmod_perf_session *session;\n\n'
     '#if defined(__APPLE__)\n'
     '   (void)caps;\n   (void)backend;\n   return NULL;\n'
     '#endif\n'
     '   session = pan_kmod_dev_alloc(dev, sizeof(*session));\n'),
    ('src/gallium/drivers/panfrost/pan_bo.c',
     '   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {',
     '#if defined(__APPLE__)\n'
     '   if (d2sdrm_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n'
     '#else\n'
     '   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n'
     '#endif'),
]


def main():
    root = Path(sys.argv[1])
    for rel, old, new in EDITS:
        p = root / rel
        s = p.read_text()
        if new in s:
            print(f'{rel}: already applied')
            continue
        n = s.count(old)
        if n != 1:
            sys.exit(f'{rel}: expected 1 match, found {n}')
        p.write_text(s.replace(old, new))
        print(f'{rel}: patched')


if __name__ == '__main__':
    main()
