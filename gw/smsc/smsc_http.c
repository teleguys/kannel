/* ==================================================================== 
 * The Kannel Software License, Version 1.0 
 * 
 * Copyright (c) 2001-2003 Kannel Group  
 * Copyright (c) 1998-2001 WapIT Ltd.   
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 * 
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in 
 *    the documentation and/or other materials provided with the 
 *    distribution. 
 * 
 * 3. The end-user documentation included with the redistribution, 
 *    if any, must include the following acknowledgment: 
 *       "This product includes software developed by the 
 *        Kannel Group (http://www.kannel.org/)." 
 *    Alternately, this acknowledgment may appear in the software itself, 
 *    if and wherever such third-party acknowledgments normally appear. 
 * 
 * 4. The names "Kannel" and "Kannel Group" must not be used to 
 *    endorse or promote products derived from this software without 
 *    prior written permission. For written permission, please  
 *    contact org@kannel.org. 
 * 
 * 5. Products derived from this software may not be called "Kannel", 
 *    nor may "Kannel" appear in their name, without prior written 
 *    permission of the Kannel Group. 
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES 
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED.  IN NO EVENT SHALL THE KANNEL GROUP OR ITS CONTRIBUTORS 
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,  
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT  
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR  
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,  
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE  
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,  
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 * ==================================================================== 
 * 
 * This software consists of voluntary contributions made by many 
 * individuals on behalf of the Kannel Group.  For more information on  
 * the Kannel Group, please see <http://www.kannel.org/>. 
 * 
 * Portions of this software are based upon software originally written at  
 * WapIT Ltd., Helsinki, Finland for the Kannel project.  
 */ 

/*
 * smsc_http.c - interface to various HTTP based content/SMS gateways
 *
 * HTTP based "SMSC Connection" is meant for gateway connections,
 * and has following features:
 *
 * o Kannel listens to certain (HTTP server) port for MO SMS messages.
 *   The exact format of these HTTP calls are defined by type of HTTP based
 *   connection. Kannel replies to these messages as ACK, but does not
 *   support immediate reply. Thus, if Kannel is linked to another Kannel,
 *   only 'max-messages = 0' services are practically supported - any
 *   replies must be done with SMS PUSH (sendsms)
 *
 * o For MT messages, Kannel does HTTP GET or POST to given address, in format
 *   defined by type of HTTP based protocol
 *
 * The 'type' of requests and replies are defined by 'system-type' variable.
 * The only type of HTTP requests currently supported are basic Kannel.
 * If new support is added, smsc_http_create is modified accordingly and new
 * functions added.
 *
 *
 * KANNEL->KANNEL linking: (UDH not supported in MO messages)
 *
 *****
 * FOR CLIENT/END-POINT KANNEL:
 *
 *  group = smsc
 *  smsc = http
 *  system-type = kannel
 *  port = NNN
 *  smsc-username = XXX
 *  smsc-password = YYY
 *  send-url = "server.host:PORT"
 *
 *****
 * FOR SERVER/RELAY KANNEL:
 *
 *  group = smsbox
 *  sendsms-port = PORT
 *  ...
 * 
 *  group = sms-service
 *  keyword = ...
 *  url = "client.host:NNN/sms?user=XXX&pass=YYY&from=%p&to=%P&text=%a"
 *  max-messages = 0
 *
 *  group = send-sms
 *  username = XXX
 *  password = YYY
 *  
 * Kalle Marjola for Project Kannel 2001
 * Stipe Tolj <tolj@wapme-systems.de>
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <limits.h>

#include "gwlib/gwlib.h"
#include "smscconn.h"
#include "smscconn_p.h"
#include "bb_smscconn_cb.h"
#include "msg.h"
#include "sms.h"
#include "dlr.h"

typedef struct conndata {
    HTTPCaller *http_ref;
    long receive_thread;
    long send_cb_thread;
    int shutdown;
    int	port;   /* port for receiving SMS'es */
    Octstr *allow_ip;
    Octstr *send_url;
    long open_sends;
    Octstr *username;   /* if needed */
    Octstr *password;   /* as said */
    int no_sender;      /* ditto */
    int no_coding;      /* this, too */
    int no_sep;         /* not to mention this */

    /* callback functions set by HTTP-SMSC type */
    void (*send_sms) (SMSCConn *conn, Msg *msg);
    void (*parse_reply) (SMSCConn *conn, Msg *msg, int status,
                         List *headers, Octstr *body);
    void (*receive_sms) (SMSCConn *conn, HTTPClient *client,
                         List *headers, Octstr *body, List *cgivars);
} ConnData;


static void conndata_destroy(ConnData *conndata)
{
    if (conndata == NULL)
        return;
    if (conndata->http_ref)
        http_caller_destroy(conndata->http_ref);
    octstr_destroy(conndata->allow_ip);
    octstr_destroy(conndata->send_url);
    octstr_destroy(conndata->username);
    octstr_destroy(conndata->password);

    gw_free(conndata);
}


/*
 * Thread to listen to HTTP requests from SMSC entity
 */
