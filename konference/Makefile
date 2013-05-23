#
# app_konference
#
# A channel independent conference application for Asterisk
#
# Copyright (C) 2002,2003 Junghanns.NET GmbH
#
# This program may be modified and distributed under the
# terms of the GNU General Public License. You should have received
# a copy of the GNU General Public License along with this
# program; if not, write to the Free Software Foundation, Inc.
# 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#

# asterisk source directory
ASTERISK_SRC_DIR = /home/agrusha/Workspace/Git/asterisk/

ifndef	ASTERISK_SRC_DIR
  $(warning Asterisk source directory is not set)
  $(error Modify the source directory variable in the Makefile or set it on the command line)
endif

# asterisk version
ASTERISK_SRC_VERSION = 1100 #$(shell if [ -e $(ASTERISK_SRC_DIR)/.version ] ; then cat $(ASTERISK_SRC_DIR)/.version | awk -F. '{printf "%01d%02d",$$1,$$2}' ; fi)

ifeq	($(ASTERISK_SRC_VERSION),)
  $(warning Asterisk version is not set)
  $(error Modify the version variable in the Makefile or set it on the command line)
endif

# asterisk include directory
ASTERISK_INCLUDE_DIR = $(ASTERISK_SRC_DIR)/include

# asterisk module directory
ASTERISK_INSTALL_DIR=/home/agrusha/Workspace/asterisk
INSTALL_MODULES_DIR=$(ASTERISK_INSTALL_DIR)/lib/asterisk/modules
INSTALL_SOUNDS_DIR=$(ASTERISK_INSTALL_DIR)/var/lib/asterisk/sounds/en
INSTALL_MOH_DIR=$(ASTERISK_INSTALL_DIR)/var/lib/asterisk/moh

# module revision
REVISION = $(shell git rev-parse HEAD)

#
# defines which can be passed on the command-line
#

# cache_control_blocks ( 0 == OFF, 1 == ON )
CACHE_CONTROL_BLOCKS ?= 1

# score board ( 0 == OFF, 1 == ON )
SPEAKER_SCOREBOARD ?= 1

# score board table size
SPEAKER_SCOREBOARD_SIZE ?= 4096

# channel table size
CHANNEL_TABLE_SIZE ?= 997

# conference table size
CONFERENCE_TABLE_SIZE ?= 199

# silence detection ( 0 = OFF 1 = libwebrtc 2 = libspeex )
SILDET := 1

#
# objects to build
#

OBJS = app_conference.o conference.o member.o frame.o cli.o
INCS = app_conference.h  cli.h  conf_frame.h  conference.h  frame.h  member.h
TARGET = app_konference.so

#
# compiler settings
#

#PROC = $(shell uname -m)
INSTALL = install

INCLUDE = -I$(ASTERISK_INCLUDE_DIR)
#DEBUG := -g -O0

#
# compiler flags
#

CFLAGS = -pipe -Wall -Wmissing-prototypes -Wmissing-declarations -MD -MP $(DEBUG)
#CFLAGS += -O3 -march=pentium3 -msse -mfpmath=sse,387 -ffast-math
# PERF: below is 10% faster than -O2 or -O3 alone.
#CFLAGS += -O3 -ffast-math -funroll-loops
# below is another 5% faster or so.
CFLAGS += -O3 -ffast-math -funroll-all-loops -fsingle-precision-constant
#CFLAGS += -mcpu=7450 -faltivec -mabi=altivec -mdynamic-no-pic
# adding -msse -mfpmath=sse has little effect.
#CFLAGS += -O3 -msse -mfpmath=sse
#CFLAGS += $(shell if $(CC) -march=$(PROC) -S -o /dev/null -xc /dev/null >/dev/null 2>&1; then echo "-march=$(PROC)"; fi)
CFLAGS += $(shell if uname -m | grep -q ppc; then echo "-fsigned-char"; fi)
CFLAGS += -fPIC

#
# preprocessor flags
#

