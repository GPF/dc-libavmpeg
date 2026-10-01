TARGET = dc-libavmpeg.elf
BUILD_DIR = build
FIXTURE ?= fixtures/lair_320_23976_30s.m1v
CDI_IMAGE = $(BUILD_DIR)/dc-libavmpeg.cdi
FFMPEG_ROOT = vendor/ffmpeg
FFMPEG_SOURCES = \
	libavcodec/utils.c \
	libavcodec/opt.c \
	libavcodec/options.c \
	libavcodec/parser.c \
	libavcodec/bitstream.c \
	libavcodec/mpeg12.c \
	libavcodec/mpeg12data.c \
	libavcodec/mpegvideo.c \
	libavcodec/error_resilience.c \
	libavcodec/dsputil.c \
	libavcodec/imgconvert.c \
	libavcodec/faanidct.c \
	libavcodec/jrevdct.c \
	libavcodec/simple_idct.c \
	libavcodec/sh4/dsputil_align.c \
	libavcodec/sh4/dsputil_sh4.c \
	libavcodec/sh4/idct_sh4.c \
	libavutil/mem.c \
	libavutil/utils.c \
	libavutil/log.c \
	libavutil/mathematics.c \
	libavutil/rational.c \
	libavutil/avstring.c
FFMPEG_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(FFMPEG_SOURCES))
OBJS = $(BUILD_DIR)/main.o src/mpeg_player.o src/pvr_video.o src/ffmpeg_codec.o $(BUILD_DIR)/idct_island.o $(BUILD_DIR)/idct_island_asm.o $(IDCT_ASM_OBJS) $(FFMPEG_OBJS)

# Slice 2: MPEG-PS demux + MP2 decode (libavformat, MPEG audio, parsers).
PROBE_TARGET = dc-libavmpeg-probe.elf
FFMPEG_AV_SOURCES = \
	libavcodec/mpegaudiodec.c \
	libavcodec/mpegaudiodecheader.c \
	libavcodec/mpegaudio.c \
	libavcodec/mpegaudiodata.c \
	libavcodec/mpegaudio_parser.c \
	libavcodec/mpegvideo_parser.c \
	libavcodec/audioconvert.c \
	libavcodec/raw.c \
	libavformat/utils.c \
	libavformat/cutils.c \
	libavformat/aviobuf.c \
	libavformat/avio.c \
	libavformat/options.c \
	libavformat/metadata.c \
	libavformat/metadata_compat.c \
	libavformat/raw.c \
	libavformat/mpeg.c
FFMPEG_AV_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(FFMPEG_AV_SOURCES))
PROBE_OBJS = src/probe_main.o src/ffmpeg_av.o $(BUILD_DIR)/idct_island.o $(BUILD_DIR)/idct_island_asm.o $(IDCT_ASM_OBJS) $(FFMPEG_OBJS) $(FFMPEG_AV_OBJS)
AV_TARGET = dc-libavmpeg-av.elf
AV_OBJS = $(BUILD_DIR)/av_main.o src/av_source.o src/pvr_video.o src/ffmpeg_av.o src/cache_profile.o $(BUILD_DIR)/idct_island.o $(BUILD_DIR)/idct_island_asm.o $(IDCT_ASM_OBJS) $(FFMPEG_OBJS) $(FFMPEG_AV_OBJS)
AUDIO_TARGET = dc-libavmpeg-audio.elf
AUDIO_OBJS = src/audio_main.o src/ffmpeg_av.o $(BUILD_DIR)/idct_island.o $(BUILD_DIR)/idct_island_asm.o $(IDCT_ASM_OBJS) $(FFMPEG_OBJS) $(FFMPEG_AV_OBJS)
DC_IP ?=
MPEG_BENCHMARK ?= 1
MPEG_EXTRA_CFLAGS ?=
MPEG_CROP_ALIGN ?= 16384
# Reserve the 512-byte island at the low end of KOS's 16 MB main-thread stack.
MPEG_ISLAND_STACK_ADDR ?= 0x8cff3e00
PROBE_VERBOSE ?= 0
# MPEG_IDCT_ASM selects a hand-written SH-4 integer IDCT for the default
# FF_IDCT_SIMPLE path; both bypass the island trampoline. dsputil.o is not
# rebuilt when this changes: use a separate BUILD_DIR or clean.
#   1  src/idct_sh4.S      frame-less muls.w kernel, natural coefficient order
#   2  src/idct_sh4_mac.S  mac.w kernel, FF_SSE2_IDCT_PERM coefficient order
MPEG_IDCT_ASM ?= 0

ifeq ($(MPEG_BENCHMARK),1)
KOS_CFLAGS += -DMPEG_BENCHMARK=1
endif

ifeq ($(MPEG_IDCT_ASM),1)
KOS_CFLAGS += -DMPEG_IDCT_ASM=1
IDCT_ASM_OBJS = $(BUILD_DIR)/idct_sh4.o
else ifeq ($(MPEG_IDCT_ASM),2)
KOS_CFLAGS += -DMPEG_IDCT_ASM=2
IDCT_ASM_OBJS = $(BUILD_DIR)/idct_sh4_mac.o
else
ISLAND_LD = $(BUILD_DIR)/idct-island.ld
ISLAND_LDFLAGS = -Wl,-T,$(BUILD_DIR)/idct-island.ld -Wl,--section-start=.idct_private_stack=$(MPEG_ISLAND_STACK_ADDR)
endif

