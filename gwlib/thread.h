/*
 * thread.h - thread manipulation
 */

#ifndef GW_THREAD_H
#define GW_THREAD_H

#include "config.h"

#if !HAVE_PTHREAD_H
#error "You need Posix threads and <pthread.h>"
#endif

#include <pthread.h>

/*
 * Wrapper around pthread_mutex_t to avoid problems with recursive calls
 * to pthread_mutex_trylock on Linux (at least).
 */
typedef struct {
	pthread_mutex_t mutex;
	long owner;
	int dynamic;
#ifdef MUTEX_STATS
	unsigned char *filename;
	int lineno;
	long locks;
	long collisions;
#endif
} Mutex;


/*
 * Create a Mutex.
 */
#ifdef MUTEX_STATS
#define mutex_create() mutex_make_measured(mutex_create_real(), __FILE__, __LINE__)
#else
#define mutex_create() mutex_create_real()
#endif

/*
 * Create a Mutex.  Call these functions via the macro defined above.
 */
Mutex *mutex_create_measured(Mutex *mutex, unsigned char *filename, int lineno);
Mutex *mutex_create_real(void);


/*
 * Initialize a statically allocated Mutex.  We need those inside gwlib
 * modules that are in turn used by the mutex wrapper, such as "gwmem" and
 * "protected".
 */
#ifdef MUTEX_STATS
#define mutex_init_static(mutex) mutex_make_measured(mutex_init_static_real(mutex), __FILE__, __LINE__)
#else
#define mutex_init_static(mutex) mutex_init_static_real(mutex)
#endif

Mutex *mutex_init_static_real(Mutex *mutex);


/*
 * Destroy a Mutex.
 */
void mutex_destroy(Mutex *mutex);


/* lock given mutex. PANICS if fails (non-initialized mutex or other
 *  coding error) */ 
void mutex_lock(Mutex *mutex);


/* lock given mutex. PANICS if fails (non-initialized mutex or other
 * coding error). Same as mutex_lock, except returns 0 if the lock was
 * made and -1 if not. */ 
int mutex_try_lock(Mutex *mutex);


/* unlock given mutex, PANICX if fails (so do not call for non-locked)
 */
void mutex_unlock(Mutex *mutex);


/* delete mutex data structures (in case if Linux, just check whether it is 
 * locked).
 */
void mutex_destroy(Mutex *mutex);

#endif


