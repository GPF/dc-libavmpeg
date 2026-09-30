#ifndef DC_LIBAVMPEG_FFMPEG_AV_H
#define DC_LIBAVMPEG_FFMPEG_AV_H

/* Registers only what Slice 2 needs: MPEG-PS demuxer, MPEG-1 video and MP2
 * decoders, and the MPEG video/audio parsers. */
void ffmpeg_register_mpegps_av(void);

#endif
