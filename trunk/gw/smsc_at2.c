/*
 * smsc_at2.c
 * New driver for serial connected AT based
 * devices.
 * 4.9.2001
 * Andreas Fink <afink@smsrelay.com>
 * 
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <time.h>

#include "gwlib/gwlib.h"
#include "gwlib/charset.h"
#include "smscconn.h"
#include "smscconn_p.h"
#include "bb_smscconn_cb.h"
#include "msg.h"
#include "sms.h"
#include "dlr.h"

typedef struct ModemDef {
    Octstr	*id;
    Octstr	*name;
    Octstr	*detect_string;
    Octstr	*detect_string2;
    Octstr	*init_string;
    long	speed;
    Octstr	*enable_hwhs;
    int		need_sleep;
    int		no_pin;
    int		no_smsc;
    long	sendline_sleep;
    Octstr	*keepalive_cmd;
    int		broken;
} ModemDef;
 
/* maximum data to attempt to read in one go */
#define	MAX_READ	1023

/******************************************************************************
 * Message types defines
 */
#define AT_DELIVER_SM   0
#define AT_SUBMIT_SM    1

/******************************************************************************
 * type of phone number defines
 */
#define PNT_UNKNOWN     0
#define PNT_INTER       1
#define PNT_NATIONAL    2

#define	O_DESTROY(a)	{ if(a) octstr_destroy(a); a=NULL; }

/* The number of times to attempt to send a message should sending fail */
#define RETRY_SEND 3
 
typedef struct PrivAT2data
{
    List	*outgoing_queue;
    ModemDef	*modem;
    long	device_thread;
    int		shutdown;	  /* Internal signal to shut down */
    Octstr	*device;
    long	speed;
    long	keepalive;
    int		fd;	/* file descriptor */
    Octstr	*ilb;	/*input line buffer */
    Octstr	*lines; /* the last few lines before OK was seen */
    Octstr	*pin;   /* pincode */
    int		pin_ready;
    SMSCConn	*conn;
    int 	phase2plus;
    Octstr	*validityperiod;    
    int		alt_dcs;
    int 	retry;
    Octstr	*my_number;
    Octstr	*sms_center;
    Octstr	*name;
    Octstr	*configfile;
} PrivAT2data;


int	at2_open_device(PrivAT2data *privdata);
void	at2_close_device(PrivAT2data *privdata);
void    at2_read_buffer(PrivAT2data *privdata);
Octstr *at2_wait_line(PrivAT2data *privdata, time_t timeout, int gt_flag);
Octstr *at2_read_line(PrivAT2data *privdata, int gt_flag);
int	at2_write(PrivAT2data *privdata, char* line);
int	at2_write_line(PrivAT2data *privdata, char* line);
int	at2_write_ctrlz(PrivAT2data *privdata);
void	at2_flush_buffer(PrivAT2data *privdata);
int	at2_init_device(PrivAT2data *privdata);
int	at2_send_modem_command(PrivAT2data *privdata,char *cmd, time_t timeout, int greaterflag);
int	at2_wait_modem_command(PrivAT2data *privdata, time_t timeout, int greaterflag);
void	at2_set_speed(PrivAT2data *privdata, int bps);
void	at2_device_thread(void *arg);
int	at2_shutdown_cb(SMSCConn *conn, int finish_sending);
long	at2_queued_cb(SMSCConn *conn);
void	at2_start_cb(SMSCConn *conn);
int	at2_add_msg_cb(SMSCConn *conn, Msg *sms);
int 	smsc_at2_create(SMSCConn *conn, CfgGroup *cfg);
int	at2_pdu_extract(PrivAT2data *privdata, Octstr **pdu, Octstr *buffer);
int 	at2_hexchar(int hexc);
Msg	*at2_pdu_decode(Octstr *data, PrivAT2data *privdata);
Msg	*at2_pdu_decode_deliver_sm(Octstr *data, PrivAT2data *privdata);
Octstr	*at2_convertpdu(Octstr *pdutext);
void	at2_decode7bituncompressed(Octstr *input, int len, Octstr *decoded, int offset);
void 	at2_send_messages(PrivAT2data *privdata);
void 	at2_send_one_message(PrivAT2data *privdata, Msg *msg);
int	at2_pdu_encode(Msg *msg, unsigned char *pdu, PrivAT2data *privdata);
int 	at2_encode7bituncompressed(Octstr *input, unsigned char *encoded,int offset);
int	at2_encode8bituncompressed(Octstr *input, unsigned char *encoded);
int 	at2_numtext(int num);
int	at2_detect_speed(PrivAT2data *privdata);
int	at2_detect_modem_type(PrivAT2data *privdata);
ModemDef *at2_read_modems(PrivAT2data *privdata, Octstr *file, Octstr *id, int idnumber);
void	at2_destroy_modem(ModemDef *modem);

/******************************************************************************
** For debugging purposes octstr_destroy might be a macro,
** so function pointers won't work. at2_octstr_destroy is
** here to make them work again. It is semantically identical
** to octstr_destroy from gwlib/octstr.h
**
*/
static void at2_octstr_destroy(Octstr *ostr)
{
    octstr_destroy(ostr);
}


/******************************************************************************
** at2_open_device
** opens the device port
**
*/

int	at2_open_device1(PrivAT2data *privdata)
{
    info(0,"AT2[%s]: opening device",octstr_get_cstr(privdata->name));
    privdata->fd = open(octstr_get_cstr(privdata->device), O_RDWR|O_NONBLOCK|O_NOCTTY);
    if(privdata->fd == -1)
    {
    	error(errno,"AT2[%s]: open failed! ERRNO=%d",octstr_get_cstr(privdata->name),errno);
  	privdata->fd = -1;
    	return -1;
    }
    debug("bb.smsc.at2",0,"AT2[%s]: device opened",octstr_get_cstr(privdata->name));

    return 0;
}


int	at2_open_device(PrivAT2data *privdata)
{
    struct termios tios;
    int ret;
    
    if(0 !=  (ret=at2_open_device1(privdata)))
    	return ret;

    tcgetattr(privdata->fd, &tios);

    kannel_cfmakeraw(&tios);


    /* ignore break &  parity errors */
    tios.c_iflag |= IGNBRK;
    
    /* INPCK: disable parity check */
    tios.c_iflag &= ~INPCK;
    
     /* hangup on close */
     tios.c_cflag |= HUPCL;
     
     /* enable receiver */
     tios.c_cflag |=  CREAD ;
    
    /* set to 8 bit */
    tios.c_cflag &= ~CSIZE;
    tios.c_cflag |=CS8;
    
    /* no NL to CR-NL mapping outgoing */
    tios.c_oflag &= ~ONLCR;
    
    /* ignore parity */
    tios.c_iflag |= IGNPAR;
    tios.c_iflag &= ~INPCK;

    /* enable hardware flow control */
    tios.c_cflag |= CRTSCTS;
    
    tios.c_cc[VSUSP] = 0; /* otherwhise we can not send CTRL Z */
    /*
    if ( ModemTypes[privdata->modemid].enable_parity )
    	tios.c_cflag ^= PARODD;
  */

     ret = tcsetattr(privdata->fd, TCSANOW, &tios); /* apply changes now */
     if(ret == -1)
     {
     	error(errno,"AT2[%s]: at_data_link: fail to set termios attribute",
	      octstr_get_cstr(privdata->name));
     }
     tcflush(privdata->fd, TCIOFLUSH);


   /* Nokia 7110 and 6210 need some time between opening
    * the connection and sending the first AT commands */
    if ( privdata->modem->need_sleep )
    	sleep(1);
    debug("bb.smsc.at2",0,"AT2[%s]: device opened",octstr_get_cstr(privdata->name));
    return 0;
}

/******************************************************************************
** at2_close_device
** closes the device port
**
*/

void	at2_close_device(PrivAT2data *privdata)
{
    info(0,"AT2[%s]: closing device",octstr_get_cstr(privdata->name));
    close(privdata->fd);
    privdata->fd = -1;
}


/******************************************************************************
** at2_read_buffer
** checks if there are any incoming bytes
** and adds them to the line buffer
*/


