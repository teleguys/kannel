/*
 * bb_udpc.c : bearerbox UDP sender/receiver module
 *
 * handles start/restart/shutdown/suspend/die operations of the UDP
 * WDP interface
 *
 * Kalle Marjola <rpr@wapit.com> 2000 for project Kannel
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <assert.h>

#include "gwlib/gwlib.h"
#include "msg.h"
#include "new_bb.h"

/* passed from bearerbox core */

extern volatile sig_atomic_t bb_status;
extern List *incoming_wdp;

/* our own thingies */

static volatile sig_atomic_t udp_running;
static List *udpc_list;


typedef struct _udpc {
    int fd;
    Octstr *addr;
    List *outgoing_list;
    pthread_t receiver;
} Udpc;


/* forward declarations */

static void udpc_destroy(Udpc *udpc);

/*-------------------------------------------------
 *  receiver thingies
 */

static void *udp_receiver(void *arg)
{
    Octstr *datagram, *cliaddr;
    int ret;
    Msg *msg;
    Udpc *conn = arg;

    list_add_producer(incoming_wdp);
    
    /* remove messages from socket until it is closed */
    while(bb_status != BB_DEAD && bb_status != BB_SHUTDOWN) {

	// if (bb_status == bb_suspended)
        // wait_for_status_change(&bb_status, bb_suspended);

	if (read_available(conn->fd, 100000) < 1)
	    continue;
	ret = udp_recvfrom(conn->fd, &datagram, &cliaddr);
	if (ret == -1) {
	    if (errno == EAGAIN)
		/* No datagram available, don't block. */
		continue;

	    error(errno, "Failed to receive an UDP");
	    break;
	}
	debug("bb.udp", 0, "datagram received");
	msg = msg_create(wdp_datagram);

	msg->wdp_datagram.source_address = udp_get_ip(cliaddr);
	msg->wdp_datagram.source_port    = udp_get_port(cliaddr);
	msg->wdp_datagram.destination_address = udp_get_ip(conn->addr);
	msg->wdp_datagram.destination_port    = udp_get_port(conn->addr);
	msg->wdp_datagram.user_data = datagram;

	octstr_destroy(cliaddr);

	list_produce(incoming_wdp, msg);
    }    
    list_remove_producer(incoming_wdp);
    return NULL;
}


/*---------------------------------------------
 * sender thingies
 */

static int send_udp(int fd, Msg *msg)
{
    Octstr *cliaddr;
    int ret;

    cliaddr = udp_create_address(msg->wdp_datagram.destination_address,
				 msg->wdp_datagram.destination_port);
    ret = udp_sendto(fd, msg->wdp_datagram.user_data, cliaddr);
    if (ret == -1)
	error(errno, "WDP/UDP: could not send UDP datagram");
    octstr_destroy(cliaddr);
    return ret;
}


static void *udp_sender(void *arg)
{
    Msg *msg;
    Udpc *conn = arg;

    while(bb_status != BB_DEAD) {

	if ((msg = list_consume(conn->outgoing_list)) == NULL)
	    break;

	debug("bb.udp", 0, "udp: sending message");
	
        if (send_udp(conn->fd, msg) == -1)
	    /* ok, we failed... tough
	     * XXX log the message or something like that... but this
	     * is not as fatal as it is with SMS-messages...
	     */
	    continue;

	msg_destroy(msg);
    }
    debug("bb.udp", 0, "udp_sender done.");
    
    if (pthread_join(conn->receiver, NULL) != 0)
	error(0, "Join failed in udp_sender");

    debug("bb.udp", 0, "udp_sender exiting");
    udpc_destroy(conn);
    return NULL;
}

/*---------------------------------------------------------------
 * accept/create thingies
 */


