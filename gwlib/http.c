/*
 * http.c - HTTP protocol server and client implementation
 *
 * Implements major parts of the Hypertext Transfer Protocol HTTP/1.1 (RFC 2616)
 * See http://www.w3.org/Protocols/rfc2616/rfc2616.txt
 *
 * Lars Wirzenius
 */
 
/* XXX re-implement socket pools, with idle connection killing to 
    	save sockets */
/* XXX implement http_abort */
/* XXX give maximum input size */
/* XXX kill http_get_real */
/* XXX the proxy exceptions list should be a dict, I guess */
/* XXX set maximum number of concurrent connections to same host, total? */
/* XXX 100 status codes. */
/* XXX stop destroying persistent connections when a request is redirected */

#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gwlib.h"

/* comment this out if you don't want Keep-Alive HTTP requests */
#define USE_KEEPALIVE 1


/***********************************************************************
 * Stuff used in several sub-modules.
 */


/*
 * Default port to connect to for HTTP connections.
 */
enum { HTTP_PORT = 80,
       HTTPS_PORT = 443 };


/*
 * Status of this module.
 */
static enum { 
    limbo, 
    running, 
    terminating 
} run_status = limbo;


/*
 * Read some headers, i.e., until the first empty line (read and discard
 * the empty line as well). Return -1 for error, 0 for all headers read,
 * 1 for more headers to follow.
 */
static int read_some_headers(Connection *conn, List *headers)
{
    Octstr *line, *prev;

    if (list_len(headers) == 0)
        prev = NULL;
    else
    	prev = list_get(headers, list_len(headers) - 1);

    for (;;) {
	line = conn_read_line(conn);
	if (line == NULL) {
	    if (conn_eof(conn))
	    	return -1;
	    return 1;
	}
        if (octstr_len(line) == 0) {
            octstr_destroy(line);
            break;
        }
        if (isspace(octstr_get_char(line, 0)) && prev != NULL) {
            octstr_append(prev, line);
            octstr_destroy(line);
        } else {
            list_append(headers, line);
            prev = line;
        }
    }

    return 0;
}


/*
 * Check that the HTTP version string is valid. Return -1 for invalid,
 * 0 for version 1.0, 1 for 1.x.
 */
static int parse_http_version(Octstr *version)
{
    Octstr *prefix;
    long prefix_len;
    int digit;
    
    prefix = octstr_imm("HTTP/1.");
    prefix_len = octstr_len(prefix);

    if (octstr_ncompare(version, prefix, prefix_len) != 0)
    	return -1;
    if (octstr_len(version) != prefix_len + 1)
    	return -1;
    digit = octstr_get_char(version, prefix_len);
    if (!isdigit(digit))
    	return -1;
    if (digit == '0')
    	return 0;
    return 1;
}


/***********************************************************************
 * Proxy support.
 */


/*
 * Data and functions needed to support proxy operations. If proxy_hostname 
 * is NULL, no proxy is used.
 */
static Mutex *proxy_mutex = NULL;
static Octstr *proxy_hostname = NULL;
static int proxy_port = 0;
static Octstr *proxy_username = NULL;
static Octstr *proxy_password = NULL;
static List *proxy_exceptions = NULL;


static void proxy_add_authentication(List *headers)
{
    Octstr *os;
    
    if (proxy_username == NULL || proxy_password == NULL)
    	return;

    os = octstr_format("%S:%S", proxy_username, proxy_password);
    octstr_binary_to_base64(os);
    octstr_strip_blanks(os);
    octstr_insert(os, octstr_imm("Basic "), 0);
    http_header_add(headers, "Proxy-Authorization", octstr_get_cstr(os));
    octstr_destroy(os);
}


static void proxy_init(void)
{
    proxy_mutex = mutex_create();
    proxy_exceptions = list_create();
}


static void proxy_shutdown(void)
{
    http_close_proxy();
    mutex_destroy(proxy_mutex);
    proxy_mutex = NULL;
}


static int proxy_used_for_host(Octstr *host)
{
    int i;

    mutex_lock(proxy_mutex);

    if (proxy_hostname == NULL) {
        mutex_unlock(proxy_mutex);
        return 0;
    }

    for (i = 0; i < list_len(proxy_exceptions); ++i) {
        if (octstr_compare(host, list_get(proxy_exceptions, i)) == 0) {
            mutex_unlock(proxy_mutex);
            return 0;
        }
    }

    mutex_unlock(proxy_mutex);
    return 1;
}


void http_use_proxy(Octstr *hostname, int port, List *exceptions,
    	    	    Octstr *username, Octstr *password)
{
    Octstr *e;
    int i;

    gw_assert(run_status == running);
    gw_assert(hostname != NULL);
    gw_assert(octstr_len(hostname) > 0);
    gw_assert(port > 0);

    http_close_proxy();
    mutex_lock(proxy_mutex);

    proxy_hostname = octstr_duplicate(hostname);
    proxy_port = port;
    proxy_exceptions = list_create();
    for (i = 0; i < list_len(exceptions); ++i) {
        e = list_get(exceptions, i);
	debug("gwlib.http", 0, "HTTP: Proxy exception `%s'.",
	      octstr_get_cstr(e));
        list_append(proxy_exceptions, octstr_duplicate(e));
    }
    proxy_username = octstr_duplicate(username);
    proxy_password = octstr_duplicate(password);
    debug("gwlib.http", 0, "Using proxy <%s:%d>", 
    	  octstr_get_cstr(proxy_hostname), proxy_port);

    mutex_unlock(proxy_mutex);
}


void http_close_proxy(void)
{
    gw_assert(run_status == running || run_status == terminating);

    mutex_lock(proxy_mutex);
    proxy_port = 0;
    octstr_destroy(proxy_hostname);
    octstr_destroy(proxy_username);
    octstr_destroy(proxy_password);
    proxy_hostname = NULL;
    proxy_username = NULL;
    proxy_password = NULL;
    list_destroy(proxy_exceptions, octstr_destroy_item);
    proxy_exceptions = NULL;
    mutex_unlock(proxy_mutex);
}


/***********************************************************************
 * Common functions for reading request or result entities.
 */

/*
 * Value to pass to entity_create.
 */
enum body_expectation {
   /*
    * Message must not have a body, even if the headers indicate one.
    * (i.e. response to HEAD method).
    */
   expect_no_body,
   /*
    * Message will have a body if Content-Length or Transfer-Encoding
    * headers are present (i.e. most request methods).
    */
   expect_body_if_indicated,
   /*
    * Message will have a body, possibly zero-length.
    * (i.e. 200 OK responses to a GET method.)
    */
   expect_body
};

enum entity_state {
    reading_headers,
    reading_chunked_body_len,
    reading_chunked_body_data,
    reading_chunked_body_crlf,
    reading_chunked_body_trailer,
    reading_body_until_eof,
    reading_body_with_length,
    body_error,
    entity_done
};

typedef struct {
    List *headers;
    Octstr *body;
    enum body_expectation expect_state;
    enum entity_state state;
    long chunked_body_chunk_len;
    long expected_body_len;
} HTTPEntity;


/*
 * The rules for message bodies (length and presence) are defined
 * in RFC2616 paragraph 4.3 and 4.4.
 */
static void deduce_body_state(HTTPEntity *ent)
{
    Octstr *h = NULL;

    if (ent->expect_state == expect_no_body) {
	ent->state = entity_done;
	return;
    }

    ent->state = body_error;  /* safety net */

    h = http_header_find_first(ent->headers, "Transfer-Encoding");
    if (h != NULL) {
        octstr_strip_blanks(h);
        if (octstr_str_compare(h, "chunked") != 0) {
            error(0, "HTTP: Unknown Transfer-Encoding <%s>",
                  octstr_get_cstr(h));
	    ent->state = body_error;
        } else {
            ent->state = reading_chunked_body_len;
	}
        octstr_destroy(h);
	return;
    }

    h = http_header_find_first(ent->headers, "Content-Length");
    if (h != NULL) {
        if (octstr_parse_long(&ent->expected_body_len, h, 0, 10) == -1) {
	    error(0, "HTTP: Content-Length header wrong: <%s>",
		  octstr_get_cstr(h));
	    ent->state = body_error;
        } else {
            ent->state = reading_body_with_length;
	}
        octstr_destroy(h);
	return;
    }

    if (ent->expect_state == expect_body)
        ent->state = reading_body_until_eof;
    else
	ent->state = entity_done;
}


/*
 * Create a HTTPEntity structure suitable for reading the expected
 * result or request message and decoding the transferred entity (if any).
 * See the definition of enum body_expectation for the possible values
 * of exp.
 */
static HTTPEntity *entity_create(enum body_expectation exp)
{
    HTTPEntity *ent;

    ent = gw_malloc(sizeof(*ent));
    ent->headers = http_create_empty_headers();
    ent->body = octstr_create("");
    ent->chunked_body_chunk_len = -1;
    ent->expected_body_len = -1;
    ent->state = reading_headers;
    ent->expect_state = exp;

    return ent;
}


static void entity_destroy(HTTPEntity *ent)
{
    if (ent == NULL)
        return;

    http_destroy_headers(ent->headers);
    octstr_destroy(ent->body);
    gw_free(ent);
}


