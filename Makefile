#
# Makefile for WapIT (WAP/SMS) Gateway.
#
#
# Define where the programs will be installed.
#

prefix = /usr/local
bindir = $(prefix)/bin


#
# Define here how the programs will be compiled. You can use or not use
# the pthread thread library, according to your wishes.
#
#

# Name of PID file (uncomment and modify if you want to change the default)
#PID_FILE=-DPID_FILE=\"/your/value/here/gateway.pid\"

VERSION=$(shell head -1 VERSION)

CC=gcc
LIBS=
# CFLAGS=-Wall -Werror -g -DHAVE_THREADS=1 $(PID_FILE) -DVERSION=\"$(VERSION)\"
CFLAGS=-Wall -g -DHAVE_THREADS=1 $(PID_FILE) -DVERSION=\"$(VERSION)\"
LDFLAGS=

MKDEPEND=$(CC) -MM

# Some systems require ranlib to be run on a library after it is created.
# Some don't even have ranlib. Uncomment appropriately.
RANLIB=:
#RANLIB=ranlib

# For Linux, uncomment the following:
CFLAGS += -DHAVE_SOCKLEN_T
THREADLIB = -lpthread

# For FreeBSD, uncomment the following:
#LIBS += -lc_r
#THREADLIB = 

# For Solaris, uncomment the following:
#CFLAGS += -DSOLARIS

# Uncomment one of these if you want to use a malloc debugger.
#EFENCELIB = -lefence

# Generic libraries.
LIBS += $(THREADLIB) $(EFENCELIB) $(DMALLOCLIB) -lm

# For Solaris uncomment the following
LIBS += -lsocket -lnsl

#
# You probably don't need to touch anything below this, if you're just
# compiling and installing the software.
#

progs = fakesmsc bearerbox smsbox wapbox wapbox_feeder

libobjs = wapitlib.o html.o http.o config.o octstr.o \
          urltrans.o cgi.o

all: progs 
progs: $(progs)

SMSC = smsc.o smsc_smpp.o smsc_emi.o smsc_fake.o smsc_cimd.o
BBOBJS = $(SMSC) boxc.o csdr.o bb_msg.o sms_msg.o msg.o bearerbox.o


#doc: dummy
#	cd doc && make
dummy:

fakesmsc: fakesmsc.o libgw.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o fakesmsc fakesmsc.o libgw.a $(LIBS)

bearerbox: $(BBOBJS) libgw.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o bearerbox $(BBOBJS) libgw.a $(LIBS)

smsbox: smsbox.o smsbox_req.o msg.o libgw.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o smsbox smsbox.o smsbox_req.o msg.o libgw.a $(LIBS)

wapbox: wapbox.o msg.o libgw.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o wapbox wapbox.o msg.o libgw.a $(LIBS)

wapbox_feeder: wapbox_feeder.o msg.o libgw.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o wapbox_feeder wapbox_feeder.o msg.o libgw.a $(LIBS)

libgw.a: $(libobjs)
	ar rc libgw.a $(libobjs)
	$(RANLIB) libgw.a

bearerbox.o: smsc.h html.h wapitlib.h http.h config.h urltrans.h cgi.h \
	sms_msg.h VERSION
fakesmsc.o: wapitlib.h

depend .depend:
	$(MKDEPEND) *.c > .depend

include .depend

clean:
	rm -f a.out core *.o $(progs) libgw.a gateway.pid .cvsignore
#	cd doc && make clean

cvsignore:
	rm -f .cvsignore
	for i in $(progs); do echo $$i >> .cvsignore; done
	echo .cvsignore >> .cvsignore
	echo .depend >> .cvsignore
	cd doc && $(MAKE) cvsignore

install: all
	mkdir -p $(bindir)
	install fakesmsc gateway $(bindir)
