/*
 * bearerbox.h
 *
 * General typedefs and functions for bearerbox
 */

#include "gwlib/gwlib.h"
#include "msg.h"

/* general bearerbox state */

enum {
    BB_RUNNING = 0,
    BB_ISOLATED = 1,	/* do not receive new messgaes from UDP/SMSC */
    BB_SUSPENDED = 2,	/* do not transfer any messages */
    BB_SHUTDOWN = 3,
    BB_DEAD = 4,
    BB_FULL = 5         /* message queue to long, do not accept ne messages */
};


/* type of output given by various status functions */
enum {
    BBSTATUS_HTML = 0,
    BBSTATUS_TEXT = 1,
    BBSTATUS_WML = 2,
    BBSTATUS_XML = 3
};

/*---------------------------------------------------------------
 * Module interface to core bearerbox
 *
 * Modules implement one or more of the following interfaces:
 *
 * XXX_start(Cfg *config) - start the module
 * XXX_restart(Cfg *config) - restart the module, according to new config
 * XXX_shutdown() - start the avalanche - started from UDP/SMSC
 * XXX_die() - final cleanup
 *
 * XXX_addwdp() - only for SMSC/UDP: add a new WDP message to outgoing system
 */


/*---------------
 * bb_boxc.c (SMS and WAPBOX connections)
 */

int smsbox_start(Cfg *config);
int smsbox_restart(Cfg *config);

int wapbox_start(Cfg *config);

Octstr *boxc_status(int status_type);
/* tell total number of messages in seperate wapbox incoming queues */
int boxc_incoming_wdp_queue(void);

/* Clean up after box connections have died. */
void boxc_cleanup(void);

/*---------------
 * bb_udp.c (UDP receiver/sender)
 */

int udp_start(Cfg *config);
/* int udp_restart(Cfg *config); */
int udp_shutdown(void);
int udp_die(void);	/* called when router dies */

/* add outgoing WDP. If fails, return -1 and msg is untouched, so
 * caller must think of new uses for it */
int udp_addwdp(Msg *msg);
/* tell total number of messages in seperate UDP outgoing port queues */
int udp_outgoing_queue(void);



/*---------------
 * bb_smscconn.c (SMS Center connections)
 */

int smsc2_start(Cfg *config);
int smsc2_restart(Cfg *config);

void smsc2_suspend(void);    /* suspend (can still send but not receive) */
void smsc2_resume(void);     /* resume */
int smsc2_shutdown(void);
void smsc2_cleanup(void); /* final clean-up */

Octstr *smsc2_status(int status_type);
/* Route message to SMSC. If finds a good one, puts into it and returns 1
 * If finds only bad ones, but acceptable, queues and returns 0
 * (like all acceptable currently disconnected)
 * If cannot find nothing at all, returns -1 and message is NOT destroyed
 * (otherwise it is) */
int smsc2_rout(Msg *msg);

int smsc2_stop_smsc(Octstr *id);   /* shutdown a specific smsc */
int smsc2_restart_smsc(Octstr *id);  /* re-start a specific smsc */


/*---------------
 * bb_http.c (HTTP Admin)
 */

int httpadmin_start(Cfg *config);
/* int http_restart(Cfg *config); */
void httpadmin_stop(void);


/*-----------------
 * bb_store.c (SMS storing/retrieval functions)
 */

/* return number of SMS messages in current store (file) */
long store_messages(void);

/* assign ID and save given message to store. Return -1 if save failed */
int store_save(Msg *msg);

/* load store from file; delete any messages that have been relayed,
 * and create a new store file from remaining. Calling this function
 * might take a while, depending on store size
 * Return -1 if something fails (bb can then PANIC normally)
 */
int store_load(void);

/* dump currently non-acknowledged messages into file. This is done
 * automatically now and then, but can be forced. Return -1 if file
 * problems
 */
int store_dump(void);

/* initialize system. Return -1 if fname is baad (too long), otherwise
 * load data from disk */
int store_init(Octstr *fname);

/* init shutdown (system dies when all acks have been processed) */
void store_shutdown(void);

/* return all containing messages in the current store */
Octstr *store_status(int status_type);

/*----------------------------------------------------------------
 * Core bearerbox public functions;
 * used only via HTTP adminstration
 */

int bb_shutdown(void);
int bb_isolate(void);
int bb_suspend(void);
int bb_resume(void);
int bb_restart(void);
int bb_flush_dlr(void);
int bb_stop_smsc(Octstr *id);
int bb_restart_smsc(Octstr *id);

/* return string of current status */
Octstr *bb_print_status(int status_type);


/*----------------------------------------------------------------
 * common function to all (in bearerbox.c)
 */

/* return linebreak for given output format, or NULL if format
 * not supported */
char *bb_status_linebreak(int status_type);