static void read_chunked_body_len(HTTPEntity *ent, Connection *conn)
{
    Octstr *os;
    long len;
    
    os = conn_read_line(conn);
    if (os == NULL) {
        if (conn_read_error(conn) || conn_eof(conn))
	    ent->state = body_error;
        return;
    }
    if (octstr_parse_long(&len, os, 0, 16) == -1) {
        octstr_destroy(os);
	ent->state = body_error;
        return;
    }
    octstr_destroy(os);
    if (len == 0)
        ent->state = reading_chunked_body_trailer;
    else {
        ent->state = reading_chunked_body_data;
        ent->chunked_body_chunk_len = len;
    }
}


static void read_chunked_body_data(HTTPEntity *ent, Connection *conn)
{
    Octstr *os;

    os = conn_read_fixed(conn, ent->chunked_body_chunk_len);
    if (os == NULL) {
        if (conn_read_error(conn) || conn_eof(conn))
	    ent->state = body_error;
    } else {
        octstr_append(ent->body, os);
        octstr_destroy(os);
        ent->state = reading_chunked_body_crlf;
    }
}


static void read_chunked_body_crlf(HTTPEntity *ent, Connection *conn)
{
    Octstr *os;

    os = conn_read_line(conn);
    if (os == NULL) {
        if (conn_read_error(conn) || conn_eof(conn))
	    ent->state = body_error;
    } else {
        octstr_destroy(os);
        ent->state = reading_chunked_body_len;
    }
}


static void read_chunked_body_trailer(HTTPEntity *ent, Connection *conn)
{
    int ret;

    ret = read_some_headers(conn, ent->headers);
    if (ret == -1)
	ent->state = body_error;
    if (ret == 0)
        ent->state = entity_done;
}


static void read_body_until_eof(HTTPEntity *ent, Connection *conn)
{
    Octstr *os;

    while ((os = conn_read_everything(conn)) != NULL) {
        octstr_append(ent->body, os);
        octstr_destroy(os);
    }
    if (conn_read_error(conn))
	ent->state = body_error;
    if (conn_eof(conn))
	ent->state = entity_done;
}


static void read_body_with_length(HTTPEntity *ent, Connection *conn)
{
    Octstr *os;

    os = conn_read_fixed(conn, ent->expected_body_len);
    if (os == NULL)
        return;
    octstr_destroy(ent->body);
    ent->body = os;
    ent->state = entity_done;
}


/*
 * Read headers and body (if any) from this connection.  Return 0 if it's
 * complete, 1 if we expect more input, and -1 if there is something wrong.
 */
static int entity_read(HTTPEntity *ent, Connection *conn)
{
    int ret;
    enum entity_state old_state;

    /*
     * In this loop, each state will process as much input as it needs
     * and then switch to the next state, unless it's a final state in
     * which case it returns directly, or unless it needs more input.
     * So keep looping as long as the state changes.
     */
    do {
	old_state = ent->state;
	switch (ent->state) {
	case reading_headers:
	    ret = read_some_headers(conn, ent->headers);
            if (ret == 0)
	        deduce_body_state(ent);
	    if (ret < 0)
		return -1;
	    break;

	case reading_chunked_body_len:
	    read_chunked_body_len(ent, conn);
	    break;
		
	case reading_chunked_body_data:
	    read_chunked_body_data(ent, conn);
	    break;

	case reading_chunked_body_crlf:
	    read_chunked_body_crlf(ent, conn);
	    break;

	case reading_chunked_body_trailer:
	    read_chunked_body_trailer(ent, conn);
	    break;

	case reading_body_until_eof:
	    read_body_until_eof(ent, conn);
	    break;

	case reading_body_with_length:
	    read_body_with_length(ent, conn);
	    break;

	case body_error:
	    return -1;

	case entity_done:
	    return 0;

	default:
	    panic(0, "Internal error: Invalid HTTPEntity state.");
	}
    } while (ent->state != old_state);

    /*
     * If we got here, then the loop ended because a non-final state
     * needed more input.
     */
    return 1;
}


/***********************************************************************
 * HTTP client interface.
 */


/*
 * Maximum number of HTTP redirections to follow. Making this infinite
 * could cause infinite looping if the redirections loop.
 */
enum { HTTP_MAX_FOLLOW = 5 };


/*
 * The implemented HTTP method strings
 * Order is sequenced by the enum in the header
 */
static char *http_methods[] = {
    "GET", "POST", "HEAD"
};

/*
 * Information about a server we've connected to.
 */
typedef struct {
    HTTPCaller *caller;
    void *request_id;
    int method;             /* uses enums from http.h for the HTTP methods */
    Octstr *url;
    List *request_headers;
    Octstr *request_body;   /* NULL for GET or HEAD, non-NULL for POST */
    enum {
      connecting,
	request_not_sent,
	reading_status,
	reading_entity,
	transaction_done
    } state;
    long status;
    int persistent;
    HTTPEntity *response; /* Can only be NULL if status < 0 */
    Connection *conn;
    Octstr *host;
    long port;
    int retrying;
    int follow_remaining;
    Octstr *certkeyfile;
    int ssl;
    Octstr *username;	/* For basic authentication */
    Octstr *password;
} HTTPServer;


static int send_request(HTTPServer *trans);

static HTTPServer *server_create(HTTPCaller *caller, int method, Octstr *url,
                                 List *headers, Octstr *body, int follow_remaining,
                                 Octstr *certkeyfile)
{
    HTTPServer *trans;
    
    trans = gw_malloc(sizeof(*trans));
    trans->caller = caller;
    trans->request_id = NULL;
    trans->method = method;
    trans->url = octstr_duplicate(url);
    trans->request_headers = http_header_duplicate(headers);
    trans->request_body = octstr_duplicate(body);
    trans->state = request_not_sent;
    trans->status = -1;
    trans->persistent = 0;
    trans->response = NULL;
    trans->conn = NULL;
    trans->host = NULL;
    trans->port = 0;
    trans->username = NULL;
    trans->password = NULL;
    trans->retrying = 0;
    trans->follow_remaining = follow_remaining;
    trans->certkeyfile = certkeyfile;
    trans->ssl = 0;
    return trans;
}


static void server_destroy(void *p)
{
    HTTPServer *trans;
    
    trans = p;
    octstr_destroy(trans->url);
    http_destroy_headers(trans->request_headers);
    trans->request_headers = NULL;
    octstr_destroy(trans->request_body);
    entity_destroy(trans->response);
    octstr_destroy(trans->host);
    gw_free(trans);
}


/*
 * Pool of open, but unused connections to servers or proxies. Key is
 * "servername:port", value is List with Connection objects.
 */
static Dict *conn_pool = NULL;
static Mutex *conn_pool_lock = NULL;


static void conn_pool_item_destroy(void *item)
{
    Connection *conn;
    
    while ((conn = list_extract_first(item)) != NULL)
    	conn_destroy(conn);
    list_destroy(item, NULL);
}

static void conn_pool_init(void)
{
    conn_pool = dict_create(1024, conn_pool_item_destroy);
    conn_pool_lock = mutex_create();
}


static void conn_pool_shutdown(void)
{
    dict_destroy(conn_pool);
    mutex_destroy(conn_pool_lock);
}


static Octstr *conn_pool_key(Octstr *host, int port)
{
    return octstr_format("%S:%d", host, port);
}


static Connection *conn_pool_get(Octstr *host, int port, int ssl, Octstr *certkeyfile,
		Octstr *our_host)
{
    Octstr *key;
    List *list;
    Connection *conn;

    mutex_lock(conn_pool_lock);
    key = conn_pool_key(host, port);
    list = dict_get(conn_pool, key);
    octstr_destroy(key);
    if (list == NULL)
    	conn = NULL;
    else {
	while (1) {
	    conn = list_extract_first(list);
	    if (conn == NULL)
		break;
	    /* Check whether the server has closed the connection while
	     * it has been in the pool. */
	    conn_wait(conn, 0);
	    if (!conn_eof(conn) && !conn_read_error(conn))
		break;
	    conn_destroy(conn);
	}
    }
    mutex_unlock(conn_pool_lock);
    
    if (conn == NULL) {
#ifdef HAVE_LIBSSL
	if (ssl) 
	    conn = conn_open_ssl(host, port, certkeyfile, our_host);
	else
#endif /* HAVE_LIBSSL */
	    conn = conn_open_tcp_nb(host, port, our_host);
	debug("gwlib.http", 0, "HTTP: Opening connection to `%s:%d' (fd=%d).",
	      octstr_get_cstr(host), port, conn_get_id(conn));
    } else {
	debug("gwlib.http", 0, "HTTP: Reusing connection to `%s:%d' (fd=%d).",
	      octstr_get_cstr(host), port, conn_get_id(conn)); 
    }
    
    return conn;
}

#ifdef USE_KEEPALIVE
static void conn_pool_put(Connection *conn, Octstr *host, int port)
{
    Octstr *key;
    List *list;
    
    mutex_lock(conn_pool_lock);
    key = conn_pool_key(host, port);
    list = dict_get(conn_pool, key);
    if (list == NULL) {
    	list = list_create();
        dict_put(conn_pool, key, list);
    }
    list_append(list, conn);
    octstr_destroy(key);
    mutex_unlock(conn_pool_lock);
}
#endif


/*
 * Internal lists of completely unhandled requests and requests for which
 * a request has been sent but response has not yet been read.
 */
static List *pending_requests = NULL;


/*
 * Have background threads been started?
 */
