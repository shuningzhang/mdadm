#
# mdadm - manage Linux "md" devices aka RAID arrays.
#
# Copyright (C) 2001-2002 Neil Brown <neilb@cse.unsw.edu.au>
#
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program; if not, write to the Free Software
#    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
#    Author: Neil Brown
#    Email: <neilb@cse.unsw.edu.au>
#    Paper: Neil Brown
#           School of Computer Science and Engineering
#           The University of New South Wales
#           Sydney, 2052
#           Australia
#

# define "CXFLAGS" to give extra flags to CC.
# e.g.  make CXFLAGS=-O to optimise
CC = gcc
CXFLAGS = -ggdb
SYSCONFDIR = /etc
CONFFILE = $(SYSCONFDIR)/mdadm.conf
CFLAGS = -Wall -Werror -Wstrict-prototypes -DCONFFILE=\"$(CONFFILE)\" $(CXFLAGS)

# If you want a static binary, you might uncomment these
# LDFLAGS = -static
# STRIP = -s

INSTALL = /usr/bin/install
DESTDIR = 
BINDIR  = /sbin
MANDIR  = /usr/share/man
MAN4DIR = $(MANDIR)/man4
MAN5DIR = $(MANDIR)/man5
MAN8DIR = $(MANDIR)/man8

OBJS =  mdadm.o config.o mdstat.o  ReadMe.o util.o Manage.o Assemble.o Build.o Create.o Detail.o Examine.o Monitor.o dlink.o Kill.o Query.o

all : mdadm mdadm.man md.man mdadm.conf.man

mdadm : $(OBJS)
	$(CC) $(LDFLAGS) -o mdadm $^

mdadm.man : mdadm.8
	nroff -man mdadm.8 > mdadm.man

md.man : md.4
	nroff -man md.4 > md.man

mdadm.conf.man : mdadm.conf.5
	nroff -man mdadm.conf.5 > mdadm.conf.man

$(OBJS) : mdadm.h

install : mdadm mdadm.8 md.4 mdadm.conf.5
	$(INSTALL) -D $(STRIP) -m 755 mdadm $(DESTDIR)$(BINDIR)/mdadm
	$(INSTALL) -D -m 644 mdadm.8 $(DESTDIR)$(MAN8DIR)/mdadm.8
	$(INSTALL) -D -m 644 md.4 $(DESTDIR)$(MAN4DIR)/md.4
	$(INSTALL) -D -m 644 mdadm.conf.5 $(DESTDIR)$(MAN5DIR)/mdadm.conf.5

clean : 
	rm -f mdadm $(OBJS) core *.man

dist : clean
	./makedist

TAGS :
	etags *.h *.c
