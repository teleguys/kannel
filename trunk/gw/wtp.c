/*
 * wtp.c - WTP implementation
 *
 *Implementation is for now very straigthforward, WTP state machines are stored
 *in an unordered linked list (this fact will change, naturally).
 *
 *By Aarno Syv�nen for WapIT Ltd.
 */

#include "wtp.h"

struct WTPMachine {
        #define INTEGER(name) long name
        #define ENUM(name) states name
        #define OCTSTR(name) Octstr *name
        #define QUEUE(name) /* XXX event queue to be implemented later */
	#define TIMER(name) WTPTimer *name
/*#if HAVE_THREADS
        #define MUTEX(name) pthread_mutex_t name
#else*/
        #define MUTEX(name) long name
/*#endif*/
        #define NEXT(name) struct WTPMachine *name
        #define MACHINE(field) field
        #include "wtp_machine-decl.h"
};


struct WTPEvent {
    enum event_name type;

    #define INTEGER(name) long name
    #define OCTSTR(name) Octstr *name
    #define EVENT(name, field) struct name field name;
    #include "wtp_events-decl.h" 
};

enum wsp_event {

      #define WSP_EVENT(name, field) name,
      #include "wsp_events-decl.h"
};

typedef enum wsp_event wsp_event;

struct WSPEvent {
       enum wsp_event name;

       #define INTEGER(name) long name
       #define OCTSTR(name) Octstr *name
       #define MACHINE(name) WTPMachine *name
       #define WSP_EVENT(name, field) struct name field name;
       #include "wsp_events-decl.h"
};

static WTPMachine *list = NULL;

/*****************************************************************************
 *
 *Prototypes of internal functions:
 *
 *Give events and the state a readable name.
 */

static char *name_event(int name);

static char *name_wsp_event(int name);

static char *name_state(int name);

/*
 * Create and initialize a WTPMachine structure. Return a pointer to it,
 * or NULL if there was a problem. Add the structure to a global list of
 * all WTPMachine structures (see wtp_machine_find).
 */
WTPMachine *wtp_machine_create(void);

/*
 * Find the WTPMachine from the global list of WTPMachine structures that
 * corresponds to the five-tuple of source and destination addresses and
 * ports and the transaction identifier. Return a pointer to the machine,
 * or NULL if not found.
 */
WTPMachine *wtp_machine_find(Octstr *source_address, long source_port,
	Octstr *destination_address, long destination_port, long tid);

/*
 *Attach a WTP machine five-tuple (addresses, ports and tid) which are used to
 *identify it.
 */
WTPMachine *name_machine(WTPMachine *machine, Octstr *source_address, 
           long source_port, Octstr *destination_address, 
           long destination_port, long tid);

WSPEvent *wsp_event_create(enum wsp_event type);

void wsp_event_destroy(WSPEvent *event);

void wsp_event_dump(WSPEvent *event);

/*
 *Packs a wsp event. Fetches flags and user data from a wtp event. Address 
 *five-tuple and tid are fields of the wtp machine.
 */
WSPEvent *pack_wsp_event(wsp_event wsp_name, WTPEvent *wtp_event, 
         WTPMachine *machine);

int wtp_tid_is_valid(WTPEvent *event);

/******************************************************************************
 *
 *EXTERNAL FUNCTIONS:
 */

WTPEvent *wtp_event_create(enum event_name type) {
	WTPEvent *event;
	
	event = malloc(sizeof(WTPEvent));
	if (event == NULL)
		goto error;

	event->type = type;
	
	#define INTEGER(name) p->name=0
	#define OCTSTR(name) p->name=octstr_create_empty();\
                             if (p->name == NULL)\
                                goto error
	#define EVENT(type, field) { struct type *p = &event->type; field } 
	#include "wtp_events-decl.h"
        return event;
/*
 *TBD: Send Abort(CAPTEMPEXCEEDED)
 */
error:
        #define INTEGER(name) p->name=0
        #define OCTSTR(name) if (p->name != NULL)\
                                octstr_destroy(p->name)
        #define EVENT(type, field) { struct type *p = &event->type; field }
        #include "wtp_events-decl.h"
        free(event);
	error(errno, "Out of memory.");
	return NULL;
}
/*
 *Note: We must use p everywhere (including events having only integer 
 *fields), otherwise we get a compiler warning.
 */