static Mutex *client_thread_lock = NULL;
static volatile sig_atomic_t client_threads_are_running = 0;


/*
 * Set of all connections to all servers. Used with conn_register to
 * do I/O on several connections with a single thread.
 */
static FDSet *client_fdset = NULL;


HTTPCaller *http_caller_create(void)
{
    HTTPCaller *caller;
    
    caller = list_create();
    list_add_producer(caller);
    return caller;
}


void http_caller_destroy(HTTPCaller *caller)
{
    list_destroy(caller, server_destroy);
}


void http_caller_signal_shutdown(HTTPCaller *caller)
{
    list_remove_producer(caller);
}


static Octstr *get_redirection_location(HTTPServer *trans)
{
    if (trans->status < 0 || trans->follow_remaining <= 0)
    	return NULL;
    if (trans->status != HTTP_MOVED_PERMANENTLY &&
    	trans->status != HTTP_FOUND && trans->status != HTTP_SEE_OTHER)
	return NULL;
    if (trans->response == NULL)
        return NULL;
    return http_header_find_first(trans->response->headers, "Location");
}


/*
 * Read and parse the status response line from an HTTP server.
 * Fill in trans->persistent and trans->status with the findings.
 * Return -1 for error, 1 for status line not yet available, 0 for OK.
 */
static int client_read_status(HTTPServer *trans)
{
    Octstr *line, *version;
    long space;
    int ret;

    line = conn_read_line(trans->conn);
    if (line == NULL) {
	if (conn_eof(trans->conn) || conn_read_error(trans->conn))
	    return -1;
    	return 1;
    }

    debug("gwlib.http", 0, "HTTP: Status line: <%s>", octstr_get_cstr(line));

    space = octstr_search_char(line, ' ', 0);
    if (space == -1)
    	goto error;
	
    version = octstr_copy(line, 0, space);
    ret = parse_http_version(version);
    octstr_destroy(version);
    if (ret == -1)
    	goto error;
    trans->persistent = ret;

    octstr_delete(line, 0, space + 1);
    space = octstr_search_char(line, ' ', 0);
    if (space == -1)
    	goto error;
    octstr_truncate(line, space);
	
    if (octstr_parse_long(&trans->status, line, 0, 10) == -1)
        goto error;

    octstr_destroy(line);
    return 0;

error:
    error(0, "HTTP: Malformed status line from HTTP server: <%s>",
	  octstr_get_cstr(line));
    octstr_destroy(line);
    return -1;
}

static int response_expectation(int method, int status)
{
    if (status == HTTP_NO_CONTENT ||
        status == HTTP_NOT_MODIFIED ||
        http_status_class(status) == HTTP_STATUS_PROVISIONAL ||
        method == HTTP_METHOD_HEAD)
	return expect_no_body;
    else
        return expect_body;
}

static void handle_transaction(Connection *conn, void *data)
{
    HTTPServer *trans;
    int ret;
    Octstr *h;
    int rc;
    char buf[128];
    
    trans = data;

    if (run_status != running) {
	conn_unregister(conn);
	return;
    }

    while (trans->state != transaction_done) {
	switch (trans->state) {
	case connecting:
	  debug("gwlib.http", 0, "Get info about connecting socket");
          if (conn_get_connect_result(trans->conn) != 0) {
            debug("gwlib.http", 0, "Socket not connected");
            conn_unregister(conn);
            goto error;
          }

          if (trans->method == HTTP_METHOD_POST) {
            /* 
             * Add a Content-Length header.  Override an existing one, if
             * necessary.  We must have an accurate one in order to use the
             * connection for more than a single request.
             */
            http_header_remove_all(trans->request_headers, "Content-Length");
            sprintf(buf, "%ld", octstr_len(trans->request_body));
            http_header_add(trans->request_headers, "Content-Length", buf);
          } 
          /* 
           * ok, this has to be an GET or HEAD request method then,
           * if it contains a body, then this is not HTTP conform, so at
           * least warn the user 
           */
          else if (trans->request_body != NULL) {
            warning(0, "HTTP: GET or HEAD method request contains body:");
            octstr_dump(trans->request_body, 0);
          }

          if ((rc = send_request(trans)) == 0) {
            trans->state = reading_status;
            conn_register(trans->conn, client_fdset, handle_transaction, 
                          trans);
          } else {
            list_produce(trans->caller, trans);
          }
          break;

	case reading_status:
	    ret = client_read_status(trans);
	    if (ret < 0) {
		/*
		 * Couldn't read the status from the socket. This may mean 
		 * that the socket had been closed by the server after an 
		 * idle timeout, so we close the connection and try again, 
		 * opening a new socket, but only once.
		 */
		if (trans->retrying) {
            debug("gwlib.http",0,"Failed while retrying");
		    goto error;
		} else {
		    conn_destroy(trans->conn);
		    trans->conn = NULL;
		    trans->retrying = 1;
		    trans->state = request_not_sent;
		    list_produce(pending_requests, trans);
		    return;
		}
	    } else if (ret == 0) {
		/* Got the status, go read headers and body next. */
		trans->state = reading_entity;
		trans->response =
		    entity_create(response_expectation(trans->method, trans->status));
	    } else
		return;
	    break;
	    
	case reading_entity:
	    ret = entity_read(trans->response, conn);
	    if (ret < 0) {
            debug("gwlib.http",0,"Failed reading entity");
            goto error;
	    } else if (ret == 0 && http_status_class(trans->status)
                                  == HTTP_STATUS_PROVISIONAL) {
                    /* This was a provisional reply; get the real one now. */
                    trans->state = reading_status;
            } else if (ret == 0) {
		    trans->state = transaction_done;
	    } else {
		return;
            }
	    break;

	default:
	    panic(0, "Internal error: Invalid HTTPServer state.");
	}
    }

    conn_unregister(trans->conn);

    h = http_header_find_first(trans->response->headers, "Connection");
    if (h != NULL && octstr_compare(h, octstr_imm("close")) == 0)
	trans->persistent = 0;
    octstr_destroy(h);

#ifdef USE_KEEPALIVE 
    if (trans->persistent)
        conn_pool_put(trans->conn, trans->host, trans->port);
    else
#endif
    	conn_destroy(trans->conn);

    trans->conn = NULL;

    h = get_redirection_location(trans);
    if (h != NULL) {
	octstr_strip_blanks(h);
	octstr_destroy(trans->url);
	trans->url = h;
	trans->state = request_not_sent;
	trans->status = -1;
	http_destroy_headers(trans->response->headers);
	trans->response->headers = list_create();
	octstr_destroy(trans->response->body);
	trans->response->body = octstr_create("");
	--trans->follow_remaining;
	conn_destroy(trans->conn);
	trans->conn = NULL;
	list_produce(pending_requests, trans);
    } else
	list_produce(trans->caller, trans);
    return;

error:
    conn_destroy(trans->conn);
    trans->conn = NULL;
    error(0, "Couldn't fetch <%s>", octstr_get_cstr(trans->url));
    trans->status = -1;
    list_produce(trans->caller, trans);
}


/*
 * Build a complete HTTP request given the host, port, path and headers. 
 * Add Host: and Content-Length: headers (and others that may be necessary).
 * Return the request as an Octstr.
 */
static Octstr *build_request(char *method_name, Octstr *path_or_url, 
                             Octstr *host, long port, List *headers, 
                             Octstr *request_body)
{
    /* XXX headers missing */
    Octstr *request;
    int i;

    request = octstr_format("%s %S HTTP/1.1\r\n",
                            method_name, path_or_url);

    octstr_format_append(request, "Host: %S", host);
    if (port != HTTP_PORT)
        octstr_format_append(request, ":%ld", port);
    octstr_append(request, octstr_imm("\r\n"));

    for (i = 0; headers != NULL && i < list_len(headers); ++i) {
        octstr_append(request, list_get(headers, i));
        octstr_append(request, octstr_imm("\r\n"));
    }
    octstr_append(request, octstr_imm("\r\n"));

    if (request_body != NULL)
        octstr_append(request, request_body);

    return request;
}


/*
 * Parse the URL to get the hostname and the port to connect to and the
 * path within the host.
 *
 * Return -1 if the URL seems malformed.
 *
 * We assume HTTP URLs of the form specified in "3.2.2 http URL" in
 * RFC 2616:
 * 
 *  http_URL = "http:" "//" [ userid : password "@"] host [ ":" port ] [ abs_path [ "?" query ]] 
 */