static void httpsmsc_receiver(void *arg)
{
    SMSCConn *conn = arg;
    ConnData *conndata = conn->data;
    HTTPClient *client;
    Octstr *ip, *url, *body;
    List *headers, *cgivars;

    ip = url = body = NULL;
    headers = cgivars = NULL;

    /* Make sure we log into our own log-file if defined */
    log_thread_to(conn->log_idx);
 
    while (conndata->shutdown == 0) {

        /* XXX if conn->is_stopped, do not receive new messages.. */
	
        client = http_accept_request(conndata->port, &ip, &url,
                                     &headers, &body, &cgivars);
        if (client == NULL)
            break;

        debug("smsc.http", 0, "HTTP[%s]: Got request `%s'", 
              octstr_get_cstr(conn->id), octstr_get_cstr(url));

        if (connect_denied(conndata->allow_ip, ip)) {
            info(0, "HTTP[%s]: Connection `%s' tried from denied "
                    "host %s, ignored", octstr_get_cstr(conn->id),
                    octstr_get_cstr(url), octstr_get_cstr(ip));
            http_close_client(client);
        } else
            conndata->receive_sms(conn, client, headers, body, cgivars);

        debug("smsc.http", 0, "HTTP[%s]: Destroying client information",
              octstr_get_cstr(conn->id));
        octstr_destroy(url);
        octstr_destroy(ip);
        octstr_destroy(body);
        http_destroy_headers(headers);
        http_destroy_cgiargs(cgivars);
    }
    debug("smsc.http", 0, "HTTP[%s]: httpsmsc_receiver dying",
          octstr_get_cstr(conn->id));

    conndata->shutdown = 1;
    http_close_port(conndata->port);
    
    /* unblock http_receive_result() if there are no open sends */
    if (conndata->open_sends == 0)
        http_caller_signal_shutdown(conndata->http_ref);
}


/*
 * Thread to handle finished sendings
 */
static void httpsmsc_send_cb(void *arg)
{
    SMSCConn *conn = arg;
    ConnData *conndata = conn->data;
    Msg *msg;
    int status;
    List *headers;
    Octstr *final_url, *body;

    /* Make sure we log into our own log-file if defined */
    log_thread_to(conn->log_idx);

    while (conndata->shutdown == 0 || conndata->open_sends) {

        msg = http_receive_result(conndata->http_ref, &status,
                                  &final_url, &headers, &body);

        if (msg == NULL)
            break;  /* they told us to die, by unlocking */

        /* Handle various states here. */

        /* request failed and we are not in shutdown mode */
        if (status == -1 && conndata->shutdown == 0) { 
            error(0, "HTTP[%s]: Couldn't connect to SMS center "
                     "(retrying in %ld seconds).",
                     octstr_get_cstr(conn->id), conn->reconnect_delay);
            conn->status = SMSCCONN_RECONNECTING; 
            gwthread_sleep(conn->reconnect_delay);
            debug("smsc.http.kannel", 0, "HTTP[%s]: Re-sending request",
                  octstr_get_cstr(conn->id));
            conndata->send_sms(conn, msg);
            continue; 
        } 
        /* request failed and we *are* in shutdown mode, drop the message */ 
        else if (status == -1 && conndata->shutdown == 1) {
        }
        /* request succeeded */    
        else {
            /* we received a response, so this link is considered online again */
            if (status && conn->status != SMSCCONN_ACTIVE) {
                conn->status = SMSCCONN_ACTIVE;
            }
            conndata->parse_reply(conn, msg, status, headers, body);
        }
   
        conndata->open_sends--;

        http_destroy_headers(headers);
        octstr_destroy(final_url);
        octstr_destroy(body);
    }
    debug("smsc.http", 0, "HTTP[%s]: httpsmsc_send_cb dying",
          octstr_get_cstr(conn->id));
    conndata->shutdown = 1;

    if (conndata->open_sends) {
        warning(0, "HTTP[%s]: Shutdown while <%ld> requests are pending.",
                octstr_get_cstr(conn->id), conndata->open_sends);
    }

    gwthread_join(conndata->receive_thread);

    conn->data = NULL;
    conndata_destroy(conndata);

    conn->status = SMSCCONN_DEAD;
    bb_smscconn_killed();
}


/*----------------------------------------------------------------
 * SMSC-type specific functions
 *
 * 3 functions are needed for each:
 *
 *   1) send SMS
 *   2) parse send SMS result
 *   3) receive SMS (and send reply)
 *
 *   These functions do not return anything and do not destroy
 *   arguments. They must handle everything that happens therein
 *   and must call appropriate bb_smscconn functions
 */

/*----------------------------------------------------------------
 * Kannel
 */

enum { HEX_NOT_UPPERCASE = 0 };
enum { MAX_SMS_OCTETS = 140 };


