#   linux makefile for gidget
# Copyright (C) 2011 HBCS, Ltd.
#
# Gidget is a filesystem event triggered script executor
# written in ANSI C by Lou Goddard and Charlie Brooks.
#
# This program is free software; you can redistribute
# it and/or modify it under the terms of the
# GNU General Public License Version 2
#

# make sure our environment is clean
SHELL = /bin/sh
.SUFFIXES:
.SUFFIXES: .c .o

# some GNU style boilerplate     
#srcdir = .
srcdir = /hbcs/code/gidget
#prefix = /usr/local
prefix = /hbcs/code/gidget
#binprefix = bin
binprefix =
bindir = $(prefix)/$(binprefix)

#CC = gcc -O
CC = gcc

#CDEBUG = -g
#CFLAGS = $(CDEBUG) -I. -I$(srcdir)
CFLAGS =

#LDFLAGS = -g
LDFLAGS =

SRCS_C  = gidget.c
SRCS_H  = gidgetmail.h
SRCS    = $(SRCS_C) $(SRCS_H)
OBJS    = $(SRCS_C:.c=.o)
AUX     = README COPYING ChangeLog Makefile  \
          gidget.initscript gidget.logrotate proc.sh gidget.man

.PHONY:  all
all:    gidget.o gidget

gidget: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

gidget.o: $(SRCS)
	$(CC) -c $(CFLAGS) $(SRCS_C) 
     
gidget.info: gidget.texinfo
	makeinfo gidget.texinfo

.PHONY : clean
clean :
	-rm gidget $(OBJS)

.PHONY : install
install :
	cp gidget /usr/local/bin
	cp gidget.initscript /etc/rc.d/init.d/gidget
	cp gidget.logrotate /etc/logrotate.d/gidget

.PHONY : uninstall
uninstall:
	rm /usr/local/bin/gidget
	rm /etc/rc.d/init.d/gidget
	rm /etc/logrotate.d/gidget