static int parse_url(Octstr *url, Octstr **host, long *port, Octstr **path, 
		     int *ssl, Octstr **username, Octstr **password)
{
    Octstr *prefix, *prefix_https;
    long prefix_len;
    int host_len, colon, slash, at, auth_sep = 0;

    prefix = octstr_imm("http://");
    prefix_https = octstr_imm("https://");
    prefix_len = octstr_len(prefix);

    if (octstr_case_search(url, prefix, 0) != 0) {
        if (octstr_case_search(url, prefix_https, 0) == 0) {
#ifdef HAVE_LIBSSL
            debug("gwlib.http", 0, "HTTPS URL; Using SSL for the connection");
            prefix = prefix_https;
            prefix_len = octstr_len(prefix_https);	
            *ssl = 1;
#else
            error(0, "Attempt to use HTTPS <%s> but SSL not compiled in", 
                  octstr_get_cstr(url));
            return -1;
#endif
        } else {
            error(0, "URL <%s> doesn't start with `%s' nor `%s'",
            octstr_get_cstr(url), octstr_get_cstr(prefix),
            octstr_get_cstr(prefix_https));
            return -1;
        }
    }

    if (octstr_len(url) == prefix_len) {
        error(0, "URL <%s> is malformed.", octstr_get_cstr(url));
        return -1;
    }

    colon = octstr_search_char(url, ':', prefix_len);
    slash = octstr_search_char(url, '/', prefix_len);
    if (colon == prefix_len || slash == prefix_len) {
        error(0, "URL <%s> is malformed.", octstr_get_cstr(url));
        return -1;
    }

    at = octstr_search_char(url, '@', prefix_len);
    if ( at != -1 ) {
	if ((slash == -1 || ( slash != -1 && at < slash))) {
	    auth_sep = octstr_search_char(url, ':', prefix_len);

	    if (auth_sep != -1 && (auth_sep < at)) {
		octstr_set_char(url, auth_sep, '@');
		colon = octstr_search_char(url, ':', prefix_len);
	    }
	} else {
	    at = -1;
	}
    }
    
    if (slash == -1 && colon == -1) {
        /* Just the hostname, no port or path. */
        host_len = octstr_len(url) - prefix_len;
#ifdef HAVE_LIBSSL
        *port = *ssl ? HTTPS_PORT : HTTP_PORT;
#else
        *port = HTTP_PORT;
#endif /* HAVE_LIBSSL */
    } else if (slash == -1) {
        /* Port, but not path. */
        host_len = colon - prefix_len;
        if (octstr_parse_long(port, url, colon + 1, 10) == -1) {
            error(0, "URL <%s> has malformed port number.",
                  octstr_get_cstr(url));
            return -1;
        }
    } else if (colon == -1 || colon > slash) {
        /* Path, but not port. */
        host_len = slash - prefix_len;
#ifdef HAVE_LIBSSL
        *port = *ssl ? HTTPS_PORT : HTTP_PORT;
#else
        *port = HTTP_PORT;
#endif /* HAVE_LIBSSL */
    } else if (colon < slash) {
        /* Both path and port. */
        host_len = colon - prefix_len;
        if (octstr_parse_long(port, url, colon + 1, 10) == -1) {
            error(0, "URL <%s> has malformed port number.",
                  octstr_get_cstr(url));
            return -1;
        }
    } else {
        error(0, "Internal error in URL parsing logic.");
        return -1;
    }


    if (at != -1) {
	int at2, i;
	at2 = octstr_search_char(url, '@', prefix_len);
	*username = octstr_copy(url, prefix_len, at2 - prefix_len);

	if (at2 != at)
	    *password = octstr_copy(url, at2 + 1, at - at2 - 1);
	else
	    *password = NULL;
    
    if (auth_sep != -1)
        octstr_set_char(url, auth_sep, ':');

	for(i = at2 + 1; i < at ; i++)
	    octstr_set_char(url, i, '*');
	host_len = host_len - at + prefix_len - 1;
	prefix_len = at + 1;
    }

    *host = octstr_copy(url, prefix_len, host_len);
    if (slash == -1)
        *path = octstr_create("/");
    else
        *path = octstr_copy(url, slash, octstr_len(url) - slash);


    return 0;
}

static Connection *get_connection(HTTPServer *trans) 
{
  Octstr *path;
    Connection *conn;
    Octstr *host, *our_host = NULL;
    int port;

    conn = NULL;
    path = NULL;

    /* May not be NULL if we're retrying this transaction. */
    octstr_destroy(trans->host);
    trans->host = NULL;

    if (parse_url(trans->url, &trans->host, &trans->port, &path, &trans->ssl,
                  &trans->username, &trans->password) == -1)
        goto error;


    if (proxy_used_for_host(trans->host)) {
        host = proxy_hostname;
        port = proxy_port;
    } else {
        host = trans->host;
        port = trans->port;
    }

    if (trans->retrying) {
#ifdef HAVE_LIBSSL
    if (trans->ssl) conn = conn_open_ssl(host, port, trans->certkeyfile, our_host);
        else
#endif /* HAVE_LIBSSL */
      conn = conn_open_tcp_nb(host, port, our_host);
            debug("gwlib.http", 0, "HTTP: Opening NEW connection to `%s:%d' (fd=%d).",
                  octstr_get_cstr(host), port, conn_get_id(conn));
    } else
    conn = conn_pool_get(host, port, trans->ssl, trans->certkeyfile,
                         our_host);
    if (conn == NULL)
        goto error;

  octstr_destroy(path);

  return conn;

 error:
  conn_destroy(conn);
  octstr_destroy(path);
  error(0, "Couldn't send request to <%s>", octstr_get_cstr(trans->url));
  return NULL;
}

/*
 * Build and send the HTTP request. Return socket from which the
 * response can be read or -1 for error.
 */

static int send_request(HTTPServer *trans)
{
  Octstr *path, *request;

  path = NULL;
  request = NULL;

  octstr_destroy(trans->host);
  trans->host = NULL;

  if (parse_url(trans->url, &trans->host, &trans->port, &path, &trans->ssl,
		&trans->username, &trans->password) == -1)
    goto error;

  if (trans->username != NULL)
    http_add_basic_auth(trans->request_headers, trans->username,
			trans->password);

  if (proxy_used_for_host(trans->host)) {
    proxy_add_authentication(trans->request_headers);
    request = build_request(http_method2name(trans->method),
			    trans->url, trans->host, trans->port, 
			    trans->request_headers, 
			    trans->request_body);
  } else {
    request = build_request(http_method2name(trans->method),path, 
			    trans->host, trans->port,
			    trans->request_headers,
			    trans->request_body);
  }
  
    debug("wsp.http", 0, "HTTP: Sending request:");
    octstr_dump(request, 0);
  if (conn_write(trans->conn, request) == -1)
        goto error;

    octstr_destroy(path);
    octstr_destroy(request);

  return 0;

 error:
  conn_destroy(trans->conn);
  trans->conn = NULL;
    octstr_destroy(path);
    octstr_destroy(request);
    error(0, "Couldn't send request to <%s>", octstr_get_cstr(trans->url));
  return -1;
}

/*
 * This thread starts the transaction: it connects to the server and sends
 * the request. It then sends the transaction to the read_response_thread
 * via started_requests_queue.
 */
static void write_request_thread(void *arg)
{
    HTTPServer *trans;
    char buf[128];    
    int rc;

    while (run_status == running) {
        trans = list_consume(pending_requests);
        if (trans == NULL)
            break;

        gw_assert(trans->state == request_not_sent);

	trans->conn = get_connection(trans);

	if (trans->conn == NULL)
	  list_produce(trans->caller, trans);
        else {
          if (conn_is_connected(trans->conn) == 0) {
	    debug("gwlib.http", 0, "Socket connected at once");

        if (trans->method == HTTP_METHOD_POST) {
            /* 
             * Add a Content-Length header.  Override an existing one, if
             * necessary.  We must have an accurate one in order to use the
             * connection for more than a single request.
             */
            http_header_remove_all(trans->request_headers, "Content-Length");
            sprintf(buf, "%ld", octstr_len(trans->request_body));
            http_header_add(trans->request_headers, "Content-Length", buf);
        } 
            /* 
             * ok, this has to be an GET or HEAD request method then,
             * if it contains a body, then this is not HTTP conform, so at
             * least warn the user 
             */
        else if (trans->request_body != NULL) {
            warning(0, "HTTP: GET or HEAD method request contains body:");
            octstr_dump(trans->request_body, 0);
        }
	    if ((rc = send_request(trans)) == 0) {
            trans->state = reading_status;
              conn_register(trans->conn, client_fdset, handle_transaction, 
                            trans);
            } else {
              list_produce(trans->caller, trans);
            }

          } else { /* Socket not connected, wait for connection */
            debug("gwlib.http", 0, "Socket connecting");
            trans->state = connecting;
            conn_register(trans->conn, client_fdset, handle_transaction, trans);
        }
	  
	}
    }
}


static void start_client_threads(void)
{
    if (!client_threads_are_running) {
	/* 
	 * To be really certain, we must repeat the test, but use the
	 * lock first. If the test failed, however, we _know_ we've
	 * already initialized. This strategy of double testing avoids
	 * using the lock more than a few times at startup.
	 */
	mutex_lock(client_thread_lock);
	if (!client_threads_are_running) {
	    client_fdset = fdset_create();
	    gwthread_create(write_request_thread, NULL);
	    client_threads_are_running = 1;
	}
	mutex_unlock(client_thread_lock);
    }
}


void http_start_request(HTTPCaller *caller, int method, Octstr *url, List *headers,
    	    	    	Octstr *body, int follow, void *id, Octstr *certkeyfile)
{
    HTTPServer *trans;
    int follow_remaining;
    
    if (follow)
    	follow_remaining = HTTP_MAX_FOLLOW;
    else
    	follow_remaining = 0;

    trans = server_create(caller, method, url, headers, body, follow_remaining, 
			  certkeyfile);

    if (id == NULL)
	/* We don't leave this NULL so http_receive_result can use NULL
	 * to signal no more requests */
	trans->request_id = http_start_request;
    else
	trans->request_id = id;
    list_produce(pending_requests, trans);
    start_client_threads();
}


