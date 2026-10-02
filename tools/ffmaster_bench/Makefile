# Benchmark for an UNMODIFIED FFmpeg checkout built for the SH-4 (research only).
#   1. Cross-build libavutil/libavcodec/libavformat of a current FFmpeg with the KOS
#      toolchain (see CONFIGURE below), into $(FFBUILD).
#   2. make -C tools/ffmaster_bench FFSRC=/path/to/ffmpeg FFBUILD=/path/to/build
#   3. kos-tool -t <ip> -x tools/ffmaster_bench/ffmaster_bench.elf -m .   (repo root)
FFSRC   ?= $(HOME)/gpf/ffmpeg-master
FFBUILD ?= /tmp/ffbuild
CONFIGURE = $(FFSRC)/configure --enable-cross-compile --cc=kos-cc --cross-prefix=sh-elf- \
  --arch=sh4 --target-os=none --disable-everything --disable-programs --disable-doc \
  --disable-network --disable-autodetect --disable-asm --disable-avdevice --disable-swscale \
  --disable-swresample --disable-avfilter --disable-pthreads --disable-debug \
  --enable-decoder=mpeg1video --enable-decoder=mp2 --enable-demuxer=mpegps \
  --enable-parser=mpegvideo --enable-parser=mpegaudio --enable-protocol=file \
  --extra-cflags="-O3 -ffast-math -ffunction-sections -fdata-sections \
  -Wno-error=incompatible-pointer-types -Wno-error=int-conversion"

all: ffmaster_bench.elf

ffmaster_bench.elf: bench.c
	kos-cc -O3 -I$(FFSRC) -I$(FFBUILD) -o $@ bench.c \
	  $(FFBUILD)/libavformat/libavformat.a $(FFBUILD)/libavcodec/libavcodec.a \
	  $(FFBUILD)/libavutil/libavutil.a -lm -Wl,--gc-sections

clean:
	-rm -f ffmaster_bench.elf
