/* ==================================================================== 
 * The Kannel Software License, Version 1.0 
 * 
 * Copyright (c) 2001-2004 Kannel Group  
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
 * shared.c - some utility routines shared by all Kannel boxes
 *
 * Lars Wirzenius
 */

#include <sys/utsname.h>
#include <libxml/xmlversion.h>

#include "gwlib/gwlib.h"
#include "shared.h"
#include "sms.h"
#include "dlr.h"

#if defined(HAVE_LIBSSL) || defined(HAVE_WTLS_OPENSSL) 
#include <openssl/opensslv.h>
#endif
#ifdef HAVE_MYSQL 
#include <mysql_version.h>
#include <mysql.h>
#endif
#ifdef HAVE_SQLITE 
#include <sqlite.h>
#endif


volatile enum program_status program_status = starting_up;


void report_versions(const char *boxname)
{
    Octstr *os;
    
    os = version_report_string(boxname);
    debug("gwlib.gwlib", 0, "%s", octstr_get_cstr(os));
    octstr_destroy(os);
}


Octstr *version_report_string(const char *boxname)
{
    struct utsname u;

    uname(&u);
    return octstr_format(GW_NAME " %s version `%s'.\nBuild `%s', compiler `%s'.\n"
    	    	    	 "System %s, release %s, version %s, machine %s.\n"
			 "Hostname %s, IP %s.\n"
			 "Libxml version %s.\n"
#ifdef HAVE_LIBSSL
             "Using "
#ifdef HAVE_WTLS_OPENSSL
             "WTLS library "
#endif
             "%s.\n"
#endif
#ifdef HAVE_MYSQL
             "Compiled with MySQL %s, using MySQL %s.\n"
#endif
#ifdef HAVE_SDB
             "Using LibSDB %s.\n"
#endif
#ifdef HAVE_SQLITE
             "Using SQLite %s.\n"
#endif
             "Using %s malloc.\n",
			 boxname, GW_VERSION,
#ifdef __GNUC__ 
             (__DATE__ " " __TIME__) ,
             __VERSION__,
#else 
             "unknown" , "unknown"
#endif 
			 u.sysname, u.release, u.version, u.machine,
			 octstr_get_cstr(get_official_name()),
			 octstr_get_cstr(get_official_ip()),
			 LIBXML_DOTTED_VERSION,
#ifdef HAVE_LIBSSL
             OPENSSL_VERSION_TEXT,
#endif
#ifdef HAVE_MYSQL
             MYSQL_SERVER_VERSION, mysql_get_client_info(),
#endif
#ifdef HAVE_SDB
             LIBSDB_VERSION,
#endif
#ifdef HAVE_SQLITE
             SQLITE_VERSION,
#endif
             octstr_get_cstr(gwmem_type()));
}


/***********************************************************************
 * Communication with the bearerbox.
 */

/* this is a static connection if only *one* boxc connection is 
 * established from a foobarbox to bearerbox. */
static Connection *bb_conn;


Connection *connect_to_bearerbox_real(Octstr *host, int port, int ssl, Octstr *our_host)
{
    Connection *conn;

#ifdef HAVE_LIBSSL
	if (ssl) 
	    conn = conn_open_ssl(host, port, NULL, our_host);
        /* XXX add certkeyfile to be given to conn_open_ssl */
	else
#endif /* HAVE_LIBSSL */
    conn = conn_open_tcp(host, port, our_host);
    if (conn == NULL)
    	panic(0, "Couldn't connect to the bearerbox.");
    if (ssl)
        info(0, "Connected to bearerbox at %s port %d using SSL.",
	         octstr_get_cstr(host), port);
    else
        info(0, "Connected to bearerbox at %s port %d.",
	         octstr_get_cstr(host), port);

    return conn;
}


void connect_to_bearerbox(Octstr *host, int port, int ssl, Octstr *our_host)
{
    bb_conn = connect_to_bearerbox_real(host, port, ssl, our_host);
}


void close_connection_to_bearerbox_real(Connection *conn)
{
    conn_destroy(conn);
    conn = NULL;
}


void close_connection_to_bearerbox(void)
{
    close_connection_to_bearerbox_real(bb_conn);
}


void write_to_bearerbox_real(Connection *conn, Msg *pmsg)
{
    Octstr *pack;

    pack = msg_pack(pmsg);
    if (conn_write_withlen(conn, pack) == -1)
    	error(0, "Couldn't write Msg to bearerbox.");

    msg_destroy(pmsg);
    octstr_destroy(pack);
}


