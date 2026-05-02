# Makefile for the ELKS command set.
#
###############################################################################
#
# Include standard rules.

BASEDIR = .

include $(BASEDIR)/Makefile-rules

###############################################################################
#
# Subdirectories for clean / build / install

# All subdirectories to build & clean

IA16DIRS =       \
	lib         \
	advent      \
	ash         \
	basic       \
	bc          \
	busyelks    \
	debug       \
	disk_utils  \
	fsck_dos    \
	ekermit	    \
	elvis	    \
	file_utils  \
	gui         \
	inet        \
	ktcp        \
	minix1      \
	minix2      \
	minix3      \
	misc_utils  \
	oss         \
	romprg      \
	sash        \
	screen      \
	cron        \
	sh_utils    \
	sys_utils   \
	tui         \
	test        \
	# EOL

ifeq ($(wildcard $(BASEDIR)/test/Makefile),)
IA16DIRS := $(filter-out test, $(IA16DIRS))
endif

ifeq ($(CONFIG_APP_ESD_ELKS), y)
IA16DIRS += ../extapps/esd-elks
endif

ifeq ($(CONFIG_APP_SBVOL), y)
IA16DIRS += ../extapps/sbvol
endif

ifeq ($(CONFIG_APP_PC1640_NVR), y)
IA16DIRS += ../extapps/amstrad-1640-bios-config
endif

NANOXDIR =
ifeq ($(CONFIG_APP_NANOX), y)
ifneq ($(wildcard $(BASEDIR)/../extapps/microwindows/src/Makefile.elks),)
NANOXDIR = ../extapps/microwindows/src
endif
endif

OWCDIRS =       \
    basic       \
    fsck_dos    \
    gui         \
    # EOL

C86DIRS =       \
    gui         \
    # EOL

###############################################################################
#
# Compile all IA16 apps

ALL = ia16

all: $(ALL)

ia16:
	@if [ ! -e $(TOPDIR)/include/autoconf.h ]; \
	then echo -e "\n*** ERROR: You must configure ELKS first ***\n" >&2; exit 1; fi
	for DIR in $(IA16DIRS); do $(MAKE) -C $$DIR all || exit 1; done
	@if [ -n "$(NANOXDIR)" ]; then $(MAKE) -C $(NANOXDIR) -f Makefile.elks all || exit 1; fi

owc:
	for DIR in $(OWCDIRS); do $(MAKE) -C $$DIR -f Makefile.owc all || exit 1; done

c86:
	for DIR in $(C86DIRS); do $(MAKE) -C $$DIR -f Makefile.c86 all || exit 1; done

clean: owclean c86clean
	for DIR in $(IA16DIRS); do $(MAKE) -C $$DIR clean || exit 1; done
	@if [ -n "$(NANOXDIR)" ]; then $(MAKE) -C $(NANOXDIR) -f Makefile.elks clean || exit 1; fi

owclean:
	for DIR in $(OWCDIRS); do $(MAKE) -C $$DIR -f Makefile.owc clean || exit 1; done

c86clean:
	for DIR in $(C86DIRS); do $(MAKE) -C $$DIR -f Makefile.c86 clean || exit 1; done

install:
	$(MAKE) -f Make.install "CONFIG=$(TOPDIR)/.config"

###############################################################################
