/*
 * radius_acct.c - RADIUS accounting proxy thread
 *
 * Stipe Tolj <tolj@wapme-systems.de>
 */

#include <string.h>

#include "gwlib/gwlib.h"
#include "radius/radius_pdu.h"

static Dict *radius_table = NULL;      /* maps client ip -> msisdn */
static Dict *session_table = NULL;     /* maps session id -> client ip */
static Dict *client_table = NULL;      /* maps client ip -> session id */

/* we will initialize hash tables in the size of our NAS ports */
#define RADIUS_NAS_PORTS    30

static Mutex *radius_mutex;
static int run_thread = 0;

/* 
 * Beware that the official UDP port for RADIUS accounting packets 
 * is 1813 (according to RFC2866). The previously used port 1646 has
 * been conflicting with an other protocol and "should" not be used.
 */
static Octstr *our_host = NULL;
static long our_port = 1813;
static Octstr *remote_host = NULL;
static long remote_port = 1813;

/* the shared secrets for NAS and remote RADIUS communication */
static Octstr *secret_nas = NULL;
static Octstr *secret_radius = NULL;

/* the global unified-prefix list */
static Octstr *unified_prefix = NULL;

/*************************************************************************
 *
 */

/*
 * Updates the internal RADIUS mapping tables. Returns 1 if the 
 * mapping has been processes and the PDU should be proxied to the
 * remote RADIUS server, otherwise if it is a duplicate returns 0.
 */
static int update_tables(RADIUS_PDU *pdu)
{
    Octstr *client_ip, *msisdn;
    Octstr *type, *session_id;
    int ret = 0;
    
    client_ip = msisdn = type = session_id = NULL;

    /* only add if we have a Accounting-Request PDU */
    if (pdu->type == 0x04) {

        /* check if we have a START or STOP event */
        type = dict_get(pdu->attr, octstr_imm("Acct-Status-Type"));
        
        /* get the sesion id */
        session_id = dict_get(pdu->attr, octstr_imm("Acct-Session-Id"));

        /* grep the needed data */
        client_ip = dict_get(pdu->attr, octstr_imm("Framed-IP-Address"));
        msisdn = dict_get(pdu->attr, octstr_imm("Calling-Station-Id"));

        /* we can't add mapping without both components */
        if (client_ip == NULL || msisdn == NULL) {
            warning(0, "RADIUS: NAS did either not send 'Framed-IP-Address' or/and "
                       "'Calling-Station-Id', dropping mapping but will forward.");
            /* anyway forward the packet to remote RADIUS server */
            return 1;
        }

        if (octstr_compare(type, octstr_imm("1")) == 0 && session_id && msisdn) {
            /* session START */
            if (dict_get(radius_table, client_ip) == NULL && 
                dict_get(session_table, session_id) == NULL) {
                Octstr *put_msisdn = octstr_duplicate(msisdn);
                Octstr *put_client_ip = octstr_duplicate(client_ip);
                Octstr *put_session_id = octstr_duplicate(session_id);
                Octstr *old_session_id, *old_client_ip;

                /* ok, this is a new session. If it contains an IP that is still
                 * in the session/client tables then remove the old session from the
                 * two tables session/client */
                if ((old_session_id = dict_get(client_table, client_ip)) != NULL &&
                    (old_client_ip = dict_get(session_table, old_session_id)) != NULL &&
                    octstr_compare(old_session_id, session_id) != 0) {
                    dict_remove(client_table, client_ip);
                    dict_remove(session_table, old_session_id);
                    octstr_destroy(old_session_id);
                    octstr_destroy(old_client_ip);
                }

                /* insert both, new client IP and session to mapping tables */
                dict_put(radius_table, client_ip, put_msisdn); 
                dict_put(session_table, session_id, put_client_ip);
                dict_put(client_table, client_ip, put_session_id);

                info(0, "RADIUS: Mapping `%s <-> %s' for session id <%s> added.",
                     octstr_get_cstr(client_ip), octstr_get_cstr(msisdn),
                     octstr_get_cstr(session_id));
                ret = 1;
            } else {
                warning(0, "RADIUS: Duplicate mapping `%s <-> %s' for session "
                        "id <%s> received, ignoring.",
                        octstr_get_cstr(client_ip), octstr_get_cstr(msisdn),
                        octstr_get_cstr(session_id));
            }
        }
        else if (octstr_compare(type, octstr_imm("2")) == 0) {
            /* session STOP */
            Octstr *comp_client_ip;
            if ((msisdn = dict_get(radius_table, client_ip)) != NULL &&
                (comp_client_ip = dict_get(session_table, session_id)) != NULL &&
                octstr_compare(client_ip, comp_client_ip) == 0) {
                dict_remove(radius_table, client_ip);
                info(0, "RADIUS: Mapping `%s <-> %s' for session id <%s> removed.",
                     octstr_get_cstr(client_ip), octstr_get_cstr(msisdn),
                     octstr_get_cstr(session_id));
                octstr_destroy(msisdn);
                ret = 1;
            } else {
                warning(0, "RADIUS: Could not find mapping for `%s' session "
                        "id <%s>, ignoring.",
                        octstr_get_cstr(client_ip), octstr_get_cstr(session_id));
            }
                
        }
        else {
            error(0, "RADIUS: unknown Acct-Status-Type `%s' received, ignoring.", 
                  octstr_get_cstr(type));
        }
    }

    return ret;
}


