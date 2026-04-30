ELKS_TOP ?= ../..
TOPDIR := $(abspath $(ELKS_TOP))

ifeq ($(wildcard $(TOPDIR)/elkscmd/Makefile-rules),)
$(error Set ELKS_TOP to an ELKS checkout containing elkscmd/Makefile-rules)
endif

include $(TOPDIR)/elkscmd/Makefile-rules

INSTALL ?= install
PRGS = esd esdplay

all: $(PRGS)

esd: esd.o
	$(LD) $(LDFLAGS) -maout-heap=4096 -maout-stack=4096 -o $@ $^ $(LDLIBS)

esdplay: esdplay.o
	$(LD) $(LDFLAGS) -maout-heap=2048 -maout-stack=2048 -o $@ $^ $(LDLIBS)

install: all
	@if [ -z "$(DESTDIR)" ]; then \
		echo "Set DESTDIR=/path/to/elks-rootfs-staging"; \
		exit 1; \
	fi
	mkdir -p $(DESTDIR)/bin
	$(INSTALL) $(PRGS) $(DESTDIR)/bin

clean:
	$(RM) *.o $(PRGS)