void wtp_event_destroy(WTPEvent *event) {
	#define INTEGER(name) p->name = 0
        #define OCTSTR(name) octstr_destroy(p->name)
        #define EVENT(type, field) { struct type *p = &event->type; field } 
        #include "wtp_events-decl.h"

	free(event);
}

void wtp_event_dump(WTPEvent *event) {

  	debug(0, "Event %p:", (void *) event); 
	debug(0, " type = %s", name_event(event->type));
	#define INTEGER(name) debug(0, "Integer field %s,%ld:",#name,p->name) 
	#define OCTSTR(name)  debug(0, "Octstr field %s:",#name);\
                              octstr_dump(p->name)
	#define EVENT(type, field) { struct type *p = &event->type; field } 
	#include "wtp_events-decl.h"
}

/*
 *Mark a WTP state machine unused. Normal functions do not remove machines.
 */
void wtp_machine_mark_unused(WTPMachine *machine){

        WTPMachine *temp;
        /*int ret;*/

/*      ret=pthread_mutex_lock(&list->mutex);
        if (ret == EINVAL){
           info(errno, "Mutex not iniatilized. (List probably empty.)");
           return;
	}*/

        temp=list;
        /*ret=pthread_mutex_lock(&temp->next->mutex);*/

        while (temp != NULL && temp->next != machine){
	    /*ret=pthread_mutex_unlock(&temp->mutex);*/
            temp=temp->next;
            /*if (temp != NULL)
	         ret=pthread_mutex_lock(&temp->next->mutex);*/
        }

        if (temp == NULL){
	    /*if (ret != EINVAL)
	         ret=pthread_mutex_unlock(&temp->mutex);*/
            debug(0, "Machine unknown");
            return;
	}
       
        temp->in_use=0;
        /*if (ret != EINVAL)
	     ret=pthread_mutex_unlock(&temp->mutex);*/
        return;
}

/*
 *Really removes a WTP state machine. Used only by the garbage collection.
 */
void wtp_machine_destroy(WTPMachine *machine){

        WTPMachine *temp;
/*      int ret, d_ret;*/

/*      ret=pthread_mutex_lock(&list->mutex);
        if (ret == EINVAL){
           error(errno, "Empty list (mutex not iniatilized)");
           return;
        }
        ret=pthread_mutex_lock(&list->next->mutex);
*/
        if (list == machine) {
           list=machine->next;         
/*      if (ret != EINVAL)
              ret=pthread_mutex_unlock(&list->next->mutex);
	      ret=pthread_mutex_unlock(&list->mutex);*/

        } else {
          temp=list;

          while (temp != NULL && temp->next != machine){ 
	        /*ret=pthread_mutex_unlock(&temp->mutex);*/
                temp=temp->next;
/*              if (temp != NULL)
		   ret=pthread_mutex_lock(&temp->next->mutex);*/
          }

          if (temp == NULL){
              
/*            if (ret != EINVAL)
                  ret=pthread_mutex_unlock(&temp->next->mutex);
	      ret=pthread_mutex_unlock(&temp->mutex);*/
              info(0, "Machine unknown");
              return;
	  }
          temp->next=machine->next;
	}

        #define INTEGER(name)
        #define ENUM(name)        
        #define OCTSTR(name) octstr_destroy(temp->name)
        #define TIMER(name) wtp_timer_destroy(temp->name)
        #define QUEUE(name) /*queue to be implemented later*/
/*#if HAVE_THREADS
        #define MUTEX(name) d_ret=pthread_mutex_destroy(&temp->name)
#else*/
        #define MUTEX(name)
/*#endif*/
        #define NEXT(name)
        #define MACHINE(field) field
        #include "wtp_machine-decl.h"

        free(temp);
/*      if (ret != EINVAL)
           ret=pthread_mutex_unlock(&machine->next->mutex);
	ret=pthread_mutex_unlock(&machine->mutex);*/
        
        return;
}

/*
 *Write state machine fields, using debug function from a project library 
 *wapitlib.c.
 */
