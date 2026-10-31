/*
 * d2s: Darwin has no timerfd (and no struct itimerspec). Like eventfd, Mesa's
 * panfrost uses it only for performance-counter sessions (panfrost_kmod.c),
 * which d2sdrm does not support: creating one fails.
 */
#ifndef D2S_SYS_TIMERFD_H
#define D2S_SYS_TIMERFD_H

#include <errno.h>
#include <time.h>

#ifndef TFD_CLOEXEC
#define TFD_CLOEXEC		0x80000
#define TFD_NONBLOCK		0x800
#define TFD_TIMER_ABSTIME	1
#endif

#if defined(__APPLE__) && !defined(D2S_HAVE_ITIMERSPEC)
#define D2S_HAVE_ITIMERSPEC
struct itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};
#endif

static inline int
timerfd_create(int clockid, int flags)
{
	(void)clockid;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

static inline int
timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
    struct itimerspec *old_value)
{
	(void)fd;
	(void)flags;
	(void)new_value;
	(void)old_value;
	errno = ENOSYS;
	return -1;
}

#endif
