/*
 * SMS BOX
 *
 * (WAP/SMS) Gateway
 *
 * Kalle Marjola 1999 for Wapit ltd.
 *
 */

/*
 * this is a SMS Service BOX
 *
 * it's main function is to receive SMS Messages from
 * (gateway) bearerbox and then fulfill requests in those
 * messages
 *
 * It may also send SMS Messages on its own, sending them
 * to bearerbox and that way into SMS Centers
 *
 * 
 * FUNCTION:
 *
 * 1. main loop opens a TCP/IP socket into the bearerbox, doing
 *    necessary handshake
 *
 * 2. for each SMS Message received, a new thread is created to
 *    handle the request
 *
 * 3. replies to requests and HTTP-initiated messages are sent
 *    (back) to the bearerbox. A global mutex is used for locking
 *    purposes
 *
 * THREAD FUNCTION:
 *
 * this program can also be used as a separate thread in bearerbox
 * When used this way, request thread is directly created by the
 * main program in bearerbox and repolies directly added to the
 * bearerbox reply queue. TODO: This functionality is added later.
 *
 * CONFIGURATION:
 *
 * - Information required for connecting the bearerbox is stored into
 *   a seperate configuration file.
 * - Service handling information is received from the bearerbox during
 *   handshake procedure (currently: from same configuration as rest)
 *
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "gwlib/gwlib.h"

#include "msg.h"
#include "bb.h"
#include "shared.h"

#include "smsbox_req.h"


/* global variables */

static Config 	*cfg;
static int 	bb_port;
static int	sendsms_port = 0;
static char 	*bb_host;
static char	*pid_file;
static int	sms_len = 160;
static char	*global_sender;
static int	heartbeat_freq;
static char	*accepted_chars = NULL;

static int 	socket_fd;
static int	only_try_http = 0;

/* thread handling */

static Mutex	 	*socket_mutex;
static volatile sig_atomic_t 	abort_program = 0;


/*
 * function to do the actual sending; called from smsbox_req via
 * pointer we give during initialization
 *
 * MUST DO: free (or otherwise get rid of) pmsg, and
 * return 0 if OK, -1 if failed
 */
static int socket_sender(Msg *pmsg)
{
    Octstr *pack;

    pack = msg_pack(pmsg);
    if (pack == NULL)
	goto error;

    mutex_lock(socket_mutex);
    if (octstr_send(socket_fd, pack) < 0) {
	mutex_unlock(socket_mutex);
	goto error;
    }
    mutex_unlock(socket_mutex);

    info(0, "Message sent to bearerbox, receiver <%s>",
         octstr_get_cstr(pmsg->sms.receiver));
   
#if 0
    debug("sms", 0, "write <%.*s> [%ld]",
	  (int) octstr_len(pmsg->sms.msgdata),
	  octstr_get_cstr(pmsg->sms.msgdata),
	  octstr_len(pmsg->sms.udhdata));
#endif

    octstr_destroy(pack);
    msg_destroy(pmsg);

    return 0;

error:
    msg_destroy(pmsg);
    octstr_destroy(pack);
    return -1;
}

/*
 * start a new thread for each request
 */
static void new_request(Octstr *pack)
{
    Msg *msg;

    gw_assert(pack != NULL);
    msg = msg_unpack(pack);
    if (msg == NULL)
	error(0, "Failed to unpack data!");
    else if (msg_type(msg) != sms)
	warning(0, "Received other message than sms, ignoring!");
    else
	gwthread_create(smsbox_req_thread, msg);
}



/*-----------------------------------------------------------
 * HTTP ADMINSTRATION
 */


static void http_request_thread(void *arg)
{
    HTTPClient *client;
    Octstr *ip, *url, *body, *answer;
    List *hdrs, *args, *reply_hdrs;

    reply_hdrs = list_create();
    http_header_add(reply_hdrs, "Content-type", "text/html");

    for (;;) {
    	client = http_accept_request(&ip, &url, &hdrs, &body, &args);
	if (client == NULL)
	    break;

	info(0, "smsbox: Got HTTP request <%s> from <%s>",
	    octstr_get_cstr(url), octstr_get_cstr(ip));

	if (octstr_str_compare(url, "/cgi-bin/sendsms") == 0)
	    answer = octstr_create(smsbox_req_sendsms(args, 
	    	    	    	    	    	      octstr_get_cstr(ip)));
	else if (octstr_str_compare(url, "/cgi-bin/sendota") == 0)
	    answer = octstr_create(smsbox_req_sendota(args, 
	    	    	    	    	    	      octstr_get_cstr(ip)));
	else
	    answer = octstr_create("unknown request\n");
        debug("sms.http", 0, "Answer: <%s>", octstr_get_cstr(answer));

	octstr_destroy(ip);
	octstr_destroy(url);
	http_destroy_headers(hdrs);
	octstr_destroy(body);
	http_destroy_cgiargs(args);
	
	http_send_reply(client, HTTP_OK, reply_hdrs, answer);

	octstr_destroy(answer);
    }

    http_destroy_headers(reply_hdrs);
}