void	at2_read_buffer(PrivAT2data *privdata)
{
    char buf[MAX_READ+1];
    int s, ret;
    int count;
    fd_set read_fd;
    struct timeval tv;
 
    if(privdata->fd == -1)
    {
     	error(errno,"AT2[%s]: at2_read_buffer: fd = -1. Can not read", octstr_get_cstr(privdata->name));     
  	return;
    }
    count = MAX_READ;

#ifdef	SSIZE_MAX
    if(count > SSIZE_MAX)
    	count = SSIZE_MAX;
#endif
	
    tv.tv_sec = 0;
    tv.tv_usec = 1000;

    FD_ZERO(&read_fd);
    FD_SET(privdata->fd, &read_fd);
    ret = select(privdata->fd + 1, &read_fd, NULL, NULL, &tv);
    if (ret == -1) {
	if (! (errno == EINTR || errno == EAGAIN))
	    error(errno, "AT2[%s]: error on select",octstr_get_cstr(privdata->name));
	return;
    }

    s = read(privdata->fd, buf,count);
    if(s > 0)
    	octstr_append_data(privdata->ilb, buf, s);
}

/******************************************************************************
** at2_wait_line
** looks for a full line to be read from the buffer
** returns the line and removes it from the buffer
** or if no full line is yet received
** waits until the line is there or a timeout occurs
** If gt_flag is set, it is also looking for
** a line containing > even there is no CR yet.
*/

Octstr *at2_wait_line(PrivAT2data *privdata, time_t timeout, int gt_flag)
{
    Octstr *line;
    time_t end_time;
    time_t cur_time;

    time(&end_time);
    if(timeout == 0)
    	timeout = 3;
    end_time += timeout;

    if(privdata->lines != NULL)
    	octstr_destroy(privdata->lines);
    privdata->lines = octstr_create("");
    while(time(&cur_time) <= end_time)
    {
    	line = at2_read_line(privdata, gt_flag);
    	if(line)
    	   return line;
    }
    return NULL;
}


/******************************************************************************
** at2_read_line
** looks for a full line to be read from the buffer
** returns the line and removes it from the buffer
** or if no full line is yet received
** returns NULL. If gt_flag is set, it is also looking for
** a line containing > even there is no CR yet.
*/

Octstr *at2_read_line(PrivAT2data *privdata, int gt_flag)
{
    int	 eol;
    int  gtloc;
    int len;
    Octstr *line;
    Octstr *buf2;
    int i;


    at2_read_buffer(privdata);

    len = octstr_len(privdata->ilb);
    if(len == 0)
    	return NULL;

    if(gt_flag)
        gtloc = octstr_search_char(privdata->ilb, '>', 0); /* looking for > if needed */
    else 
    	gtloc = -1;

 /*   if (gt_flag && (gtloc != -1))
	debug("bb.smsc.at2",0,"in at2_read_line with gt_flag=1, gtloc=%d, ilb=%s",gtloc,octstr_get_cstr(privdata->ilb));
*/
    eol = octstr_search_char(privdata->ilb, '\r', 0); /* looking for CR */

    if (  (gtloc != -1) && ( (eol == -1) || (eol > gtloc) ) )
    	eol = gtloc;

    if(eol == -1)
    	return NULL;

    line = octstr_copy(privdata->ilb, 0, eol);
    buf2 = octstr_copy(privdata->ilb, eol+1, len);
    octstr_destroy(privdata->ilb);
    privdata->ilb = buf2;

    /* remove any non printable chars (including linefeed for example */
    for (i=0; i< octstr_len(line); i++)
    {
	if (octstr_get_char(line, i) < 32)
	    octstr_set_char(line, i, ' ');
    }
    octstr_strip_blanks(line);
 
    if ((strcmp(octstr_get_cstr(line),"") == 0) && ( gt_flag == 0)) /* empty line, skipping */
    {
    	octstr_destroy(line);
	return NULL;
    }
    if((gt_flag) && (gtloc != -1))
    {
        /* debug("bb.smsc.at2", 0, "reappending >"); */
    	octstr_append_cstr(line,">"); /* got to re-add it again as the parser needs to see it */
    }
    debug("bb.smsc.at2", 0, "AT2[%s]: <-- %s", octstr_get_cstr(privdata->name),octstr_get_cstr(line));
    return line;
}

/******************************************************************************
** at_write_line_cstr
** write a line out to the device
** and adds a carriage return/linefeed to it
** returns number of characters sent.
*/

int  at2_write_line(PrivAT2data *privdata, char* line)
{
    int count;
    int s=0;
    Octstr *linestr = NULL;
    
    linestr = octstr_format("%s\r",line);
    
    debug("bb.smsc.at2", 0, "AT2[%s]: --> %s^M", octstr_get_cstr(privdata->name), line);
    
    count = octstr_len(linestr);
    s = write(privdata->fd, octstr_get_cstr(linestr), count);
    O_DESTROY(linestr);
    if( s < 0)
    {
   	debug("bb.smsc.at2", 0, "AT2[%s]: write failed with errno %d", octstr_get_cstr(privdata->name), errno);
   	return s;
    }
    tcdrain (privdata->fd);
    gwthread_sleep( (double) (privdata->modem == NULL ? 100 : privdata->modem->sendline_sleep) / 1000);
    return s;
}

/* #define		at2_write_ctrlz(a)	at2_write(a,"\032") */


int  at2_write_ctrlz(PrivAT2data *privdata)
{
    int s;
    char *ctrlz =  "\032" ;
  
    debug("bb.smsc.at2", 0, "AT2[%s]: --> ^Z", octstr_get_cstr(privdata->name));
    s = write(privdata->fd, ctrlz, 1);
    if( s < 0)
    {
   	debug("bb.smsc.at2", 0, "AT2[%s]: write failed with errno %d", octstr_get_cstr(privdata->name), errno);
   	return s;
    }
    tcdrain (privdata->fd);
    gwthread_sleep( (double) privdata->modem->sendline_sleep / 1000);
    return s;
}



int  at2_write(PrivAT2data *privdata, char* line)
{
    int count;
    int s;
   
    count = strlen(line);
    debug("bb.smsc.at2", 0, "AT2[%s]: --> %s", octstr_get_cstr(privdata->name), line);
    s = write(privdata->fd, line, count);
    tcdrain (privdata->fd);
    return s;
}


/******************************************************************************
** at2_flush_buffer
** clears incoming buffer
*/

void  at2_flush_buffer(PrivAT2data *privdata)
{
    at2_read_buffer(privdata);
    octstr_destroy(privdata->ilb);
    privdata->ilb = octstr_create("");
}


/******************************************************************************
** at2_init_device
** initializes the device after being opened
** detects the modem type, sets speed settings etc.
** on failure returns -1
*/