void write_to_bearerbox(Msg *pmsg)
{
    write_to_bearerbox_real(bb_conn, pmsg);
}


int deliver_to_bearerbox_real(Connection *conn, Msg *msg) 
{
     
    Octstr *pack;
    
    pack = msg_pack(msg);
    if (conn_write_withlen(conn, pack) == -1) {
    	error(0, "Couldn't deliver Msg to bearerbox.");
        octstr_destroy(pack);
        return -1;
    }
                                   
    octstr_destroy(pack);
    msg_destroy(msg);
    return 0;
}


int deliver_to_bearerbox(Msg *msg)
{
    return deliver_to_bearerbox_real(bb_conn, msg);
}
                                           

Msg *read_from_bearerbox_real(Connection *conn, double seconds)
{
    int ret;
    Octstr *pack;
    Msg *msg;

    pack = NULL;
    while (program_status != shutting_down) {
        pack = conn_read_withlen(conn);
        gw_claim_area(pack);
        if (pack != NULL)
            break;

        if (conn_read_error(conn)) {
            info(0, "Error reading from bearerbox, disconnecting.");
            return NULL;
        }
        if (conn_eof(conn)) {
            info(0, "Connection closed by the bearerbox.");
            return NULL;
        }

        ret = conn_wait(conn, seconds);
        if (ret < 0) {
            error(0, "Connection to bearerbox broke.");
            return NULL;
        }
        else if (ret == 1) {
            info(0, "Connection to bearerbox timed out after %.2f seconds.", seconds);
            return NULL;
        }
    }
    
    if (pack == NULL)
        return NULL;

    msg = msg_unpack(pack);
    octstr_destroy(pack);

    if (msg == NULL)
        error(0, "Failed to unpack data!");

    return msg;
}


Msg *read_from_bearerbox(double seconds)
{
    return read_from_bearerbox_real(bb_conn, seconds);
}


/*****************************************************************************
 *
 * Function validates an OSI date. Return unmodified octet string date when it
 * is valid, NULL otherwise.
 */

Octstr *parse_date(Octstr *date)
{
    long date_value;

    if (octstr_get_char(date, 4) != '-')
        goto error;
    if (octstr_get_char(date, 7) != '-')
        goto error;
    if (octstr_get_char(date, 10) != 'T')
        goto error;
    if (octstr_get_char(date, 13) != ':')
        goto error;
    if (octstr_get_char(date, 16) != ':')
        goto error;
    if (octstr_get_char(date, 19) != 'Z')
        goto error;

    if (octstr_parse_long(&date_value, date, 0, 10) < 0)
        goto error;
    if (octstr_parse_long(&date_value, date, 5, 10) < 0)
        goto error;
    if (date_value < 1 || date_value > 12)
        goto error;
    if (octstr_parse_long(&date_value, date, 8, 10) < 0)
        goto error;
    if (date_value < 1 || date_value > 31)
        goto error;
    if (octstr_parse_long(&date_value, date, 11, 10) < 0)
        goto error;
    if (date_value < 0 || date_value > 23)
        goto error;
    if (octstr_parse_long(&date_value, date, 14, 10) < 0)
        goto error;
    if (date_value < 0 || date_value > 59)
        goto error;
    if (date_value < 0 || date_value > 59)
        goto error;
    if (octstr_parse_long(&date_value, date, 17, 10) < 0)
        goto error;

    return date;

error:
    warning(0, "parse_date: not an ISO date");
    return NULL;
}

/*****************************************************************************
 *
 * Split an SMS message into smaller ones.
 */

static void prepend_catenation_udh(Msg *sms, int part_no, int num_messages,
    	    	    	    	   int msg_sequence)
{
    if (sms->sms.udhdata == NULL)
        sms->sms.udhdata = octstr_create("");
    if (octstr_len(sms->sms.udhdata) == 0)
	octstr_append_char(sms->sms.udhdata, CATENATE_UDH_LEN);
    octstr_format_append(sms->sms.udhdata, "%c\3%c%c%c", 
    	    	    	 0, msg_sequence, num_messages, part_no);

    /* 
     * Now that we added the concatenation information the
     * length is all wrong. we need to recalculate it. 
     */
    octstr_set_char(sms->sms.udhdata, 0, octstr_len(sms->sms.udhdata) - 1 );
}


static Octstr *extract_msgdata_part(Octstr *msgdata, Octstr *split_chars,
    	    	    	    	    int max_part_len)
{
    long i, len;
    Octstr *part;

    len = max_part_len;
    if (split_chars != NULL)
	for (i = max_part_len; i > 0; i--)
	    if (octstr_search_char(split_chars,
				   octstr_get_char(msgdata, i - 1), 0) != -1) {
		len = i;
		break;
	    }
    part = octstr_copy(msgdata, 0, len);
    octstr_delete(msgdata, 0, len);
    return part;
}