/*------------------------------------------------------------*/


static void write_pid_file(void) {
    FILE *f;
        
    if (pid_file != NULL) {
	f = fopen(pid_file, "w");
	fprintf(f, "%d\n", (int)getpid());
	fclose(f);
    }
}


static void signal_handler(int signum) {
    /* Signals are normally delivered to all threads.  We only want
     * to handle each signal once for the entire box, so we ignore
     * all except the one sent to the main thread. */
    if (gwthread_self() != MAIN_THREAD_ID)
	return;

    if (signum == SIGINT) {
	if (abort_program == 0) {
	    error(0, "SIGINT received, aborting program...");
	    abort_program = 1;
	}
    } else if (signum == SIGHUP) {
        warning(0, "SIGHUP received, catching and re-opening logs");
        reopen_log_files();
    }
}


static void setup_signal_handlers(void) {
    struct sigaction act;

    act.sa_handler = signal_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGPIPE, &act, NULL);
}



static void init_smsbox(Config *cfg)
{
    ConfigGroup *grp;
    char *logfile = NULL;
    char *p;
    int lvl = 0;
    Octstr *http_proxy_host = NULL;
    int http_proxy_port = -1;
    List *http_proxy_exceptions = NULL;
    Octstr *http_proxy_username = NULL;
    Octstr *http_proxy_password = NULL;


    bb_port = BB_DEFAULT_SMSBOX_PORT;
    bb_host = BB_DEFAULT_HOST;
    heartbeat_freq = BB_DEFAULT_HEARTBEAT;

    /*
     * first we take the port number in bearerbox from the main
     * core group in configuration file
     */
    if (config_sanity_check(cfg)==-1)
	panic(0, "Cannot start with malformed configuration");

    grp = config_find_first_group(cfg, "group", "core");
    
    if ((p = config_get(grp, "smsbox-port")) == NULL)
	panic(0, "No 'smsbox-port' in core group");
    bb_port = atoi(p);

    if ((p = config_get(grp, "http-proxy-host")) != NULL)
    	http_proxy_host = octstr_create(p);
    if ((p = config_get(grp, "http-proxy-port")) != NULL)
    	http_proxy_port = atoi(p);
    if ((p = config_get(grp, "http-proxy-username")) != NULL)
    	http_proxy_username = octstr_create(p);
    if ((p = config_get(grp, "http-proxy-password")) != NULL)
    	http_proxy_password = octstr_create(p);
    if ((p = config_get(grp, "http-proxy-exceptions")) != NULL) {
	    Octstr *os;
	    
	    os = octstr_create(p);
	    http_proxy_exceptions = octstr_split_words(os);
	    octstr_destroy(os);
    }


    /*
     * get the remaining values from the smsbox group
     */
    if ((grp = config_find_first_group(cfg, "group", "smsbox")) == NULL)
	panic(0, "No 'smsbox' group in configuration");

    if ((p = config_get(grp, "bearerbox-host")) != NULL)
	bb_host = p;
    if ((p = config_get(grp, "sendsms-port")) != NULL)
	sendsms_port = atoi(p);
    if ((p = config_get(grp, "sms-length")) != NULL)
	sms_len = atoi(p);
    /*
     *if ((p = config_get(grp, "heartbeat-freq")) != NULL)
     *	heartbeat_freq = atoi(p);
     *if ((p = config_get(grp, "pid-file")) != NULL)
     *	pid_file = p; */
    if ((p = config_get(grp, "global-sender")) != NULL)
	global_sender = p;
    if ((p = config_get(grp, "sendsms-chars")) != NULL)
	accepted_chars = p;
    if ((p = config_get(grp, "log-file")) != NULL)
	logfile = p;
    if ((p = config_get(grp, "log-level")) != NULL)
	lvl = atoi(p);

    if (logfile != NULL) {
	info(0, "Starting to log to file %s level %d", logfile, lvl);
	open_logfile(logfile, lvl);
    }
    if (global_sender != NULL)
	info(0, "Service global sender set as '%s'", global_sender);
    
    if ((p = config_get(grp, "access-log")) != NULL)
	alog_open(p, 1);	/* XXX should be able to use gmtime, too */

    if (sendsms_port > 0) {
	if (http_open_server(sendsms_port) == -1) {
	    if (only_try_http)
		error(0, "Failed to open HTTP socket, ignoring it");
	    else
		panic(0, "Failed to open HTTP socket");
	}
	else {
	    info(0, "Set up send sms service at port %d", sendsms_port);
	    gwthread_create(http_request_thread, NULL);
	}
    }

    if (http_proxy_host != NULL && http_proxy_port > 0) {
    	http_use_proxy(http_proxy_host, http_proxy_port,
		       http_proxy_exceptions, http_proxy_username,
                       http_proxy_password);
    }

    octstr_destroy(http_proxy_host);
    octstr_destroy(http_proxy_username);
    octstr_destroy(http_proxy_password);
    list_destroy(http_proxy_exceptions, octstr_destroy_item);
}


