/*
 * wsp.c - Implement WSP
 *
 * Lars Wirzenius <liw@wapit.com>
 * Capabilities/headers by Kalle Marjola <rpr@wapit.com>
 */


#include <string.h>

#include "gwlib/gwlib.h"
#include "wsp.h"
#include "wsp_headers.h"

/* WAP standard defined values for capabilities */

#define WSP_CAPS_CLIENT_SDU_SIZE	0x00
#define WSP_CAPS_SERVER_SDU_SIZE	0x01
#define WSP_CAPS_PROTOCOL_OPTIONS	0x02
#define WSP_CAPS_METHOD_MOR		0x03
#define WSP_CAPS_PUSH_MOR		0x04
#define WSP_CAPS_EXTENDED_METHODS    	0x05
#define WSP_CAPS_HEADER_CODE_PAGES    	0x06
#define WSP_CAPS_ALIASES	   	0x07



enum {
	Bad_PDU = -1,
	Connect_PDU = 0x01,
	ConnectReply_PDU = 0x02,
	Redirect_PDU = 0x03,
	Reply_PDU = 0x04,
	Disconnect_PDU = 0x05,
	Push_PDU = 0x06,
	ConfirmedPush_PDU = 0x07,
	Suspend_PDU = 0x08,
	Resume_PDU = 0x09,
	Get_PDU = 0x40,
	Options_PDU = 0x41,
	Head_PDU = 0x42,
	Delete_PDU = 0x43,
	Trace_PDU = 0x44,
	Post_PDU = 0x60,
	Put_PDU = 0x61
};

typedef enum {
	#define STATE_NAME(name) name,
	#define ROW(state, event, condition, action, next_state)
	#include "wsp_state-decl.h"
	WSPState_count
} WSPState;


static List *session_machines = NULL;
static Counter *session_id_counter = NULL;


static void append_to_event_queue(WSPMachine *machine, WSPEvent *event);
static WSPEvent *remove_from_event_queue(WSPMachine *machine);

static int unpack_connect_pdu(WSPMachine *m, Octstr *user_data);
static int unpack_get_pdu(Octstr **url, List **headers, Octstr *pdu);
static int unpack_post_pdu(Octstr **url, Octstr **headers, Octstr *pdu);

static int unpack_uint8(unsigned long *u, Octstr *os, int *off);
static int unpack_uintvar(unsigned long *u, Octstr *os, int *off);
static int unpack_octstr(Octstr **ret, int len, Octstr *os, int *off);

static char *wsp_state_to_string(WSPState state);
static long wsp_next_session_id(void);

static void append_uint8(Octstr *pdu, long n);
static void append_uintvar(Octstr *pdu, long n);
static void append_octstr(Octstr *pdu, Octstr *os);

static Octstr *make_connectionmode_pdu(long type);
static Octstr *make_connectreply_pdu(WSPMachine *m, long session_id);
static Octstr *make_reply_pdu(long status, long type, Octstr *body);

static long convert_http_status_to_wsp_status(long http_status);

static long new_server_transaction_id(void);
static int transaction_belongs_to_session(void *session, void *wtp);
static int same_client(void *sm1, void *sm2);



void wsp_init(void) {
	session_machines = list_create();
	session_id_counter = counter_create();
}



void wsp_shutdown(void) {
	while (list_len(session_machines) > 0)
		wsp_machine_destroy(list_get(session_machines, 0));
	list_destroy(session_machines);
	counter_destroy(session_id_counter);
}



WSPEvent *wsp_event_create(WSPEventType type) {
	WSPEvent *event;
	
	event = gw_malloc(sizeof(WSPEvent));
	event->type = type;
#if 0
	event->next = NULL;
#endif

	#define INTEGER(name) p->name = 0
	#define OCTSTR(name) p->name = NULL
	#define WTP_MACHINE(name) p->name = NULL
	#define SESSION_MACHINE(name) p->name = NULL
	#define WSP_EVENT(name, fields) \
		{ struct name *p = &event->name; fields }
	#define HTTPHEADER(name) p->name = NULL
	#include "wsp_events-decl.h"

	return event;
}


