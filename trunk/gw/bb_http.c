/*
 * bb_http.c : bearerbox http adminstration commands
 *
 * NOTE: this is a special bearerbox module - it does call
 *   functions from core module! (other modules are fully
 *    encapsulated, and only called outside)
 *
 * Kalle Marjola <rpr@wapit.com> 2000 for project Kannel
 */

#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "gwlib/gwlib.h"
#include "bearerbox.h"

/* passed from bearerbox core */

extern volatile sig_atomic_t bb_status;

/* our own thingies */

static volatile sig_atomic_t httpadmin_running;

static int	ha_port;
static char	*ha_password;
static char	*ha_status_pw;
static char	*ha_allow_ip;
static char	*ha_deny_ip;


/*---------------------------------------------------------
 * static functions
 */

/*
 * check if the password matches. Return NULL if
 * it does (or is not required)
 */
static Octstr *httpd_check_authorization(List *cgivars, int status)
{
    Octstr *password;
    static double sleep = 0.01;

    password = http_cgi_variable(cgivars, "password");

    if (status) {
	if (ha_status_pw == NULL)
	    return NULL;

	if (password == NULL)
	    goto denied;

	if (octstr_str_compare(password, ha_password)!=0
	    && octstr_str_compare(password, ha_status_pw)!=0)
	    goto denied;
    }
    else {
	if (password == NULL || octstr_str_compare(password, ha_password)!=0)
	    goto denied;
    }
    sleep = 0.0;
    return NULL;	/* allowed */
denied:
    gwthread_sleep(sleep);
    sleep += 1.0;		/* little protection against brute force
				 * password cracking */
    return octstr_create("Denied");
}

/*
 * check if we still have time to do things
 */
static Octstr *httpd_check_status(void)
{
    if (bb_status == BB_SHUTDOWN || bb_status == BB_DEAD)
	return octstr_create("Avalanche has already started, too late to "
	    	    	     "save the sheeps");
    return NULL;
}



static Octstr *httpd_status(List *cgivars, int status_type)
{
    Octstr *reply;
    if ((reply = httpd_check_authorization(cgivars, 1))!= NULL) return reply;
    return bb_print_status(status_type);
}

static Octstr *httpd_shutdown(List *cgivars)
{
    Octstr *reply;
    if ((reply = httpd_check_authorization(cgivars, 0))!= NULL) return reply;
    if (bb_status == BB_SHUTDOWN)
	bb_status = BB_DEAD;
    else
	bb_shutdown();
    return octstr_create("Bringing system down");
}

static Octstr *httpd_isolate(List *cgivars)
{
    Octstr *reply;
    if ((reply = httpd_check_authorization(cgivars, 0))!= NULL) return reply;
    if ((reply = httpd_check_status())!= NULL) return reply;

    if (bb_isolate() == -1)
	return octstr_create("Already isolated");
    else
	return octstr_create("Kannel isolated from message providers");
}

static Octstr *httpd_suspend(List *cgivars)
{
    Octstr *reply;
    if ((reply = httpd_check_authorization(cgivars, 0))!= NULL) return reply;
    if ((reply = httpd_check_status())!= NULL) return reply;

    if (bb_suspend() == -1)
	return octstr_create("Already suspended");
    else
	return octstr_create("Kannel suspended");
}

static Octstr *httpd_resume(List *cgivars)
{
    Octstr *reply;
    if ((reply = httpd_check_authorization(cgivars, 0))!= NULL) return reply;
    if ((reply = httpd_check_status())!= NULL) return reply;
 
    if (bb_resume() == -1)
	return octstr_create("Already running");
    else
	return octstr_create("Running resumed");
}