static void kannel_send_sms(SMSCConn *conn, Msg *sms)
{
    ConnData *conndata = conn->data;
    Octstr *url;
    List *headers;

    if (!conndata->no_sep) {
        url = octstr_format("%S?"
			    "username=%E&password=%E&to=%E&text=%E",
			     conndata->send_url,
			     conndata->username, conndata->password,
			     sms->sms.receiver, sms->sms.msgdata);
    } else {
        octstr_binary_to_hex(sms->sms.msgdata, HEX_NOT_UPPERCASE);
        url = octstr_format("%S?"
			    "username=%E&password=%E&to=%E&text=%S",
			     conndata->send_url,
			     conndata->username, conndata->password,
			     sms->sms.receiver, 
                             sms->sms.msgdata); 
    }   

    if (octstr_len(sms->sms.udhdata)) {
        if (!conndata->no_sep) {
	    octstr_format_append(url, "&udh=%E", sms->sms.udhdata);
        } else {
	    octstr_binary_to_hex(sms->sms.udhdata, HEX_NOT_UPPERCASE);
            octstr_format_append(url, "&udh=%S", sms->sms.udhdata);
	}
    }

    if (!conndata->no_sender)
        octstr_format_append(url, "&from=%E", sms->sms.sender);
    if (sms->sms.mclass != MC_UNDEF)
	octstr_format_append(url, "&mclass=%d", sms->sms.mclass);
    if (!conndata->no_coding && sms->sms.coding != DC_UNDEF)
	octstr_format_append(url, "&coding=%d", sms->sms.coding);
    if (sms->sms.mwi != MWI_UNDEF)
	octstr_format_append(url, "&mwi=%d", sms->sms.mwi);
    if (sms->sms.account) /* prepend account with local username */
	octstr_format_append(url, "&account=%E:%E", sms->sms.service, sms->sms.account);
    if (sms->sms.binfo) /* prepend billing info */
	octstr_format_append(url, "&binfo=%S", sms->sms.binfo);
    if (sms->sms.smsc_id) /* proxy the smsc-id to the next instance */
	octstr_format_append(url, "&smsc=%S", sms->sms.smsc_id);
    if (sms->sms.dlr_url) {
        octstr_url_encode(sms->sms.dlr_url);
        octstr_format_append(url, "&dlr-url=%S", sms->sms.dlr_url);
    }
    if (sms->sms.dlr_mask != DLR_UNDEFINED && sms->sms.dlr_mask != DLR_NOTHING)
        octstr_format_append(url, "&drl-mask=%d", sms->sms.dlr_mask);

    headers = list_create();
    debug("smsc.http.kannel", 0, "HTTP[%s]: Start request",
          octstr_get_cstr(conn->id));
    http_start_request(conndata->http_ref, HTTP_METHOD_GET, url, headers, 
                       NULL, 0, sms, NULL);

    octstr_destroy(url);
    http_destroy_headers(headers);

}

static void kannel_parse_reply(SMSCConn *conn, Msg *msg, int status,
			       List *headers, Octstr *body)
{
    /* Test on three cases:
     * 1. an smsbox reply of an remote kannel instance
     * 2. an smsc_http response (if used for MT to MO looping)
     * 3. an smsbox reply of partly sucessfull sendings */
    if ((status == HTTP_OK || status == HTTP_ACCEPTED)
        && (octstr_case_compare(body, octstr_imm("Sent.")) == 0 ||
            octstr_case_compare(body, octstr_imm("Ok.")) == 0 ||
            octstr_ncompare(body, octstr_imm("Result: OK"),10) == 0)) {
        bb_smscconn_sent(conn, msg, NULL);
    } else {
        bb_smscconn_send_failed(conn, msg,
	            SMSCCONN_FAILED_MALFORMED, octstr_duplicate(body));
    }
}