/*************************************************************************
 * The main proxy thread.
 */

static void proxy_thread(void *arg) 
{
    int ss, cs; /* server and client sockets */
	Octstr *addr;
    int forward;

    run_thread = 1;

   	/* create client binding */
	cs = udp_client_socket();
	addr = udp_create_address(remote_host, remote_port);

    /* create server binding */
	ss = udp_bind(our_port, octstr_get_cstr(our_host));
	if (ss == -1)
		panic(0, "RADIUS: Couldn't set up server socket for port %ld.", our_port);

	while (run_thread) {
        RADIUS_PDU *pdu, *r;
        Octstr *data, *rdata;
        Octstr *from_nas, *from_radius;

        /* get request from NAS */
		if (udp_recvfrom(ss, &data, &from_nas) == -1) {
            error(0, "RADIUS: Couldn't receive request data from NAS");
            continue;
        }
		info(0, "RADIUS: Got data from NAS <%s:%d>", 
             octstr_get_cstr(udp_get_ip(from_nas)), udp_get_port(from_nas));
        octstr_dump(data, 0);

        /* unpacking the RADIUS PDU */
        pdu = radius_pdu_unpack(data);
        info(0, "RADIUS PDU type: %s", pdu->type_name);

        /* FIXME: XXX authenticator md5 check does not work?! */
        //radius_authenticate_pdu(pdu, data, secret_nas); 

        /* store to hash table if not present yet */
        mutex_lock(radius_mutex);
        forward = update_tables(pdu);
        mutex_unlock(radius_mutex);

        /* create response PDU for NAS */
        r = radius_pdu_create(0x05, pdu);

        /* 
         * create response authenticator 
         * code+identifier(req)+length+authenticator(req)+(attributes)+secret 
         */
        r->u.Accounting_Response.identifier = pdu->u.Accounting_Request.identifier;
        r->u.Accounting_Response.authenticator = 
            octstr_duplicate(pdu->u.Accounting_Request.authenticator);

        /* pack response for NAS */
        rdata = radius_pdu_pack(r);

        /* creates response autenticator in encoded PDU */
        radius_authenticate_pdu(r, &rdata, secret_nas);

        /* forward request to remote RADIUS server only if updated */
        if (forward) {
            if (udp_sendto(cs, data, addr) == -1) {
                error(0, "RADIUS: Couldn't send to remote RADIUS <%s:%ld>.",
                      octstr_get_cstr(remote_host), remote_port);
            }
            else if (udp_recvfrom(cs, &data, &from_radius) == -1) {
                error(0, "RADIUS: Couldn't receive from remote RADIUS <%s:%ld>.",
                      octstr_get_cstr(remote_host), remote_port);
            }
            else {
                info(0, "RADIUS: Got data from remote RADIUS <%s:%d>", 
                     octstr_get_cstr(udp_get_ip(from_radius)), udp_get_port(from_radius));
                octstr_dump(data, 0);

                /* XXX unpack the response PDU and check if the response 
                 * authenticator is valid */
            }
        }

        /* send response to NAS */
        if (udp_sendto(ss, rdata, from_nas) == -1)
			error(0, "RADIUS: Couldn't send response data to NAS <%s:%d>.",
                     octstr_get_cstr(udp_get_ip(from_nas)), udp_get_port(from_nas));

        radius_pdu_destroy(pdu);
        radius_pdu_destroy(r);

        octstr_destroy(rdata);
        octstr_destroy(data);

        debug("radius.proxy",0,"RADIUS: Mapping table contains %ld elements", 
              dict_key_count(radius_table)); 
        debug("radius.proxy",0,"RADIUS: Session table contains %ld elements", 
              dict_key_count(session_table)); 
        debug("radius.proxy",0,"RADIUS: Client table contains %ld elements", 
              dict_key_count(client_table)); 
	}

    octstr_destroy(addr);
}