void *http_receive_result(HTTPCaller *caller, int *status, Octstr **final_url,
    	    	    	 List **headers, Octstr **body)
{
    HTTPServer *trans;
    void *request_id;

    trans = list_consume(caller);
    if (trans == NULL)
    	return NULL;

    request_id = trans->request_id;
    *status = trans->status;
    
    if (trans->status >= 0) {
	*final_url = trans->url;
	*headers = trans->response->headers;
	*body = trans->response->body;

	trans->url = NULL;
	trans->response->headers = NULL;
	trans->response->body = NULL;
    } else {
	*final_url = NULL;
	*headers = NULL;
	*body = NULL;
    }

    server_destroy(trans);
    return request_id;
}


int http_get_real(int method, Octstr *url, List *request_headers, Octstr **final_url,
                  List **reply_headers, Octstr **reply_body)
{
    HTTPCaller *caller;
    int status;
    void *ret;
    
    caller = http_caller_create();
    http_start_request(caller, method, url, request_headers, 
                       NULL, 1, http_get_real, NULL);
    ret = http_receive_result(caller, &status, final_url, 
    	    	    	      reply_headers, reply_body);
    http_caller_destroy(caller);
    if (ret == NULL)
    	return -1;
    return status;
}


static void client_init(void)
{
    pending_requests = list_create();
    list_add_producer(pending_requests);
    client_thread_lock = mutex_create();
}


static void client_shutdown(void)
{
    list_remove_producer(pending_requests);
    gwthread_join_every(write_request_thread);
    list_destroy(pending_requests, server_destroy);
    mutex_destroy(client_thread_lock);
    fdset_destroy(client_fdset);
}


/***********************************************************************
 * HTTP server interface.
 */


/*
 * Information about a client that has connected to the server we implement.
 */
struct HTTPClient {
    int port;
    Connection *conn;
    Octstr *ip;
    enum {
        reading_request_line,
        reading_request,
        request_is_being_handled,
	sending_reply
    } state;
    int method;  /* HTTP_METHOD_ value */
    Octstr *url;
    int use_version_1_0;
    int persistent_conn;
    HTTPEntity *request;
};


static HTTPClient *client_create(int port, Connection *conn, Octstr *ip)
{
    HTTPClient *p;
    
#ifdef HAVE_LIBSSL
    if (conn_get_ssl(conn)) 
        debug("gwlib.http", 0, "HTTP: Creating SSL-enabled HTTPClient for `%s', using cipher '%s'.",
    	      octstr_get_cstr(ip), SSL_get_cipher_version(conn_get_ssl(conn)));
    else
#endif    
    debug("gwlib.http", 0, "HTTP: Creating HTTPClient for `%s'.",
    	  octstr_get_cstr(ip));
    p = gw_malloc(sizeof(*p));
    p->port = port;
    p->conn = conn;
    p->ip = ip;
    p->state = reading_request_line;
    p->url = NULL;
    p->use_version_1_0 = 0;
    p->persistent_conn = 1;
    p->request = NULL;
    return p;
}


static void client_destroy(void *client)
{
    HTTPClient *p;
    
    if (client == NULL)
    	return;

    p = client;
    debug("gwlib.http", 0, "HTTP: Destroying HTTPClient area %p.", p);
    gw_assert_allocated(p, __FILE__, __LINE__, __func__);
    debug("gwlib.http", 0, "HTTP: Destroying HTTPClient for `%s'.",
    	  octstr_get_cstr(p->ip));
    conn_destroy(p->conn);
    octstr_destroy(p->ip);
    octstr_destroy(p->url);
    entity_destroy(p->request);
    gw_free(p);
}


static void client_reset(HTTPClient *p)
{
    debug("gwlib.http", 0, "HTTP: Resetting HTTPClient for `%s'.",
    	  octstr_get_cstr(p->ip));
    p->state = reading_request_line;
    gw_assert(p->request == NULL);
}


/*
 * Checks whether the client connection is meant to be persistent or not.
 * Returns 1 for true, 0 for false.
 */

static int client_is_persistent(List *headers, int use_version_1_0)
{
    Octstr *h = http_header_find_first(headers, "Connection");

    if (h == NULL) {
        return !use_version_1_0;
    } else {
        if (!use_version_1_0) {
            if (octstr_case_compare(h, octstr_imm("keep-alive")) == 0) {
                octstr_destroy(h);
                return 1;
            } else {
                octstr_destroy(h);
                return 0;
            }
	    } else if (octstr_case_compare(h, octstr_imm("close")) == 0) {
            octstr_destroy(h);
            return 0;
        }
        octstr_destroy(h);
    }

    return 1;
}


/*
 * Port specific lists of clients with requests.
 */


struct port {
    List *clients_with_requests;
    Counter *active_consumers;
};


static Mutex *port_mutex = NULL;
static Dict *port_collection = NULL;


static void port_init(void)
{
    port_mutex = mutex_create();
    port_collection = dict_create(1024, NULL);
}

static void port_shutdown(void)
{
    mutex_destroy(port_mutex);
    dict_destroy(port_collection);
}


static Octstr *port_key(int port)
{
    return octstr_format("%d", port);
}


static void port_add(int port)
{
    Octstr *key;
    struct port *p;

    p = gw_malloc(sizeof(*p));
    p->clients_with_requests = list_create();
    list_add_producer(p->clients_with_requests);
    p->active_consumers = counter_create();

    key = port_key(port);
    mutex_lock(port_mutex);
    dict_put(port_collection, key, p);
    mutex_unlock(port_mutex);
    octstr_destroy(key);
}


static void port_remove(int port)
{
    Octstr *key;
    struct port *p;

    key = port_key(port);
    mutex_lock(port_mutex);
    p = dict_remove(port_collection, key);
    mutex_unlock(port_mutex);
    octstr_destroy(key);

    list_remove_producer(p->clients_with_requests);
    while (counter_value(p->active_consumers) > 0)
       gwthread_sleep(0.1);    /* Reasonable use of busy waiting. */
    list_destroy(p->clients_with_requests, client_destroy);
    counter_destroy(p->active_consumers);
    gw_free(p);
}


static void port_put_request(HTTPClient *client)
{
    Octstr *key;
    struct port *p;

    mutex_lock(port_mutex);
    key = port_key(client->port);
    p = dict_get(port_collection, key);
    gw_assert(p != NULL);
    list_produce(p->clients_with_requests, client);
    octstr_destroy(key);
    mutex_unlock(port_mutex);
}


static HTTPClient *port_get_request(int port)
{
    Octstr *key;
    struct port *p;
    HTTPClient *client;
    
    mutex_lock(port_mutex);
    key = port_key(port);
    p = dict_get(port_collection, key);
    octstr_destroy(key);

    if (p == NULL) {
       client = NULL;
       mutex_unlock(port_mutex);
    } else {
       counter_increase(p->active_consumers);
       mutex_unlock(port_mutex);   /* Placement of this unlock is tricky. */
       client = list_consume(p->clients_with_requests);
       counter_decrease(p->active_consumers);
    }
    return client;
}


/*
 * Maximum number of servers (ports) we have open at the same time.
 */
enum { MAX_SERVERS = 32 };


/*
 * Variables related to server side implementation.
 */
static Mutex *server_thread_lock = NULL;
static volatile sig_atomic_t server_thread_is_running = 0;
static long server_thread_id = -1;
static FDSet *server_fdset = NULL;
static List *new_server_sockets = NULL;
static List *closed_server_sockets = NULL;
static int keep_servers_open = 0;


static int parse_request_line(int *method, Octstr **url,
                              int *use_version_1_0, Octstr *line)
{
    List *words;
    Octstr *version;
    Octstr *method_str;
    int ret;

    words = octstr_split_words(line);
    if (list_len(words) != 3) {
        list_destroy(words, octstr_destroy_item);
	return -1;
    }

    method_str = list_get(words, 0);
    *url = list_get(words, 1);
    version = list_get(words, 2);
    list_destroy(words, NULL);

    if (octstr_compare(method_str, octstr_imm("GET")) == 0)
	*method = HTTP_METHOD_GET;
    else if (octstr_compare(method_str, octstr_imm("POST")) == 0)
	*method = HTTP_METHOD_POST;
    else if (octstr_compare(method_str, octstr_imm("HEAD")) == 0)
	*method = HTTP_METHOD_HEAD;
    else
        goto error;

    ret = parse_http_version(version);
    if (ret < 0)
        goto error;
    *use_version_1_0 = !ret;

    octstr_destroy(method_str);
    octstr_destroy(version);
    return 0;

error:
    octstr_destroy(method_str);
    octstr_destroy(*url);
    octstr_destroy(version);
    *url = NULL;
    return -1;
}


