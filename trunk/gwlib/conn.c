/* conn.c - implement Connection type
 *
 * This file implements the interface defined in conn.h.
 *
 * Richard Braakman <dark@wapit.com>
 */

/* TODO: unlocked_close() on error */
/* TODO: have I/O functions check if connection is open */
/* TODO: handle eof when reading */
/* TODO: implement conn_open_tcp */
/* TODO: implement conn_read_packet */

#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>

#include "gwlib/gwlib.h"

struct Connection {
	Mutex *mutex;
	volatile sig_atomic_t claimed;
#ifndef NDEBUG
	pthread_t claimer;
#endif

	int fd;

	Octstr *outbuf;
	long outbufpos;  /* start of unwritten data in outbuf */

	Octstr *inbuf;
	long inbufpos;   /* start of unread data in inbuf */
};

/* Lock a Connection, if it is unclaimed */
static void lock(Connection *conn) {
	gw_assert(conn != NULL);

	if (conn->claimed)
		gw_assert(pthread_equal(conn->claimer, pthread_self()));
	else
		mutex_lock(conn->mutex);
}

/* Unlock a Connection, if it is unclaimed */
static void unlock(Connection *conn) {
	gw_assert(conn != NULL);

	if (!conn->claimed)
		mutex_unlock(conn->mutex);
}

/* Return the number of bytes in the Connection's output buffer */
static long unlocked_outbuf_len(Connection *conn) {
	return octstr_len(conn->outbuf) - conn->outbufpos;
}

/* Return the number of bytes in the Connection's input buffer */
static long unlocked_inbuf_len(Connection *conn) {
	return octstr_len(conn->inbuf) - conn->inbufpos;
}

/* Send as much data as can be sent without blocking.  Return the number
 * of bytes written, or -1 in case of error. */
static long unlocked_write(Connection *conn) {
	long ret;

	ret = octstr_write_data(conn->outbuf, conn->fd, conn->outbufpos);

	if (ret < 0)
		return -1;

	conn->outbufpos += ret;

	/* Heuristic: Discard the already-written data if it's more than
	 * half of the total.  This should keep the buffer size small
	 * without wasting too much cycles on moving data around. */
	if (conn->outbufpos > octstr_len(conn->outbuf) / 2) {
		octstr_delete(conn->outbuf, 0, conn->outbufpos);
		conn->outbufpos = 0;
	}

	return ret;
}

/* Try to empty the output buffer without blocking.  Return 0 for success,
 * 1 if there is still data left in the buffer, and -1 for errors. */
static int unlocked_try_write(Connection *conn) {
	long len;

	len = unlocked_outbuf_len(conn);
	if (len == 0)
		return 0;

	/* Heuristic: avoid using too many separate write() calls by
	 * writing only 4kB or larger chunks.  Let conn_wait() flush
	 * leftover data. */
	if (len < 4096)
		return 1;

	if (unlocked_write(conn) < 0)
		return -1;

	if (unlocked_outbuf_len(conn) > 0)
		return 1;

	return 0;
}

/* Read whatever data is currently available, up to an internal maximum. */
static void unlocked_read(Connection *conn) {
	unsigned char buf[4096];
	long len;

	if (conn->inbufpos > 0) {
		octstr_delete(conn->inbuf, 0, conn->inbufpos);
		conn->inbufpos = 0;
	}

	len = read(conn->fd, buf, sizeof(buf));
	if (len < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		error(errno, "Error reading from fd %d:", conn->fd);
		return;
	}

	octstr_append_data(conn->inbuf, buf, len);
}

/* Cut "length" octets from the input buffer and return them as an Octstr */
static Octstr *unlocked_get(Connection *conn, long length) {
	Octstr *result = NULL;

	gw_assert(unlocked_inbuf_len(conn) >= length);
	result = octstr_copy(conn->inbuf, conn->inbufpos, length);
	conn->inbufpos += length;

	return result;
}

Connection *conn_open_tcp(Octstr *host, int port) {
	panic(0, "conn_open_tcp not implemented");
	return NULL;
}

Connection *conn_wrap_fd(int fd) {
	Connection *conn;

	conn = gw_malloc(sizeof(*conn));
	conn->mutex = mutex_create();
	conn->claimed = 0;

	conn->outbuf = octstr_create_empty();
	conn->outbufpos = 0;
	conn->inbuf = octstr_create_empty();
	conn->inbufpos = 0;

	conn->fd = fd;

	return conn;
}

void conn_destroy(Connection *conn) {
	int ret;

	if (conn == NULL)
		return;

	/* No locking done here.  conn_destroy should not be called
	 * if any thread might still be interested in the connection. */

	if (conn->fd >= 0) {
		ret = close(conn->fd);
		if (ret < 0)
			error(errno, "conn_destroy: error on close");
		conn->fd = -1;
	}

	octstr_destroy(conn->outbuf);
	octstr_destroy(conn->inbuf);
	mutex_destroy(conn->mutex);

	gw_free(conn);
}

void conn_claim(Connection *conn) {
	if (conn->claimed)
		panic(0, "Connection is being claimed twice!");
	conn->claimed = 1;
#ifndef NDEBUG
	conn->claimer = pthread_self();
#endif
}

long conn_outbuf_len(Connection *conn) {
	long len;

	lock(conn);
	len = unlocked_outbuf_len(conn);
	unlock(conn);

	return len;
}

long conn_inbuf_len(Connection *conn) {
	long len;

	lock(conn);
	len = unlocked_inbuf_len(conn);
	unlock(conn);

	return len;
}

