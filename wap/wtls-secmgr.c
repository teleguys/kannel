/*
 * gw/wtls-secmgr.c - wapbox wtls security manager
 *
 * The security manager's interface consists of two functions:
 *
 *      wtls_secmgr_start()
 *              This starts the security manager thread.
 *
 *      wtls_secmgr_dispatch(event)
 *              This adds a new event to the security manager's event
 *              queue.
 *
 * The wtls security manager is a thread that reads events from its event
 * queue, and feeds back events to the WTLS layer. Here is where various
 * approvals or rejections are made to requested security settings.
 *
 */

#if (HAVE_WTLS_OPENSSL)

#include <string.h>

#include "gwlib/gwlib.h"
#include "wtls.h"

/*
 * Give the status the module:
 *
 *      limbo
 *              not running at all
 *      running
 *              operating normally
 *      terminating
 *              waiting for operations to terminate, returning to limbo
 */
static enum { limbo, running, terminating } run_status = limbo;


/*
 * The queue of incoming events.
 */
static List *secmgr_queue = NULL;

/*
 * Private functions.
 */

static void main_thread(void *);

/***********************************************************************
 * The public interface to the application layer.
 */

void wtls_secmgr_init(void) {
        gw_assert(run_status == limbo);
        secmgr_queue = list_create();
        list_add_producer(secmgr_queue);
        run_status = running;
        gwthread_create(main_thread, NULL);
}


void wtls_secmgr_shutdown(void) {
        gw_assert(run_status == running);
        list_remove_producer(secmgr_queue);
        run_status = terminating;
        
        gwthread_join_every(main_thread);
        
        list_destroy(secmgr_queue, wap_event_destroy_item);
}


void wtls_secmgr_dispatch(WAPEvent *event) {
        gw_assert(run_status == running);
        list_produce(secmgr_queue, event);
}


long wtls_secmgr_get_load(void) {
        gw_assert(run_status == running);
        return list_len(secmgr_queue);
}


/***********************************************************************
 * Private functions.
 */


static void main_thread(void *arg) {
        WAPEvent *ind, *res, *req, *term;
        
        while (run_status == running && (ind = list_consume(secmgr_queue)) != NULL) {
                switch (ind->type) {
                case SEC_Create_Ind:
                        /* Process the cipherlist */
                        /* Process the MAClist */
                        /* Process the PKIlist */
                        /* Dispatch a SEC_Create_Res */
                        res = wap_event_create(SEC_Create_Res);
                        res->u.SEC_Create_Res.addr_tuple =
                                wap_addr_tuple_duplicate(ind->u.SEC_Create_Ind.addr_tuple);
                        wtls_dispatch_event(res);
                        debug("wtls_secmgr : main_thread", 0,"Dispatching SEC_Create_Res event");
                        /* Dispatch a SEC_Exchange_Req or maybe a SEC_Commit_Req */
                        req = wap_event_create(SEC_Exchange_Req);
                        req->u.SEC_Exchange_Req.addr_tuple =
                                wap_addr_tuple_duplicate(ind->u.SEC_Create_Ind.addr_tuple);
                        wtls_dispatch_event(req);
                        debug("wtls_secmgr : main_thread", 0,"Dispatching SEC_Exchange_Req event");
                        wap_event_destroy(ind);
                        break;
                case SEC_Terminate_Req:
                        /* Dispatch a SEC_Terminate_Req */
                        term = wap_event_create(SEC_Terminate_Req);
                        term->u.SEC_Terminate_Req.addr_tuple =
                                wap_addr_tuple_duplicate(ind->u.SEC_Create_Ind.addr_tuple);
                        term->u.SEC_Terminate_Req.alert_desc = 0;
                        term->u.SEC_Terminate_Req.alert_level = 3;
                       wtls_dispatch_event(term);
                default:
                        panic(0, "WTLS-secmgr: Can't handle %s event",
                              wap_event_name(ind->type));
                        break;
                }
        }
}

#endif