static void receive_request(Connection *conn, void *data)
{
    HTTPClient *client;
    Octstr *line;
    int ret;

    if (run_status != running) {
	conn_unregister(conn);
	return;
    }

    client = data;
    
    for (;;) {
	switch (client->state) {
	case reading_request_line:
    	    line = conn_read_line(conn);
	    if (line == NULL) {
		if (conn_eof(conn) || conn_read_error(conn))
		    goto error;
	    	return;
	    }
	    ret = parse_request_line(&client->method, &client->url,
                                     &client->use_version_1_0, line);
	    octstr_destroy(line);
	    if (ret == -1)
	    	goto error;
   	    /*
	     * RFC2616 (4.3) says we should read a message body if there
	     * is one, even on GET requests.
	     */
	    client->request = entity_create(expect_body_if_indicated);
	    client->state = reading_request;
	    break;
	    
	case reading_request:
	    ret = entity_read(client->request, conn);
	    if (ret < 0)
		goto error;
	    if (ret == 0) {
	    	client->state = request_is_being_handled;
		conn_unregister(conn);
		port_put_request(client);
	    }
	    return;

	case sending_reply:
	    if (conn_outbuf_len(conn) > 0)
		return;
	    /* Reply has been sent completely */
	    if (!client->persistent_conn) {
		client_destroy(client);
		return;
	    }
	    /* Start reading another request */
	    client_reset(client);
	    break;

    	default:
	    panic(0, "Internal error: HTTPClient state is wrong.");
	}
    }
    
error:
    client_destroy(client);
}


struct server {
    int fd;
    int port;
    int ssl;
};


static void server_thread(void *dummy)
{
    struct pollfd tab[MAX_SERVERS];
    int ports[MAX_SERVERS];
    int ssl[MAX_SERVERS];
    long i, j, n, fd;
    int *portno;
    struct server *p;
    struct sockaddr_in addr;
    int addrlen;
    Connection *conn;
    HTTPClient *client;
    int ret;

    n = 0;
    while (run_status == running && keep_servers_open) {
	if (n == 0 || (n < MAX_SERVERS && list_len(new_server_sockets) > 0)) {
	    p = list_consume(new_server_sockets);
	    if (p == NULL) {
		debug("gwlib.http", 0, "HTTP: No new servers. Quitting.");
	    	break;
	    }
	    tab[n].fd = p->fd;
	    tab[n].events = POLLIN;
	    ports[n] = p->port;
        ssl[n] = p->ssl;
	    ++n;
	    gw_free(p);
	}

	if ((ret = gwthread_poll(tab, n, -1.0)) == -1) {
	    if (errno != EINTR)
	        warning(0, "HTTP: gwthread_poll failed.");
	    continue;
	}

    	for (i = 0; i < n; ++i) {
	    if (tab[i].revents & POLLIN) {
		addrlen = sizeof(addr);
		fd = accept(tab[i].fd, (struct sockaddr *) &addr, &addrlen);
		if (fd == -1) {
		    error(errno, "HTTP: Error accepting a client.");
    	    	    (void) close(tab[i].fd);
		    port_remove(ports[i]);
		    tab[i].fd = -1;
		    ports[i] = -1;
            ssl[i] = 0;
		} else {
            /*
             * Be aware that conn_wrap_fd() will return NULL if SSL handshake
             * has failed, so we only client_create() if there is an conn.
             */             
            if ((conn = conn_wrap_fd(fd, ssl[i]))) {
    	        client = client_create(ports[i], conn, host_ip(addr));
		        conn_register(conn, server_fdset, receive_request, 
		    	    	      client);
            } else {
                error(0, "HTTP: unsuccessfull SSL handshake for client `%s'",
                      octstr_get_cstr(host_ip(addr)));
            }
		}
	    }
	}
	
	while ((portno = list_extract_first(closed_server_sockets)) != NULL) {
	    for (i = 0; i < n; ++i) {
		if (ports[i] == *portno) {
		    (void) close(tab[i].fd);
		    port_remove(ports[i]);
		    tab[i].fd = -1;
		    ports[i] = -1;
            ssl[i] = 0;
		}
	    }
	    gw_free(portno);
	}
       
    	j = 0;
	for (i = 0; i < n; ++i) {
	    if (tab[i].fd != -1) {
	    	tab[j] = tab[i];
		ports[j] = ports[i];
        ssl[j] = ssl[i];
		++j;
	    }
	}
	n = j;
    }
    
    for (i = 0; i < n; ++i) {
	(void) close(tab[i].fd);
	port_remove(ports[i]);
    }
}


static void start_server_thread(void)
{
    if (!server_thread_is_running) {
	/* 
	 * To be really certain, we must repeat the test, but use the
	 * lock first. If the test failed, however, we _know_ we've
	 * already initialized. This strategy of double testing avoids
	 * using the lock more than a few times at startup.
	 */
	mutex_lock(server_thread_lock);
	if (!server_thread_is_running) {
	    server_fdset = fdset_create();
	    server_thread_id = gwthread_create(server_thread, NULL);
	    server_thread_is_running = 1;
	}
	mutex_unlock(server_thread_lock);
    }
}


int http_open_port_if(int port, int ssl, Octstr *interface)
{
    struct server *p;

    if (ssl) debug("gwlib.http", 0, "HTTP: Opening SSL server at port %d.", port);
    else debug("gwlib.http", 0, "HTTP: Opening server at port %d.", port);
    p = gw_malloc(sizeof(*p));
    p->port = port;
    p->ssl = ssl;
    p->fd = make_server_socket(port, (interface ? octstr_get_cstr(interface) : NULL));
    if (p->fd == -1) {
	gw_free(p);
    	return -1;
    }

    port_add(port);
    list_produce(new_server_sockets, p);
    keep_servers_open = 1;
    start_server_thread();
    gwthread_wakeup(server_thread_id);

    return 0;
}


int http_open_port(int port, int ssl)
{
    return http_open_port_if(port, ssl, NULL);
}


void http_close_port(int port)
{
    int *p;
    
    p = gw_malloc(sizeof(*p));
    *p = port;
    list_produce(closed_server_sockets, p);
    gwthread_wakeup(server_thread_id);
}


void http_close_all_ports(void)
{
    if (server_thread_id != -1) {
	keep_servers_open = 0;
	gwthread_wakeup(server_thread_id);
	gwthread_join_every(server_thread);
	fdset_destroy(server_fdset);
	server_fdset = NULL;
    }
}


/*
 * Parse CGI variables from the path given in a GET. Return a list
 * of HTTPCGIvar pointers. Modify the url so that the variables are
 * removed.
 */
static List *parse_cgivars(Octstr *url)
{
    HTTPCGIVar *v;
    List *list;
    int query, et, equals;
    Octstr *arg, *args;

    query = octstr_search_char(url, '?', 0);
    if (query == -1)
        return list_create();

    args = octstr_copy(url, query + 1, octstr_len(url));
    octstr_truncate(url, query);

    list = list_create();

    while (octstr_len(args) > 0) {
        et = octstr_search_char(args, '&', 0);
        if (et == -1)
            et = octstr_len(args);
        arg = octstr_copy(args, 0, et);
        octstr_delete(args, 0, et + 1);

        equals = octstr_search_char(arg, '=', 0);
        if (equals == -1)
            equals = octstr_len(arg);

        v = gw_malloc(sizeof(HTTPCGIVar));
        v->name = octstr_copy(arg, 0, equals);
        v->value = octstr_copy(arg, equals + 1, octstr_len(arg));
        octstr_url_decode(v->name);
        octstr_url_decode(v->value);

        octstr_destroy(arg);

        list_append(list, v);
    }
    octstr_destroy(args);

    return list;
}


HTTPClient *http_accept_request(int port, Octstr **client_ip, Octstr **url, 
    	    	    	    	List **headers, Octstr **body, 
				List **cgivars)
{
    HTTPClient *client;

    client = port_get_request(port);
    if (client == NULL) {
	debug("gwlib.http", 0, "HTTP: No clients with requests, quitting.");
    	return NULL;
    }

    *client_ip = octstr_duplicate(client->ip);
    *url = client->url;
    *headers = client->request->headers;
    *body = client->request->body;
    *cgivars = parse_cgivars(client->url);

    if (client->method != HTTP_METHOD_POST) {
	octstr_destroy(*body);
	*body = NULL;
    }

    client->persistent_conn = client_is_persistent(client->request->headers,
						   client->use_version_1_0);
    
    client->url = NULL;
    client->request->headers = NULL;
    client->request->body = NULL;
    entity_destroy(client->request);
    client->request = NULL;

    return client;
}


void http_send_reply(HTTPClient *client, int status, List *headers, 
    	    	     Octstr *body)
{
    Octstr *response;
    long i;
    int ret;

    if (client->use_version_1_0)
    	response = octstr_format("HTTP/1.0 %d Foo\r\n", status);
    else
    	response = octstr_format("HTTP/1.1 %d Foo\r\n", status);

    /* identify ourselfs */
    octstr_format_append(response, "Server: " GW_NAME "/%s\r\n", VERSION);

    octstr_format_append(response, "Content-Length: %ld\r\n",
			 octstr_len(body));

    /* 
     * RFC2616, sec. 8.1.2.1 says that if the server chooses to close the 
     * connection, it *should* send a coresponding header
     */
    if (!client->use_version_1_0 && !client->persistent_conn)
        octstr_format_append(response, "Connection: close\r\n");

    for (i = 0; i < list_len(headers); ++i)
    	octstr_format_append(response, "%S\r\n", list_get(headers, i));
    octstr_format_append(response, "\r\n");
    
    if (body != NULL && client->method != HTTP_METHOD_HEAD)
    	octstr_append(response, body);
	
    ret = conn_write(client->conn, response);
    octstr_destroy(response);

    if (ret == 0) {	/* Sent already */
	if (!client->persistent_conn)
	    client_destroy(client);
	else {
	    client_reset(client);
	    conn_register(client->conn, server_fdset, receive_request, client);
	}
    }
    else if (ret == 1) {      /* Queued for sending, we don't want to block */
	client->state = sending_reply;
	conn_register(client->conn, server_fdset, receive_request, client);
    }
    else	/* Error */
	client_destroy(client);
}