static void kannel_receive_sms(SMSCConn *conn, HTTPClient *client,
			       List *headers, Octstr *body, List *cgivars)
{
    ConnData *conndata = conn->data;
    Octstr *user, *pass, *from, *to, *text, *udh, *account, *binfo, *tmp_string;
    Octstr *retmsg;
    int	mclass, mwi, coding, validity, deferred;
    List *reply_headers;
    int ret;

    mclass = mwi = coding = validity = deferred = 0;

    user = http_cgi_variable(cgivars, "username");
    pass = http_cgi_variable(cgivars, "password");
    from = http_cgi_variable(cgivars, "from");
    to = http_cgi_variable(cgivars, "to");
    text = http_cgi_variable(cgivars, "text");
    udh = http_cgi_variable(cgivars, "udh");
    account = http_cgi_variable(cgivars, "account");
    binfo = http_cgi_variable(cgivars, "binfo");
    tmp_string = http_cgi_variable(cgivars, "flash");
    if(tmp_string) {
	sscanf(octstr_get_cstr(tmp_string),"%d", &mclass);
    }
    tmp_string = http_cgi_variable(cgivars, "mclass");
    if(tmp_string) {
	sscanf(octstr_get_cstr(tmp_string),"%d", &mclass);
    }
    tmp_string = http_cgi_variable(cgivars, "mwi");
    if(tmp_string) {
	sscanf(octstr_get_cstr(tmp_string),"%d", &mwi);
    }
    tmp_string = http_cgi_variable(cgivars, "coding");
    if(tmp_string) {
	sscanf(octstr_get_cstr(tmp_string),"%d", &coding);
    }
    tmp_string = http_cgi_variable(cgivars, "validity");
    if(tmp_string) {
	sscanf(octstr_get_cstr(tmp_string),"%d", &validity);
    }
    tmp_string = http_cgi_variable(cgivars, "deferred");
    if(tmp_string) {
	sscanf(octstr_get_cstr(tmp_string),"%d", &deferred);
    }
    debug("smsc.http.kannel", 0, "HTTP[%s]: Received an HTTP request",
          octstr_get_cstr(conn->id));
    
    if (user == NULL || pass == NULL ||
	    octstr_compare(user, conndata->username) != 0 ||
	    octstr_compare(pass, conndata->password) != 0) {

        error(0, "HTTP[%s]: Authorization failure",
              octstr_get_cstr(conn->id));
        retmsg = octstr_create("Authorization failed for sendsms");
    }
    else if (from == NULL || to == NULL || text == NULL) {
	
        error(0, "HTTP[%s]: Insufficient args",
              octstr_get_cstr(conn->id));
        retmsg = octstr_create("Insufficient args, rejected");
    }
    else if (udh != NULL && (octstr_len(udh) != octstr_get_char(udh, 0) + 1)) {
        error(0, "HTTP[%s]: UDH field misformed, rejected",
              octstr_get_cstr(conn->id));
        retmsg = octstr_create("UDH field misformed, rejected");
    }
    else if (udh != NULL && octstr_len(udh) > MAX_SMS_OCTETS) {
        error(0, "HTTP[%s]: UDH field is too long, rejected",
              octstr_get_cstr(conn->id));
        retmsg = octstr_create("UDH field is too long, rejected");
    }
    else {

	Msg *msg;
	msg = msg_create(sms);

	debug("smsc.http.kannel", 0, "HTTP[%s]: Constructing new SMS",
          octstr_get_cstr(conn->id));
	
	msg->sms.sender = octstr_duplicate(from);
	msg->sms.receiver = octstr_duplicate(to);
	msg->sms.msgdata = octstr_duplicate(text);
	msg->sms.udhdata = octstr_duplicate(udh);

	msg->sms.smsc_id = octstr_duplicate(conn->id);
	msg->sms.time = time(NULL);
	msg->sms.mclass = mclass;
	msg->sms.mwi = mwi;
	msg->sms.coding = coding;
	msg->sms.validity = validity;
	msg->sms.deferred = deferred;
	msg->sms.account = octstr_duplicate(account);
	msg->sms.binfo = octstr_duplicate(binfo);
	ret = bb_smscconn_receive(conn, msg);
	if (ret == -1)
	    retmsg = octstr_create("Not accepted");
	else
	    retmsg = octstr_create("Sent.");
    }
    reply_headers = list_create();
    http_header_add(reply_headers, "Content-Type", "text/plain");
    debug("smsc.http.kannel", 0, "HTTP[%s]: Sending reply",
          octstr_get_cstr(conn->id));
    http_send_reply(client, HTTP_ACCEPTED, reply_headers, retmsg);

    octstr_destroy(retmsg);
    http_destroy_headers(reply_headers);
}


/*----------------------------------------------------------------
 * Brunet - A german aggregator (mainly doing T-Mobil D1 connections)
 *
 *  o bruHTT v1.3L (for MO traffic) 
 *  o bruHTP v2.1 (date 22.04.2003) (for MT traffic)
 *
 * Stipe Tolj <tolj@wapme-systems.de>
 */

/* MT related function */
static void brunet_send_sms(SMSCConn *conn, Msg *sms)
{
    ConnData *conndata = conn->data;
    Octstr *url, *id, *tid;
    List *headers;

    /* 
     * Construct TransactionId like this: <timestamp>-<receiver msisdn>-<msg.id> 
     * and then run md5 hash function to garantee uniqueness. 
     */
    id = octstr_format("%d%S%d", time(NULL), sms->sms.receiver, sms->sms.id);
    tid = md5(id);

    /* format the URL for call */
    url = octstr_format("%S?"
        "CustomerId=%E&MsIsdn=%E&Originator=%E&MessageType=%E"
        "&Text=%E&TransactionId=%E"
        "&SMSCount=1&ActionType=A&ServiceDeliveryType=P", /* static parts */
        conndata->send_url,
        conndata->username, sms->sms.receiver, sms->sms.sender,
        (octstr_len(sms->sms.udhdata) ? octstr_imm("B") : octstr_imm("S")),
        sms->sms.msgdata, tid);

    /* add binary UDH header */
    if (octstr_len(sms->sms.udhdata)) {
        octstr_format_append(url, "&XSer=01%02x%E", octstr_len(sms->sms.udhdata), 
                             sms->sms.udhdata);
    }

    /* 
     * We use &binfo=<foobar> from sendsms interface to encode any additionaly
     * proxied parameters, ie. billing information.
     */
    if (octstr_len(sms->sms.binfo)) {
        octstr_url_decode(sms->sms.binfo);
        octstr_format_append(url, "&%s", octstr_get_cstr(sms->sms.binfo));
    }

    headers = list_create();
    debug("smsc.http.brunet", 0, "HTTP[%s]: Sending request <%s>",
          octstr_get_cstr(conn->id), octstr_get_cstr(url));

    /* 
     * Brunet requires an SSL-enabled HTTP client call, this is handled
     * transparently by the Kannel HTTP layer module.
     */
    http_start_request(conndata->http_ref, HTTP_METHOD_GET, url, headers, 
                       NULL, 0, sms, NULL);

    octstr_destroy(url);
    octstr_destroy(id);
    octstr_destroy(tid);
    http_destroy_headers(headers);
}

