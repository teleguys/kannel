/*
 * Push PPG implementation. This module implements the general logic of a push
 * proxy gateway.
 *
 * By Aarno Syv�nen for Wapit Ltd.
 */

#include "wap_push_ppg.h"
#include "gwlib/gwlib.h"
#include "wap/wap_events.h"

/*****************************************************************************
 *
 * Internal data structures
 */

/*
 * Give the status of the push ppg module:
 *
 *	limbo
 *		not running at all
 *	running
 *		operating normally
 *	terminating
 *		waiting for operations to terminate, returning to limbo
 */
static enum {limbo, running, terminating} run_status = limbo;

/*
 * The event queue for this module
 */
static List *ppg_queue = NULL;

wap_dispatch_func_t *dispatch_to_ota;

/*****************************************************************************
 *
 * Prototypes of internal functions
 */

static void main_thread(void *arg);
static void handle_ppg_event(WAPEvent *e);

/*****************************************************************************
 *
 * EXTERNAL FUNCTIONS
 */

void wap_push_ppg_init(wap_dispatch_func_t *ota_dispatch)
{
    ppg_queue = list_create();
    list_add_producer(ppg_queue);

    dispatch_to_ota = ota_dispatch;

    gw_assert(run_status == limbo);
    run_status = running;
    gwthread_create(main_thread, NULL);
}

void wap_push_ppg_shutdown(void)
{
     gw_assert(run_status == running);
     run_status = terminating;
     list_remove_producer(ppg_queue);
     
     gwthread_join_every(main_thread);
     list_destroy(ppg_queue, wap_event_destroy_item);

}

void wap_push_ppg_dispatch_event(WAPEvent *e)
{
    list_produce(ppg_queue, e);
}


/*****************************************************************************
 *
 * INTERNAL FUNCTIONS
 */

static void main_thread(void *arg)
{
    WAPEvent *e;

    while (run_status == running && (e = list_consume(ppg_queue)) != NULL) {
        handle_ppg_event(e);
    } 

}

static void handle_ppg_event(WAPEvent *e)
{
    switch(e->type) {
    case Pom_Connect_Ind:
    break;

    case Pom_Disconnect_Ind:
    break;

    case Po_ConfirmedPush_Cnf:
    break;

    case Po_PushAbort_Ind:
    break;

    default:
        debug("wap.ppg", 0, "An unhandled event in ppg module");
        wap_event_dump(e);
    break;
    }

    wap_event_destroy(e);
}