void wtp_machine_dump(WTPMachine  *machine){

        /*int ret;*/

        if (machine != NULL){

           debug(0, "The machine was %p:", (void *) machine); 
	   #define INTEGER(name) \
           debug(0, "Integer field %s,%ld:", #name, machine->name)
           #define ENUM(name) debug(0, "state=%s.", name_state(machine->name))
	   #define OCTSTR(name)  debug(0, "Octstr field %s :", #name);\
                                 octstr_dump(machine->name)
           #define TIMER(name)   debug(0, "Machine timer %p:", (void *) \
                              machine->name)
           #define QUEUE(name)   /*to be implemented later*/
/*#if HAVE_THREADS
           #define MUTEX(name)   ret=pthread_mutex_trylock(&machine->name);\
                                 if (ret == EBUSY)\
                                    debug(0, "Machine locked");\
                                 else {\
                                    debug(0, "Machine unlocked");\
                                    ret=pthread_mutex_unlock(&machine->name);\
                                 }
#else*/
           #define MUTEX(name)
/*#endif*/
           #define NEXT(name)
	   #define MACHINE(field) field
	   #include "wtp_machine-decl.h"
	}
}


WTPMachine *create_or_find_wtp_machine(Msg *msg, WTPEvent *event){

           WTPMachine *machine;

           machine=wtp_machine_find(msg->wdp_datagram.source_address,
                                    msg->wdp_datagram.source_port, 
                                    msg->wdp_datagram.destination_address,
                                    msg->wdp_datagram.destination_port,
                                    event->RcvInvoke.tid);
           if (machine == NULL){
	      machine=wtp_machine_create();

              if (machine == NULL)
                 return NULL;
              else {
                 machine=name_machine(machine, 
                                    msg->wdp_datagram.source_address,
                                    msg->wdp_datagram.source_port, 
                                    msg->wdp_datagram.destination_address,
                                    msg->wdp_datagram.destination_port,
                                    event->RcvInvoke.tid);
              }
           }

           return machine;
}

/*
 *Transfers data from fields of a message to fields of WTP event. User data has
 *the host byte order. Updates the log and sends protocol error messages.
 */

