/*
 * msg-decl.h - message declarations
 *
 * This file contains declarations of the message types. See the
 * architecture document to see how these should be interpreted and
 * modified.
 *
 * This file is included by a number of other files.
 *
 * Lars Wirzenius <liw@wapit.com>
 */

MSG(heartbeat,
	{
		INTEGER(load);
	})

MSG(plain_sms,
	{
		OCTSTR(sender);
		OCTSTR(receiver);
		OCTSTR(text);
		INTEGER(time);
	})

MSG(wdp_datagram,
	{
		OCTSTR(source_address);
		INTEGER(source_port);
		OCTSTR(destination_address);
		INTEGER(destination_port);
		OCTSTR(user_data);
	})

#undef MSG
#undef INTEGER
#undef OCTSTR