/*
 * send the heartbeat packet
 */
static int send_heartbeat(void)
{
    Msg *msg;
    Octstr *pack;
    int ret;
    
    msg = msg_create(heartbeat);
    msg->heartbeat.load = smsbox_req_count();
    pack = msg_pack(msg);

    if (msg->heartbeat.load > 0)
	debug("sms", 0, "sending heartbeat load %ld", msg->heartbeat.load); 
    msg_destroy(msg);

    ret = octstr_send(socket_fd, pack);
    octstr_destroy(pack);
    return ret;
}


static void main_loop(void)
{
    time_t start, t;
    int ret, secs;
    int total = 0;
    fd_set rf;
    struct timeval to;

    start = t = time(NULL);
    mutex_lock(socket_mutex);
    while(!abort_program) {

	if (time(NULL)-t > heartbeat_freq) {
	    if (send_heartbeat() == -1)
		goto error;
	    t = time(NULL);
	}
	FD_ZERO(&rf);
	FD_SET(socket_fd, &rf);
	to.tv_sec = 0;
	to.tv_usec = 0;

	ret = select(FD_SETSIZE, &rf, NULL, NULL, &to);

	if (ret < 0) {
	    if(errno==EINTR) continue;
	    if(errno==EAGAIN) continue;
	    error(errno, "Select failed");
	    goto error;
	}
	else if (ret > 0 && FD_ISSET(socket_fd, &rf)) {

	    Octstr *pack;

	    ret = octstr_recv(socket_fd, &pack);
	    if (ret == 0) {
		info(0, "Connection closed by the Bearerbox");
		break;
	    }
	    else if (ret == -1) {
#if 0 /* XXX we assume run_kannel_box will re-start us, yes? --liw */
		info(0, "Connection to Bearerbox failed, reconnecting");
	    reconnect:
		socket_fd = tcpip_connect_to_server(bb_host, bb_port);
		if (socket_fd > -1)
		    continue;
		sleep(10);
		goto reconnect;
#else
		panic(0, "Connection to Bearerbox failed, NOT reconnecting");
#endif
	    }
	    mutex_unlock(socket_mutex);

	    if (total == 0)
		start = time(NULL);
	    total++;
	    new_request(pack);
	    octstr_destroy(pack);
	    
	    mutex_lock(socket_mutex);
	    continue;
	}
	mutex_unlock(socket_mutex);
	    
	usleep(1000);

	mutex_lock(socket_mutex);
    }
    secs = time(NULL) - start;
    info(0, "Received (and handled?) %d requests in %d seconds (%.2f per second)",
	 total, secs, (float)total/secs);
    return;

error:
    panic(0, "Mutex error, exiting");
}


static int check_args(int i, int argc, char **argv) {
    if (strcmp(argv[i], "-H")==0 || strcmp(argv[i], "--tryhttp")==0)
    {
	only_try_http = 1;
    }
    else
	return -1;

    return 0;
} 


int main(int argc, char **argv)
{
    int cf_index;
    URLTranslationList *translations;

    gwlib_init();
    cf_index = get_and_set_debugs(argc, argv, check_args);
    
    socket_mutex = mutex_create();

    setup_signal_handlers();
    cfg = config_from_file(argv[cf_index], "kannel.conf");
    if (cfg == NULL)
	panic(0, "No configuration, aborting.");

    report_versions("smsbox");

    init_smsbox(cfg);

    debug("sms", 0, "----------------------------------------------");
    debug("sms", 0, "Gateway SMS BOX version %s starting", VERSION);
    write_pid_file();

    translations = urltrans_create();
    if (translations == NULL)
	panic(errno, "urltrans_create failed");
    if (urltrans_add_cfg(translations, cfg) == -1)
	panic(errno, "urltrans_add_cfg failed");

    /*
     * initialize smsbox-request module
     */
    smsbox_req_init(translations, cfg, sms_len, global_sender, NULL,
		    socket_sender);
    
    while(!abort_program) {
	socket_fd = tcpip_connect_to_server(bb_host, bb_port);
	if (socket_fd > -1)
	    break;
	sleep(10);
    }
    info(0, "Connected to Bearer Box at %s port %d", bb_host, bb_port);

    main_loop();

    info(0, "Smsbox terminating.");

    alog_close();
    http_close_all_servers();
    gwthread_join_every(http_request_thread);
    mutex_destroy(socket_mutex);
    urltrans_destroy(translations);
    smsbox_req_shutdown();
    config_destroy(cfg);
    gwlib_shutdown();
    return 0;
}
