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

#if defined(HAVE_LIBSSL) || defined(HAVE_WTLS_OPENSSL) 
#include <openssl/opensslv.h>
#endif
#ifdef HAVE_MYSQL 
#include <mysql_version.h>
#endif


enum program_status program_status = starting_up;


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
    return octstr_format(GW_NAME " %s version `%s'.\n"
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
             "Using MySQL %s.\n"
#endif
#ifdef HAVE_SDB
             "Using LibSDB %s.\n"
#endif
             "Using %s malloc.\n",
			 boxname, VERSION,
			 u.sysname, u.release, u.version, u.machine,
			 octstr_get_cstr(get_official_name()),
			 octstr_get_cstr(get_official_ip()),
			 LIBXML_VERSION_STRING,
#ifdef HAVE_LIBSSL
             OPENSSL_VERSION_TEXT,
#endif
#ifdef HAVE_MYSQL
             MYSQL_SERVER_VERSION,
#endif
#ifdef HAVE_SDB
             LIBSDB_VERSION,
#endif
             octstr_get_cstr(gwmem_type()));
}


/***********************************************************************
 * Communication with the bearerbox.
 */


static Connection *bb_conn;


void connect_to_bearerbox(Octstr *host, int port, int ssl, Octstr *our_host)
{
#ifdef HAVE_LIBSSL
	if (ssl) 
	    bb_conn = conn_open_ssl(host, port, NULL, our_host);
        /* XXX add certkeyfile to be given to conn_open_ssl */
	else
#endif /* HAVE_LIBSSL */
    bb_conn = conn_open_tcp(host, port, our_host);
    if (bb_conn == NULL)
    	panic(0, "Couldn't connect to the bearerbox.");
    if (ssl)
        info(0, "Connected to bearerbox at %s port %d using SSL.",
	         octstr_get_cstr(host), port);
    else
        info(0, "Connected to bearerbox at %s port %d.",
	         octstr_get_cstr(host), port);
}


Connection *get_connect_to_bearerbox(Octstr *host, int port, int ssl, Octstr *our_host)
{
#ifdef HAVE_LIBSSL
       if (ssl)
           bb_conn = conn_open_ssl(host, port, NULL, our_host);
        /* XXX add certkeyfile to be given to conn_open_ssl */
       else
#endif /* HAVE_LIBSSL */
    bb_conn = conn_open_tcp(host, port, our_host);

    return bb_conn;
}


void close_connection_to_bearerbox(void)
{
    conn_destroy(bb_conn);
    bb_conn = NULL;
}


void write_to_bearerbox(Msg *pmsg)
{
    Octstr *pack;

    pack = msg_pack(pmsg);
    if (conn_write_withlen(bb_conn, pack) == -1)
    	error(0, "Couldn't write Msg to bearerbox.");

    msg_destroy(pmsg);
    octstr_destroy(pack);
}


int deliver_to_bearerbox(Msg *msg) 
{
     
    Octstr *pack;
    
    pack = msg_pack(msg);
    if (conn_write_withlen(bb_conn, pack) == -1) {
    	error(0, "Couldn't deliver Msg to bearerbox.");
        octstr_destroy(pack);
        return -1;
    }
                                   
    octstr_destroy(pack);
    msg_destroy(msg);
    return 0;
}
                                               

Msg *read_from_bearerbox(void)
{
    int ret;
    Octstr *pack;
    Msg *msg;

    pack = NULL;
    while (program_status != shutting_down) {
	pack = conn_read_withlen(bb_conn);
	gw_claim_area(pack);
	if (pack != NULL)
	    break;
	if (conn_read_error(bb_conn)) {
	    info(0, "Error reading from bearerbox, disconnecting");
	    return NULL;
	}
	if (conn_eof(bb_conn)) {
	    info(0, "Connection closed by the bearerbox");
	    return NULL;
	}

	ret = conn_wait(bb_conn, -1.0);
	if (ret < 0) {
	    error(0, "Connection to bearerbox broke.");
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
	    udh_len = 1;  /* To add the udh total length octet */
	udh_len += CATENATE_UDH_LEN;
	if (orig->sms.coding == DC_8BIT || orig->sms.coding == DC_UCS2)
	    max_part_len = max_octets - udh_len - hf_len;
	else
	    max_part_len = max_octets * 8 / 7 - (udh_len * 8 + 6) / 7 - hf_len;
    }

    temp = msg_duplicate(orig);
    msgno = 0;
    list = list_create();
    do {
	msgno++;
         /* if its a DLR request message getting split, only ask DLR for the first one */
        part = msg_duplicate(orig);
	if((msgno > 1) && (part->sms.dlr_mask))
        {
           octstr_destroy(part->sms.dlr_url);
           part->sms.dlr_url = NULL;
           part->sms.dlr_mask = 0;
        }
	octstr_destroy(part->sms.msgdata);
	if (sms_msgdata_len(temp) <= max_part_len || msgno == max_messages) {
	    part->sms.msgdata = octstr_copy(temp->sms.msgdata, 0, max_part_len);
	    last = 1;
	}
	else {
	    part->sms.msgdata = extract_msgdata_part_by_coding(temp, split_chars,
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






