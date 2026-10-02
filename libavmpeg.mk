# Builds libavmpeg.a (src/avmpeg.h) out of tree, for embedding in another KOS project
# (DCSinge's CMake runs this). The KOS environment (environ.sh) must be active.
#
#   make -f libavmpeg.mk OUT=/path/to/build [AV_STREAM=2]
#
# Fixed configuration: the bit-exact MAC.W IDCT (MPEG_IDCT_ASM=2), which needs no IDCT-island
# linker script, so a plain link works. AV_STREAM selects the input mode (see src/av_source.h):
# 0 = whole file in RAM, 1 = small read buffer, 2 = prebuffer ring (default here: movies are
# far bigger than the heap). Switching AV_STREAM needs a clean OUT.

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
OUT ?= build-lib
AV_STREAM ?= 2
MPEG_CROP_ALIGN ?= 16384
MPEG_BENCHMARK ?= 1
LIB = $(OUT)/libavmpeg.a

include $(ROOT)/ffmpeg_sources.mk

CFLAGS = $(KOS_CFLAGS) -DMPEG_BENCHMARK=$(MPEG_BENCHMARK) -DMPEG_IDCT_ASM=2 \
	-DMPEG_CROP_ALIGN=$(MPEG_CROP_ALIGN) -DAV_STREAM=$(AV_STREAM) \
	-I$(ROOT)/src -I$(ROOT)/vendor/ffmpeg -I$(ROOT)/vendor/ffmpeg/libavcodec \
	-I$(ROOT)/vendor/ffmpeg/libavutil -I$(ROOT)/vendor/ffmpeg/libavformat
FFCFLAGS = $(CFLAGS) -DHAVE_AV_CONFIG_H -I$(ROOT)/src/ffmpeg_config

FF_OBJS = $(patsubst %.c,$(OUT)/ff/%.o,$(FFMPEG_SOURCES) $(FFMPEG_AV_SOURCES))
OBJS = $(OUT)/avmpeg.o $(OUT)/av_source.o $(OUT)/ffmpeg_av.o $(OUT)/idct_island.o \
	$(OUT)/idct_island_asm.o $(OUT)/idct_sh4_mac.o $(FF_OBJS)

all: $(LIB)

$(LIB): $(OBJS)
	rm -f $@
	$(KOS_AR) rcs $@ $(OBJS)

$(OUT)/%.o: $(ROOT)/src/%.c
	@mkdir -p $(dir $@)
	kos-cc $(CFLAGS) -c $< -o $@

$(OUT)/idct_island_asm.o: $(ROOT)/src/idct_island.S
	@mkdir -p $(dir $@)
	kos-cc $(CFLAGS) -c $< -o $@

$(OUT)/idct_sh4_mac.o: $(ROOT)/src/idct_sh4_mac.S
	@mkdir -p $(dir $@)
	kos-cc $(CFLAGS) -DASM_UNDERSCORE -c $< -o $@

$(OUT)/ff/%.o: $(ROOT)/vendor/ffmpeg/%.c $(ROOT)/src/ffmpeg_config/config.h
	@mkdir -p $(dir $@)
	kos-cc $(FFCFLAGS) -c $< -o $@

# mpegaudiodec.c / aviobuf.c pass pointers of identical ABI but different declared types.
$(OUT)/ff/libavcodec/mpegaudiodec.o $(OUT)/ff/libavformat/aviobuf.o: \
	FFCFLAGS += -Wno-error=incompatible-pointer-types

.PHONY: all