static Octstr *extract_msgdata_part_by_coding(Msg *msg, Octstr *split_chars,
											  int max_part_len)
{
	Octstr *temp = NULL;
	int pos, esc_count;

	if (msg->sms.coding == DC_8BIT || msg->sms.coding == DC_UCS2) {
        /* nothing to do here, just call the original extract_msgdata_part */
		return extract_msgdata_part(msg->sms.msgdata, split_chars, max_part_len);
	}

	/* 
     * else we need to do something special. I'll just get charset_gsm_truncate to
     * cut the string to the required length and then count real characters. 
     */
	temp = octstr_duplicate(msg->sms.msgdata);
	charset_latin1_to_gsm(temp);
	charset_gsm_truncate(temp, max_part_len);
	
	pos = esc_count = 0;

	while ((pos = octstr_search_char(temp, 27, pos)) != -1) {
		++pos;
    	++esc_count;
	}

	octstr_destroy(temp);

	/* now just call the original extract_msgdata_part with the new length */
	return extract_msgdata_part(msg->sms.msgdata, split_chars, max_part_len - esc_count);
}


List *sms_split(Msg *orig, Octstr *header, Octstr *footer, 
                Octstr *nonlast_suffix, Octstr *split_chars, 
                int catenate, unsigned long msg_sequence,
                int max_messages, int max_octets)
{
    long max_part_len, udh_len, hf_len, nlsuf_len;
    unsigned long total_messages, msgno;
    long last;
    List *list;
    Msg *part, *temp;

    hf_len = octstr_len(header) + octstr_len(footer);
    nlsuf_len = octstr_len(nonlast_suffix);
    udh_len = octstr_len(orig->sms.udhdata);

    /* First check whether the message is under one-part maximum */
    if (orig->sms.coding == DC_8BIT || orig->sms.coding == DC_UCS2)
        max_part_len = max_octets - udh_len - hf_len;
    else
        max_part_len = max_octets * 8 / 7 - (udh_len * 8 + 6) / 7 - hf_len;

    if (sms_msgdata_len(orig) > max_part_len && catenate) {
        /* Change part length to take concatenation overhead into account */
        if (udh_len == 0)
            udh_len = 1;  /* Add the udh total length octet */
        udh_len += CATENATE_UDH_LEN;
        if (orig->sms.coding == DC_8BIT || orig->sms.coding == DC_UCS2)
            max_part_len = max_octets - udh_len - hf_len;
        else
            max_part_len = max_octets * 8 / 7 - (udh_len * 8 + 6) / 7 - hf_len;
    }

    /* ensure max_part_len is never negativ */
    max_part_len = max_part_len > 0 ? max_part_len : 0;

    temp = msg_duplicate(orig);
    msgno = 0;
    list = list_create();

    do {
        msgno++;
        part = msg_duplicate(orig);

        /* 
         * if its a DLR request message getting split, 
         * only ask DLR for the first one 
         */
        if ((msgno > 1) && DLR_IS_ENABLED(part->sms.dlr_mask)) {
            octstr_destroy(part->sms.dlr_url);
            part->sms.dlr_url = NULL;
            part->sms.dlr_mask = 0;
        }
        octstr_destroy(part->sms.msgdata);
        if (sms_msgdata_len(temp) <= max_part_len || msgno == max_messages) {
            part->sms.msgdata = temp->sms.msgdata ? 
                octstr_copy(temp->sms.msgdata, 0, max_part_len) : octstr_create("");
            last = 1;
        }
        else {
            part->sms.msgdata = 
                extract_msgdata_part_by_coding(temp, split_chars,
                                               max_part_len - nlsuf_len);
            last = 0;
        }
        if (header)
            octstr_insert(part->sms.msgdata, header, 0);
        if (footer)
            octstr_append(part->sms.msgdata, footer);
        if (!last && nonlast_suffix)
            octstr_append(part->sms.msgdata, nonlast_suffix);
        list_append(list, part);
    } while (!last);

    total_messages = msgno;
    msg_destroy(temp);
    if (catenate && total_messages > 1) {
        for (msgno = 1; msgno <= total_messages; msgno++) {
            part = list_get(list, msgno - 1);
            prepend_catenation_udh(part, msgno, total_messages, msg_sequence);
        }
    }

    return list;
}