WTPEvent *wtp_unpack_wdp_datagram(Msg *msg){

         WTPEvent *event;
         int octet,
             this_octet,

             con,
             pdu_type,
             gtr,
             ttr,
	     first_tid,  /*first octet of the tid, in the host order*/ 
	     last_tid,   /*second octet of the tid, in the host order*/
             tid,
             version,
             tcl,
             abort_type,
             tpi_length_type,
             tpi_length;

/*
 *Every message type uses the second and the third octets for tid. Bytes are 
 *already in host order. Not that the iniator turns the first bit off, do we
 *have a genuine tid.
 */
         first_tid=octstr_get_char(msg->wdp_datagram.user_data,1);
         last_tid=octstr_get_char(msg->wdp_datagram.user_data,2);
         tid=first_tid;
         tid=(tid << 8) + last_tid;

         debug(0, "first_tid=%d last_tid=%d tid=%d", first_tid, 
               last_tid, tid);

         this_octet=octet=octstr_get_char(msg->wdp_datagram.user_data, 0);
         if (octet == -1)
            goto no_datagram;

         con=this_octet>>7; 
         if (con == 0){
            this_octet=octet;
            pdu_type=this_octet>>3&15;
            this_octet=octet;

            if (pdu_type == 0){
               goto no_segmentation;
            }
/*
 *Message type was invoke
 */
            if (pdu_type == 1){

               event=wtp_event_create(RcvInvoke);
               if (event == NULL)
                  goto cap_error;
               event->RcvInvoke.tid=tid;

               gtr=this_octet>>2&1;
               this_octet=octet;
               ttr=this_octet>>1&1;
               if (gtr == 0 || ttr == 0){
		  goto no_segmentation;
               }
               this_octet=octet;
               event->RcvInvoke.rid=this_octet&1; 

               this_octet=octet=octstr_get_char(
                          msg->wdp_datagram.user_data, 3);
               version=this_octet>>6&3;
               if (version != 0){
                  goto wrong_version;
               } 
               this_octet=octet;
               event->RcvInvoke.tid_new=this_octet>>5&1;
               this_octet=octet;
               event->RcvInvoke.up_flag=this_octet>>4&1;
               this_octet=octet;
               tcl=this_octet&3; 
               if (tcl > 2)
                  goto illegal_header;
               event->RcvInvoke.tcl=tcl; 
 
/*
 *At last, the message itself. We remove the header.
 */
               octstr_delete(msg->wdp_datagram.user_data, 0, 4);
               event->RcvInvoke.user_data=msg->wdp_datagram.user_data;     
            }
/*
 *Message type is supposed to be result. This is impossible, so we have an
 *illegal header.
 */
            if (pdu_type == 2){
               goto illegal_header;
            }
/*
 *Message type was ack.
 */
            if (pdu_type == 3){
               event=wtp_event_create(RcvAck);
               if (event == NULL)
                  goto cap_error;
               event->RcvAck.tid=tid;

               this_octet=octet=octstr_get_char(
                          msg->wdp_datagram.user_data, 0);
               event->RcvAck.tid_ok=this_octet>>2&1;
               this_octet=octet;
               event->RcvAck.rid=this_octet&1;
               info(0, "Ack event packed");
               wtp_event_dump(event);
            }

/*
 *Message type was abort.
 */
	    if (pdu_type == 4){
                event=wtp_event_create(RcvAbort);
                if (event == NULL)
                    goto cap_error;
                event->RcvAbort.tid=tid;
                
               octet=octstr_get_char(msg->wdp_datagram.user_data, 0);
               abort_type=octet&7;
               if (abort_type > 1)
                  goto illegal_header;
               event->RcvAbort.abort_type=abort_type;   

               octet=octstr_get_char(msg->wdp_datagram.user_data, 3);
               if (octet > NUMBER_OF_ABORT_REASONS)
                  goto illegal_header;
               event->RcvAbort.abort_reason=octet;
               info(0, "abort event packed");
            }

/*
 *WDP does the segmentation.
 */
            if (pdu_type > 4 && pdu_type < 8){
               goto no_segmentation;
            }
            if (pdu_type >= 8){
               goto illegal_header;
            } 

/*
 *Message is of variable length. This is possible only when we are receiving
 *an invoke message.(For now, only info tpis are supported.)
 */
         } else {
           this_octet=octet=octstr_get_char(msg->wdp_datagram.user_data, 4);
           tpi_length_type=this_octet>>2&1;
/*
 *TPI can be long
 */
           if (tpi_length_type == 1){
               
               tpi_length=1;
           } else {
/*or short*/
               tpi_length=0;
           }
         }      
         return event;
/*
 *Send Abort(WTPVERSIONZERO)
 */
wrong_version:
         wtp_event_destroy(event);
         error(0, "Version not supported");
         return NULL;
/*
 *Send Abort(NOTIMPLEMENTEDSAR)
 */
no_segmentation:
         wtp_event_destroy(event);
         error(errno, "No segmentation implemented");
         return NULL;
/*
 *TBD: Send Abort(CAPTEMPEXCEEDED), too.
 */
cap_error:
         free(event);
         error(errno, "Out of memory");
         return NULL;
/*
 *Send Abort(PROTOERR)
 */
illegal_header:
         wtp_event_destroy(event);
         error(errno, "Illegal header structure");
         return NULL;
/*
 *TBD: Another error message. 
 */
no_datagram:   
         free(event);
         error(errno, "No datagram received");
         return NULL;
}

/*
 * Feed an event to a WTP state machine. Handle all errors yourself,
 * and report them to the caller. Note: first include directive, then {}s.
 * (Calling ROW macro expands everything between define and include, including
 * surplus {}s.)
 *
 * Returns: WSP event, if succeeded and an indication or a confirmation is 
 *          generated
 *          NULL, if succeeded and no indication or confirmation is generated
 *          NULL, if failed (this information is superflous, but required by
 *          the function call syntax.)
 */
WSPEvent *wtp_handle_event(WTPMachine *machine, WTPEvent *event){

     
     states current_state=machine->state;
     long current_event=event->type;
     enum wsp_event current_primitive;
     WSPEvent *wsp_event=NULL;
     WTPTimer *timer=NULL;

     timer=wtp_timer_create();
     if (timer == NULL)
        goto mem_error;

     debug(0,"handle_event: current state=%s.",name_state(machine->state));
     #define STATE_NAME(state)
     #define ROW(wtp_state, event, condition, action, next_state) \
             if (current_state == wtp_state && current_event == event &&\
                (condition)){\
                action\
                machine->state=next_state;\
             } else 
     #include "wtp_state-decl.h"
             {debug(0, "handle_event: out of synch error");}
             return wsp_event;
/*
 *Send Abort(CAPTEMPEXCEEDED)
 */
mem_error:
     debug(0, "handle_event: out of memory");
     if (timer != NULL)
        wtp_timer_destroy(timer);
     free(timer);
     free(wsp_event);
     return NULL;
}