WSPEvent *wsp_event_duplicate(WSPEvent *event) {
	WSPEvent *copy;
	
	copy = gw_malloc(sizeof(WSPEvent));
	copy->type = event->type;

	#define INTEGER(name) p->name = q->name
	#define OCTSTR(name) p->name = octstr_duplicate(q->name)
	#define WTP_MACHINE(name) p->name = q->name
	#define SESSION_MACHINE(name) p->name = q->name
	#define WSP_EVENT(name, fields) \
		{ struct name *p = &copy->name; \
		  struct name *q = &event->name; \
		  fields }
	#define HTTPHEADER(name) p->name = http2_header_duplicate(q->name)
	#include "wsp_events-decl.h"

	return copy;
}


void wsp_event_destroy(WSPEvent *event) {
	if (event != NULL) {
		#define INTEGER(name) p->name = 0
		#define OCTSTR(name) octstr_destroy(p->name)
		#define WTP_MACHINE(name) p->name = NULL
		#define SESSION_MACHINE(name) p->name = NULL
		#define WSP_EVENT(name, fields) \
			{ struct name *p = &event->name; fields }
		#define HTTPHEADER(name) p->name = NULL
		#include "wsp_events-decl.h"

		gw_free(event);
	}
}

char *wsp_event_name(WSPEventType type) {
	switch (type) {
	#define WSP_EVENT(name, fields) case name: return #name;
	#define INTEGER
	#define OCTSTR
	#define WTP_MACHINE
	#define SESSION_MACHINE
	#define HTTPHEADER
	#include "wsp_events-decl.h"
	default:
		return "unknown WSPEvent type";
	}
}


