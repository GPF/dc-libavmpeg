#include "libavcodec/avcodec.h"
#include "ffmpeg_codec.h"

extern AVCodec mpeg1video_decoder;

void ffmpeg_register_mpeg1video(void) {
    avcodec_register(&mpeg1video_decoder);
}
