#ifndef DC_LIBAVMPEG_AVMPEG_H
#define DC_LIBAVMPEG_AVMPEG_H

/* Decoder library: one MPEG-1 video + MP2 audio program stream in, decoded YUV420P
 * frames and interleaved 16-bit PCM out. No PVR, no sound hardware, no threads: the
 * caller owns presentation, timing and scheduling (DCSinge's dcfmv worker, or the
 * demo in avmpeg_demo.c).
 *
 * Calls are NOT thread safe; serialise all calls on one handle (DCSinge already
 * holds dcfmv_state_lock across decode).
 *
 * A program stream interleaves both streams and the video runs ahead of the audio
 * that belongs to the same media time, so the library keeps one queue of demuxed
 * packets per stream. avmpeg_video_next() and avmpeg_audio_read() demux on demand;
 * when the stream asked for has no packet and the OTHER queue is full they return
 * AVMPEG_AGAIN, which means "drain the other stream first". */

#include <stddef.h>
#include <stdint.h>

typedef struct avmpeg avmpeg_t;

/* Set non-zero to print what avmpeg_seek_frame() does (landing entry, failures). */
extern int avmpeg_verbose;

enum {
    AVMPEG_OK = 1,          /* a frame / samples were produced (count for audio) */
    AVMPEG_EOF = 0,         /* nothing more will ever be produced */
    AVMPEG_ERROR = -1,
    AVMPEG_AGAIN = -2       /* drain the other stream, then retry */
};

typedef struct {
    int idct_algo;          /* FF_IDCT_* value; 0 = FF_IDCT_SIMPLE (the fast path, incl. MPEG_IDCT_ASM) */
} avmpeg_config_t;

typedef struct {
    int width, height;      /* luma size; YUV420P, both even */
    int fps_num, fps_den;   /* e.g. 24000/1001 */
    int sample_rate;        /* Hz */
    int channels;           /* 1 or 2 */
    /* Media time of the first video frame relative to the first audio sample, in
     * microseconds: video frame i is due av_offset_us + i * frame_us after audio
     * sample 0 plays. Known after avmpeg_open(). */
    int64_t av_offset_us;
    int64_t frame_us;       /* one video frame period, microseconds */
} avmpeg_info_t;

typedef struct {
    const uint8_t *plane[3];    /* Y, U, V: valid until the next video call/seek/close */
    int stride[3];
    int width, height;
    unsigned long index;        /* display-order frame number since open or seek */
    int pict_type;              /* 1 = I, 2 = P, 3 = B (FFmpeg picture types) */
} avmpeg_frame_t;

/* Cumulative, microseconds spent inside the library; reset with avmpeg_reset_stats(). */
typedef struct {
    uint64_t video_decode_us, audio_decode_us, demux_us;
    unsigned long video_frames, audio_chunks, video_errors, audio_errors;
    unsigned video_queue_max, audio_queue_max;      /* packets */
} avmpeg_stats_t;

/* path: any file the platform fopen() can read ("/cd/x.mpg", "/pc/x.mpg"). Returns
 * NULL on failure (reason printed). The input mode (whole file in RAM, small read
 * buffer, or prebuffer ring) is chosen at build time with -DAV_STREAM=0/1/2, see
 * av_source.h. cfg may be NULL. */
avmpeg_t *avmpeg_open(const char *path, const avmpeg_config_t *cfg);
void avmpeg_close(avmpeg_t *m);
const avmpeg_info_t *avmpeg_info(const avmpeg_t *m);

/* Decode the next video frame in display order. AVMPEG_OK fills *out. */
int avmpeg_video_next(avmpeg_t *m, avmpeg_frame_t *out);

/* Fill dst with up to max_frames interleaved PCM16 sample frames (channels samples
 * each). Returns the count (> 0), AVMPEG_EOF, AVMPEG_AGAIN or AVMPEG_ERROR. May
 * return fewer than max_frames; leftover decoded samples are kept for the next call. */
int avmpeg_audio_read(avmpeg_t *m, int16_t *dst, int max_frames);

/* Input top-up while idle (AV_STREAM=2 only; no-op otherwise). Returns 1 if it read. */
int avmpeg_pump(avmpeg_t *m);

/* Seek index: a `.pidx` file in the format of the pl_mpeg project's tools/build_pidx.py
 * (uint32 count, then count * { double pts_seconds; uint32 byte_offset } packed, 12 bytes
 * each, optional trailer 'PCNT' + uint32 picture count). byte_offset is the position just
 * after the 00 00 01 E0 start code of the video PES packet holding the I-frame. FFmpeg
 * 0.5's PS demuxer cannot seek by time on a freshly opened file (it has no timestamp
 * search), so seeking requires the index. Returns 0, or -1 (reason printed). */
int avmpeg_load_index(avmpeg_t *m, const char *pidx_path);

/* Exact picture count from the index trailer, or -1 if unknown. */
long avmpeg_frame_count(const avmpeg_t *m);

/* Position so the next avmpeg_video_next() returns display-order frame `frame`
 * (0 = first picture of the file): jumps to the nearest preceding indexed I-frame and
 * decodes forward, discarding, to the target. Audio restarts at the target's media
 * time (earlier audio packets are dropped). Needs avmpeg_load_index(). With AV_STREAM=2 a
 * jump outside the prebuffer ring restarts the ring from the file (the first reads after
 * it wait on the file). Returns 0, or -1. */
int avmpeg_seek_frame(avmpeg_t *m, long frame);

/* Same by time. */
int avmpeg_seek(avmpeg_t *m, double seconds);

const avmpeg_stats_t *avmpeg_stats(const avmpeg_t *m);
void avmpeg_reset_stats(avmpeg_t *m);

#endif