int	at2_init_device(PrivAT2data *privdata)
{
    int res;
    int ret;
    Octstr *setpin;

    info(0,"AT2[%s]: init device",octstr_get_cstr(privdata->name));

    at2_set_speed(privdata,privdata->speed);
    res = at2_send_modem_command(privdata,"AT",0,0);
    if(res == -1)
    {
        /* first try failed, maybe we need another one after just having changed the speed */
        res = at2_send_modem_command(privdata,"AT",0,0);
    }
    if(res == -1)
    {
    	error(0,"AT2[%s]: no answer from modem",octstr_get_cstr(privdata->name));
	return -1;
    }

    at2_flush_buffer(privdata);

    if(at2_send_modem_command(privdata, "AT&F", 0, 0) == -1)
	return -1;

    if(at2_send_modem_command(privdata, "ATE0", 0, 0) == -1)
	return -1;

    at2_flush_buffer(privdata);
    
    /* enable hardware handshake */
    if(octstr_len(privdata->modem->enable_hwhs)) {
	if(at2_send_modem_command(privdata, octstr_get_cstr(privdata->modem->enable_hwhs), 0, 0) == -1)
	    info(0,"AT2[%s]: cannot enable hardware handshake",octstr_get_cstr(privdata->name));
    }
 
    /* Check does the modem require a PIN and, if so, send it
     * This is not supported by the Nokia Premicell */
    if(! privdata->modem->no_pin)
    {
        ret = at2_send_modem_command(privdata, "AT+CPIN?", 10, 0);

	if (!privdata->pin_ready)
	{
	    if(ret == 2)
	    {
		if(privdata->pin == NULL)
		    return -1;
		setpin = octstr_format("AT+CPIN=%s", octstr_get_cstr(privdata->pin));
		ret = at2_send_modem_command(privdata, octstr_get_cstr(setpin), 0, 0);
		octstr_destroy(setpin);
		if(ret !=0 )
		    return -1;
	    } else if(ret == -1)
		return -1;
	}

 
 	/* we have to wait until +CPIN: READY appears before issuing
    	the next command. 10 sec should be suficient */
    	if(!privdata->pin_ready)
    	{
   	   at2_wait_modem_command(privdata,10, 0);
    	   if(!privdata->pin_ready) {
	       at2_send_modem_command(privdata, "AT+CPIN?", 10, 0);
	       if(!privdata->pin_ready) {
		   return -1; /* give up */
	       }
	   }
    	}
    } 
    // Set the GSM SMS message center address if supplied
    if (octstr_len(privdata->sms_center)) {
	Octstr *temp;
	temp = octstr_create("AT+CSCA=");
	octstr_append_char(temp, 34); // "
	octstr_append(temp, privdata->sms_center);
	octstr_append_char(temp, 34); // "
	/* XXX If some modem don't process the +, remove it and add ",145" 
	       and ",129" to national numbers
	*/
	ret = at2_send_modem_command(privdata, octstr_get_cstr(temp), 0, 0);
	octstr_destroy(temp);
	if(ret == -1)
	    return -1;
	if(ret > 0) {
	    info(0,"AT2[%s]: Cannot set SMS message center, continuing",octstr_get_cstr(privdata->name));
	}
    }

    /* Set the modem to PDU mode and autodisplay of new messages */
    ret = at2_send_modem_command(privdata, "AT+CMGF=0", 0, 0);
    if(ret !=0 )
    	return -1;
   
   /* lets see if it supports GSM SMS 2+ mode */
   ret = at2_send_modem_command(privdata, "AT+CSMS=?",0, 0);
   if(ret !=0)
   	privdata->phase2plus = 0; /* if it doesnt even understand the command, I'm sure it wont support it */
   else
   {
        /* we have to take a part a string like +CSMS: (0,1,128) */
   	Octstr *ts;
  	int i;
  	List *vals;
 
  	ts = privdata->lines;
  	privdata->lines = NULL;
  	
  	i = octstr_search_char(ts,'(',0);
        if (i>0)
	{
	   octstr_delete(ts,0,i+1);
	}
  	i = octstr_search_char(ts,')',0);
        if (i>0)
	{
	   octstr_truncate(ts,i);
	}
	vals = octstr_split(ts, octstr_imm(","));
	octstr_destroy(ts);
	ts = list_search(vals, octstr_imm("1"),(void *) octstr_item_match);
	if(ts)
	    privdata->phase2plus = 1;
	list_destroy(vals,(void *) at2_octstr_destroy);
    }
    if(privdata->phase2plus)
    {
    	info(0,"AT2[%s]: Phase 2+ is supported",octstr_get_cstr(privdata->name));
        ret = at2_send_modem_command(privdata, "AT+CSMS=1", 0, 0);
    	if(ret != 0)
    	    return -1;
    }

    /* Send Initstring */ 
     ret = at2_send_modem_command(privdata, octstr_get_cstr(privdata->modem->init_string), 0, 0);
    	if(ret != 0)
    	    return -1;
    info(0, "AT2[%s]: AT SMSC successfully opened.", octstr_get_cstr(privdata->name));
    return 0;
}
   

/******************************************************************************
** at2_send_modem_command
** sends an AT command to the modem and waits for a reply
** return values:
**  0 = OK
**  1 = ERROR
**  2 = SIM PIN
**  3 = >
**  4 = READY
**  5 = CMGS
** -1 = timeout occurred
*/

int at2_send_modem_command(PrivAT2data *privdata,char *cmd, time_t timeout, int gt_flag)
{
    at2_write_line(privdata,cmd);
    return at2_wait_modem_command(privdata, timeout, gt_flag);
}


/******************************************************************************
** at2_wait_modem_command
** waits for the modem to send us something
*/

int at2_wait_modem_command(PrivAT2data *privdata, time_t timeout, int gt_flag)
{
    Octstr *line = NULL;
    Octstr *line2 = NULL;
    Octstr *pdu = NULL;
    int ret;
    time_t end_time;
    time_t cur_time;
    Msg	*msg;
    int len;
 
    time(&end_time);
    if(timeout == 0)
    	timeout = 3;
    end_time += timeout;

    if(privdata->lines != NULL)
    	octstr_destroy(privdata->lines);
    privdata->lines = octstr_create("");
    while(time(&cur_time) <= end_time)
    {
        O_DESTROY(line);
    	line = at2_read_line(privdata, gt_flag);
    	if(line)
    	{
 	    octstr_append(privdata->lines,line);
	    octstr_append_cstr(privdata->lines,"\n");	

           if (-1 != octstr_search(line, octstr_imm("SIM PIN"), 0))
           {
           	ret = 2;
           	
           	goto end;
           }
           
           if (-1 != octstr_search(line, octstr_imm("OK"), 0))
           {
		ret = 0;
		goto end;
	   }
	   
 	   if ((gt_flag ) && (-1 != octstr_search(line, octstr_imm(">"), 0)))
           {
		ret = 1;
		goto end;
	   }
	   
           if (-1 != octstr_search(line, octstr_imm("RING"), 0))
           {
  	    	at2_write_line(privdata,"ATH0");
		continue;
	   }

           if (-1 != octstr_search(line, octstr_imm("+CPIN: READY"), 0))
           {
           	privdata->pin_ready = 1;
		continue;
	   }

	   if (-1 != octstr_search(line, octstr_imm("+CMS ERROR"),0))
	   {
		error(0,"AT2[%s]: CMS ERROR: %s", octstr_get_cstr(privdata->name), octstr_get_cstr(line));
	   	ret = 1;
	   	goto end;
	   }
           if (-1 != octstr_search(line, octstr_imm("+CMT:"), 0))
           {
           	line2 = at2_wait_line(privdata,1,0);
 
           	if(line2 == NULL)
           	{
		    error(0,"AT2[%s]: got +CMT but waiting for next line timed out", octstr_get_cstr(privdata->name));
		}
		else
		{
       		    octstr_append_cstr(line,"\n");
           	    octstr_append(line,line2);
           	    O_DESTROY(line2);
    		    at2_pdu_extract(privdata, &pdu, line);

		    if(pdu == NULL)
           	    {
		     	error(0,"AT2[%s]: got +CMT but pdu_extract failed", octstr_get_cstr(privdata->name));
		    }
		    else
		    {
			msg = at2_pdu_decode(pdu, privdata);
                    	if(msg != NULL)
                    	{
			    msg->sms.smsc_id = octstr_duplicate(privdata->conn->id);
                    	    bb_smscconn_receive(privdata->conn, msg);
                    	}
                    	if(privdata->phase2plus)
		    	    at2_write_line(privdata,"AT+CNMA");
			O_DESTROY(pdu);
		    }
                }
		continue;
	   }
 
           if (-1 != octstr_search(line, octstr_imm("ERROR"), 0))
           {
		ret = -1;
		goto end;
	   }
    	}
    }
    
    len = octstr_len(privdata->ilb);
/*
    error(0,"AT2[%s]: timeout. received <%s> until now, buffer size is %d, buf=%s",octstr_get_cstr(privdata->name),
    	 privdata->lines ? octstr_get_cstr(privdata->lines) : "<nothing>", len,
    	 privdata->ilb ? octstr_get_cstr(privdata->ilb) : "<nothing>");
*/
    O_DESTROY(line);
    O_DESTROY(line2);
    O_DESTROY(pdu);
    return -1; /* timeout */

end:
    octstr_append(privdata->lines,line);
    octstr_append_cstr(privdata->lines,"\n");
    O_DESTROY(line);
    O_DESTROY(line2);
    O_DESTROY(pdu);
    return ret;
}


/******************************************************************************
** at2_set_speed
** sets the serial port speed on the device
*/
void at2_set_speed(PrivAT2data *privdata, int bps)
{
    struct termios tios;
    int ret;
    int	speed;
    
    
    tcgetattr(privdata->fd, &tios);
  
    switch(bps)
    {
    case 300:
    	speed = B300;
    	break;
    case 1200:
    	speed = B1200;
    	break;
    case 2400:
    	speed = B2400;
    	break;
    case 4800:
    	speed = B4800;
    	break;
    case 9600:
    	speed = B9600;
    	break;
    case 19200:
    	speed = B19200;
    	break;
    case 38400:
    	speed = B38400;
    	break;
#ifdef	B57600
    case 57600:
    	speed = B57600;
    	break;
#endif
    default:
    	speed = B9600;
    }	  
    cfsetospeed(&tios, speed);
    cfsetispeed(&tios, speed);
    ret = tcsetattr(privdata->fd, TCSANOW, &tios); /* apply changes now */
    if(ret == -1){
        error(errno,"AT2[%s]: at_data_link: fail to set termios attribute",
	      octstr_get_cstr(privdata->name));
    }
    tcflush(privdata->fd, TCIOFLUSH);
  
    info(0,"AT2[%s]: speed set to %d",octstr_get_cstr(privdata->name), bps);
}