KOS_CFLAGS += $(MPEG_EXTRA_CFLAGS)
KOS_CFLAGS += -DMPEG_CROP_ALIGN=$(MPEG_CROP_ALIGN)
ifeq ($(PROBE_VERBOSE),1)
src/probe_main.o: KOS_CFLAGS += -DPROBE_VERBOSE=1
endif
KOS_CFLAGS += -Isrc -Ivendor/ffmpeg -Ivendor/ffmpeg/libavcodec -Ivendor/ffmpeg/libavutil -Ivendor/ffmpeg/libavformat
FFMPEG_CFLAGS = $(KOS_CFLAGS) -DHAVE_AV_CONFIG_H -Isrc/ffmpeg_config

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

$(BUILD_DIR)/island-layout-pre.elf: $(OBJS)
	mkdir -p $(BUILD_DIR)
	kos-cc -o $@ $(OBJS)

$(BUILD_DIR)/idct-island.ld: $(BUILD_DIR)/island-layout-pre.elf tools/island_layout.py
	python3 tools/island_layout.py $< --linker-script $@

$(TARGET): $(OBJS) $(ISLAND_LD)
	kos-cc $(ISLAND_LDFLAGS) -o $@ $(OBJS)

$(PROBE_TARGET): $(PROBE_OBJS) $(ISLAND_LD)
	kos-cc $(ISLAND_LDFLAGS) -o $@ $(PROBE_OBJS)

$(AUDIO_TARGET): $(AUDIO_OBJS) $(ISLAND_LD)
	kos-cc $(ISLAND_LDFLAGS) -o $@ $(AUDIO_OBJS)

$(AV_TARGET): $(AV_OBJS) $(ISLAND_LD)
	kos-cc $(ISLAND_LDFLAGS) -o $@ $(AV_OBJS)

av: $(AV_TARGET)

run-av: $(AV_TARGET)
	@test -n "$(DC_IP)" || { echo "Set DC_IP to the Dreamcast dcload-ip address"; exit 2; }
	kos-tool -t "$(DC_IP)" -x $(AV_TARGET)

probe: $(PROBE_TARGET)

audio: $(AUDIO_TARGET)

run-audio: $(AUDIO_TARGET)
	@test -n "$(DC_IP)" || { echo "Set DC_IP to the Dreamcast dcload-ip address"; exit 2; }
	kos-tool -t "$(DC_IP)" -x $(AUDIO_TARGET)

run-probe: $(PROBE_TARGET)
	@test -n "$(DC_IP)" || { echo "Set DC_IP to the Dreamcast dcload-ip address"; exit 2; }
	kos-tool -t "$(DC_IP)" -x $(PROBE_TARGET)

src/%.o: src/%.c
	kos-cc $(KOS_CFLAGS) -c $< -o $@

$(BUILD_DIR)/idct_island.o: src/idct_island.c
	mkdir -p $(BUILD_DIR)
	kos-cc $(KOS_CFLAGS) -c $< -o $@

$(BUILD_DIR)/av_main.o: src/av_main.c
	mkdir -p $(BUILD_DIR)
	kos-cc $(KOS_CFLAGS) -c $< -o $@

$(BUILD_DIR)/main.o: src/main.c
	mkdir -p $(BUILD_DIR)
	kos-cc $(KOS_CFLAGS) -c $< -o $@

$(BUILD_DIR)/idct_sh4.o: src/idct_sh4.S
	mkdir -p $(BUILD_DIR)
	kos-cc $(KOS_CFLAGS) -DASM_UNDERSCORE -c $< -o $@

$(BUILD_DIR)/idct_sh4_mac.o: src/idct_sh4_mac.S
	mkdir -p $(BUILD_DIR)
	kos-cc $(KOS_CFLAGS) -DASM_UNDERSCORE -c $< -o $@

$(BUILD_DIR)/idct_island_asm.o: src/idct_island.S
	mkdir -p $(BUILD_DIR)
	kos-cc $(KOS_CFLAGS) -c $< -o $@

# mpegaudiodec.c passes int32_t* (long on sh-elf) to int* parameters (both
# 32-bit); aviobuf.c passes URLContext* callbacks where void* is declared
# (identical ABI). Downgrade only this diagnostic, only for these objects, so
# the vendored source stays untouched.
$(BUILD_DIR)/libavcodec/mpegaudiodec.o \
$(BUILD_DIR)/libavformat/aviobuf.o: FFMPEG_CFLAGS += -Wno-error=incompatible-pointer-types

$(BUILD_DIR)/%.o: $(FFMPEG_ROOT)/%.c src/ffmpeg_config/config.h
	mkdir -p $(dir $@)
	kos-cc $(FFMPEG_CFLAGS) -c $< -o $@

run: $(TARGET)
	@test -n "$(DC_IP)" || { echo "Set DC_IP to the Dreamcast dcload-ip address"; exit 2; }
	kos-tool -t "$(DC_IP)" -x $(TARGET)

cdi: $(TARGET) $(FIXTURE)
	mkdir -p $(BUILD_DIR)
	mkdcdisc -e $(TARGET) -n dc-libavmpeg -N -f $(FIXTURE) -o $(CDI_IMAGE)

clean:
	-rm -f $(TARGET) $(PROBE_TARGET) $(AUDIO_TARGET) $(AV_TARGET) src/*.o
	-rm -rf $(BUILD_DIR)

.PHONY: all clean run cdi probe run-probe audio run-audio av run-av
