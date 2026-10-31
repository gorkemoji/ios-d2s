/*
 * d2s: Darwin has no eventfd. Mesa's panfrost uses one only for
 * performance-counter sessions (panfrost_kmod.c), which d2sdrm does not
 * support, so creating one fails and the session is not started.
 */
#ifndef D2S_SYS_EVENTFD_H
#define D2S_SYS_EVENTFD_H

#include <errno.h>
#include <stdint.h>

typedef uint64_t eventfd_t;

#define EFD_CLOEXEC	0x80000
#define EFD_NONBLOCK	0x800
#define EFD_SEMAPHORE	1

static inline int
eventfd(unsigned int initval, int flags)
{
	(void)initval;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

static inline int
eventfd_read(int fd, eventfd_t *value)
{
	(void)fd;
	(void)value;
	errno = ENOSYS;
	return -1;
}

static inline int
eventfd_write(int fd, eventfd_t value)
{
	(void)fd;
	(void)value;
	errno = ENOSYS;
	return -1;
}

#endif