/******************************************************************************
** at2_device_thread
** this is the main tread "sitting" on the device
** its task is to initialize the modem
** then wait for messages to arrive or to be sent
**
*/

void at2_device_thread(void *arg)
{
    SMSCConn	*conn = arg;
    PrivAT2data	*privdata = conn->data;

    int l, wait=0;
    long idle_timeout;
   
    conn->status = SMSCCONN_CONNECTING;
    
reconnect:    

    do {
	if (wait) {
	    if (conn->status == SMSCCONN_ACTIVE) {
		mutex_lock(conn->flow_mutex);
		conn->status = SMSCCONN_RECONNECTING;
		mutex_unlock(conn->flow_mutex);
	    }
	    info(0, "AT2[%s]: waiting for %d %s before trying to "
		    "connect again", octstr_get_cstr(privdata->name),
		    (wait < 60 ? wait : wait/60),
	            (wait < 60 ? "seconds" : "minutes"));
	    gwthread_sleep(wait);
	    wait = wait > (privdata->retry ? 3600 : 600) ?
	    (privdata->retry ? 3600 : 600) : wait * 2;
	}
	else
	    wait = 15;

	if(privdata->speed == 0) {
	    if(-1 == at2_detect_speed(privdata)) {
		if(!privdata->retry)
		    return;
		else
		    continue;
	    }
	}
    	
	if(privdata->modem == NULL) {
	    if(-1 == at2_detect_modem_type(privdata)) {
		if(!privdata->retry)
		    return;
		else
		    continue;
	    }
	}
   
	if( at2_open_device(privdata) ) {
	    error(errno, "AT2[%s]: at2_device_thread: open_at2_device failed. Terminating",octstr_get_cstr(privdata->name));
	    if(!privdata->retry)
		return;
	    else
		continue;
	}

	if (at2_init_device(privdata) != 0) {
	    error(0, "AT2[%s]: Opening failed. Terminating",octstr_get_cstr(privdata->name));
	    if(!privdata->retry) {
		privdata->shutdown = 1;
		return;
	    } else
		continue;
	}

	/* If we got here, then the device is opened */
	break;
    } while(privdata->retry && ! privdata->shutdown); 
    
    conn->status = SMSCCONN_ACTIVE;
    bb_smscconn_connected(conn);

    idle_timeout=0;
    while (!privdata->shutdown)
    {
    	 l = list_len(privdata->outgoing_queue);
	if(l>0) {
	    at2_send_messages(privdata);
	    idle_timeout = time(NULL);
	} else
	    at2_wait_modem_command(privdata,1,0);

	if(privdata->keepalive && 
	   idle_timeout + privdata->keepalive < time(NULL)) {
	    if(at2_send_modem_command(privdata, octstr_get_cstr(privdata->modem->keepalive_cmd), 5, 0) < 0) { 
		at2_close_device(privdata);
		conn->status = SMSCCONN_RECONNECTING;
		wait = 15;
		goto reconnect;
	    }
	    idle_timeout = time(NULL);
	}
   }
    at2_close_device(privdata);
    conn->status = SMSCCONN_DISCONNECTED;
    /* maybe some cleanup here?*/
    at2_destroy_modem(privdata->modem);
    gw_free(conn->data);
    conn->data = NULL;
    conn->why_killed = SMSCCONN_KILLED_SHUTDOWN;
    conn->status = SMSCCONN_DEAD;
    bb_smscconn_killed();
}

int at2_shutdown_cb(SMSCConn *conn, int finish_sending)
{
    PrivAT2data *privdata = conn->data;

    debug("bb.sms", 0, "AT2[%s]: Shutting down SMSCConn, %s", 
	  octstr_get_cstr(privdata->name),
	  finish_sending ? "slow" : "instant");

    /* Documentation claims this would have been done by smscconn.c,
       but isn't when this code is being written. */
    conn->why_killed = SMSCCONN_KILLED_SHUTDOWN;
    privdata->shutdown = 1; /* Separate from why_killed to avoid locking, as
			   why_killed may be changed from outside? */

    if (finish_sending == 0)
    {
	Msg *msg;
	while((msg = list_extract_first(privdata->outgoing_queue)) != NULL) {
	    bb_smscconn_send_failed(conn, msg, SMSCCONN_FAILED_SHUTDOWN);
	}
    }
    gwthread_wakeup(privdata->device_thread);
    return 0;

}

long at2_queued_cb(SMSCConn *conn)
{
    PrivAT2data *privdata = conn->data;
    long ret = list_len(privdata->outgoing_queue);

    /* use internal queue as load, maybe something else later */

    conn->load = ret;
    return ret;
}

void at2_start_cb(SMSCConn *conn)
{
    PrivAT2data *privdata = conn->data;

    /* in case there are messages in the buffer already */
    gwthread_wakeup(privdata->device_thread);
    debug("smsc.at2", 0, "AT2[%s]: start called", 
	  octstr_get_cstr(privdata->name));
}

int at2_add_msg_cb(SMSCConn *conn, Msg *sms)
{
    PrivAT2data *privdata = conn->data;
    Msg *copy;

    copy = msg_duplicate(sms);
    list_produce(privdata->outgoing_queue, copy);
    gwthread_wakeup(privdata->device_thread);
    return 0;
}


/**********************************************************************
 * smsc_at2_create
 * starts the whole thing up
 *
 */

int  smsc_at2_create(SMSCConn *conn, CfgGroup *cfg)
{
    PrivAT2data	*privdata;
    Octstr *modem_type_string;
   
    privdata = gw_malloc(sizeof(PrivAT2data));
    privdata->outgoing_queue = list_create();

    privdata->configfile = cfg_get_configfile(cfg);

    privdata->device = cfg_get(cfg, octstr_imm("device"));
    if (privdata->device == NULL)
    {
	error(0, "AT2[-]: 'device' missing in at2 configuration.");
	goto error;
    }

    privdata->name = cfg_get(cfg, octstr_imm("smsc-id"));
    if(privdata->name == NULL) {
	privdata->name = octstr_duplicate(privdata->device);
    }

    privdata->speed = 0;
    cfg_get_integer(&privdata->speed, cfg, octstr_imm("speed"));

    privdata->keepalive = 0;
    cfg_get_integer(&privdata->keepalive, cfg, octstr_imm("keepalive"));

    cfg_get_bool(&privdata->retry, cfg, octstr_imm("retry"));
    privdata->my_number = cfg_get(cfg, octstr_imm("my-number"));
    privdata->sms_center = cfg_get(cfg, octstr_imm("sms-center"));
    modem_type_string = cfg_get(cfg, octstr_imm("modemtype"));

    privdata->modem = NULL;

    if(modem_type_string != NULL) {
	if(octstr_compare(modem_type_string, octstr_imm("auto")) == 0 ||
	   octstr_compare(modem_type_string, octstr_imm("autodetect")) == 0)
	    O_DESTROY(modem_type_string);
    }

    if(octstr_len(modem_type_string) == 0)
    {
        info(0,"AT2[%s]: configuration doesn't show modemtype. will autodetect",
	     octstr_get_cstr(privdata->name));
    }
    else
    {
        info(0,"AT2[%s]: configuration shows modemtype <%s>", 
	     octstr_get_cstr(privdata->name),
	     octstr_get_cstr(modem_type_string));
	privdata->modem = at2_read_modems(privdata, privdata->configfile,
		modem_type_string, 0);
	if(privdata->modem == NULL) {
	    info(0,"AT2[%s]: modemtype not found, revert to autodetect",
	    	octstr_get_cstr(privdata->name));
	} else {
	    info(0,"AT2[%s]: read modem definition for <%s>",
		 octstr_get_cstr(privdata->name),
		 octstr_get_cstr(privdata->modem->name));
	    if (privdata->speed == 0)
		privdata->speed = privdata->modem->speed;
	}
	O_DESTROY(modem_type_string);
    }

    privdata->ilb = octstr_create("");
    privdata->fd = -1;
    privdata->lines = NULL; 
    privdata->pin = cfg_get(cfg, octstr_imm("pin"));
    privdata->pin_ready = 0;
    privdata->conn = conn;
    privdata->phase2plus = 0;
    privdata->validityperiod = cfg_get(cfg, octstr_imm("validityperiod"));

    cfg_get_bool(&privdata->alt_dcs, cfg, octstr_imm("alt-dcs"));
   
    conn->data = privdata;
    conn->name = octstr_format("AT2[%s]", octstr_get_cstr(privdata->name));

    privdata->shutdown = 0;

    conn->status = SMSCCONN_CONNECTING;
    conn->connect_time = time(NULL);

    if((privdata->device_thread = gwthread_create(at2_device_thread, conn)) == -1)
    {
    	privdata->shutdown = 1;
	goto error;
    }

    conn->shutdown = at2_shutdown_cb;
    conn->queued = at2_queued_cb;
    conn->start_conn = at2_start_cb;
    conn->send_msg = at2_add_msg_cb;
    return 0;

error:
    error(0, "AT2[%s]: Failed to create at2 smsc connection",
	  octstr_len(privdata->name) ? octstr_get_cstr(privdata->name) : "");
    if (privdata != NULL)
    {
	list_destroy(privdata->outgoing_queue, NULL);
    }
    gw_free(privdata);
    conn->why_killed = SMSCCONN_KILLED_CANNOT_CONNECT;
    conn->status = SMSCCONN_DEAD;
    info(0, "AT2[%s]: exiting", octstr_get_cstr(privdata->name));
    return -1;
}




