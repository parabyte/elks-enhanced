BASEDIR=..

include $(BASEDIR)/Makefile-rules

PRGS = ossinfo oss_beep ossdsp

all: $(PRGS)

ossinfo: ossinfo.o
	$(LD) $(LDFLAGS) -o ossinfo ossinfo.o $(LDLIBS)

oss_beep: oss_beep.o
	$(LD) $(LDFLAGS) -o oss_beep oss_beep.o $(LDLIBS)

ossdsp: ossdsp.o
	$(LD) $(LDFLAGS) -o ossdsp ossdsp.o $(LDLIBS)

install: all
	$(INSTALL) $(PRGS) $(DESTDIR)/bin

clean:
	$(RM) *.o $(PRGS) esd esdplay
