/*
 * msg.h - declarations for message manipulation
 * 
 * This file declares the Msg data type and the functions to manipulate it.
 * 
 * Lars Wirzenius
 */


#ifndef MSG_H
#define MSG_H

#include "gwlib/gwlib.h"

enum msg_type {
	#define MSG(type, stmt) type,
	#include "msg-decl.h"
	msg_type_count
};

typedef struct {
	enum msg_type type;

	#define INTEGER(name) long name
	#define OCTSTR(name) Octstr *name
	#define MSG(type, stmt) struct type stmt type;
	#include "msg-decl.h"
} Msg;


/* enums for Msg fields */

/* sms message type */

enum {
    mo = 0,
    mt_reply = 1,
    mt_push = 2,
    report = 3
};

/* admin commands */
enum {
    cmd_shutdown = 0,
    cmd_suspend = 1,
    cmd_resume = 2,
    cmd_identify = 3
};

/*
 * Create a new, empty Msg object. Panics if fails.
 */
Msg *msg_create(enum msg_type type);


/*
 * Create a new Msg object that is a copy of an existing one.
 * Panics if fails.
 */
Msg *msg_duplicate(Msg *msg);


/*
 * Return type of the message
 */
enum msg_type msg_type(Msg *msg);


/*
 * Destroy an Msg object. All fields are also destroyed.
 */
void msg_destroy(Msg *msg);


/*
 * Destroy an Msg object. Wrapper around msg_destroy to make it suitable for
 * list_destroy.
 */
void msg_destroy_item(void *msg);


/*
 * For debugging: Output with `debug' (in gwlib/log.h) the contents of
 * an Msg object.
 */
void msg_dump(Msg *msg, int level);


/*
 * Pack an Msg into an Octstr. Panics if fails.
  */
Octstr *msg_pack(Msg *msg);


/*
 * Unpack an Msg from an Octstr. Return NULL for failure, otherwise a pointer
 * to the Msg.
 */
Msg *msg_unpack(Octstr *os);

#endif