static void httpd_serve(HTTPClient *client, Octstr *url, List *headers, 
    	    	    	Octstr *body, List *cgivars)
{
    Octstr *reply;
    char *content_type;
    int status_type;

    /* Set default reply format according to client
     * Accept: header */
    
    if (http_type_accepted(headers, "text/vnd.wap.wml")) {
	status_type = BBSTATUS_WML;
	content_type = "text/vnd.wap.wml";
    }
    else if (http_type_accepted(headers, "text/html")) {
	status_type = BBSTATUS_HTML;
	content_type = "text/html";
    } else {
	status_type = BBSTATUS_TEXT;
	content_type = "text/plain";
    }    

    if (octstr_str_compare(url, "/cgi-bin/status")==0) {
	reply = httpd_status(cgivars, status_type);
    } else if (octstr_str_compare(url, "/cgi-bin/status.html")==0) {
	reply = httpd_status(cgivars, BBSTATUS_HTML);
	content_type = "text/html";
    } else if (octstr_str_compare(url, "/cgi-bin/status.wml")==0) {
	reply = httpd_status(cgivars, BBSTATUS_WML);
	content_type = "text/vnd.wap.wml";
    } else if (octstr_str_compare(url, "/cgi-bin/status.txt")==0) {
	reply = httpd_status(cgivars, BBSTATUS_TEXT);
	content_type = "text/plain";
    } else if (octstr_str_compare(url, "/cgi-bin/status.xml")==0) {
	reply = httpd_status(cgivars, BBSTATUS_XML);
	content_type = "text/x-kannelstatus";
    } else if (octstr_str_compare(url, "/cgi-bin/shutdown")==0) {
	reply = httpd_shutdown(cgivars);
	content_type = "text/html";
    } else if (octstr_str_compare(url, "/cgi-bin/suspend")==0) {
	reply = httpd_suspend(cgivars);
	content_type = "text/html";
    } else if (octstr_str_compare(url, "/cgi-bin/isolate")==0) {
	reply = httpd_isolate(cgivars);
	content_type = "text/html";
    } else if (octstr_str_compare(url, "/cgi-bin/resume")==0) {
	reply = httpd_resume(cgivars);
	content_type = "text/html";
    /*
     * reconfig? restart?
     */
    } else  {
	reply = octstr_format("Unknown command %S", url);
	content_type = "text/plain";
    }

    gw_assert(reply != NULL);
    
    debug("bb.http", 0, "Result: '%s'", octstr_get_cstr(reply));
    
    http_destroy_headers(headers);
    headers = list_create();
    http_header_add(headers, "Content-Type", content_type);

    http_send_reply(client, HTTP_OK, headers, reply);

    octstr_destroy(url);
    octstr_destroy(body);
    octstr_destroy(reply);
    http_destroy_headers(headers);
    http_destroy_cgiargs(cgivars);
}

static void httpadmin_run(void *arg)
{
    HTTPClient *client;
    Octstr *ip, *url, *body;
    List *headers, *cgivars;

    while(bb_status != BB_DEAD) {
	if (bb_status == BB_SHUTDOWN)
	    bb_shutdown();
    	client = http_accept_request(&ip, &url, &headers, &body, &cgivars);
	if (client == NULL)
	    break;
	if (is_allowed_ip(ha_allow_ip, ha_deny_ip, ip) == 0) {
	    info(0, "HTTP admin tried from denied host <%s>, disconnected",
		 octstr_get_cstr(ip));
	    http_close_client(client);
	    continue;
	}
        httpd_serve(client, url, headers, body, cgivars);
	octstr_destroy(ip);
    }

    httpadmin_running = 0;
}


/*-------------------------------------------------------------
 * public functions
 *
 */

int httpadmin_start(Config *config)
{
    char *p;
    ConfigGroup *grp;
    
    if (httpadmin_running) return -1;


    grp = config_find_first_group(config, "group", "core");
    if ((p = config_get(grp, "admin-port")) == NULL)
	panic(0, "Missing admin-port variable, cannot start HTTP admin");

    ha_password = config_get(grp, "admin-password");
    if (ha_password == NULL)
	panic(0, "You MUST set HTTP admin-password");
    
    ha_status_pw = config_get(grp, "status-password");

    ha_port = atoi(p);
    ha_allow_ip = config_get(grp, "admin-allow-ip");
    ha_deny_ip = config_get(grp, "admin-deny-ip");
    
    http_open_server(ha_port);

    if (gwthread_create(httpadmin_run, NULL) == -1)
	panic(0, "Failed to start a new thread for HTTP admin");

    httpadmin_running = 1;
    return 0;
}


void httpadmin_stop(void)
{
    http_close_all_servers();
    gwthread_join_every(httpadmin_run);
}
