BASEDIR=..

include $(BASEDIR)/Makefile-rules

###############################################################################

###############################################################################

PRGS = ramdisk mkfs mkfs.ext2 mkfat fsck e2fsck partype fdisk df

all: $(PRGS)

install: all
	$(INSTALL) $(PRGS) $(DESTDIR)/bin

df: df.o $(TINYPRINTF)
	$(LD) $(LDFLAGS) -o df df.o $(TINYPRINTF) $(LDLIBS)

fsck: fsck.o
	$(LD) $(LDFLAGS) -o fsck -maout-heap=0xffff fsck.o $(LDLIBS)

e2fsck: e2fsck.o
	$(LD) $(LDFLAGS) -o e2fsck -maout-heap=0xffff e2fsck.o $(LDLIBS)

fdisk: fdisk.o $(TINYPRINTF)
	$(LD) $(LDFLAGS) -o fdisk fdisk.o $(TINYPRINTF) $(LDLIBS)

mkfs: mkfs.o $(TINYPRINTF)
	$(LD) $(LDFLAGS) -o mkfs mkfs.o $(TINYPRINTF) $(LDLIBS)

mkfs.ext2: mkfs_ext2.o
	$(LD) $(LDFLAGS) -o mkfs.ext2 -maout-heap=0xffff mkfs_ext2.o $(LDLIBS)

mkfat: mkfat.o $(TINYPRINTF)
	$(LD) $(LDFLAGS) -o mkfat mkfat.o $(TINYPRINTF) $(LDLIBS)

partype: partype.o
	$(LD) $(LDFLAGS) -o partype partype.o $(LDLIBS)

ramdisk: ramdisk.o
	$(LD) $(LDFLAGS) -o ramdisk ramdisk.o $(LDLIBS)

clean:
	$(RM) *.o $(PRGS)
