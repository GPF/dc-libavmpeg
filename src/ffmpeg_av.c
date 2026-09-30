#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "ffmpeg_av.h"

extern AVCodec mpeg1video_decoder;
extern AVCodec mpeg2video_decoder;
extern AVCodec mp2_decoder;
extern AVCodecParser mpegvideo_parser;
extern AVCodecParser mpegaudio_parser;
extern AVInputFormat mpegps_demuxer;
extern AVInputFormat mpegvideo_demuxer;

void ffmpeg_register_mpegps_av(void) {
    avcodec_register(&mpeg1video_decoder);
    /* The 0.5 PS demuxer labels video CODEC_ID_MPEG2VIDEO; the MPEG video
     * parser rewrites it to MPEG1VIDEO once it sees the sequence header. */
    avcodec_register(&mpeg2video_decoder);
    avcodec_register(&mp2_decoder);
    av_register_codec_parser(&mpegvideo_parser);
    av_register_codec_parser(&mpegaudio_parser);
    av_register_input_format(&mpegps_demuxer);
    /* The PS demuxer tags plain video streams CODEC_ID_PROBE; libavformat
     * resolves them by probing the registered raw ES demuxers. */
    av_register_input_format(&mpegvideo_demuxer);
}
