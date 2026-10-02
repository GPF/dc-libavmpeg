# Vendored FFmpeg 0.5 source lists, shared by Makefile and libavmpeg.mk.

# Video decode (MPEG-1) and the SH-4 specifics.
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

# Demux (MPEG-PS) and MP2 audio decode.
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
