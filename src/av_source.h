#ifndef DC_LIBAVMPEG_AV_SOURCE_H
#define DC_LIBAVMPEG_AV_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

/* Input modes (build with -DAV_STREAM=n):
 *   0  whole file preloaded into RAM (default; clip must fit the free heap)
 *   1  read through a small buffer (AV_STREAM_BUF bytes, default 16 KB); every
 *      read blocks the demuxer
 *   2  prebuffer ring (AV_RING_BYTES, default 4 MB, filled before playback and
 *      topped up in AV_RING_CHUNK pieces by av_source_pump() when the player is
 *      idle); the demuxer only copies from RAM unless the ring runs dry */
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
    uint8_t *ring;          /* AV_STREAM=2 prebuffer */
    size_t ring_start;      /* file offsets: oldest retained byte, read position, */
    size_t ring_rd;         /* and end of the data read from the file */
    size_t ring_end;
    unsigned long ring_pump_n, ring_block_n;   /* idle top-ups / blocking refills */
    size_t ring_min_ahead;
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

/* AV_STREAM=2 only (no-ops otherwise). prefill reads ahead until the ring is full;
 * pump reads one chunk if there is room and returns 1 if it read, 0 if not;
 * print_stats reports how often the ring was topped up while idle versus
 * refilled while the demuxer waited. */
void av_source_prefill(av_source_t *src);
int av_source_pump(av_source_t *src);
void av_source_print_stats(const av_source_t *src);

#endif
