#ifndef GW_FAKE_GETNAMEINFO_H
#define GW_FAKE_GETNAMEINFO_H

/* taken from some netdb.h file - neccessary defines for the getnameinfo()
 * stuff used by OpenWAP. These functions are completely absent from older
 * LIBC systems (5 and below), and they seem to be missing from GLIBC2.0 also.
 * The system where I found the header file had it under ncurses/...
 * Also needed on Solaris 2.6
 */

#include <config.h>

#if HAVE_GETNAMEINFO

#include <netdb.h>

#else

#include <sys/socket.h>

# define NI_MAXHOST      1025
# define NI_MAXSERV      32

# define NI_NUMERICHOST	1	/* Don't try to look up hostname.  */
# define NI_NUMERICSERV 2	/* Don't convert port number to name.  */
# define NI_NOFQDN	4	/* Only return nodename portion.  */
# define NI_NAMEREQD	8	/* Don't return numeric addresses.  */
# define NI_DGRAM	16	/* Look up UDP service rather than TCP.  */

/* Translate a socket address to a location and service name.  */
extern int getnameinfo (__const struct sockaddr *__sa,
			socklen_t __salen,
			char *__host, size_t __hostlen,
			char *__serv, size_t __servlen,
			int __flags);

#endif

#endif