static void brunet_parse_reply(SMSCConn *conn, Msg *msg, int status,
                               List *headers, Octstr *body)
{
    if (status == HTTP_OK || status == HTTP_ACCEPTED) {
        if (octstr_case_compare(body, octstr_imm("Status=0")) == 0) {
            bb_smscconn_sent(conn, msg, NULL);
        } else {
            error(0, "HTTP[%s]: Message was malformed. SMSC response `%s'.",
                  octstr_get_cstr(conn->id), octstr_get_cstr(body));
            bb_smscconn_send_failed(conn, msg,
	                SMSCCONN_FAILED_MALFORMED, octstr_duplicate(body));
        }
    } else {
        error(0, "HTTP[%s]: Message was rejected. SMSC reponse `%s'.",
              octstr_get_cstr(conn->id), octstr_get_cstr(body));
        bb_smscconn_send_failed(conn, msg,
	            SMSCCONN_FAILED_REJECTED, octstr_duplicate(body));
    }
}

/* MO related function */
static void brunet_receive_sms(SMSCConn *conn, HTTPClient *client,
                               List *headers, Octstr *body, List *cgivars)
{
    ConnData *conndata = conn->data;
    Octstr *user, *from, *to, *text, *udh, *date, *type;
    Octstr *retmsg;
    int	mclass, mwi, coding, validity, deferred;
    List *reply_headers;
    int ret;

    mclass = mwi = coding = validity = deferred = 0;

    user = http_cgi_variable(cgivars, "CustomerId");
    from = http_cgi_variable(cgivars, "MsIsdn");
    to = http_cgi_variable(cgivars, "Recipient");
    text = http_cgi_variable(cgivars, "SMMO");
    udh = http_cgi_variable(cgivars, "XSer");
    date = http_cgi_variable(cgivars, "DateReceived");
    type = http_cgi_variable(cgivars, "MessageType");

    debug("smsc.http.brunet", 0, "HTTP[%s]: Received a request",
          octstr_get_cstr(conn->id));
    
    if (user == NULL || octstr_compare(user, conndata->username) != 0) {
        error(0, "HTTP[%s]: Authorization failure. CustomerId was <%s>.",
              octstr_get_cstr(conn->id), octstr_get_cstr(user));
        retmsg = octstr_create("Authorization failed for MO submission.");
    }
    else if (from == NULL || to == NULL || text == NULL) {
        error(0, "HTTP[%s]: Insufficient args.",
              octstr_get_cstr(conn->id));
        retmsg = octstr_create("Insufficient arguments, rejected.");
    }
    else {
        Msg *msg;
        msg = msg_create(sms);

        debug("smsc.http.brunet", 0, "HTTP[%s]: Received new MO SMS.",
              octstr_get_cstr(conn->id));
	
        msg->sms.sender = octstr_duplicate(from);
        msg->sms.receiver = octstr_duplicate(to);
        msg->sms.msgdata = octstr_duplicate(text);
        msg->sms.udhdata = octstr_duplicate(udh);

        msg->sms.smsc_id = octstr_duplicate(conn->id);
        msg->sms.time = time(NULL); /* XXX maybe extract from DateReceived */ 
        msg->sms.mclass = mclass;
        msg->sms.mwi = mwi;
        msg->sms.coding = coding;
        msg->sms.validity = validity;
        msg->sms.deferred = deferred;

        ret = bb_smscconn_receive(conn, msg);
        if (ret == -1)
            retmsg = octstr_create("Status=1");
        else
            retmsg = octstr_create("Status=0");
    }

    reply_headers = list_create();
    http_header_add(reply_headers, "Content-Type", "text/plain");
    debug("smsc.http.brunet", 0, "HTTP[%s]: Sending reply `%s'.",
          octstr_get_cstr(conn->id), octstr_get_cstr(retmsg));
    http_send_reply(client, HTTP_OK, reply_headers, retmsg);

    octstr_destroy(retmsg);
    http_destroy_headers(reply_headers);
}


/*----------------------------------------------------------------
 * Xidris - An austrian aggregator 
 * Implementing version 1.3, 06.05.2003
 *
 * Stipe Tolj <tolj@wapme-systems.de>
 */