/******************************************************************************
 * Extract the first PDU in the string
 */
 
int at2_pdu_extract(PrivAT2data *privdata, Octstr **pdu, Octstr *line)
{
    Octstr *buffer;
    long len = 0;
    int pos = 0;
    int tmp;

    buffer = octstr_duplicate(line);
    /* find the beginning of a message from the modem*/ 
    pos = octstr_search(buffer, octstr_imm("+CMT:"), 0);
    if(pos == -1) 
	goto nomsg;
    pos += 5;

    tmp = octstr_search(buffer, octstr_imm(","), pos);
    if(! privdata->modem->broken && tmp == -1)
	goto nomsg;
    if(tmp != -1)
	pos=tmp + 1;

    /* The message length is after the comma */
    pos = octstr_parse_long(&len, buffer, pos, 10);
    if(pos == -1)
	goto nomsg;

    /* skip the spaces and line return */
    while( isspace(octstr_get_char(buffer, pos)))
	pos++;
    
    /* skip the SMSC address on some modem types */
    if(! privdata->modem->no_smsc)
    {
	tmp = at2_hexchar(octstr_get_char(buffer, pos))*16
	    + at2_hexchar(octstr_get_char(buffer, pos+1));
	if (tmp < 0)
	    goto nomsg;
	tmp = 2 + tmp * 2;
	pos += tmp;
    }
    
    /* check if the buffer is long enough to contain the full message */
    if( ! privdata->modem->broken && octstr_len(buffer) < len * 2 + pos)
	goto nomsg;

    if( privdata->modem->broken && octstr_len(buffer) < len * 2)
	goto nomsg;
    
    /* copy the PDU then remove it from the input buffer*/
    *pdu = octstr_copy(buffer, pos, len*2);

    octstr_destroy(buffer);
    return 1;

 nomsg:
    octstr_destroy(buffer);
    return 0;
}

/**********************************************************************
 * Get the numeric value of the text hex
 */
int at2_hexchar(int hexc)
{
    hexc = toupper(hexc) - 48;
    return (hexc>9) ? hexc-7 : hexc;
}



/******************************************************************************
 * Decode a raw PDU into a Msg
 */
Msg *at2_pdu_decode(Octstr *data, PrivAT2data *privdata)
{
        int type;
        Msg *msg = NULL;

        /* Get the PDU type */
        type = octstr_get_char(data, 1) & 3;
        
        switch(type) {

        case AT_DELIVER_SM:
                msg = at2_pdu_decode_deliver_sm(data, privdata);
                break;

                /* Add other message types here: */
        
        }

        return msg;
}

/******************************************************************************
 * Decode a DELIVER PDU
 */
Msg *at2_pdu_decode_deliver_sm(Octstr *data, PrivAT2data *privdata)
{
        int len, pos, i, ntype;
        int udhi, dcs, udhlen, pid;
        Octstr *origin = NULL;
        Octstr *udh = NULL;
        Octstr *text = NULL, *tmpstr;
        Octstr *pdu = NULL;
        Msg *message = NULL;
        struct universaltime mtime;     /* time structure */
        long stime;             /* time in seconds */

        /* Note: some parts of the PDU are not decoded because they are
         * not needed for the Msg type. */

        /* convert the pdu to binary format for ease of processing */
        pdu = at2_convertpdu(data);
        
        /* UDH Indicator */
        udhi = (octstr_get_char(pdu, 0) & 64) >> 6;

        /* originating address */
        len = octstr_get_char(pdu, 1);
	ntype = octstr_get_char(pdu, 2);

        pos = 3;
	if((ntype & 0xD0) == 0xD0) {
	    /* Alphanumeric sender */
	    origin = octstr_create("");
	    tmpstr = octstr_copy(pdu, 3, len);
	    at2_decode7bituncompressed(tmpstr, (((len - 1) * 4 - 3)/7) + 1, origin, 0);
	    octstr_destroy(tmpstr);
	    debug("bb.smsc.at2", 0, "AT2[%s]: Alphanumeric sender <%s>", octstr_get_cstr(privdata->name), octstr_get_cstr(origin));
	    pos += (len + 1) / 2;
	} else {
	    origin = octstr_create("");
	    if((ntype & 0x90) == 0x90) {
		/* International number */
		octstr_append_char(origin, '+');
	    }
	    for(i=0; i<len; i+=2, pos++) {
		octstr_append_char(origin, (octstr_get_char(pdu, pos) & 15) + 48);
		if(i+1 < len)
		    octstr_append_char(origin, (octstr_get_char(pdu, pos) >> 4) + 48);
	    }
	    debug("bb.smsc.at2", 0, "AT2[%s]: Numeric sender %s <%s>", octstr_get_cstr(privdata->name), ((ntype & 0x90) == 0x90 ? "(international)" : ""), octstr_get_cstr(origin));
	}

        /* PID */
	pid = octstr_get_char(pdu, pos); 
        pos++;
	
        /* DCS */
	dcs = octstr_get_char(pdu, pos); 
        pos++;
        
        /* get the timestamp */
        mtime.year   = octstr_get_char(pdu, pos) + 1900; pos++;
        mtime.month  = octstr_get_char(pdu, pos); pos++;
        mtime.day    = octstr_get_char(pdu, pos); pos++;
        mtime.hour   = octstr_get_char(pdu, pos); pos++;
        mtime.minute = octstr_get_char(pdu, pos); pos++;
        mtime.second = octstr_get_char(pdu, pos); pos++;
        /* time zone: */
        /* XXX handle negative time zones */
        mtime.hour  += octstr_get_char(pdu, pos); pos++;
        stime = date_convert_universal(&mtime);
        
        /* get data length */
        len = octstr_get_char(pdu, pos);
        pos++;

        /* if there is a UDH */
    	udhlen = 0;
        if(udhi) {
                udhlen = octstr_get_char(pdu, pos);
                pos++;
                udh = octstr_copy(pdu, pos, udhlen);
                pos += udhlen;
                len -= udhlen +1;
        }

        /* build the message */         
        message = msg_create(sms);
	if (!dcs_to_fields(&message, dcs)) {
	    /* XXX Should reject this message ? */
	    debug("bb.smsc.at2", 0, "AT2[%s]: Invalid DCS",
		  octstr_get_cstr(privdata->name));
	    dcs_to_fields(&message, 0);
	}

	message->sms.pid = pid;

        /* deal with the user data -- 7 or 8 bit encoded */     
        tmpstr = octstr_copy(pdu,pos,len);
        if(message->sms.coding == DC_8BIT || message->sms.coding == DC_UCS2) {
                text = octstr_duplicate(tmpstr);
        } else {
                int offset=0;
                text = octstr_create("");
                if (udhi && message->sms.coding == DC_7BIT) {
                        int nbits;
                        nbits = (udhlen + 1)*8;
                        offset = (((nbits/7)+1)*7-nbits)%7;     /* Fill bits for UDH to septet boundary */
                }
                at2_decode7bituncompressed(tmpstr, len, text, offset);
        }

        message->sms.sender = origin;
	if(octstr_len(privdata->my_number)) {
	    message->sms.receiver = octstr_duplicate(privdata->my_number);
	} else {
            /* Put a dummy address in the receiver for now (SMSC requires one) */
	    message->sms.receiver = octstr_create_from_data("1234", 4);
	}
        if (udhi) {
                message->sms.udhdata = udh;
        }
        message->sms.msgdata = text;
        message->sms.time = stime;

        /* cleanup */
        octstr_destroy(pdu);
        octstr_destroy(tmpstr);
        
        return message;
}

/******************************************************************************
 * Converts the text representation of hexa to binary
 */
