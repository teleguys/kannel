/*
 * wap_addr.h - interface to WAPAddr and WAPAddrTuple types.
 */


#ifndef WAP_ADDR_H
#define WAP_ADDR_H

#include "gwlib/gwlib.h"

typedef struct {
	Octstr *address;
	long port;
} WAPAddr;

typedef struct {
	WAPAddr *remote, *local;
} WAPAddrTuple;


WAPAddr *wap_addr_create(Octstr *address, long port);
void wap_addr_destroy(WAPAddr *addr);
int wap_addr_same(WAPAddr *a, WAPAddr *b);

WAPAddrTuple *wap_addr_tuple_create(Octstr *rmt_addr, long rmt_port,
				    Octstr *lcl_addr, long lcl_port);
void wap_addr_tuple_destroy(WAPAddrTuple *tuple);
int wap_addr_tuple_same(WAPAddrTuple *a, WAPAddrTuple *b);
WAPAddrTuple *wap_addr_tuple_duplicate(WAPAddrTuple *tuple);
void wap_addr_tuple_dump(WAPAddrTuple *tuple);

#endif