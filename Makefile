PROG=rpp
RM=rm -f
FPMTYPE=deb
VERSION?=0.7.3
INSTALL=install

all: $(PROG)

version:
	@echo $(VERSION)

package: 
	mkdir -p /tmp/fpm-pkg-rpp/usr/bin
	mkdir -p /tmp/fpm-pkg-rpp/etc/init
	make install DESTDIR=/tmp/fpm-pkg-rpp
	fpm -s dir -t $(FPMTYPE) -C /tmp/fpm-pkg-rpp   \
            -n $(PROG) -v $(VERSION) .
	rm -r /tmp/fpm-pkg-rpp

install: all
	$(INSTALL) $(PROG) $(DESTDIR)/usr/bin
	$(INSTALL) $(PROG).conf $(DESTDIR)/etc/$(PROG).conf.example
	$(INSTALL) $(PROG).upstart $(DESTDIR)/etc/init/$(PROG).conf

clean:
	$(RM) *.deb
