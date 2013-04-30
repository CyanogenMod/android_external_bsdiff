BINARIES = bsdiff bspatch

INSTALL = install
CFLAGS += -O3 -Wall -Werror

DESTDIR ?=
PREFIX = /usr
BINDIR = $(PREFIX)/bin
DATADIR = $(PREFIX)/share
MANDIR = $(DATADIR)/man
MAN1DIR = $(MANDIR)/man1
INSTALL_PROGRAM ?= $(INSTALL) -c -m 755
INSTALL_MAN ?= $(INSTALL) -c -m 444

.PHONY: all clean
all: $(BINARIES)
clean:
	rm -f *.o $(BINARIES)

bsdiff: bsdiff.o
bsdiff: LDLIBS += -lbz2 -ldivsufsort -ldivsufsort64
bspatch: bspatch.o extents.o exfile.o
bspatch: LDLIBS += -lbz2

bspatch.o: extents.h exfile.h
extents.o: extents.h exfile.h
exfile.o: exfile.h

install:
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MAN1DIR)
	$(INSTALL_PROGRAM) $(BINARIES) $(DESTDIR)$(BINDIR)
ifndef WITHOUT_MAN
	$(INSTALL_MAN) $(BINARIES:=.1) $(DESTDIR)$(MAN1DIR)
endif