/*****************************************************************************
 *
 *INTERNAL FUNCTIONS:
 *
 *Give the name of an event in a readable form. 
 */

static char *name_event(int s){

       switch (s){
              #define EVENT(type, field) case type: return #type;
              #include "wtp_events-decl.h"
              default:
                      return "unknown event";
       }
 }


static char *name_wsp_event(int s){

       switch (s){
              #define WSP_EVENT(type, field) case type: return #type;
              #include "wsp_events-decl.h"
              default:
                      return "unknown event";
       }
 }

static char *name_state(int s){

       switch (s){
              #define STATE_NAME(state) case state: return #state;
              #define ROW(state, event, condition, action, new_state)
              #include "wtp_state-decl.h"
              default:
                      return "unknown state";
       }
}


WTPMachine *wtp_machine_find(Octstr *source_address, long source_port,
	   Octstr *destination_address, long destination_port, long tid){

           WTPMachine *temp;
           /*int ret;*/

/*
 *We are interested only machines in use, it is, having in_use-flag 1.
 */
/*         ret=pthread_mutex_lock(&list->mutex);
           if (ret == EINVAL){
               error(errno, "Empty list (mutex not iniatilized)");
           return NULL;
           }
*/
           if (list == NULL)
              return NULL;
           
           temp=list;

           while (temp != NULL){
   
                if ((temp->source_address != source_address &&
                   temp->source_port != source_port &&
                   temp->destination_address != destination_address &&
                   temp->destination_port != destination_port &&
		   temp->tid != tid) || temp->in_use == 0){

		  /*pthread_mutex_unlock(&temp->mutex);*/
                   temp=temp->next;
		   /* pthread_mutex_lock(&temp->mutex);*/

                } else {
		  /*pthread_mutex_unlock(&temp->mutex);*/
                   debug(0, "Machine %p found", (void *) temp);
                   return temp;
               }              
           }
           /*pthread_mutex_unlock(&temp->mutex);*/
           debug(0, "Machine %p not found", (void *) temp);
           return temp;
}

WTPMachine *wtp_machine_create(void){

        WTPMachine *machine;
        /*int dummy, ret;*/

        machine=malloc(sizeof(WTPMachine));
        if (machine == NULL)
           goto error;
        
        #define INTEGER(name) machine->name=0
        #define ENUM(name) machine->name=LISTEN
        #define OCTSTR(name) machine->name=octstr_create_empty();\
                             if (machine->name == NULL)\
                                goto error
        #define QUEUE(name) /*Queue will be implemented later*/
/*#if HAVE_THREADS
        #define MUTEX(name) dummy=pthread_mutex_init(&machine->name, NULL)
#else*/
        #define MUTEX(name) machine->name=0
/*#endif*/                
        #define TIMER(name) machine->name=wtp_timer_create();\
                            if (machine->name == NULL)\
                               goto error
        #define NEXT(name) machine->name=NULL
        #define MACHINE(field) field
        #include "wtp_machine-decl.h"

        machine->in_use=1;

        /*ret=pthread_mutex_lock(&list->mutex);*/
        machine->next=list;
        list=machine;
/*
 *List was not empty
 */
/*        if (ret != EINVAL)
	     ret=pthread_mutex_unlock(&list->mutex);*/

          debug(0, "create_machine: machine created");
          return machine;
/*
 *Message Abort(CAPTEMPEXCEEDED), to be added later. 
 *Thou shalt not leak memory... Note, that a macro could be called many times.
 *So it is possible one call to succeed and another to fail. 
 */
 error:  if (machine != NULL) {
            #define INTEGER(name)
            #define ENUM(name)
            #define OCTSTR(name) if (machine->name != NULL)\
                                    octstr_destroy(machine->name)
            #define QUEUE(name)  /*to be implemented later*/
/*#if HAVE_THREADS
            #define MUTEX(name)  ret=pthread_mutex_lock(&machine->name);\
                                 ret=pthread_mutex_destroy(&machine->name)
#else*/
            #define MUTEX(name)
/*#endif*/
            #define TIMER(name) if (machine->name != NULL)\
                                   wtp_timer_destroy(machine->name)
            #define NEXT(name)
            #define MACHINE(field) field
            #include "wtp_machine-decl.h"
        }
        free(machine);
        error(errno, "Out of memory");
        return NULL;
}

/*
 *Attach a WTP machine five-tuple (addresses, ports and tid) which are used to
 *identify it.
 */