/* MT related function */
static void xidris_send_sms(SMSCConn *conn, Msg *sms)
{
    ConnData *conndata = conn->data;
    Octstr *url, *raw, *new_msg;
    List *headers;
    int dcs, esm_class;

    url = raw = new_msg = NULL;
    dcs = esm_class = 0;

    /* RAW additions to called URL */
    if (octstr_len(sms->sms.udhdata)) {

        /* set the data coding scheme (DCS) and ESM class fields */
        dcs = fields_to_dcs(sms, sms->sms.alt_dcs);
        /* ESM_CLASS_SUBMIT_STORE_AND_FORWARD_MODE | 
           ESM_CLASS_SUBMIT_UDH_INDICATOR */
        esm_class = 0x03 | 0x40; 
    
        /* prepend UDH header to message block */
        new_msg = octstr_duplicate(sms->sms.udhdata);
        octstr_append(new_msg, sms->sms.msgdata);

        raw = octstr_format("&dcs=%d&esm=%d", dcs, esm_class);
    }

    /* format the URL for call */
    url = octstr_format("%S?"
        "app_id=%E&key=%E&dest_addr=%E&source_addr=%E"
        "&type=%E&message=%E",
        conndata->send_url,
        conndata->username, conndata->password, sms->sms.receiver, sms->sms.sender,
        (raw ? octstr_imm("200") : (sms->sms.mclass ? octstr_imm("1") : octstr_imm("0"))), 
        (raw ? new_msg : sms->sms.msgdata));

    if (raw) {
        octstr_append(url, raw);
    }

    /* 
     * We use &account=<foobar> from sendsms interface to encode any additionaly
     * proxied parameters, ie. billing information.
     */
    if (octstr_len(sms->sms.account)) {
        octstr_url_decode(sms->sms.account);
        octstr_format_append(url, "&%s", octstr_get_cstr(sms->sms.account));
    }

    headers = list_create();
    debug("smsc.http.xidris", 0, "HTTP[%s]: Sending request <%s>",
          octstr_get_cstr(conn->id), octstr_get_cstr(url));

    http_start_request(conndata->http_ref, HTTP_METHOD_GET, url, headers, 
                       NULL, 0, sms, NULL);

    octstr_destroy(url);
    octstr_destroy(raw);
    octstr_destroy(new_msg);
    http_destroy_headers(headers);
}

/* 
 * Parse for an parameter of an given XML tag and return it as Octstr
 */
static Octstr *parse_xml_tag(Octstr *body, Octstr *tag)
{
    Octstr *stag, *etag, *ret;
    int spos, epos;
   
    stag = octstr_format("<%s>", octstr_get_cstr(tag));
    if ((spos = octstr_search(body, stag, 0)) == -1) {
        octstr_destroy(stag);
        return NULL;
    }
    etag = octstr_format("</%s>", octstr_get_cstr(tag));
    if ((epos = octstr_search(body, etag, spos+octstr_len(stag))) == -1) {
        octstr_destroy(stag);
        octstr_destroy(etag);
        return NULL;
    }
    
    ret = octstr_copy(body, spos+octstr_len(stag), epos+1 - (spos+octstr_len(etag)));  
    octstr_strip_blanks(ret);
    octstr_strip_crlfs(ret);

    octstr_destroy(stag);
    octstr_destroy(etag);

    return ret;
}

static void xidris_parse_reply(SMSCConn *conn, Msg *msg, int status,
                               List *headers, Octstr *body)
{
    Octstr *code, *desc;

    if (status == HTTP_OK || status == HTTP_ACCEPTED) {
        /* now parse the XML document for error code */
        code = parse_xml_tag(body, octstr_imm("status"));
        desc = parse_xml_tag(body, octstr_imm("description"));
        if (octstr_case_compare(code, octstr_imm("0")) == 0) {
            bb_smscconn_sent(conn, msg, NULL);
        } else {
            error(0, "HTTP[%s]: Message not accepted. Status code <%s> "
                  "description `%s'.", octstr_get_cstr(conn->id),
                  octstr_get_cstr(code), octstr_get_cstr(desc));
            bb_smscconn_send_failed(conn, msg,
	                SMSCCONN_FAILED_MALFORMED, octstr_duplicate(desc));
        }
    } else {
        error(0, "HTTP[%s]: Message was rejected. SMSC reponse was:",
              octstr_get_cstr(conn->id));
        octstr_dump(body, 0);
        bb_smscconn_send_failed(conn, msg,
	            SMSCCONN_FAILED_REJECTED, octstr_create("REJECTED"));
    }
}

