/*
 * gwthread.h - threads wrapper with interruptible sleep and poll operations.
 *
 * This is a (partial) encapsulation of threads.  It provides functions
 * to create new threads and to manipulate threads.  It will eventually
 * be extended to encapsulate all pthread functions we use, so that
 * non-POSIX platforms can plug in their own versions.
 *
 * Richard Braakman <dark@wapit.com>
 */

#ifndef GWTHREAD_H
#define GWTHREAD_H

typedef void Threadfunc(void *arg);

/* Called by the gwlib init code */
void gwthread_init(void);
void gwthread_shutdown(void);

/* Start a new thread, running func(arg).  Return the new thread ID
 * on success, or -1 on failure.  Thread IDs are unique during the lifetime
 * of the entire process, unless you use more than LONG_MAX threads. */
long gwthread_create_real(Threadfunc *func, const char *funcname, void *arg);
#define gwthread_create(func, arg) \
	(gwthread_create_real(func, __FILE__ ":" ## #func, arg))

/* Wait for the other thread to terminate.  Return immediately if it
 * has already terminated. */
void gwthread_join(long thread);

/* Return the thread id of this thread. */
long gwthread_self(void);

/* If the other thread is currently in gwthread_pollfd or gwthread_sleep,
 * make it return immediately.  Otherwise, make it return immediately the
 * next time it calls one of those functions. */
void gwthread_wakeup(long thread);

/* Wrapper around the poll() system call, for one file descriptor.
 * "events" is a set of the flags defined in <sys/poll.h>, usually
 * POLLIN, POLLOUT, or (POLLIN|POLLOUT).  Return when one of the
 * events is true, or when another thread calls gwthread_wakeup on us, or
 * when the timeout expires.  The timeout is specified in seconds,
 * and a negative value means do not time out.  Return the revents
 * structure filled in by poll() for this fd.  Return -1 if something
 * went wrong. */
int gwthread_pollfd(int fd, int events, double timeout);

/* Sleep until "seconds" seconds have elapsed, or until another thread
 * calls gwthread_wakeup on us.  Fractional seconds are allowed. */
void gwthread_sleep(double seconds);

#endif
