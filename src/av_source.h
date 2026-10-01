#ifndef DC_LIBAVMPEG_AV_SOURCE_H
#define DC_LIBAVMPEG_AV_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

/* Build with -DAV_STREAM=1 to read the file through a small buffer (AV_STREAM_BUF
 * bytes, default 128 KB) instead of loading all of it into RAM first. */
#ifndef AV_STREAM
#define AV_STREAM 0
#endif

/* One MPEG-PS (held in RAM, or streamed with AV_STREAM), opened through the vendored libavformat with an
 * MPEG-1 video and an MP2 audio decoder ready. The struct must not move after
 * av_source_open() (ic->pb points into it). */
typedef struct {
    uint8_t *file_data;     /* whole file; NULL when streaming */
    size_t file_size;
    void *fp;               /* FILE *, streaming only */
    uint8_t *iobuf;         /* ByteIOContext buffer, streaming only */
    ByteIOContext pb;
    AVFormatContext *ic;
    AVCodecContext *vc;     /* NULL if not opened */
    AVCodecContext *ac;
    int vidx;
    int aidx;
} av_source_t;

#define AV_SOURCE_NO_FILE (-2)

/* Returns 0, AV_SOURCE_NO_FILE (nothing printed) if the file cannot be read,
 * or -1 (reason printed) for any other failure. */
int av_source_open(av_source_t *src, const char *path, int idct_algo);
void av_source_close(av_source_t *src);

#endif