/* MO related function */
static void xidris_receive_sms(SMSCConn *conn, HTTPClient *client,
                               List *headers, Octstr *body, List *cgivars)
{
    ConnData *conndata = conn->data;
    Octstr *user, *pass, *from, *to, *text, *account;
    Octstr *retmsg;
    int	mclass, mwi, coding, validity, deferred; 
    List *reply_headers;
    int ret, status;

    mclass = mwi = coding = validity = deferred = 0;
    retmsg = NULL;

    user = http_cgi_variable(cgivars, "app_id");
    pass = http_cgi_variable(cgivars, "key");
    from = http_cgi_variable(cgivars, "source_addr");
    to = http_cgi_variable(cgivars, "dest_addr");
    text = http_cgi_variable(cgivars, "message");
    account = http_cgi_variable(cgivars, "operator");

    debug("smsc.http.xidris", 0, "HTTP[%s]: Received a request",
          octstr_get_cstr(conn->id));

    if (user == NULL || pass == NULL ||
	    octstr_compare(user, conndata->username) != 0 ||
	    octstr_compare(pass, conndata->password) != 0) {
        error(0, "HTTP[%s]: Authorization failure. username was <%s>.",
              octstr_get_cstr(conn->id), octstr_get_cstr(user));
        retmsg = octstr_create("Authorization failed for MO submission.");
        status = HTTP_UNAUTHORIZED;
    }
    else if (from == NULL || to == NULL || text == NULL) {
        error(0, "HTTP[%s]: Insufficient args.",
              octstr_get_cstr(conn->id));
        retmsg = octstr_create("Insufficient arguments, rejected.");
        status = HTTP_BAD_REQUEST;
    }
    else {
        Msg *msg;
        msg = msg_create(sms);

        debug("smsc.http.xidris", 0, "HTTP[%s]: Received new MO SMS.",
              octstr_get_cstr(conn->id));
	
        msg->sms.sender = octstr_duplicate(from);
        msg->sms.receiver = octstr_duplicate(to);
        msg->sms.msgdata = octstr_duplicate(text);
        msg->sms.account = octstr_duplicate(account);

        msg->sms.smsc_id = octstr_duplicate(conn->id);
        msg->sms.time = time(NULL);
        msg->sms.mclass = mclass;
        msg->sms.mwi = mwi;
        msg->sms.coding = coding;
        msg->sms.validity = validity;
        msg->sms.deferred = deferred;

        ret = bb_smscconn_receive(conn, msg);
        status = (ret == 0 ? HTTP_OK : HTTP_FORBIDDEN);
    }

    reply_headers = list_create();
    debug("smsc.http.xidris", 0, "HTTP[%s]: Sending reply with HTTP status <%d>.",
          octstr_get_cstr(conn->id), status);

    http_send_reply(client, status, reply_headers, retmsg);

    octstr_destroy(retmsg);
    http_destroy_headers(reply_headers);
}


/*----------------------------------------------------------------
 * Wapme SMS Proxy
 *
 * Stipe Tolj <tolj@wapme-systems.de>
 */

static void wapme_smsproxy_send_sms(SMSCConn *conn, Msg *sms)
{
    ConnData *conndata = conn->data;
    Octstr *url;
    List *headers;

    url = octstr_format("%S?command=forward&smsText=%E&phoneNumber=%E"
                        "&serviceNumber=%E&smsc=%E",
                        conndata->send_url,
                        sms->sms.msgdata, sms->sms.sender, sms->sms.receiver,
                        sms->sms.smsc_id);

    headers = list_create();
    debug("smsc.http.wapme", 0, "HTTP[%s]: Start request",
          octstr_get_cstr(conn->id));
    http_start_request(conndata->http_ref, HTTP_METHOD_GET, url, headers, 
                       NULL, 0, sms, NULL);

    octstr_destroy(url);
    http_destroy_headers(headers);

}

static void wapme_smsproxy_parse_reply(SMSCConn *conn, Msg *msg, int status,
			       List *headers, Octstr *body)
{
    if (status == HTTP_OK || status == HTTP_ACCEPTED) {
        bb_smscconn_sent(conn, msg, NULL);
    } else {
        bb_smscconn_send_failed(conn, msg,
	            SMSCCONN_FAILED_MALFORMED, octstr_duplicate(body));
    }
}

/*
 * static void wapme_smsproxy_receive_sms(SMSCConn *conn, HTTPClient *client,
 *                                List *headers, Octstr *body, List *cgivars)
 *
 * The HTTP server for MO messages will act with the same interface as smsbox's 
 * sendsms interface, so that the logical difference is hidden and SMS Proxy 
 * can act transparently. So there is no need for an explicite implementation
 * here.
 */


/*-----------------------------------------------------------------
 * functions to implement various smscconn operations
 */

static int httpsmsc_send(SMSCConn *conn, Msg *msg)
{
    ConnData *conndata = conn->data;
    Msg *sms = msg_duplicate(msg);
    double delay = 0;

    if (conn->throughput) {
        delay = 1.0 / conn->throughput;
    }

    conndata->open_sends++;
    conndata->send_sms(conn, sms);

    /* obey throughput speed limit, if any */
    if (conn->throughput)
        gwthread_sleep(delay);

    return 0;
}


static long httpsmsc_queued(SMSCConn *conn)
{
    ConnData *conndata = conn->data;

    return (conndata ? (conn->status != SMSCCONN_DEAD ? 
            conndata->open_sends : 0) : 0);
}


static int httpsmsc_shutdown(SMSCConn *conn, int finish_sending)
{
    ConnData *conndata = conn->data;

    debug("httpsmsc_shutdown", 0, "HTTP[%s]: Shutting down",
          octstr_get_cstr(conn->id));
    conn->why_killed = SMSCCONN_KILLED_SHUTDOWN;
    conndata->shutdown = 1;

    http_close_port(conndata->port);
    return 0;
}