void http_close_client(HTTPClient *client)
{
    client_destroy(client);
}


static void server_init(void)
{
    new_server_sockets = list_create();
    list_add_producer(new_server_sockets);
    closed_server_sockets = list_create();
    server_thread_lock = mutex_create();
}


static void destroy_struct_server(void *p)
{
    struct server *pp;
    
    pp = p;
    (void) close(pp->fd);
    gw_free(pp);
}


static void destroy_int_pointer(void *p)
{
    (void) close(*(int *) p);
    gw_free(p);
}


static void server_shutdown(void)
{
    list_remove_producer(new_server_sockets);
    if (server_thread_id != -1) {
	gwthread_wakeup(server_thread_id);
	gwthread_join_every(server_thread);
    }
    mutex_destroy(server_thread_lock);
    fdset_destroy(server_fdset);
    list_destroy(new_server_sockets, destroy_struct_server);
    list_destroy(closed_server_sockets, destroy_int_pointer);
}


/***********************************************************************
 * CGI variable manipulation.
 */


void http_destroy_cgiargs(List *args)
{
    HTTPCGIVar *v;

    gwlib_assert_init();

    if (args == NULL)
        return ;

    while ((v = list_extract_first(args)) != NULL) {
        octstr_destroy(v->name);
        octstr_destroy(v->value);
        gw_free(v);
    }
    list_destroy(args, NULL);
}


Octstr *http_cgi_variable(List *list, char *name)
{
    int i;
    HTTPCGIVar *v;

    gwlib_assert_init();
    gw_assert(list != NULL);
    gw_assert(name != NULL);

    for (i = 0; i < list_len(list); ++i) {
        v = list_get(list, i);
        if (octstr_str_compare(v->name, name) == 0)
            return v->value;
    }
    return NULL;
}


/***********************************************************************
 * Header manipulation.
 */


static int header_is_called(Octstr *header, char *name)
{
    long colon;

    colon = octstr_search_char(header, ':', 0);
    if (colon == -1)
        return 0;
    if ((long) strlen(name) != colon)
        return 0;
    return strncasecmp(octstr_get_cstr(header), name, colon) == 0;
}


List *http_create_empty_headers(void)
{
    gwlib_assert_init();
    return list_create();
}


void http_destroy_headers(List *headers)
{
    gwlib_assert_init();
    list_destroy(headers, octstr_destroy_item);
}


void http_header_add(List *headers, char *name, char *contents)
{
    gwlib_assert_init();
    gw_assert(headers != NULL);
    gw_assert(name != NULL);
    gw_assert(contents != NULL);

    list_append(headers, octstr_format("%s: %s", name, contents));
}


void http_header_get(List *headers, long i, Octstr **name, Octstr **value)
{
    Octstr *os;
    long colon;

    gwlib_assert_init();
    gw_assert(i >= 0);
    gw_assert(name != NULL);
    gw_assert(value != NULL);

    os = list_get(headers, i);
    if (os == NULL)
        colon = -1;
    else
        colon = octstr_search_char(os, ':', 0);
    if (colon == -1) {
        error(0, "HTTP: Header does not contain a colon. BAD.");
        *name = octstr_create("X-Unknown");
        *value = octstr_duplicate(os);
    } else {
        *name = octstr_copy(os, 0, colon);
        *value = octstr_copy(os, colon + 1, octstr_len(os));
        octstr_strip_blanks(*value);
    }
}

Octstr *http_header_value(List *headers, Octstr *name)
{
    Octstr *value;
    long i;
    Octstr *os;
    long colon;
    Octstr *current_name;
    
    gwlib_assert_init();
    gw_assert(name);
    
    value = NULL;
    i = 0;
    while (i < list_len(headers)) {
        os = list_get(headers, i);
        if (os == NULL)
            colon = -1;
        else
            colon = octstr_search_char(os, ':', 0);
        if (colon == -1) {
            return NULL;      
        } else {
            current_name = octstr_copy(os, 0, colon);
        }
        if (octstr_case_compare(current_name, name) == 0) {
            value = octstr_copy(os, colon + 1, octstr_len(os));
            octstr_strip_blanks(value);
            return value;
        }
        ++i;
    }
    
    return value;
}

List *http_header_duplicate(List *headers)
{
    List *new;
    long i;

    gwlib_assert_init();

    if (headers == NULL)
        return NULL;

    new = http_create_empty_headers();
    for (i = 0; i < list_len(headers); ++i)
        list_append(new, octstr_duplicate(list_get(headers, i)));
    return new;
}


void http_header_pack(List *headers)
{
    gwlib_assert_init();
    gw_assert(headers != NULL);
    /* XXX not implemented yet. */
}


void http_append_headers(List *to, List *from)
{
    Octstr *header;
    long i;

    gwlib_assert_init();
    gw_assert(to != NULL);
    gw_assert(from != NULL);

    for (i = 0; i < list_len(from); ++i) {
        header = list_get(from, i);
        list_append(to, octstr_duplicate(header));
    }
}


void http_header_combine(List *old_headers, List *new_headers)
{
    long i;
    Octstr *name;
    Octstr *value;

    /*
     * Avoid doing this scan if old_headers is empty anyway.
     */
    if (list_len(old_headers) > 0) {
        for (i = 0; i < list_len(new_headers); i++) {
  	    http_header_get(new_headers, i, &name, &value);
	    http_header_remove_all(old_headers, octstr_get_cstr(name));
            octstr_destroy(name);
            octstr_destroy(value);
        }
    }

    http_append_headers(old_headers, new_headers);
}


Octstr *http_header_find_first(List *headers, char *name)
{
    long i, name_len;
    Octstr *h, *value;

    gwlib_assert_init();
    gw_assert(headers != NULL);
    gw_assert(name != NULL);

    name_len = strlen(name);

    for (i = 0; i < list_len(headers); ++i) {
        h = list_get(headers, i);
        if (header_is_called(h, name)) {
            value = octstr_copy(h, name_len + 1, octstr_len(h));
	    octstr_strip_blanks(value);
	    return value;
	}
    }
    return NULL;
}


List *http_header_find_all(List *headers, char *name)
{
    List *list;
    long i;
    Octstr *h;

    gwlib_assert_init();
    gw_assert(headers != NULL);
    gw_assert(name != NULL);

    list = list_create();
    for (i = 0; i < list_len(headers); ++i) {
        h = list_get(headers, i);
        if (header_is_called(h, name))
            list_append(list, octstr_duplicate(h));
    }
    return list;
}


long http_header_remove_all(List *headers, char *name)
{
    long i;
    Octstr *h;
    long count;

    gwlib_assert_init();
    gw_assert(headers != NULL);
    gw_assert(name != NULL);

    i = 0;
    count = 0;
    while (i < list_len(headers)) {
	h = list_get(headers, i);
	if (header_is_called(h, name)) {
	    list_delete(headers, i, 1);
	    octstr_destroy(h);
	    count++;
	} else
	    i++;
    }

    return count;
}


void http_remove_hop_headers(List *headers)
{
    Octstr *h;
    List *connection_headers;

    gwlib_assert_init();
    gw_assert(headers != NULL);

    /*
     * The hop-by-hop headers are a standard list, plus those named
     * in the Connection header(s).
     */

    connection_headers = http_header_find_all(headers, "Connection");
    while ((h = list_consume(connection_headers))) {
	List *hop_headers;
	Octstr *e;

	octstr_delete(h, 0, strlen("Connection:"));
	hop_headers = http_header_split_value(h);
	octstr_destroy(h);

	while ((e = list_consume(hop_headers))) {
	    http_header_remove_all(headers, octstr_get_cstr(e));
	    octstr_destroy(e);
	}

	list_destroy(hop_headers, NULL);
    }
    list_destroy(connection_headers, NULL);
   
    http_header_remove_all(headers, "Connection");
    http_header_remove_all(headers, "Keep-Alive");
    http_header_remove_all(headers, "Proxy-Authenticate");
    http_header_remove_all(headers, "Proxy-Authorization");
    http_header_remove_all(headers, "TE");
    http_header_remove_all(headers, "Trailers");
    http_header_remove_all(headers, "Transfer-Encoding");
    http_header_remove_all(headers, "Upgrade");
}


void http_header_mark_transformation(List *headers,
    	    	    	    	     Octstr *new_body, Octstr *new_type)
{
    Octstr *new_length = NULL;

    /* Remove all headers that no longer apply to the new body. */
    http_header_remove_all(headers, "Content-Length");
    http_header_remove_all(headers, "Content-MD5");
    http_header_remove_all(headers, "Content-Type");

    /* Add headers that we need to describe the new body. */
    new_length = octstr_format("%ld", octstr_len(new_body));
    http_header_add(headers, "Content-Length", octstr_get_cstr(new_length));
    http_header_add(headers, "Content-Type", octstr_get_cstr(new_type));

    /* Perhaps we should add Warning: 214 "Transformation applied" too? */

    octstr_destroy(new_length);
}


