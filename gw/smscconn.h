#ifndef SMSCCONN_H
#define SMSCCONN_H

/*
 * SMSC Connection
 *
 * Interface for main bearerbox to SMS center connection modules
 *
 * At first stage a simple wrapper for old ugly smsc module
 *
 * Kalle Marjola 2000
 */

#include "gwlib/gwlib.h"
#include "gw/msg.h"

/*
 * Structure hierarchy:
 *
 * Bearerbox keeps list of pointers to SMSCConn structures, which
 * include all data needed by connection, including routing data.
 * If any extra data not linked to that SMSC Connection is needed,
 * that is held in bearerbox 
 *
 * SMSCConn is internal structure for smscconn module. It has a list
 * of common variables like number of sent/received messages and
 * and pointers to appropriate lists, and then it has a void pointer
 * to appropriate smsc structure defined and used by corresponding smsc
 * connection type module (like CIMD2, SMPP etc al)
 *
 * Concurrency notes:
 *
 * bearerbox is responsible for not calling cleanup at the same time
 * as it calls other functions, but must call it after it has noticed that
 * status == KILLED
 */

typedef struct smscconn SMSCConn;

/* create new SMS center connection from given configuration group,
 * or return NULL if failed.
 *
 * The new connection does its work in its own privacy, and calls
 * callback functions at bb_smscconn_cb module. It calls function
 * bb_smscconn_ready when it has put everything up.
 *
 * NOTE: this function starts one or more threads to
 *   handle traffic with SMSC, and caller does not need to
 *   care about it afterwards.
 */
SMSCConn *smscconn_create(ConfigGroup *cfg, int start_as_stopped);

/* shutdown/destroy smscc. Stop receiving messages and accepting
 * new message to-be-sent. Die when any internal queues are empty,
 * if finish_sending != 0, or if set to 0, kill connection ASAP and
 * call send_failed -callback for all messages still in queue
 */
void smscconn_shutdown(SMSCConn *smscconn, int finish_sending);

/* this is final function to cleanup all memory still held by
 * SMSC Connection after it has been killed (for synchronization
 *  problems it cannot be cleaned automatically)
 * Call this after send returns problems or otherwise notice that
 * status is KILLED. Returns 0 if OK, -1 if it cannot be (yet) destroyed.
 */
int smscconn_destroy(SMSCConn *smscconn);

/* stop smscc. A stopped smscc does not receive any messages, but can
 * still send messages, so that internal queue can be emptied. The caller
 * is responsible for not to add new messages into queue if the caller wants
 * the list to empty at some point
 */
int smscconn_stop(SMSCConn *smscconn);

/* start stopped smscc. Return -1 if failed, 0 otherwise */
void smscconn_start(SMSCConn *smscconn);

/* Return name of the SMSC, as reference - caller may not free it! */
Octstr *smscconn_name(SMSCConn *smscconn);

/* Return ID of the SMSC, as reference - caller may not free it! */
Octstr *smscconn_id(SMSCConn *conn);

/* Check if this SMSC Connection is usable as sender for given
 * message. The bearerbox must then select the good SMSC for sending
 * according to load levels and conbnected/disconnected status, this
 * function only checks preferred/denied strings and overall status
 *
 * Return -1 if not (denied or permanently down), 0 if okay,
 * 1 if preferred one.
 */
int smscconn_usable(SMSCConn *conn, Msg *msg);

/* Call SMSC specific function to handle sending of 'msg'
 * Returns immediately, with 0 if successful and -1 if failed.
 * In any case the caller is responsible for 'msg' after that.
 * Note that return value does NOT mean that message has been send
 * or send has failed, but SMSC Connection calls appropriate callback
 * function later
 */
int smscconn_send(SMSCConn *smsccconn, Msg *msg);

/* Return just status as defined below */
int smscconn_status(SMSCConn *smscconn);

typedef struct smsc_state {
    int	status;		/* see enumeration, below */
    int killed;		/* if we are killed, why */
    int is_stopped;	/* is connection currently in stopped state? */
    long received;	/* total number */
    long sent;		/* total number */
    long failed;	/* total number */
    long queued;	/* set our internal outgoing queue length */
    long online;	/* in seconds */
    int load;		/* subjective value 'how loaded we are' for
			 * routing purposes, similar to sms/wapbox load */
} StatusInfo;


enum {
    SMSCCONN_UNKNOWN_STATUS = 0,
    SMSCCONN_STARTING,
    SMSCCONN_ACTIVE,
    SMSCCONN_CONNECTING,
    SMSCCONN_RECONNECTING,
    SMSCCONN_DISCONNECTED,
    SMSCCONN_KILLED	/* ready to be cleaned */
};

enum {
    SMSCCONN_ALIVE = 0,
    SMSCCONN_KILLED_WRONG_PASSWORD = 1,
    SMSCCONN_KILLED_CANNOT_CONNECT = 2,
    SMSCCONN_KILLED_SHUTDOWN = 3
};

/* return current status of the SMSC connection, filled to infotable.
 * For unknown numbers, put -1. Return -1 if either argument was NULL.
 */
int smscconn_info(SMSCConn *smscconn, StatusInfo *infotable);



#endif