Octstr *at2_convertpdu(Octstr *pdutext)
{
    Octstr *pdu;
    int i;
    int len = octstr_len(pdutext);

    pdu = octstr_create("");
    for (i=0; i<len; i+=2)
    {
    	octstr_append_char(pdu, at2_hexchar(octstr_get_char(pdutext,i))*16 
                              + at2_hexchar(octstr_get_char(pdutext,i+1)));
    }
    return pdu;
}


/**********************************************************************
 * Decode 7bit uncompressed user data
 */
int at2_rmask[8] = { 0, 1, 3, 7, 15, 31, 63, 127 };
int at2_lmask[8] = { 0, 128, 192, 224, 240, 248, 252, 254 };

void at2_decode7bituncompressed(Octstr *input, int len, Octstr *decoded, int offset)
{
        unsigned char septet, octet, prevoctet;
        int i;
        int r = 1;
        int c = 7;
        int pos = 0;
        
        /* Shift the buffer offset bits to the left */
        if (offset > 0) {
                unsigned char *ip;
                for (i = 0, ip = octstr_get_cstr(input); i < octstr_len(input); i++) {
                        if (i == octstr_len(input) - 1)
                                *ip = *ip >> offset;
                        else
                                *ip = (*ip >> offset) | (*(ip + 1) << (8 - offset));
                        ip++;
                }
        }
        octet = octstr_get_char(input, pos);
        prevoctet = 0;
        for(i=0; i<len; i++) {
                septet = ((octet & at2_rmask[c]) << (r-1)) + prevoctet;
                octstr_append_char(decoded, septet);

                prevoctet = (octet & at2_lmask[r]) >> c;
        
                /* When r=7 we have a full character in prevoctet*/
                if((r==7) && (i<len-1)){
                        i++;
                        octstr_append_char(decoded, prevoctet);
                        prevoctet = 0;
                }

                r = (r>6)? 1 : r+1;
                c = (c<2)? 7 : c-1;

                pos++;
                octet = octstr_get_char(input, pos);
        }
        charset_gsm_to_latin1(decoded);
}



/******************************************************************************
 * Sends messages from the queue
 */

void at2_send_messages(PrivAT2data *privdata)
{
    Msg *msg;
    
    do
    {
	if(list_len(privdata->outgoing_queue) > 1)
	    at2_send_modem_command(privdata, "AT+CMMS=2", 0, 0);
    	if( ( msg = list_extract_first(privdata->outgoing_queue )))
	    at2_send_one_message(privdata, msg);
    } while (msg);
}

/******************************************************************************
** sends a single message
** after having it sent, the msg is no longe belonging to us
*/
void at2_send_one_message(PrivAT2data *privdata, Msg *msg)
{
    unsigned char command[500], pdu[500];
    int ret = -1; 
    char sc[3];
    int retries = RETRY_SEND;
   
    if(octstr_len(privdata->my_number)) {
	octstr_destroy(msg->sms.sender);
	msg->sms.sender = octstr_duplicate(privdata->my_number);
    }
   /* the standard says you should be prepending the PDU with 00 to indicate to use the default SC.
      some older modems dont expect this so it can be disabled 
     * NB: This extra padding is not counted in the CMGS byte count */

    sc[0] = '\0';

    if(! privdata->modem->no_smsc)
    	strcpy(sc, "00");

    if(msg_type(msg)==sms)
    {
   	at2_pdu_encode(msg, &pdu[0], privdata);
   	
        ret = -99;
        retries = RETRY_SEND;
        while ((ret != 0) && (retries-- > 0))
        {
	    /* send the initial command and then wait for > */
    	    sprintf(command, "AT+CMGS=%d", strlen(pdu)/2);
    	    ret = at2_send_modem_command(privdata, command, 5, 1);
	    debug("bb.at", 0, "AT2[%s]: send command status: %d", 
		  octstr_get_cstr(privdata->name), ret);
    	    if(ret != 1) /* > only! */
    	    	continue;
    	    /* ok the > has been see now so we can send the PDU now and a control Z but no CR or LF */
	    sprintf(command, "%s%s", sc, pdu);
	    at2_write(privdata,command);
	    at2_write_ctrlz(privdata);
        /* wait 20 secs for modem command */
	    ret = at2_wait_modem_command(privdata, 20, 0);
	    debug("bb.at", 0, "AT2[%s]: send command status: %d", 
		  octstr_get_cstr(privdata->name), ret);
	    if(ret != 0) /* OK only */
	    	continue;
	    counter_increase(privdata->conn->sent);
	    bb_smscconn_sent(privdata->conn, msg);
	}
	if(ret != 0)
	{
		/* 
         * no need to do counter_increase(privdata->conn->failed) here, 
         * since bb_smscconn_send_failed() will inc the counter on 
         * SMSCCONN_FAILED_MALFORMED
         */
	    bb_smscconn_send_failed(privdata->conn,msg,SMSCCONN_FAILED_MALFORMED);
	}
    }
}


/******************************************************************************
 * Encode a Msg into a PDU
 */