static Udpc *udpc_create(int port, char *interface_name)
{
    Udpc *udpc;
    Octstr *os;
    int fl;
    
    udpc = gw_malloc(sizeof(Udpc));
    udpc->fd = udp_bind(port);

    os = octstr_create(interface_name);
    udpc->addr = udp_create_address(os, port);
    octstr_destroy(os);
    if (udpc->addr == NULL) {
	error(0, "updc_create: could not resolve interface <%s>",
	      interface_name);
	close(udpc->fd);
	free(udpc);
	return NULL;
    }

    fl = fcntl(udpc->fd, F_GETFL);
    fcntl(udpc->fd, F_SETFL, fl | O_NONBLOCK);

    os = udp_get_ip(udpc->addr);
    debug("bb.udp", 0, "udpc_create: Bound to UDP <%s:%d>",
	  octstr_get_cstr(os), udp_get_port(udpc->addr));

    octstr_destroy(os);
    
    udpc->outgoing_list = list_create();

    return udpc;
}    


static void udpc_destroy(Udpc *udpc)
{
    if (udpc == NULL)
	return;
    
    if (udpc->fd >= 0)
	close(udpc->fd);
    octstr_destroy(udpc->addr);
    list_destroy(udpc->outgoing_list);

    gw_free(udpc);
}    


static int add_service(int port, char *interface_name)
{
    Udpc *udpc;
    
    udpc = udpc_create(port, interface_name);
    list_add_producer(udpc->outgoing_list);

    if ((int)(udpc->receiver = start_thread(0, udp_receiver, udpc, 0)) == -1)
	goto error;

    if ((int)start_thread(0, udp_sender, udpc, 0) == -1)
	goto error;

    list_append(udpc_list, udpc);
    return 0;
    
error:    
    error(0, "Failed to start UDP receiver/sender thread");
    udpc_destroy(udpc);
    return -1;
}



/*-------------------------------------------------------------
 * public functions
 *
 */

int udp_start(Config *config)
{
    char *p, *interface_name;
    
    if (udp_running) return -1;

    
    debug("bb.udp", 0, "starting UDP sender/receiver module");

    if ((p = config_get(config_find_first_group(config, "group", "core"),
			"wdp-interface-name")) == NULL) {
	error(0, "Missing wdp-interface-name variable, cannot start UDP");
	return -1;
    }
    interface_name = p;
    
    udpc_list = list_create();	/* have a list of running systems */

    /* add_service(9200, interface_name);	 * wsp 		*/
    add_service(9201, interface_name);		/* wsp/wtp	*/
    /* add_service(9202, interface_name);	 * wsp/wtls	*/
    /* add_service(9203, interface_name);	 * wsp/wtp/wtls */
    /* add_service(9204, interface_name);	 * vcard	*/
    /* add_service(9205, interface_name);	 * vcal		*/
    /* add_service(9206, interface_name);	 * vcard/wtls	*/
    /* add_service(9207, interface_name);	 * vcal/wtls	*/
    
    list_add_producer(incoming_wdp);
    udp_running = 1;
    return 0;
}


/*
 * this function receives an WDP message and adds it to
 * corresponding outgoing_list.
 */
int udp_addwdp(Msg *msg)
{
    int i;
    Udpc *udpc;
    
    if (!udp_running) return -1;
    assert(msg != NULL);
    assert(msg_type(msg) == wdp_datagram);
    
    list_lock(udpc_list);
    /* select in which list to add this */
    for (i=0; i < list_len(udpc_list); i++) {
	udpc = list_get(udpc_list, i);

	if (msg->wdp_datagram.source_port == udp_get_port(udpc->addr))
	{
	    list_produce(udpc->outgoing_list, msg);
	    list_unlock(udpc_list);
	    return 0;
	}
    }
    list_unlock(udpc_list);
    return -1;
}

int udp_shutdown(void)
{
    list_remove_producer(incoming_wdp);
    return 0;
}


int udp_die(void)
{
    Udpc *udpc;

    if (!udp_running) return -1;
    
    /*
     * remove producers from all outgoing lists.
     */
    debug("bb.udp", 0, "udp_die: removing producers from udp-lists");

    while((udpc = list_consume(udpc_list)) != NULL) {
	list_remove_producer(udpc->outgoing_list);
    }
    udp_running = 0;
    return 0;
}