CPPFLAGS = $(INCLUDE) -D_REENTRANT -D_GNU_SOURCE
CPPFLAGS += -DREVISION=\"$(REVISION)\"
CPPFLAGS += -DASTERISK_SRC_VERSION=$(ASTERISK_SRC_VERSION)
CPPFLAGS += -DCHANNEL_TABLE_SIZE=$(CHANNEL_TABLE_SIZE)
CPPFLAGS += -DCONFERENCE_TABLE_SIZE=$(CONFERENCE_TABLE_SIZE)
CPPFLAGS += -DCACHE_CONF_FRAMES

#
#
# Uncomment this if you want to mix frames using gcc vector extensions
#
# CPPFLAGS += -DVECTORS
#

#
# Uncomment this if you want to use linux timerfd for timing
#
# CPPFLAGS += -DTIMERFD
# CPPFLAGS += -DTIMERFD_EXPIRATIONS
#

#
# Uncomment this if you want to use *bsd kqueue for timing
#
# CPPFLAGS += -DKQUEUE
# CPPFLAGS += -DKQUEUE_EXPIRATIONS
#

#
# Uncomment this if you want G.729A support (need to have the actual codec installed)
#
# CPPFLAGS += -DAC_USE_G729A
#

#
# Uncomment this if you want G.722 support (requires asterisk 1.6 or 1.8)
#
 CPPFLAGS += -DAC_USE_G722
#

#
# Uncomment this if you want OPUS support (requires asterisk 11)
#
 CPPFLAGS += -DAC_USE_OPUS
#

#
# Uncomment this if you want SPEEX support (need to have the actual codec installed)
#
# CPPFLAGS += -DAC_USE_SPEEX
#

ifeq ($(CACHE_CONTROL_BLOCKS), 1)
CPPFLAGS += -DCACHE_CONTROL_BLOCKS
ifeq ($(SPEAKER_SCOREBOARD), 1)
CPPFLAGS += -DSPEAKER_SCOREBOARD -DSPEAKER_SCOREBOARD_SIZE=$(SPEAKER_SCOREBOARD_SIZE)
endif
endif

#
# additional flag values for silence detection
#

ifeq ($(SILDET), 1)
OBJS += libwebrtc/get_scaling_square.o libwebrtc/division_operations.o libwebrtc/energy.o libwebrtc/vad_core.o libwebrtc/vad_filterbank.o libwebrtc/vad_gmm.o libwebrtc/vad_sp.o libwebrtc/webrtc_vad.o
INCS += libwebrtc/signal_processing_library.h libwebrtc/spl_inl.h libwebrtc/webrtc_vad.h libwebrtc/vad_core.h libwebrtc/vad_filterbank.h libwebrtc/vad_gmm.h libwebrtc/vad_sp.h
CPPFLAGS += -Ilibwebrtc -DSILDET=1
else ifeq ($(SILDET), 2)
OBJS += libspeex/preprocess.o libspeex/misc.o libspeex/smallft.o
INCS += libspeex/speex_preprocess.h libspeex/smallft.h libspeex/misc.h
CPPFLAGS += -Ilibspeex -DSILDET=2
endif

OSARCH=$(shell uname -s)
ifeq (${OSARCH},Darwin)
SOLINK=-dynamic -bundle -undefined suppress -force_flat_namespace
else
SOLINK=-shared -Xlinker -x
endif

DEPS += $(subst .o,.d,$(OBJS))

#
# targets
#

all: $(TARGET)

.PHONY: clean
clean:
	$(RM) $(TARGET) $(OBJS) $(DEPS)

$(OBJS): $(INCS)

$(TARGET): $(OBJS)
	$(CC) $(SOLINK) -o $@ $(OBJS)

install:
	if [ -f $(TARGET) ]; then $(INSTALL) -m 755 $(TARGET) $(INSTALL_MODULES_DIR); fi
	$(INSTALL) -m 644 *.wav $(INSTALL_SOUNDS_DIR)
	$(INSTALL) -m 644 moh/*.wav $(INSTALL_MOH_DIR)

