PROG=riemann-puppet-ping
SRCS=riemann.pb-c.c log.c ping.c riemann-puppet-ping.c
OBJS=$(SRCS:.c=.o)
CC=cc
RM=rm -f
LDADD=-lprotobuf-c -levent
CFLAGS=-Wall -Werror -ggdb
PROTOC=protoc-c
FPMTYPE=deb
VERSION?=0.5.0
INSTALL=install

all: $(PROG)

version:
	@echo $(VERSION)

package: 
	mkdir -p /tmp/fpm-pkg-rpp/usr/bin
	mkdir -p /tmp/fpm-pkg-rpp/etc/init
	make install DESTDIR=/tmp/fpm-pkg-rpp
	fpm -s dir -t $(FPMTYPE) -C /tmp/fpm-pkg-rpp   \
            -d libprotobuf-c0  -d liboping0            \
            -n $(PROG) -v $(VERSION) .
	rm -r /tmp/fpm-pkg-rpp

install: all
	$(INSTALL) $(PROG) $(DESTDIR)/usr/bin
	$(INSTALL) $(PROG).conf $(DESTDIR)/etc/$(PROG).conf.example
	$(INSTALL) $(PROG).upstart $(DESTDIR)/etc/init/$(PROG).conf

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $(PROG) $(OBJS) $(LDADD)

riemann.pb-c.c: riemann.proto
	$(PROTOC) --c_out=. $<

clean:
	$(RM) $(PROG) *.deb *.o *core riemann.pb-c.c riemann.pb-c.h *~