int conn_wait(Connection *conn, double seconds) {
	struct pollfd pollinfo;
	int milliseconds;
	int ret;

	lock(conn);

	/* Try to write any data that might still be waiting to be sent */
	if (unlocked_write(conn) < 0)
		goto error_unlock;

	/* Normally, we block until there is more data available.  But
	 * if any data still needs to be sent, we block until we can
	 * send it (or there is more data available). */
	if (unlocked_outbuf_len(conn) > 0)
		pollinfo.events = POLLIN | POLLOUT;
	else
		pollinfo.events = POLLIN;
	pollinfo.fd = conn->fd;

	/* Don't keep the connection locked while we wait */
	unlock(conn);

	milliseconds = seconds * 1000;
	ret = poll(&pollinfo, 1, milliseconds);
	if (ret < 0) {
		if (errno == EINTR)
			return 0;
		error(errno, "conn_wait: poll failed on fd %d:", pollinfo.fd);
		return -1;
	}

	if (pollinfo.revents & POLLNVAL) {
		error(0, "conn_wait: fd %d not open.", pollinfo.fd);
		return -1;
	}

	if (pollinfo.revents & (POLLERR | POLLHUP)) {
		/* Call unlocked_read report the specific error,
		 * and handle the results of the error.  We can't be
		 * certain that the error still exists, because we
		 * released the lock for a while. */
		lock(conn);
		unlocked_read(conn);
		unlock(conn);
		return -1;
	}

	if (pollinfo.revents & (POLLOUT | POLLIN)) {
		lock(conn);

		/* If POLLOUT was on, then we must have wanted
		 * to write something. */
		if (pollinfo.revents & POLLOUT)
			unlocked_write(conn);

		/* Since we always select for reading, we must always
		 * try to read here.  Otherwise, if the caller loops
		 * around conn_wait without making conn_read* calls
		 * in between, we will keep polling this same data. */
		if (pollinfo.revents & POLLIN)
			unlocked_read(conn);

		unlock(conn);
	}

	return 0;

error_unlock:
	unlock(conn);
	return -1;
}

int conn_write(Connection *conn, Octstr *data) {
	int ret;

	lock(conn);
	octstr_append(conn->outbuf, data);
	ret = unlocked_try_write(conn);
	unlock(conn);

	return ret;
}

int conn_write_data(Connection *conn, unsigned char *data, long length) {
	int ret;

	lock(conn);
	octstr_append_data(conn->outbuf, data, length);
	ret = unlocked_try_write(conn);
	unlock(conn);

	return ret;
}

int conn_write_withlen(Connection *conn, Octstr *data) {
	int ret;
	unsigned char lengthbuf[4];

	encode_network_long(lengthbuf, octstr_len(data));
	lock(conn);
	octstr_append_data(conn->outbuf, lengthbuf, 4);
	octstr_append(conn->outbuf, data);
	ret = unlocked_try_write(conn);
	unlock(conn);

	return ret;
}

Octstr *conn_read_fixed(Connection *conn, long length) {
	Octstr *result = NULL;

	/* See if the data is already available.  If not, try a read(),
	 * then see if we have enough data after that.  If not, give up. */
	lock(conn);
	if (unlocked_inbuf_len(conn) < length) {
		unlocked_read(conn);
		if (unlocked_inbuf_len(conn) < length) {
			unlock(conn);
			return NULL;
		}
	}
	result = unlocked_get(conn, length);
	unlock(conn);

	return result;
}

Octstr *conn_read_line(Connection *conn) {
	Octstr *result = NULL;
	long pos;

	lock(conn);
	/* 10 is the code for linefeed.  We don't rely on \n because that
	 * might be a different value on some (strange) systems. */
	pos = octstr_search_char_from(conn->inbuf, 10, conn->inbufpos);
	if (pos < 0) {
		unlocked_read(conn);
		pos = octstr_search_char_from(conn->inbuf,
					10, conn->inbufpos);
		if (pos < 0) {
			unlock(conn);
			return NULL;
		}
	}
		
	result = unlocked_get(conn, pos - conn->inbufpos);
	/* Check if it was a CR LF */
	if (octstr_len(result) > 0 &&
	    octstr_get_char(result, octstr_len(result) - 1) == 13)
		octstr_delete(result, octstr_len(result) - 1, 1);

	unlock(conn);
	return result;
}

Octstr *conn_read_withlen(Connection *conn) {
	Octstr *result = NULL;
	unsigned char lengthbuf[4];
	long length;

	lock(conn);

retry:
	/* First get the length. */
	if (unlocked_inbuf_len(conn) < 4) {
		unlocked_read(conn);
		if (unlocked_inbuf_len(conn) < 4) {
			unlock(conn);
			return NULL;
		}
	}
	octstr_get_many_chars(lengthbuf, conn->inbuf, conn->inbufpos, 4);
	length = decode_network_long(lengthbuf);

	if (length < 0) {
		warning(0, __FUNCTION__ ": got negative length, skipping");
		conn->inbufpos += 4;
		goto retry;
	}

	/* Then get the data. */
	if (unlocked_inbuf_len(conn) - 4 < length) {
		unlocked_read(conn);
		if (unlocked_inbuf_len(conn) - 4 < length) {
			unlock(conn);
			return NULL;
		}
	}

	conn->inbufpos += 4;
	result = unlocked_get(conn, length);

	unlock(conn);
	return result;
}

Octstr *conn_read_packet(Connection *conn, int startmark, int endmark) {
	panic(0, "conn_read_packet not implemented");
	return NULL;
}