void wsp_event_dump(WSPEvent *event) {
	debug("wap.wsp", 0, "Dump of WSPEvent %p follows:", (void *) event);
	debug("wap.wsp", 0, "  type: %s (%d)", wsp_event_name(event->type), event->type);
	#define INTEGER(name) debug("wap.wsp", 0, "  %s.%s: %d", t, #name, p->name)
	#define OCTSTR(name) \
		debug("wap.wsp", 0, "  %s.%s:", t, #name); \
		octstr_dump(p->name, 1)
	#define WTP_MACHINE(name) \
		debug("wap.wsp", 0, "  %s.%s at %p", t, #name, (void *) p->name)
	#define SESSION_MACHINE(name) \
		debug("wap.wsp", 0, "  %s.%s at %p", t, #name, (void *) p->name)
	#define WSP_EVENT(tt, fields) \
		if (tt == event->type) \
			{ char *t = #tt; struct tt *p = &event->tt; fields }
	#define HTTPHEADER(name) \
		debug("wap.wsp", 0, "  %s.%s: HTTP headers:", t, #name); \
		http2_header_dump(p->name)
	#include "wsp_events-decl.h"
	debug("wap.wsp", 0, "Dump of WSPEvent %p ends.", (void *) event);
}


void wsp_dispatch_event(WTPMachine *wtp_sm, WSPEvent *event) {
	WSPMachine *sm;
	
	/* XXX this should probably be moved to a condition function --liw */
	if (event->type == TR_Invoke_Ind &&
	    event->TR_Invoke_Ind.tcl == 2 &&
	    wsp_deduce_pdu_type(event->TR_Invoke_Ind.user_data, 0) == Connect_PDU) {
		/* Client wants to start new session. Igore existing
		   machines. */
		sm = NULL;
	} else {
		sm = list_search(session_machines, wtp_sm,
				 transaction_belongs_to_session);
	}

	if (sm == NULL) {
		sm = wsp_machine_create();
		sm->client_address = octstr_duplicate(wtp_sm->source_address);
		sm->client_port = wtp_sm->source_port;
		sm->server_address = 
			octstr_duplicate(wtp_sm->destination_address);
		sm->server_port = wtp_sm->destination_port;
	}

	wsp_handle_event(sm, event);
}


WSPMachine *wsp_machine_create(void) {
	WSPMachine *p;
	
	p = gw_malloc(sizeof(WSPMachine));
	
	#define MUTEX(name) p->name = mutex_create();
	#define INTEGER(name) p->name = 0;
	#define OCTSTR(name) p->name = NULL;
	#define METHOD_POINTER(name) p->name = NULL;
	#define EVENT_POINTER(name) p->name = NULL;
	#define SESSION_POINTER(name) p->name = NULL;
	#define HTTPHEADER(name) p->name = NULL;
	#define LIST(name) p->name = list_create();
	#define SESSION_MACHINE(fields) fields
	#define METHOD_MACHINE(fields)
	#include "wsp_machine-decl.h"
	
	p->state = NULL_STATE;

	/* set capabilities to default values (defined in 1.1) */

	p->client_SDU_size = 1400;
	p->server_SDU_size = 1400;
        /* p->protocol_options = 0x00;	 */
	p->MOR_method = 1;
	p->MOR_push = 1;
	
	list_append(session_machines, p);

	return p;
}


void wsp_machine_mark_unused(WSPMachine *p) {
	p->unused = 1;
}


void wsp_machine_destroy(WSPMachine *p) {
	list_delete_equal(session_machines, p);
	while (list_len(p->event_queue) > 0)
		wsp_event_destroy(list_extract_first(p->event_queue));
	#define MUTEX(name) mutex_destroy(p->name);
	#define INTEGER(name) p->name = 0;
	#define OCTSTR(name) octstr_destroy(p->name);
	#define METHOD_POINTER(name) p->name = NULL;
	#define EVENT_POINTER(name) p->name = NULL;
	#define SESSION_POINTER(name) p->name = NULL;
	#define HTTPHEADER(name) http2_destroy_headers(p->name);
	#define LIST(name) list_destroy(p->name);
	#define SESSION_MACHINE(fields) fields
	#define METHOD_MACHINE(fields)
	#include "wsp_machine-decl.h"
	gw_free(p);
}


void wsp_machine_dump(WSPMachine *machine) {
	WSPMachine *p;

	p = machine;
	debug("wap.wsp", 0, "WSPMachine %p dump starts:", (void *) p);
	#define MUTEX(name) 
	#define INTEGER(name) debug("wap.wsp", 0, "  %s: %ld", #name, p->name);
	#define OCTSTR(name) \
		debug("wap.wsp", 0, "  %s:", #name); \
		octstr_dump(p->name, 1);
	#define METHOD_POINTER(name) \
		debug("wap.wsp", 0, "  %s: %p", #name, (void *) p->name);
	#define EVENT_POINTER(name) \
		debug("wap.wsp", 0, "  %s: %p", #name, (void *) p->name);
	#define SESSION_POINTER(name) \
		debug("wap.wsp", 0, "  %s: %p", #name, (void *) p->name);
	#define HTTPHEADER(name) \
		debug("wap.wsp", 0, "  %s: %p", #name, (void *) p->name);
	#define LIST(name) \
		debug("wap.wsp", 0, "  %s: %p", #name, (void *) p->name);
	#define SESSION_MACHINE(fields) fields
	#define METHOD_MACHINE(fields)
	#include "wsp_machine-decl.h"
	debug("wap.wsp", 0, "WSPMachine dump ends.");
}


void wsp_handle_event(WSPMachine *sm, WSPEvent *current_event) {
	/* 
	 * If we're already handling events for this machine, add the
	 * event to the queue.
	 */
	if (mutex_try_lock(sm->mutex) == -1) {
		append_to_event_queue(sm, current_event);
		return;
	}

	do {
		debug("wap.wsp", 0, "WSP: machine %p, state %s, event %s",
			(void *) sm,
			wsp_state_to_string(sm->state), 
			wsp_event_name(current_event->type));

		#define STATE_NAME(name)
		#define ROW(state_name, event, condition, action, next_state) \
			{ \
				struct event *e; \
				e = &current_event->event; \
				if (sm->state == state_name && \
				   current_event->type == event && \
				   (condition)) { \
					action \
					sm->state = next_state; \
					goto end; \
				} \
			}
		#include "wsp_state-decl.h"
		
		if (current_event->type == TR_Invoke_Ind) {
			WTPEvent *abort;
			
			error(0, "WSP: Can't handle TR-Invoke.ind, aborting transaction.");
			abort = wtp_event_create(TR_Abort_Req);
			abort->TR_Abort_Req.tid = 
				current_event->TR_Invoke_Ind.machine->tid;
			abort->TR_Abort_Req.abort_type = 0x01; /* USER */
			abort->TR_Abort_Req.abort_reason = 0xE0; /* PROTOERR */

			wtp_handle_event(current_event->TR_Invoke_Ind.machine,
					 abort);
			wsp_machine_mark_unused(sm);
		} else {
			error(0, "WSP: Can't handle event.");
			debug("wap.wsp", 0, "WSP: The unhandled event:");
			wsp_event_dump(current_event);
		}

	end:
		wsp_event_destroy(current_event);
		current_event = remove_from_event_queue(sm);
	} while (sm != NULL && current_event != NULL);

	if (sm->unused)
		wsp_machine_destroy(sm);
	else
		mutex_unlock(sm->mutex);
}




int wsp_deduce_pdu_type(Octstr *pdu, int connectionless) {
	int off;
	unsigned long o;

	if (connectionless)
		off = 1;
	else
		off = 0;
	if (unpack_uint8(&o, pdu, &off) == -1)
		o = Bad_PDU;
	return o;
}


/***********************************************************************
 * Local functions
 */



static void unpack_caps(Octstr *caps, WSPMachine *m)
{
    int off, flags;
    unsigned long length, uiv, mor, caps_id;
    
    off = 0;
    while (off < octstr_len(caps)) {
	unpack_uintvar(&length, caps, &off);

	/* XXX
	 * capablity identifier is defined as 'multiple octets'
	 * and encoded as Field-Name, but current supported
	 * capabilities can be identified via one number
	 */

	unpack_uintvar(&caps_id, caps, &off);

	switch(caps_id) {
	    
	case WSP_CAPS_CLIENT_SDU_SIZE:
	    if (unpack_uintvar(&uiv, caps, &off) == -1)
		warning(0, "Problems getting client SDU size capability");
	    else {
		if (WSP_MAX_CLIENT_SDU && uiv > WSP_MAX_CLIENT_SDU) {
		    debug("wap.wsp", 0, "Client tried client SDU size %lu larger "
			  "than our max %d", uiv, WSP_MAX_CLIENT_SDU);
		} else if (!(m->set_caps & WSP_CSDU_SET)) {
		    debug("wap.wsp", 0, "Client SDU size negotiated to %lu", uiv);
		    /* Motorola Timeport / Phone.com hack */
		    if (uiv == 3) {
		    	uiv = 1350;
		        debug("wap.wsp", 0, "Client SDU size forced to %lu", uiv);
		    }
		    m->client_SDU_size = uiv;
		    m->set_caps |= WSP_CSDU_SET;
		}
	    }
	    break;
	case WSP_CAPS_SERVER_SDU_SIZE:
	    if (unpack_uintvar(&uiv, caps, &off) == -1)
		warning(0, "Problems getting server SDU size capability");
	    else {
		if (WSP_MAX_SERVER_SDU && uiv > WSP_MAX_SERVER_SDU) {
		    debug("wap.wsp", 0, "Client tried server SDU size %lu larger "
			  "than our max %d", uiv, WSP_MAX_SERVER_SDU);
		} else if (!(m->set_caps & WSP_SSDU_SET)) {
		    debug("wap.wsp", 0, "Server SDU size negotiated to %lu", uiv);
		    m->server_SDU_size = uiv;
		    m->set_caps |= WSP_SSDU_SET;
		}
	    }
	    break;
	case WSP_CAPS_PROTOCOL_OPTIONS:
	    /* XXX should be taken as octstr or something - and
		  * be sure, that there is that information */

	    flags = (octstr_get_char(caps,off));
	    if (!(m->set_caps & WSP_PO_SET)) {

		/* we do not support anything yet, so answer so */

		debug("wap.wsp", 0, "Client protocol option flags %0xd, not supported.", flags);
		     
		m->protocol_options = WSP_MAX_PROTOCOL_OPTIONS;
		m->set_caps |= WSP_PO_SET;
	    }
	    break;
	case WSP_CAPS_METHOD_MOR:
	    if (unpack_uint8(&mor, caps, &off) == -1)
		warning(0, "Problems getting MOR methods capability");
	    else {
		if (mor > WSP_MAX_METHOD_MOR) {
		    debug("wap.wsp", 0, "Client tried method MOR %lu larger "
			  "than our max %d", mor, WSP_MAX_METHOD_MOR);
		} else if (!(m->set_caps & WSP_MMOR_SET)) {
		    debug("wap.wsp", 0, "Method MOR negotiated to %lu", mor);
		    m->MOR_method = mor;
		    m->set_caps |= WSP_MMOR_SET;
		}
	    }
	    break;
	case WSP_CAPS_PUSH_MOR:
	    if (unpack_uint8(&mor, caps, &off) == -1)
		warning(0, "Problems getting MOR push capability");
	    else {
		if (mor > WSP_MAX_PUSH_MOR) {
		    debug("wap.wsp", 0, "Client tried push MOR %lu larger "
			  "than our max %d", mor, WSP_MAX_PUSH_MOR);
		} else if (!(m->set_caps & WSP_PMOR_SET)) {
		    debug("wap.wsp", 0, "Push MOR negotiated to %lu", mor);
		    m->MOR_push = mor;
		    m->set_caps |= WSP_PMOR_SET;
		}
	    }
	    break;
	case WSP_CAPS_EXTENDED_METHODS:
	    debug("wap.wsp", 0, "Extended methods capability ignored");
	    break;
	case WSP_CAPS_HEADER_CODE_PAGES:
	    debug("wap.wsp", 0, "Header code pages capability ignored");
	    break;
	case WSP_CAPS_ALIASES:
	    debug("wap.wsp", 0, "Aliases capability ignored");
	    break;
	default:
	    /* unassigned */
	    debug("wap.wsp", 0, "Unknown capability '%d' ignored",
		  octstr_get_char(caps,off-1));
	    break;
	}
    }
}

static void append_to_event_queue(WSPMachine *machine, WSPEvent *event) {
	list_append(machine->event_queue, event);
}

static WSPEvent *remove_from_event_queue(WSPMachine *machine) {
	return list_extract_first(machine->event_queue);
}


static int unpack_connect_pdu(WSPMachine *m, Octstr *user_data) {
	int off;
	unsigned long version, caps_len, headers_len;
	Octstr *caps, *headers;

	off = 1;	/* ignore PDU type */
	if (unpack_uint8(&version, user_data, &off) == -1 ||
	    unpack_uintvar(&caps_len, user_data, &off) == -1 ||
	    unpack_uintvar(&headers_len, user_data, &off) == -1 ||
	    unpack_octstr(&caps, caps_len, user_data, &off) == -1 ||
	    unpack_octstr(&headers, headers_len, user_data, &off) == -1)
		return -1;

#if 0
	debug("wap.wsp", 0, "Unpacked Connect PDU: version=%lu, caps_len=%lu, hdrs_len=%lu",
	      version, caps_len, headers_len);
#endif
	if (caps_len > 0) {
	    unpack_caps(caps, m);
	}
	if (headers_len > 0) {
	    List *hdrs;
	    
	    hdrs = unpack_headers(headers);

	    /* pack them for more compact form */
	    http2_header_pack(hdrs);

	    m->http_headers = hdrs;
	}

        octstr_destroy(caps);
	octstr_destroy(headers);
	return 0;
}


static int unpack_get_pdu(Octstr **url, List **headers, Octstr *pdu) {
	unsigned long url_len;
	int off;
	Octstr *h;

	off = 1; /* Offset 0 has type octet. */
	if (unpack_uintvar(&url_len, pdu, &off) == -1 ||
	    unpack_octstr(url, url_len, pdu, &off) == -1)
		return -1;
	if (off < octstr_len(pdu)) {
		h = octstr_copy(pdu, off, octstr_len(pdu) - off);
		*headers = unpack_headers(h);
		octstr_destroy(h);
	} else
		*headers = NULL;
	return 0;
}


static int unpack_post_pdu(Octstr **url, Octstr **headers, Octstr *pdu) {
	unsigned long url_len;
	unsigned long param_len;
	Octstr 		*param;
	Octstr 		*head;
	int off;

	off = 1; /* Offset 0 has type octet. */
	/* 
		0x60 : Post
		u8	: URL len
		u8 	: Header Len
		URL	:
		Vars	:

	*/
	if (unpack_uintvar(&url_len, pdu, &off) == -1)
	{
		return -1;
	}
	 if( unpack_uintvar(&param_len,pdu,&off) == -1)
	{
		return -1;
	}
	if(  unpack_octstr(url, url_len, pdu, &off) == -1)
	{
		return -1;
	}
	debug("wap.wsp", 0, "WSP: Post PDU had URL <%s>", octstr_get_cstr(*url));

	if(unpack_octstr(&head,param_len,pdu,&off)==-1)
	{
		return -1;
	}
	debug("wap.wsp", 0, "WSP: Got headers. <%d> Total len <%d> offset",(int)octstr_len(pdu),off);

	if(unpack_octstr(&param,octstr_len(pdu)-off,pdu,&off)==-1)
	{
		return -1;
	}

/*
	if (off < octstr_len(pdu))
		error(0, "unpack_post_pdu: Post PDU has headers, ignored them");
*/
		
	*headers = NULL;

	debug("wap.wsp", 0, "WSP: Post PDU had data <%s>", octstr_get_cstr(param));

	octstr_destroy(head);
	/* Now we concatenante the two thingies */
	head=octstr_create("?");
	octstr_insert(*url,head,url_len);	
	octstr_insert(*url,param,octstr_len(*url));	
	octstr_destroy(param);
	octstr_destroy(head);
/*
	octstr_set_char	(*url,url_len,'?');
*/	
	debug("wap.wsp", 0, "WSP: Final URL is  <%s>", octstr_get_cstr(*url));
	debug("wap.wsp", 0, "WSP: URL unpacked");
	return 0;
}


static int unpack_uint8(unsigned long *u, Octstr *os, int *off) {
	if (*off >= octstr_len(os)) {
		error(0, "WSP: Trying to unpack uint8 past PDU");
		return -1;
	}
	*u = octstr_get_char(os, *off);
	++(*off);
	return 0;
}


static int unpack_uintvar(unsigned long *u, Octstr *os, int *off) {
	unsigned long o;
	
	*u = 0;
	do {
		if (unpack_uint8(&o, os, off) == -1) {
			error(0, "WSP: unpack_uint failed in unpack_uintvar");
			return -1;
		}
		*u = ((*u) << 7) | (o & 0x7F);
	} while ((o & 0x80) != 0);

	return 0;
}


static int unpack_octstr(Octstr **ret, int len, Octstr *os, int *off) {
	if (*off + len > octstr_len(os)) {
		error(0, "WSP: Trying to unpack string past PDU");
		return -1;
	}
	*ret = octstr_copy(os, *off, len);
	*off += len;
	return 0;
}


static char *wsp_state_to_string(WSPState state) {
	switch (state) {
	#define STATE_NAME(name) case name: return #name;
	#define ROW(state, event, cond, stmt, next_state)
	#include "wsp_state-decl.h"
	default:
		return "unknown wsp state";
	}
}


/* XXX this function is not thread safe. --liw 
 *     it is nowadays? --rpr */
static long wsp_next_session_id(void) {
	return counter_increase(session_id_counter);
}


static Octstr *make_connectionmode_pdu(long type) {
	Octstr *pdu;
	
	pdu = octstr_create_empty();
	append_uint8(pdu, type);
	return pdu;
}


static void append_uint8(Octstr *pdu, long n) {
	unsigned char c;
	
	c = (unsigned char) n;
	octstr_insert_data(pdu, octstr_len(pdu), &c, 1);
}


static void append_uintvar(Octstr *pdu, long n) {
	long bytes[5];
	unsigned long u;
	int i;
	
	u = n;
	for (i = 4; i >= 0; --i) {
		bytes[i] = u & 0x7F;
		u >>= 7;
	}
	for (i = 0; i < 4 && bytes[i] == 0; ++i)
		continue;
	for (; i < 4; ++i)
		append_uint8(pdu, 0x80 | bytes[i]);
	append_uint8(pdu, bytes[4]);
}


static void append_octstr(Octstr *pdu, Octstr *os) {
	octstr_insert(pdu, os, octstr_len(pdu));
}


static Octstr *make_connectreply_pdu(WSPMachine *m, long session_id) {
	Octstr *pdu, *caps = NULL, *hdrs = NULL, *tmp;
	
	pdu = make_connectionmode_pdu(ConnectReply_PDU);
	append_uintvar(pdu, session_id);
	/* set CapabilitiesLen */
	if (m->set_caps) {
	    caps = octstr_create_empty();
	    tmp = octstr_create_empty();

	    /* XXX put negotiated capabilities into octstr */

	    if (m->set_caps & WSP_CSDU_SET) {
		octstr_truncate(tmp, 0);
		append_uint8(tmp, WSP_CAPS_SERVER_SDU_SIZE);
		append_uintvar(tmp, m->client_SDU_size);

		append_uintvar(caps, octstr_len(tmp));
		append_octstr(caps, tmp);
	    }
	    if (m->set_caps & WSP_SSDU_SET) {
		octstr_truncate(tmp, 0);
		append_uint8(tmp, WSP_CAPS_SERVER_SDU_SIZE);
		append_uintvar(tmp, m->server_SDU_size);

		append_uintvar(caps, octstr_len(tmp));
		append_octstr(caps, tmp);
	    }

	    if (m->set_caps & WSP_MMOR_SET) {
		append_uintvar(caps, 2);
		append_uint8(caps, WSP_CAPS_METHOD_MOR);
		append_uint8(caps, m->MOR_method);
	    }
	    if (m->set_caps & WSP_PMOR_SET) {
		append_uintvar(caps, 2);
		append_uint8(caps, WSP_CAPS_PUSH_MOR);
		append_uint8(caps, m->MOR_push);
	    }
	    /* rest are not supported, yet */
	    
	    append_uintvar(pdu, octstr_len(caps));
	    octstr_destroy(tmp);
	} else
	    append_uintvar(pdu, 0);

	/* set HeadersLen */
	append_uintvar(pdu, 0);

	if (caps != NULL) {
	    append_octstr(pdu, caps);
	    octstr_destroy(caps);
	}
	if (hdrs != NULL) {
	    append_octstr(pdu, hdrs);
	    octstr_destroy(hdrs);
	}
	return pdu;
}


static Octstr *make_reply_pdu(long status, long type, Octstr *body) {
	Octstr *pdu;
	
	/* XXX this is a hardcoded kludge */
	pdu = make_connectionmode_pdu(Reply_PDU);
	append_uint8(pdu, convert_http_status_to_wsp_status(status));
	append_uintvar(pdu, 1);
	gw_assert(type >= 0x00);
	gw_assert(type < 0x80);
	append_uint8(pdu, type | 0x80);
	if (body != NULL)
		append_octstr(pdu, body);
	return pdu;
}


static long convert_http_status_to_wsp_status(long http_status) {
	static struct {
		long http_status;
		long wsp_status;
	} tab[] = {
		{ 200, 0x20 },
		{ 413, 0x4D },
		{ 415, 0x4F },
		{ 500, 0x60 },
	};
	int num_items = sizeof(tab) / sizeof(tab[0]);
	int i;
	
	for (i = 0; i < num_items; ++i)
		if (tab[i].http_status == http_status)
			return tab[i].wsp_status;
	error(0, "WSP: Unknown status code used internally. Oops.");
	return 0x60; /* Status 500, or "Internal Server Error" */
}


/* XXX this function is not thread safe. --liw */
static long new_server_transaction_id(void) {
	static long next_id = 1;
	return next_id++;
}


static int transaction_belongs_to_session(void *wsp_ptr, void *wtp_ptr) {
	WSPMachine *wsp;
	WTPMachine *wtp;
	
	wsp = wsp_ptr;
	wtp = wtp_ptr;

	return
	  !wsp->unused &&
	  wtp->in_use &&
	  wtp->source_port == wsp->client_port &&
	  wtp->destination_port == wsp->server_port &&
	  octstr_compare(wtp->source_address, wsp->client_address) == 0 &&
	  octstr_compare(wtp->destination_address, wsp->server_address) == 0;
}



static int same_client(void *a, void *b) {
	WSPMachine *sm1, *sm2;
	
	sm1 = a;
	sm2 = b;
	return !sm1->unused &&
	       !sm2->unused && 
	       sm1->client_port == sm2->client_port &&
	       sm1->server_port == sm2->server_port &&
	       octstr_compare(sm1->client_address, sm2->client_address) == 0 &&
	       octstr_compare(sm1->server_address, sm2->server_address) == 0;
}