WTPMachine *name_machine(WTPMachine *machine, Octstr *source_address, 
           long source_port, Octstr *destination_address, 
           long destination_port, long tid){

           machine->source_address=source_address;
           machine->source_port=source_port;
           machine->destination_address=destination_address;
           machine->destination_port=destination_port;
           machine->tid=tid;

           return machine;
} 


WSPEvent *wsp_event_create(enum wsp_event type) {
	WSPEvent *event;
	
	event = malloc(sizeof(WSPEvent));
	if (event == NULL)
		goto error;

	event->name = type;
	
	#define INTEGER(name) p->name=0
	#define OCTSTR(name) p->name=octstr_create_empty();\
                             if (p->name == NULL)\
                                goto error
        #define MACHINE(name) p->name=wtp_machine_create();\
                             if (p->name == NULL)\
                                goto error
	#define WSP_EVENT(type, field) {struct type *p = &event->type; field } 
	#include "wsp_events-decl.h"
        return event;
/*
 *TBD: Send Abort(CAPTEMPEXCEEDED)
 */
error:
        #define INTEGER(name) p->name=0
        #define OCTSTR(name) if (p->name != NULL)\
                                octstr_destroy(p->name)
        #define MACHINE(name) if (p->name != NULL)\
                                 wtp_machine_destroy(p->name)
        #define WSP_EVENT(type, field) { struct type *p = &event->type; field }
        #include "wsp_events-decl.h"
        free(event);
	error(errno, "Out of memory.");
	return NULL;
}


void wsp_event_destroy(WSPEvent *event){

/*
 *Note: We must use p everywhere, including events having only integer fields,
 *otherwise we get a compiler warning.
 */
        #define INTEGER(name) p->name=0
        #define OCTSTR(name) octstr_destroy(p->name)
        #define MACHINE(name) wtp_machine_destroy(p->name)
        #define WSP_EVENT(type, field)\
                { struct type *p = &event->type; field}
        #include "wsp_events-decl.h" 

       free(event);
}


void wsp_event_dump(WSPEvent *event){

        debug(0, "WSP event %p:", (void *) event);
        debug(0, "The TYPE of the event = %s", name_wsp_event(event->name));
        #define INTEGER(name) debug(0, "Int %s.%s,%ld:", t, #name, p->name)
        #define OCTSTR(name) debug(0, "Octstr field %s.%s:", t, #name);\
                             octstr_dump(p->name)
        #define MACHINE(name) debug(0, "Machine %p.%s", (void *) p->name, t);\
                              wtp_machine_dump(p->name)
        #define WSP_EVENT(type, field) \
                { char *t =#type; struct type *p=&event->type; field }
        #include "wsp_events-decl.h"
}


/*
 *Packs a wsp event. Fetches flags and user data from a wtp event. Address 
 *five-tuple and tid are fields of the wtp machine.
 */
WSPEvent *pack_wsp_event(wsp_event wsp_name, WTPEvent *wtp_event, 
         WTPMachine *machine){

         WSPEvent *event=wsp_event_create(wsp_name);
/*
 *Abort(CAPTEMPEXCEEDED)
 */
         if (event == NULL){
            debug(0, "Out of memory");
            free(event);
            return NULL;
         }
         
         switch (wsp_name){
                
	        case TRInvokeIndication:
                     event->TRInvokeIndication.ack_type=machine->u_ack;
                     event->TRInvokeIndication.user_data=
                            wtp_event->RcvInvoke.user_data;
                     event->TRInvokeIndication.tcl=wtp_event->RcvInvoke.tcl;
                     event->TRInvokeIndication.machine=machine;
                break;
                
	        case TRResultConfirmation:
                     event->TRResultConfirmation.exit_info=
                            wtp_event->RcvInvoke.exit_info;
                     event->TRResultConfirmation.exit_info_present=
                            wtp_event->RcvInvoke.exit_info_present;
                     event->TRResultConfirmation.machine=machine;
                break;

	        case TRAbortIndication:
                     event->TRAbortIndication.abort_code=
                            wtp_event->RcvAbort.abort_reason;
                     event->TRAbortIndication.machine=machine;
                break;
                
	        default:
                break;
         }
         debug(0, "pack_wsp_event: packed");
         return event;
} 

int wtp_tid_is_valid(WTPEvent *event){

    return 1;
}

/*****************************************************************************/
