int at2_pdu_encode(Msg *msg, unsigned char *pdu, PrivAT2data *privdata)
{
    int pos = 0, i,len, setvalidity=0;
    int ntype = PNT_UNKNOWN; /* number type default */
    int nstartpos = 0;   /* offset for the phone number */
    int dcs;  /* data coding scheme (GSM 03.38) */
    
    /* The message is encoded directly in the text representation of 
     * the hex values that will be sent to the modem.
     * Each octet is coded with two characters. */
    
    /* message type SUBMIT
     *    01010001 = 0x51 indicating add. UDH, TP-VP(Rel) & MSG_SUBMIT
     * or 00010001 = 0x11 for just TP-VP(Rel) & MSG_SUBMIT */

    pdu[pos] = octstr_len(msg->sms.udhdata) ? at2_numtext(5) : at2_numtext(1);
    pos++;
    pdu[pos] = at2_numtext(AT_SUBMIT_SM);
    pos++;
    
    /* message reference (0 for now) */
    pdu[pos] = at2_numtext(0);
    pos++;
    pdu[pos] = at2_numtext(0);
    pos++;
    
    /* destination address */
    octstr_strip_blanks(msg->sms.receiver); /* strip blanks before length calculation */
    len = octstr_len(msg->sms.receiver);

    /* Check for international numbers
     * number starting with '+' or '00' are international,
     * others are national. */
    if (strncmp(octstr_get_cstr(msg->sms.receiver), "+", 1) == 0) {
        debug("bb.smsc.at2", 0, "AT2[%s]: international starting with + (%s)",
	      octstr_get_cstr(privdata->name), octstr_get_cstr(msg->sms.receiver) );
        nstartpos++;
        ntype = PNT_INTER; /* international */
    } else if (strncmp(octstr_get_cstr(msg->sms.receiver), "00", 2) == 0) {
        debug("bb.smsc.at2", 0, "AT2[%s]: international starting with 00 (%s)",
	      octstr_get_cstr(privdata->name), octstr_get_cstr(msg->sms.receiver) );
        nstartpos += 2;
        ntype = PNT_INTER; /* international */
    }
    
    /* address length */
    pdu[pos] = at2_numtext(((len - nstartpos) & 240) >> 4);
    pos++;
    pdu[pos] = at2_numtext((len - nstartpos) & 15);
    pos++;
            
    /* Type of number */
    pdu[pos] = at2_numtext(8 + ntype);
    pos++;
    /* numbering plan: ISDN/Telephone numbering plan */
    pdu[pos] = at2_numtext(1);
    pos++;
    
    /* make sure there is no blank in the phone number and encode
     * an even number of digits */
    octstr_strip_blanks(msg->sms.receiver);
    for(i=nstartpos; i<len; i+=2) {
        if (i+1 < len) {
            pdu[pos] = octstr_get_char(msg->sms.receiver, i+1);
        } else {
            pdu[pos] = at2_numtext (15);
        }
        pos++;
        pdu[pos] = octstr_get_char(msg->sms.receiver, i);
        pos++;
    }
    
    /* protocol identifier */
    /* 0x00 implicit */
    pdu[pos] = at2_numtext((msg->sms.pid & 240) >> 4);
    pos++;
    pdu[pos] = at2_numtext(msg->sms.pid & 15);
    pos++;
    
    /* data coding scheme */
    dcs = fields_to_dcs(msg, 
	(msg->sms.alt_dcs ? 2- msg->sms.alt_dcs : privdata->alt_dcs));

    pdu[pos] = at2_numtext(dcs >> 4);
    pos++;
    pdu[pos] = at2_numtext(dcs % 16);
    pos++;

    /* Validity-Period (TP-VP)
     * see GSM 03.40 section 9.2.3.12
     * defaults to 24 hours = 167 if not set */
    if ( msg->sms.validity) {
        if (msg->sms.validity > 635040)
        setvalidity = 255;
        if (msg->sms.validity >= 50400 && msg->sms.validity <= 635040)
        setvalidity = (msg->sms.validity - 1) / 7 / 24 / 60 + 192 + 1;
        if (msg->sms.validity > 43200 && msg->sms.validity < 50400)
        setvalidity = 197;
        if (msg->sms.validity >= 2880 && msg->sms.validity <= 43200)
        setvalidity = (msg->sms.validity - 1) / 24 / 60 + 166 + 1;
        if (msg->sms.validity > 1440 && msg->sms.validity < 2880)
        setvalidity = 168;
        if (msg->sms.validity >= 750 && msg->sms.validity <= 1440)
        setvalidity = (msg->sms.validity - 720 - 1) / 30 + 143 + 1;
        if (msg->sms.validity > 720 && msg->sms.validity < 750)
        setvalidity = 144;
        if (msg->sms.validity >= 5 && msg->sms.validity <= 720)
        setvalidity = (msg->sms.validity - 1) / 5 - 1 + 1;
        if (msg->sms.validity < 5)
        setvalidity = 0;
    } else 
        setvalidity = (privdata->validityperiod != NULL ? atoi(octstr_get_cstr(privdata->validityperiod)) : 167);
    
    if (setvalidity >= 0 && setvalidity <= 143)
        debug("bb.smsc.at2", 0, "AT2[%s]: TP-Validity-Period: %d minutes", 
              octstr_get_cstr(privdata->name), (setvalidity+1)*5);
    else if (setvalidity >= 144 && setvalidity <= 167)
        debug("bb.smsc.at2", 0, "AT2[%s]: TP-Validity-Period: %3.1f hours", 
              octstr_get_cstr(privdata->name), ((float)(setvalidity-143)/2)+12);
    else if (setvalidity >= 168 && setvalidity <= 196)
        debug("bb.smsc.at2", 0, "AT2[%s]: TP-Validity-Period: %d days", 
              octstr_get_cstr(privdata->name), (setvalidity-166));
    else 
        debug("bb.smsc.at2", 0, "AT2[%s]: TP-Validity-Period: %d weeks", 
              octstr_get_cstr(privdata->name), (setvalidity-192));
    pdu[pos] = at2_numtext((setvalidity & 240) >> 4);
    pos++;
    pdu[pos] = at2_numtext(setvalidity & 15);
    pos++;

    /* user data length - include length of UDH if it exists */
	len = sms_msgdata_len(msg);

    if(octstr_len(msg->sms.udhdata)) {
        if (msg->sms.coding == DC_8BIT || msg->sms.coding == DC_UCS2) {
            len += octstr_len(msg->sms.udhdata);
        } else {
            /*
             * The reason we branch here is because UDH data length is determined
             * in septets if we are in GSM coding, otherwise it's in octets. Adding 6
             * will ensure that for an octet length of 0, we get septet length 0,
             * and for octet length 1 we get septet length 2. 
             */
            len += (((8*octstr_len(msg->sms.udhdata)) + 6)/7);        
    }
    }

    pdu[pos] = at2_numtext((len & 240) >> 4);
    pos++;
    pdu[pos] = at2_numtext(len & 15);
    pos++;
    
    /* udh */
    if(octstr_len(msg->sms.udhdata)) {
            pos += at2_encode8bituncompressed(msg->sms.udhdata, &pdu[pos]);
    }

    /* user data */
    /* if the data is too long, it is cut */
    if(msg->sms.coding == DC_8BIT || msg->sms.coding == DC_UCS2) {
        pos += at2_encode8bituncompressed(msg->sms.msgdata, &pdu[pos]);
    } else {
        int offset=0;

        if (octstr_len(msg->sms.udhdata)) {                   /* Have UDH */
            int nbits = octstr_len(msg->sms.udhdata)*8; /* Includes UDH length byte */
            offset = (((nbits/7)+1)*7-nbits)%7;         /* Fill bits */
        }
        pos += at2_encode7bituncompressed(msg->sms.msgdata, &pdu[pos],offset);
    }
    pdu[pos] = 0;

    return 0;
}



/**********************************************************************
 * Encode 7bit uncompressed user data
 */
int at2_encode7bituncompressed(Octstr *input, unsigned char *encoded,int offset)
{

        unsigned char prevoctet, tmpenc;
        int i;
        int c = 1;
        int r = 7;
        int pos = 0;
        int len;
        unsigned char enc7bit[256];
        int j,encpos = 0;
	int ermask[8] = { 0, 1, 3, 7, 15, 31, 63, 127 };
	int elmask[8] = { 0, 64, 96, 112, 120, 124, 126, 127 };

        charset_latin1_to_gsm(input);
        len = octstr_len(input);

        /* prevoctet is set to the first character and we'll start the loop
         * at the following char. */
        prevoctet = octstr_get_char(input ,0);
        for(i=1; i<octstr_len(input); i++) {
                /* a byte is encoded with what is left of the previous character
                 * and filled with as much as possible of the current one. */
                tmpenc = prevoctet + ((octstr_get_char(input,i) & ermask[c]) << r);
                enc7bit[encpos] = tmpenc; encpos++;
                c = (c>6)? 1 : c+1;
                r = (r<2)? 7 : r-1;

                /* prevoctet becomes the part of the current octet that hasn't
                 * been copied to 'encoded' or the next char if the current has
                 * been completely copied already. */
                prevoctet = (octstr_get_char(input,i) & elmask[r]) >> (c-1);
                if(r == 7) {
                        i++;
                        prevoctet = octstr_get_char(input, i);
                }
        }

        /* if the length of the message is a multiple of 8 then we
         * are finished. Otherwise prevoctet still contains part of a 
         * character so we add it. */
        if((len/8)*8 != len) {
                enc7bit[encpos] = prevoctet;encpos++;
        }

        /* Now shift the buffer by the offset */
        if (offset > 0) {
                unsigned char nextdrop, lastdrop;

    	    	nextdrop = lastdrop = 0;
                for (i = 0; i < encpos; i++) {
                        nextdrop = enc7bit[i] >> (8 - offset);          /* This drops off by shifting */
                        if (i == 0)
                                enc7bit[i] = enc7bit[i] << offset;              /* This drops off by shifting */
                        else
                                enc7bit[i] = (enc7bit[i] << offset) | lastdrop;
                        lastdrop = nextdrop;
                }

                if (offset > ((len*7) % 8)) {
                        enc7bit [i] = nextdrop;
                        i++;
                }
        }
        else
                i = encpos;

        for (j = 0; j < i; j++) {
                encoded[pos] = at2_numtext((enc7bit [j] & 240) >> 4); pos++;
                encoded[pos] = at2_numtext(enc7bit [j] & 15); pos++;
        }
        return pos;
                
}


/**********************************************************************
 * Encode 8bit uncompressed user data
 */

int at2_encode8bituncompressed(Octstr *input, unsigned char *encoded)
{
    int len, i;

    len = octstr_len(input);
        
    for(i=0; i<len; i++) {
           /* each character is encoded in its hex representation (2 chars) */
           encoded[i*2] = at2_numtext((octstr_get_char(input, i) & 240) >> 4);
          encoded[i*2+1] = at2_numtext(octstr_get_char(input, i) & 15);
    }
    return len*2;
}


/**********************************************************************
 * Code a half-byte to its text hexa representation
 */
int at2_numtext(int num)
{
        return (num > 9) ? (num+55) : (num+48);
}





/**********************************************************************
 * at2_detect_speed
 * try to detect modem speeds
 */

int at2_detect_speed(PrivAT2data *privdata)
{

#ifdef	B57600
    int autospeeds[] = { 57600, 38400, 19200, 9600 };
#else
    int autospeeds[] = { 38400, 19200, 9600 };
#endif
    int i;
    int res;
    
    debug("bb.smsc.at2",0,"AT2[%s]: detecting modem speed. ",octstr_get_cstr(privdata->name));

    for(i=0;i< (sizeof(autospeeds) / sizeof(int));i++)
    {
	if(-1 == at2_open_device1(privdata)) 
	    return -1;

	at2_set_speed(privdata,autospeeds[i]);
        res = at2_send_modem_command(privdata,"",1,0); /* send a return so the modem can detect the speed */

  	res = at2_send_modem_command(privdata,"AT",0,0);
    	if(res !=0)
    	    res = at2_send_modem_command(privdata,"AT",0,0);
    	if(res != 0)
    	    res = at2_send_modem_command(privdata,"AT",0,0);
	if(res == 0)
	{
	    privdata->speed = autospeeds[i];
	    i = 99; /* skip out of loop */
	}
	at2_close_device(privdata);
    }
    if(privdata->speed == 0) {
	info(0, "AT2[%s]: cannot detect speed",octstr_get_cstr(privdata->name));
	return -1;
    }
    info(0, "AT2[%s]: detect speed is %ld",octstr_get_cstr(privdata->name),privdata->speed);
    return 0;
}

