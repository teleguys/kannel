#ifndef _CSDR_H
#define _CSDR_H

#include "wapitlib.h"
#include "config.h"
#include "bb_msg.h"

typedef struct csdrouter {

	char *interface_name;
	int port;
	int fd;

} CSDRouter;

/*
 * the following functions are corresponding as in SMSCenter
 */

/*
 * open a connection to the CSD Router and do all the necessary
 * handshaking and initialization
 *
 * returns a new CSDR structure, or NULL if the open failed
 */
CSDRouter *csdr_open(ConfigGroup *grp);

/*
 * close the CSD Router connection, or do nothing if already closed
 * (or pointer NULL)
 *
 * return 0, -1 if failed (currently cannot fail)
 */
int csdr_close(CSDRouter *csdr);

/*
 * check if there is any new message to be received and if there is,
 * do basic unpacking and return the new message
 *
 * if tere is no new messages or an error occurs, return NULL
 */
RQueueItem *csdr_get_message(CSDRouter *csdr);

/*
 * send a message as UDP packet
 *
 * return 0 on success, -1 on failure
 */
int csdr_send_message(CSDRouter *csdr, RQueueItem *msg);


#endif