void http_header_get_content_type(List *headers, Octstr **type,
                                  Octstr **charset)
{
    Octstr *h;
    long semicolon, equals, len;

    gwlib_assert_init();
    gw_assert(headers != NULL);
    gw_assert(type != NULL);
    gw_assert(charset != NULL);

    h = http_header_find_first(headers, "Content-Type");
    if (h == NULL) {
        *type = octstr_create("application/octet-stream");
        *charset = octstr_create("");
    } else {
        octstr_strip_blanks(h);
        semicolon = octstr_search_char(h, ';', 0);
        if (semicolon == -1) {
            *type = h;
            *charset = octstr_create("");
        } else {
            *charset = octstr_duplicate(h);
            octstr_delete(*charset, 0, semicolon + 1);
            octstr_strip_blanks(*charset);
            equals = octstr_search_char(*charset, '=', 0);
            if (equals == -1)
                octstr_truncate(*charset, 0);
            else {
                octstr_delete(*charset, 0, equals + 1);
                if (octstr_get_char(*charset, 0) == '"')
                    octstr_delete(*charset, 0, 1);
                len = octstr_len(*charset);
                if (octstr_get_char(*charset, len - 1) == '"')
                    octstr_truncate(*charset, len - 1);
            }

            octstr_truncate(h, semicolon);
            octstr_strip_blanks(h);
            *type = h;
        }
    }
}


static void http_header_add_element(List *list, Octstr *value,
				    long start, long end)
{
    Octstr *element;

    element = octstr_copy(value, start, end - start);
    octstr_strip_blanks(element);
    if (octstr_len(element) == 0)
	octstr_destroy(element);
    else
    	list_append(list, element);
}


long http_header_quoted_string_len(Octstr *header, long start)
{
    long len;
    long pos;
    int c;

    if (octstr_get_char(header, start) != '"')
	return -1;

    len = octstr_len(header);
    for (pos = start + 1; pos < len; pos++) {
	c = octstr_get_char(header, pos);
	if (c == '\\')    /* quoted-pair */
	    pos++;
	else if (c == '"')
	    return pos - start + 1;
    }

    warning(0, "Header contains unterminated quoted-string:");
    warning(0, "%s", octstr_get_cstr(header));
    return len - start;
}


List *http_header_split_value(Octstr *value)
{
    long start;  /* start of current element */
    long pos;
    long len;
    List *result;
    int c;

    /*
     * According to RFC2616 section 4.2, a field-value is either *TEXT
     * (the caller is responsible for not feeding us one of those) or
     * combinations of token, separators, and quoted-string.  We're
     * looking for commas which are separators, and have to skip
     * commas in quoted-strings.
     */
 
    result = list_create();
    len = octstr_len(value);
    start = 0;
    for (pos = 0; pos < len; pos++) {
	c = octstr_get_char(value, pos);
	if (c == ',') {
	    http_header_add_element(result, value, start, pos);
	    start = pos + 1;
	} else if (c == '"') {
            pos += http_header_quoted_string_len(value, pos);
	    pos--; /* compensate for the loop's pos++ */
        }
    }
    http_header_add_element(result, value, start, len);
    return result;
}


List *http_header_split_auth_value(Octstr *value)
{
    List *result;
    Octstr *auth_scheme;
    Octstr *element;
    long i;

    /*
     * According to RFC2617, both "challenge" and "credentials"
     * consist of an auth-scheme followed by a list of auth-param.
     * Since we have to parse a list of challenges or credentials,
     * we have to look for auth-scheme to signal the start of
     * a new element.  (We can't just split on commas because
     * they are also used to separate the auth-params.)
     *
     * An auth-scheme is a single token, while an auth-param is
     * always a key=value pair.  So we can recognize an auth-scheme
     * as a token that is not followed by a '=' sign.
     *
     * Simple approach: First split at all commas, then recombine
     * the elements that belong to the same challenge or credential.
     * This is somewhat expensive but saves programmer thinking time.
     *
     * Richard Braakman
     */
 
    result = http_header_split_value(value);

    auth_scheme = list_get(result, 0);
    i = 1;
    while (i < list_len(result)) {
	int c;
	long pos;

	element = list_get(result, i);
	/*
	 * If the element starts with: token '='
	 * then it's just an auth_param; append it to the current
	 * auth_scheme.  If it starts with: token token '='
	 * then it's the start of a new auth scheme.
	 * 
	 * To make the scan easier, we consider anything other
	 * than whitespace or '=' to be part of a token.
	 */

	/* Skip first token */
	for (pos = 0; pos < octstr_len(element); pos++) {
	    c = octstr_get_char(element, pos);
	    if (isspace(c) || c == '=')
		break;
	}

	/* Skip whitespace, if any */
	while (isspace(octstr_get_char(element, pos)))
	    pos++;

	if (octstr_get_char(element, pos) == '=') {
		octstr_append_char(auth_scheme, ';');
		octstr_append(auth_scheme, element);
		list_delete(result, i, 1);
		octstr_destroy(element);
	} else {
		unsigned char semicolon = ';';
		octstr_insert_data(element, pos, &semicolon, 1);
		auth_scheme = element;
		i++;
	}
    }

    return result;
}


void http_header_dump(List *headers)
{
    long i;

    gwlib_assert_init();

    debug("gwlib.http", 0, "Dumping HTTP headers:");
    for (i = 0; headers != NULL && i < list_len(headers); ++i)
        octstr_dump(list_get(headers, i), 1);
    debug("gwlib.http", 0, "End of dump.");
}

void http_cgivar_dump(List *cgiargs)
{
    long i;
    Octstr *arg;

    gwlib_assert_init();

    debug("gwlib.http", 0, "Dumping %ld cgi variables:", list_len(cgiargs));
    for (i = 0; cgiargs != NULL && i < list_len(cgiargs); ++i) {
         if (octstr_len(arg = list_get(cgiargs, i)) != 0)
             octstr_dump(arg, 0);
         else
	     debug("gwlib.http", 0, "Got an empty cgi arg");
    }
    debug("gwlib.http", 0, "End of dump.");
}

/* XXX this needs to go away */
static char *istrdup(char *orig)
{
    int i, len = strlen(orig);
    char *result = gw_malloc(len + 1);

    for (i = 0; i < len; i++)
        result[i] = toupper(orig[i]);

    result[i] = 0;

    return result;
}


static int http_something_accepted(List *headers, char *header_name,
                                   char *what)
{
    int found;
    long i;
    List *accepts;
    char *iwhat;

    gwlib_assert_init();
    gw_assert(headers != NULL);
    gw_assert(what != NULL);

    iwhat = istrdup(what);
    accepts = http_header_find_all(headers, header_name);

    found = 0;
    for (i = 0; !found && i < list_len(accepts); ++i) {
        char *header_value = istrdup(octstr_get_cstr(list_get(accepts, i)));
        if (strstr(header_value, iwhat) != NULL)
            found = 1;
        gw_free(header_value);
    }

    gw_free(iwhat);
    http_destroy_headers(accepts);
    return found;
}


int http_type_accepted(List *headers, char *type)
{
    return http_something_accepted(headers, "Accept", type);
}


int http_charset_accepted(List *headers, char *charset)
{
    return http_something_accepted(headers, "Accept-Charset", charset);
}


void http_add_basic_auth(List *headers, Octstr *username, Octstr *password)
{
    Octstr *os;
    
    if (password != NULL)
      os = octstr_format("%S:%S", username, password);
    else
      os = octstr_format("%S", username);
    octstr_binary_to_base64(os);
    octstr_strip_blanks(os);
    octstr_insert(os, octstr_imm("Basic "), 0);
    http_header_add(headers, "Authorization", octstr_get_cstr(os));
    octstr_destroy(os);
}


/***********************************************************************
 * Module initialization and shutdown.
 */


void http_init(void)
{
    gw_assert(run_status == limbo);

#ifdef HAVE_LIBSSL
    conn_init_ssl();
#endif /* HAVE_LIBSSL */
    proxy_init();
    client_init();
    conn_pool_init();
    server_init();
#ifdef HAVE_LIBSSL
    server_ssl_init();
#endif /* HAVE_LIBSSL */
    port_init();
    
    run_status = running;
}


void http_shutdown(void)
{
    gwlib_assert_init();
    gw_assert(run_status == running);

    run_status = terminating;

    conn_pool_shutdown();
    port_shutdown();
    client_shutdown();
    server_shutdown();
    proxy_shutdown();
#ifdef HAVE_LIBSSL
    conn_shutdown_ssl();
    server_shutdown_ssl();
#endif /* HAVE_LIBSSL */
    run_status = limbo;
}


/*
 * This function relies on the HTTP_STATUS_* enum values being
 * chosen to fit this.
 */
int http_status_class(int code)
{
    int sclass;

    if (code < 100 || code >= 600)
        sclass = HTTP_STATUS_UNKNOWN;
    else
        sclass = code - (code % 100);
    return sclass;
}


int http_name2method(Octstr *method)
{
    gw_assert(method != NULL);

    if (octstr_str_compare(method, "GET") == 0) {
        return HTTP_METHOD_GET;
    } 
    else if (octstr_str_compare(method, "POST") == 0) {
        return HTTP_METHOD_POST;
    } 
    else if (octstr_str_compare(method, "HEAD") == 0) {
        return HTTP_METHOD_HEAD;
    } 

    return -1;
}


char *http_method2name(int method)
{
    gw_assert(method > 0 && method <= 3);

    return http_methods[method-1];
}