/**********************************************************************
 * at2_detect_modem_type
 * try to detect speed
 */

int at2_detect_modem_type(PrivAT2data *privdata)
{
    int res;
    ModemDef *modem;
    int i;
    
    debug("bb.smsc.at2",0,"AT2[%s]: detecting modem type",octstr_get_cstr(privdata->name));
 
    if(-1 == at2_open_device1(privdata))
	return -1;
    at2_set_speed(privdata,privdata->speed);
    
    res = at2_send_modem_command(privdata,"",1,0); /* send a return so the modem can detect the speed */

    res = at2_send_modem_command(privdata,"AT",0,0);

    if(at2_send_modem_command(privdata, "AT&F", 0, 0) == -1)
	return -1;

    if(at2_send_modem_command(privdata, "ATE0", 0, 0) == -1)
	return -1;

    at2_flush_buffer(privdata);
    
    if(at2_send_modem_command(privdata, "ATI", 0, 0) == -1)
	return -1;

    /* we try to detect the modem automatically */


    i=1;
    while(NULL != (modem = at2_read_modems(privdata, privdata->configfile, NULL, i++))) {

	if(octstr_len(modem->detect_string) == 0) {
	    at2_destroy_modem(modem);
	    continue; 
	}

    	/* debug("bb.smsc.at2",0,"AT2[%s]: searching for %s",octstr_get_cstr(privdata->name), octstr_get_cstr(modem->name)); */

	if (-1 != octstr_search(privdata->lines, modem->detect_string, 0)) {
	    if(octstr_len(modem->detect_string2) == 0) {
		debug("bb.smsc.at2",0,"AT2[%s]: found string <%s>, using modem definition <%s>",octstr_get_cstr(privdata->name), octstr_get_cstr(modem->detect_string), octstr_get_cstr(modem->name));
		privdata->modem = modem;
		break;
	    } else {
		if (-1 != octstr_search(privdata->lines, modem->detect_string2, 0)) {
		    debug("bb.smsc.at2",0,"AT2[%s]: found string <%s> plus <%s>, using modem definition <%s>",octstr_get_cstr(privdata->name), octstr_get_cstr(modem->detect_string), octstr_get_cstr(modem->detect_string2), octstr_get_cstr(modem->name));
		    privdata->modem = modem;
		    break;
		}
	    } 
	}
    }

    if(privdata->modem == NULL) {
	debug("bb.smsc.at2",0,"AT2[%s]: Cannot detect modem, using generic", octstr_get_cstr(privdata->name));
	if(NULL == (modem = at2_read_modems(privdata, privdata->configfile, octstr_imm("generic"), 0))) {
	    panic(0,"AT2[%s]: Cannot detect modem and generic not found", octstr_get_cstr(privdata->name));
	} else {
	    privdata->modem = modem;
	}
    }

    /* lets see if it supports GSM SMS 2+ mode */
   res = at2_send_modem_command(privdata, "AT+CSMS=?",0, 0);
   if(res !=0)
   	privdata->phase2plus = 0; /* if it doesnt even understand the command, I'm sure it wont support it */
   else
   {
        /* we have to take a part a string like +CSMS: (0,1,128) */
   	Octstr *ts;
  	int i;
  	List *vals;
 
  	ts = privdata->lines;
  	privdata->lines = NULL;
  	
  	i = octstr_search_char(ts,'(',0);
        if (i>0)
	{
	   octstr_delete(ts,0,i+1);
	}
  	i = octstr_search_char(ts,')',0);
        if (i>0)
	{
	   octstr_truncate(ts,i);
	}
	vals = octstr_split(ts, octstr_imm(","));
	octstr_destroy(ts);
	ts = list_search(vals, octstr_imm("1"),(void *) octstr_item_match);
	if(ts)
	    privdata->phase2plus = 1;
	list_destroy(vals,(void *) at2_octstr_destroy);
    }
    if(privdata->phase2plus)
    	info(0,"AT2[%s]: Phase 2+ is supported",octstr_get_cstr(privdata->name));
    at2_close_device(privdata);
    return 0;
}

/* Use id and idnumber=0 or id=NULL and idnumber > 0 */
ModemDef *at2_read_modems(PrivAT2data *privdata, Octstr *file, Octstr *id, int idnumber) {

    Cfg *cfg;
    List *grplist;
    CfgGroup *grp;
    Octstr *p;
    ModemDef *modem;
    int i=1;

    if(octstr_len(id) == 0 && idnumber == 0)
	return NULL;

    if(idnumber == 0)
	debug("bb.smsc.at2",0,"AT2[%s]: Reading modem definitions from <%s>", octstr_get_cstr(privdata->name), octstr_get_cstr(file));
    cfg = cfg_create(file);

    if(cfg_read(cfg) == -1)
	panic(0, "Cannot read modem definition file");

    grplist = cfg_get_multi_group(cfg, octstr_imm("modems"));
    if(idnumber == 0)
	debug("bb.smsc.at2",0,"AT2[%s]: Found <%d> modems in config", octstr_get_cstr(privdata->name), list_len(grplist));

    if(grplist == NULL)
	panic(0, "Where are the modem definitions ?!?!");

    grp = NULL;
    while (grplist && (grp = list_extract_first(grplist)) != NULL) {
	p = cfg_get(grp, octstr_imm("id"));
	if(p == NULL) {
		info(0, "Modems group without id, bad");
		continue;
	}
	/* Check by id */
	if (octstr_len(id) != 0 && octstr_compare(p, id) == 0) {
	    O_DESTROY(p);
	    break;
	}
	/* Check by idnumber */
	if (octstr_len(id) == 0 && idnumber == i) {
	    O_DESTROY(p);
	    break;
	}
	O_DESTROY(p);
	i++;
	grp = NULL;
    }
    if(grplist != NULL)
	list_destroy(grplist, NULL);

    if(grp != NULL) {
	modem = gw_malloc(sizeof(ModemDef));

	modem->id = cfg_get(grp, octstr_imm("id"));

	modem->name = cfg_get(grp, octstr_imm("name"));
	if(modem->name == NULL)
	    modem->name = octstr_duplicate(modem->id);

	modem->detect_string = cfg_get(grp, octstr_imm("detect-string"));
	modem->detect_string2 = cfg_get(grp, octstr_imm("detect-string2"));

	modem->init_string = cfg_get(grp, octstr_imm("init-string"));
	if(modem->init_string == NULL)
	    modem->init_string = octstr_create("AT+CNMI=1,2,0,0,0");

	modem->speed = 9600;
	cfg_get_integer(&modem->speed, grp, octstr_imm("speed"));

	cfg_get_bool(&modem->need_sleep, grp, octstr_imm("need-sleep"));

	modem->enable_hwhs = cfg_get(grp, octstr_imm("enable-hwhs"));
	if(modem->enable_hwhs == NULL)
	    modem->enable_hwhs = octstr_create("AT+IFC=2,2");

	cfg_get_bool(&modem->no_pin, grp, octstr_imm("no-pin"));

	cfg_get_bool(&modem->no_smsc, grp, octstr_imm("no-smsc"));

	modem->sendline_sleep=100;
	cfg_get_integer(&modem->sendline_sleep, grp, octstr_imm("sendline-sleep"));

	modem->keepalive_cmd = cfg_get(grp, octstr_imm("keepalive-cmd"));
	if(modem->keepalive_cmd == NULL)
	    modem->keepalive_cmd = octstr_create("AT");

	cfg_get_bool(&modem->broken, grp, octstr_imm("broken"));

	cfg_destroy(cfg);
	return modem;
    } else {
	cfg_destroy(cfg);
	return NULL;
    }
}


void at2_destroy_modem(ModemDef *modem) {
    if(modem != NULL) {
	O_DESTROY(modem->id);
	O_DESTROY(modem->name);
	O_DESTROY(modem->detect_string);
	O_DESTROY(modem->detect_string2);
	O_DESTROY(modem->init_string);
	O_DESTROY(modem->enable_hwhs);
	O_DESTROY(modem->keepalive_cmd);
	gw_free(modem);
    }
}