/*************************************************************************
 * Public functions: init, shutdown, mapping.
 */

Octstr *radius_acct_get_msisdn(Octstr *client_ip)
{
    Octstr *m, *r;
    char *uf;

    /* if no proxy thread is running, then pass NULL as result */
    if (radius_table == NULL || client_ip == NULL)
        return NULL;

    mutex_lock(radius_mutex);
    m = dict_get(radius_table, client_ip);
    mutex_unlock(radius_mutex);
    r = m ? octstr_duplicate(m) : NULL;

    /* apply number normalization */
    uf = unified_prefix ? octstr_get_cstr(unified_prefix) : NULL;
    normalize_number(uf, &r);

    return r;
}
 
void radius_acct_init(CfgGroup *grp) 
{
    unsigned long nas_ports = 0;

    /* get configured parameters */
    if ((our_host = cfg_get(grp, octstr_imm("our-host"))) == NULL) {
        our_host = octstr_create("0.0.0.0");
    }
    if ((remote_host = cfg_get(grp, octstr_imm("remote-host"))) == NULL) {
        remote_host = octstr_create("localhost");
    }
    cfg_get_integer(&our_port, grp, octstr_imm("our-port"));
    cfg_get_integer(&remote_port, grp, octstr_imm("remote-port"));

    if ((cfg_get_integer(&nas_ports, grp, octstr_imm("nas-ports"))) == -1) {
        nas_ports = RADIUS_NAS_PORTS;
    }

    if ((secret_nas = cfg_get(grp, octstr_imm("secret-nas"))) == NULL) {
        panic(0, "RADIUS: No shared secret `secret-nas' for NAS in `radius-acct' provided.");
    }
    if ((secret_radius = cfg_get(grp, octstr_imm("secret-radius"))) == NULL) {
        panic(0, "RADIUS: No shared secret `secret-radius' for remote RADIUS in `radius-acct' provided.");
    }

    unified_prefix = cfg_get(grp, octstr_imm("unified-prefix"));

    info(0, "RADIUS: local RADIUS accounting proxy at <%s:%ld>",
         octstr_get_cstr(our_host), our_port);
    info(0, "RADIUS: remote RADIUS accounting server at <%s:%ld>",
         octstr_get_cstr(remote_host), remote_port);
    info(0, "RADIUS: initializing internal hash tables with %ld buckets.", nas_ports);

    radius_mutex = mutex_create();

    /* init hash tables */
    radius_table = dict_create(nas_ports, (void (*)(void *))octstr_destroy);
    session_table = dict_create(nas_ports, (void (*)(void *))octstr_destroy);
    client_table = dict_create(nas_ports, (void (*)(void *))octstr_destroy);

    gwthread_create(proxy_thread, NULL);
}

void radius_acct_shutdown(void) 
{
    mutex_lock(radius_mutex);
    run_thread = 0;
    mutex_unlock(radius_mutex);
    
    gwthread_join_every(proxy_thread);
    
    dict_destroy(radius_table);
    dict_destroy(session_table);
    dict_destroy(client_table);

    mutex_destroy(radius_mutex);

    octstr_destroy(our_host);
    octstr_destroy(remote_host);
    octstr_destroy(secret_nas);
    octstr_destroy(secret_radius);
    octstr_destroy(unified_prefix);

    info(0, "RADIUS: accounting proxy stopped.");
}