int smsc_http_create(SMSCConn *conn, CfgGroup *cfg)
{
    ConnData *conndata = NULL;
    Octstr *type;
    long portno;   /* has to be long because of cfg_get_integer */
    int ssl = 0;   /* indicate if SSL-enabled server should be used */

    if (cfg_get_integer(&portno, cfg, octstr_imm("port")) == -1) {
        error(0, "HTTP[%s]: 'port' invalid in smsc 'http' record.",
              octstr_get_cstr(conn->id));
        return -1;
    }
    if ((type = cfg_get(cfg, octstr_imm("system-type")))==NULL) {
        error(0, "HTTP[%s]: 'type' missing in smsc 'http' record.",
              octstr_get_cstr(conn->id));
        octstr_destroy(type);
        return -1;
    }
    conndata = gw_malloc(sizeof(ConnData));
    conndata->http_ref = NULL;

    conndata->allow_ip = cfg_get(cfg, octstr_imm("connect-allow-ip"));
    conndata->send_url = cfg_get(cfg, octstr_imm("send-url"));
    conndata->username = cfg_get(cfg, octstr_imm("smsc-username"));
    conndata->password = cfg_get(cfg, octstr_imm("smsc-password"));
    cfg_get_bool(&conndata->no_sender, cfg, octstr_imm("no-sender"));
    cfg_get_bool(&conndata->no_coding, cfg, octstr_imm("no-coding"));
    cfg_get_bool(&conndata->no_sep, cfg, octstr_imm("no-sep"));

    if (conndata->send_url == NULL)
        panic(0, "HTTP[%s]: Sending not allowed. No 'send-url' specified.",
              octstr_get_cstr(conn->id));

    if (octstr_case_compare(type, octstr_imm("kannel")) == 0) {
        if (conndata->username == NULL || conndata->password == NULL) {
            error(0, "HTTP[%s]: 'username' and 'password' required for Kannel http smsc",
                  octstr_get_cstr(conn->id));
            goto error;
        }
        conndata->receive_sms = kannel_receive_sms;
        conndata->send_sms = kannel_send_sms;
        conndata->parse_reply = kannel_parse_reply;
    }
    else if (octstr_case_compare(type, octstr_imm("brunet")) == 0) {
        if (conndata->username == NULL) {
            error(0, "HTTP[%s]: 'username' required for Brunet http smsc",
                  octstr_get_cstr(conn->id));
            goto error;
        }
        conndata->receive_sms = brunet_receive_sms;
        conndata->send_sms = brunet_send_sms;
        conndata->parse_reply = brunet_parse_reply;
    }
    else if (octstr_case_compare(type, octstr_imm("xidris")) == 0) {
        if (conndata->username == NULL || conndata->password == NULL) {
            error(0, "HTTP[%s]: 'username' and 'password' required for Xidris http smsc",
                  octstr_get_cstr(conn->id));
            goto error;
        }
        conndata->receive_sms = xidris_receive_sms;
        conndata->send_sms = xidris_send_sms;
        conndata->parse_reply = xidris_parse_reply;
    }
    else if (octstr_case_compare(type, octstr_imm("wapme")) == 0) {
        if (conndata->send_url == NULL) {
            error(0, "HTTP[%s]: 'send-url' required for Wapme http smsc",
                  octstr_get_cstr(conn->id));
            goto error;
        }
        else if (conndata->username == NULL || conndata->password == NULL) {
            error(0, "HTTP[%s]: 'username' and 'password' required for Wapme http smsc",
                  octstr_get_cstr(conn->id));
            goto error;
        }
        conndata->receive_sms = kannel_receive_sms; /* emulate sendsms interface */
        conndata->send_sms = wapme_smsproxy_send_sms;
        conndata->parse_reply = wapme_smsproxy_parse_reply;
    }
    /*
     * ADD NEW HTTP SMSC TYPES HERE
     */
    else {
	error(0, "HTTP[%s]: system-type '%s' unknown smsc 'http' record.",
          octstr_get_cstr(conn->id), octstr_get_cstr(type));

	goto error;
    }	
    conndata->open_sends = 0;
    conndata->http_ref = http_caller_create();
    
    conn->data = conndata;
    conn->name = octstr_format("HTTP:%S", type);
    conn->status = SMSCCONN_ACTIVE;
    conn->connect_time = time(NULL);

    conn->shutdown = httpsmsc_shutdown;
    conn->queued = httpsmsc_queued;
    conn->send_msg = httpsmsc_send;

    if (http_open_port_if(portno, ssl, conn->our_host)==-1)
	goto error;

    conndata->port = portno;
    conndata->shutdown = 0;
    
    if ((conndata->receive_thread =
	 gwthread_create(httpsmsc_receiver, conn)) == -1)
	goto error;

    if ((conndata->send_cb_thread =
	 gwthread_create(httpsmsc_send_cb, conn)) == -1)
	goto error;

    info(0, "HTTP[%s]: Initiated and ready", octstr_get_cstr(conn->id));
    
    octstr_destroy(type);
    return 0;

error:
    error(0, "HTTP[%s]: Failed to create http smsc connection",
          octstr_get_cstr(conn->id));

    conn->data = NULL;
    conndata_destroy(conndata);
    conn->why_killed = SMSCCONN_KILLED_CANNOT_CONNECT;
    conn->status = SMSCCONN_DEAD;
    octstr_destroy(type);
    return -1;
}

