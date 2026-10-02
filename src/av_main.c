/* Slice 2 milestone 2C: synchronized MPEG-1 video + MP2 audio from the
 * original MPEG-PS. Single thread. Audio path is the proven 2B path (aligned
 * PCM ring, interleaved snd_stream callback, region released only after the
 * next callback returns). Video frames are decoded ahead into a small queue of
 * copies and presented against a timer clock anchored right after
 * snd_stream_start(). No printing while playing (dcload console I/O blocks). */
#include <kos.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "av_source.h"
#ifdef MPEG_DECODE_PROFILE
#include "decoder_profile.h"
#include <dc/perfctr.h>
#endif
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "pvr_video.h"

#ifdef MPEG_ISLAND_DEBUG
extern uint32_t *mpeg_idct_private_stack_top;
extern volatile uintptr_t mpeg_idct_private_stack_highwater;
extern volatile uint32_t mpeg_idct_private_stack_fault;
extern void mpeg_idct_private_stack_init(void);
#endif

/* kos-tool cannot pass argv, so runtime settings come from a small config.ini
 * (edit it on the BBA file server; no rebuild). Looked up under /pc first, then
 * the disc. Keys: fixture=<file name in fixtures/ or disc root>,
 * sync_offset_us=<int>. Lines starting with # or ; are comments. */
#ifndef AV_CONFIG_PC_PATH
#define AV_CONFIG_PC_PATH "/pc/config.ini"
#endif
#ifndef AV_CONFIG_CD_PATH
#define AV_CONFIG_CD_PATH "/cd/config.ini"
#endif
#define AV_DEFAULT_FIXTURE "lair_320_23976_30s_spec.mpg"
#define AV_PC_FIXTURE_DIR "/pc/fixtures/"

#ifndef AV_IDCT_ALGO
#define AV_IDCT_ALGO FF_IDCT_SIMPLE
#endif

/* Positive delays video relative to audio; tune by eye/ear on hardware via
 * sync_offset_us in config.ini. */
static int sync_offset_us;
static int audio_direct;            /* config audio_path=direct */
static char cfg_fixture[128] = AV_DEFAULT_FIXTURE;
static char cd_path[160], pc_path[260];

#ifndef AUDIO_STREAM_BUFFER
#define AUDIO_STREAM_BUFFER (16 * 1024)
#endif

#define RING_SIZE (128 * 1024)
#define RING_MASK (RING_SIZE - 1)
#define PREFILL_BYTES (32 * 1024)
#define POLL_DANGER_US 93000

/* Max demux time per loop iteration. */
#ifndef DEMUX_SLICE_US
#define DEMUX_SLICE_US 6000
#endif

/* The extra audio/demux work above one packet per iteration is only needed
 * when the buffers are running low (heavy stereo scenes). Doing it always made
 * mono iterations up to ~60 ms and cost frame drops. */
#ifndef AUDIO_LOW_BYTES
#define AUDIO_LOW_BYTES (RING_SIZE * 6 / 10)
#endif
#ifndef PACKET_LOW
#define PACKET_LOW 64
#endif

/* Max audio decode time per loop iteration before video gets a turn. */
#ifndef AUDIO_SLICE_US
#define AUDIO_SLICE_US 20000
#endif

#ifndef VQ_SLOTS
#define VQ_SLOTS 12
#endif
#ifndef VQ_PREFILL
#define VQ_PREFILL 4
#endif

#define LEAD_MAX 4096
#define LATE_MAX 1024

/* ---- audio ring (same rules as 2B) ------------------------------------ */

typedef struct {
    uint8_t *data;
    uint64_t wpos;
    uint64_t rpos;
    uint64_t rel;
    uint64_t last_start;
} pcm_ring_t;

static pcm_ring_t ring;
static int demux_eof;
static int started;
static int channels = 1;
static int sample_rate = 44100;
static uint64_t t_start_us;

static unsigned long cb_calls, cb_null_underrun, cb_null_eos, cb_short, cb_wrap_short;
static uint64_t bytes_submitted;
static uint64_t depth_sum, depth_min = UINT64_MAX, depth_max;
static unsigned long depth_n;
static uint64_t poll_prev_us, poll_gap_max, poll_gap_sum;
static unsigned long poll_gaps, poll_gap_over_46ms, poll_gap_over_93ms, poll_calls;
static uint64_t poll_dur_sum, poll_dur_max;
static int poll_err_first;
static unsigned long poll_minus3;
static int64_t lead_us[LEAD_MAX];
static uint32_t lead_t_ms[LEAD_MAX];
static unsigned lead_n;

/* ---- compressed packet FIFOs ------------------------------------------
 * The PS runs video up to ~19 frames ahead of the audio that belongs to the
 * same media time, so one shared "pending packet" deadlocks before start
 * (video queue full, audio never reached). Each stream gets its own FIFO of
 * demuxed packets; the demuxer keeps reading while both have room. */

#define PF_CAP 128

typedef struct {
    AVPacket pk[PF_CAP];
    unsigned head, count, max_count;
} pkt_fifo_t;

static pkt_fifo_t vfifo, afifo;
static int audio_eof;               /* demux EOF and no audio packets left */

static void fifo_push(pkt_fifo_t *f, const AVPacket *pkt) {
    f->pk[(f->head + f->count) % PF_CAP] = *pkt;
    f->count++;
    if (f->count > f->max_count)
        f->max_count = f->count;
}

static AVPacket *fifo_front(pkt_fifo_t *f) {
    return &f->pk[f->head];
}

static void fifo_pop(pkt_fifo_t *f) {
    av_free_packet(&f->pk[f->head]);
    f->head = (f->head + 1) % PF_CAP;
    f->count--;
}

/* ---- video queue ------------------------------------------------------ */

typedef struct {
    uint8_t *plane[3];
    unsigned long idx;      /* display-order frame number */
} vslot_t;

static vslot_t vq[VQ_SLOTS];
static unsigned vq_head, vq_count;
static int vwidth, vheight;
static unsigned vlinesize[3], vplane_h[3];
static int vflushed;

static unsigned long vframes, verrors, presented, dropped;
static unsigned long late_16ms, late_frame;
static uint64_t late_max_us, late_sum_us;
static uint64_t vdec_us, vdec_max, vcopy_us, vcopy_max, pres_us, pres_max;
#ifdef MPEG_AV_FRAME_TRACE
#define AV_TRACE_MAX_DROPS 32
#define AV_TRACE_CHECKSUM_FRAMES 30
typedef struct {
    unsigned long idx;
    uint32_t time_us;
    uint32_t due_us;
} av_drop_trace_t;
typedef struct {
    unsigned long idx;
    uint32_t y, u, v;
} av_checksum_trace_t;
static av_drop_trace_t av_drop_trace[AV_TRACE_MAX_DROPS];
static av_checksum_trace_t av_checksum_trace[AV_TRACE_CHECKSUM_FRAMES];
static unsigned av_drop_trace_n, av_checksum_trace_n;

static uint32_t av_adler_plane(const uint8_t *src, int stride, int width, int height) {
    uint32_t a = 1, b = 0;
    int x, y;

    for (y = 0; y < height; y++, src += stride)
        for (x = 0; x < width; x++) {
            a = (a + src[x]) % 65521;
            b = (b + a) % 65521;
        }
    return (b << 16) | a;
}
#endif
#ifdef MPEG_IDCT_BLOCK_COUNTS
unsigned long mpeg_idct_block_counts[3][2][6];
#endif
#ifdef MPEG_DECODE_PROFILE
static uint64_t decode_stage_us[4];
static unsigned long decode_stage_samples[4];
static const char *decode_stage_name[4] = {
    "macroblock parse", "reconstruction", "motion compensation", "DCT residue/IDCT"
};
static uint64_t picture_decode_us[4], picture_decode_max[4];
static unsigned long picture_decode_count[4];
int mpeg_decode_profile_sample;
int mpeg_decode_profile_pict;
static uint64_t profile_boundary_overhead_ns;
static uint64_t profile_calibration_total_ns;
static int profile_counter_mode = -1;

#ifdef MPEG_CACHE_PROFILE_EVENT
/* Opt-in: sample one SH-4 event (PRFC1) instead of cycles. The stage totals
 * then hold raw event counts scaled x4 (1-in-4 macroblock sampling). */
static unsigned long long decode_event_total;
static unsigned long long event_stage[4][4];   /* [pict I/P/B = 1..3][stage] */
static unsigned long event_samples[4][4];
static unsigned long long event_total_by[4];   /* whole-decode events by pict type */
static uint64_t mpeg_decode_profile_clock_ns(void) {
    return perf_cntr_count(PRFC1);
}
#else
static uint64_t mpeg_decode_profile_clock_ns(void) {
    if (profile_counter_mode < 0)
        profile_counter_mode = perf_cntr_timer_enabled();
    return profile_counter_mode ? perf_cntr_count(PRFC0) * 5 : timer_ns_gettime64();
}
#endif

uint64_t mpeg_decode_profile_start(void) {
    return mpeg_decode_profile_clock_ns();
}

void mpeg_decode_profile_end(unsigned stage, uint64_t start_ns) {
    uint64_t elapsed_ns = mpeg_decode_profile_clock_ns() - start_ns;

    if (stage < 4) {
        if (elapsed_ns > profile_boundary_overhead_ns)
            elapsed_ns -= profile_boundary_overhead_ns;
        else
            elapsed_ns = 0;
        /* One sample represents four macroblocks. */
#ifdef MPEG_CACHE_PROFILE_EVENT
        decode_stage_us[stage] += elapsed_ns * 4;
        event_stage[mpeg_decode_profile_pict & 3][stage] += elapsed_ns * 4;
        event_samples[mpeg_decode_profile_pict & 3][stage]++;
#else
        decode_stage_us[stage] += elapsed_ns * 4 / 1000;
#endif
        decode_stage_samples[stage]++;
    } else if (stage == 4)
        profile_calibration_total_ns += elapsed_ns;
}

uint64_t mpeg_decode_profile_calibrate(void) {
    unsigned i;

    profile_calibration_total_ns = 0;
    for (i = 0; i < 256; i++)
        mpeg_decode_profile_end(4, mpeg_decode_profile_start());
    profile_boundary_overhead_ns = profile_calibration_total_ns / 256;
#ifdef MPEG_CACHE_PROFILE_EVENT
    printf("av: profile event 0x%02x boundary overhead %llu events/pair (integer avg)\n",
           (unsigned)MPEG_CACHE_PROFILE_EVENT, (unsigned long long)profile_boundary_overhead_ns);
    return profile_boundary_overhead_ns;
#endif
    printf("av: profile timing boundary calibrated at %llu ns\n",
           (unsigned long long)profile_boundary_overhead_ns);
    return profile_boundary_overhead_ns;
}
#endif
static uint64_t vq_depth_sum;
static unsigned vq_depth_min = 99, vq_depth_max;
static unsigned long vq_depth_n;
static int64_t late_us_log[LATE_MAX];
static uint32_t late_t_ms[LATE_MAX];
static unsigned late_n;

/* Per-iteration stage costs (us) and a log of badly late frames. */
typedef struct {
    unsigned total, demux, audio, video, present;
} iter_t;

typedef struct {
    unsigned t_ms, late_us;
    unsigned vq, vfifo, afifo;
    iter_t prev;
} late_event_t;

#define LATE_EVENTS 32
static iter_t cur_it, prev_it;
static late_event_t late_events[LATE_EVENTS];
static unsigned late_event_n;
static iter_t it_max;
static uint64_t demux_us, demux_n;
static unsigned long iters, iters_over_42ms, iters_over_60ms;
static unsigned fifo_min_v = 9999, fifo_min_a = 9999;
static unsigned long afifo_empty_iters;

/* Stereo channel check: energy of decoded even (L) and odd (R) samples in the
 * fixture's beep window (first 0.25 s) and in a beep-free window (0.30-1.90 s). */
static double ch_sumsq[2][2];       /* [window][channel] */
static unsigned long ch_count[2];

static uint64_t adec_us, adec_max;
static unsigned long aframes, aerrors;

/* Slice 3 profiling: per-second stage cost on the playback clock, so heavy
 * scenes are not diluted by whole-run averages. Diagnostic only. */
#define SEC_MAX 64
enum { ST_DEC, ST_COPY, ST_PRES, ST_ADEC, ST_DEMUX, ST_N };
typedef struct {
    uint64_t us[ST_N], dec_max;
    unsigned vdec_n, pres_n, adec_n;
} sec_t;
static sec_t secs[SEC_MAX];
static unsigned sec_last;

static sec_t *sec_cur(void) {
    unsigned k = 0;

    if (started)
        k = (unsigned)((timer_us_gettime64() - t_start_us) / 1000000ULL);
    if (k >= SEC_MAX)
        k = SEC_MAX - 1;
    if (k > sec_last)
        sec_last = k;
    return &secs[k];
}
static uint64_t decoded_bytes;

/* frame timeline: frame i is due frame_dt * i after t0_due_us */
static int64_t first_due_us;        /* (first video pts - first audio pts) */
static int fps_num = 24000, fps_den = 1001;
static uint64_t frame_dt_us;

static uint64_t ring_free(void) {
    return RING_SIZE - (ring.wpos - ring.rel);
}

static void ring_put(const void *src, size_t len) {
    size_t off = (size_t)(ring.wpos & RING_MASK);
    size_t first = RING_SIZE - off;

    if (first > len)
        first = len;
    memcpy(ring.data + off, src, first);
    if (len > first)
        memcpy(ring.data, (const uint8_t *)src + first, len - first);
    ring.wpos += len;
}

/* Bookkeeping common to both callback styles. */
static void cb_enter(uint64_t avail) {
    cb_calls++;

    /* Callback N-1's region may still be in flight; everything before its
     * start is done. */
    ring.rel = ring.last_start;
    ring.last_start = ring.rpos;

    if (started && !(audio_eof && avail == 0)) {
        int64_t elapsed = (int64_t)(timer_us_gettime64() - t_start_us);
        int64_t submitted_us = (int64_t)(bytes_submitted / (2ULL * channels)) *
                               1000000LL / sample_rate;

        depth_sum += avail;
        depth_n++;
        if (avail < depth_min)
            depth_min = avail;
        if (avail > depth_max)
            depth_max = avail;
        if (lead_n < LEAD_MAX) {
            lead_us[lead_n] = submitted_us - elapsed;
            lead_t_ms[lead_n] = (uint32_t)(elapsed / 1000);
            lead_n++;
        }
    }
}

/* Diagnostic path (audio_path=direct, stereo only): skip KOS's snd_pcm16_split
 * and write our own deinterleaved L/R straight into the two AICA channel
 * buffers. size_req is the combined byte count (both channels). The copy is
 * synchronous, so nothing stays in flight. */
static uint16_t dl_buf[2][4096] __attribute__((aligned(32)));

static size_t audio_callback_direct(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r,
                                    size_t size_req) {
    uint64_t avail = ring.wpos - ring.rpos;
    size_t frames = size_req / 4;       /* stereo frames wanted */
    size_t i;

    (void)hnd;
    cb_enter(avail);
    if (frames > 4096)
        frames = 4096;
    if (frames > (size_t)(avail / 4))
        frames = (size_t)(avail / 4);
    frames &= ~(size_t)15;              /* 32 bytes per channel */
    if (frames == 0) {
        if (started) {
            if (audio_eof && avail == 0)
                cb_null_eos++;
            else
                cb_null_underrun++;
        }
        return 0;
    }
    if (started && frames * 4 < size_req && !(audio_eof && frames * 4 == avail))
        cb_short++;
    for (i = 0; i < frames; i++) {
        const uint8_t *p = ring.data + ((ring.rpos + i * 4) & RING_MASK);

        dl_buf[0][i] = *(const uint16_t *)p;            /* left  = even sample */
        dl_buf[1][i] = *(const uint16_t *)(p + 2);      /* right = odd sample  */
    }
    spu_memload(l, dl_buf[0], frames * 2);
    spu_memload(r, dl_buf[1], frames * 2);
    ring.rpos += frames * 4;
    bytes_submitted += frames * 4;
    return frames * 4;
}

static void *audio_callback(snd_stream_hnd_t hnd, int smp_req, int *smp_recv) {
    uint64_t avail = ring.wpos - ring.rpos;
    size_t off = (size_t)(ring.rpos & RING_MASK);
    size_t contig = RING_SIZE - off;
    size_t n;

    (void)hnd;
    cb_enter(avail);

    if (contig > avail)
        contig = (size_t)avail;
    n = contig < (size_t)smp_req ? contig : (size_t)smp_req;
    n &= ~(size_t)31;

    if (n == 0) {
        *smp_recv = 0;
        if (started) {
            if (audio_eof && avail == 0)
                cb_null_eos++;
            else
                cb_null_underrun++;
        }
        return NULL;
    }
    if (started && n < (size_t)smp_req) {
        if (avail >= (uint64_t)smp_req)
            cb_wrap_short++;
        else if (!(audio_eof && n == avail))
            cb_short++;
    }

    ring.rpos += n;
    bytes_submitted += n;
    *smp_recv = (int)n;
    return ring.data + off;
}

static void do_poll(snd_stream_hnd_t hnd) {
    uint64_t t0 = timer_us_gettime64();
    uint64_t dur;
    int r;

    if (poll_prev_us) {
        uint64_t gap = t0 - poll_prev_us;

        poll_gap_sum += gap;
        poll_gaps++;
        if (gap > poll_gap_max)
            poll_gap_max = gap;
        if (gap > 46000)
            poll_gap_over_46ms++;
        if (gap > POLL_DANGER_US)
            poll_gap_over_93ms++;
    }
    poll_prev_us = t0;
    r = snd_stream_poll(hnd);
    dur = timer_us_gettime64() - t0;
    poll_calls++;
    poll_dur_sum += dur;
    if (dur > poll_dur_max)
        poll_dur_max = dur;
    if (r == -3)
        poll_minus3++;
    else if (r < 0 && !poll_err_first)
        poll_err_first = r;
}

/* ---- video ------------------------------------------------------------ */

static int vq_alloc(void) {
    unsigned s, p;

    vlinesize[0] = (unsigned)vwidth;
    vlinesize[1] = vlinesize[2] = (unsigned)vwidth / 2;
    vplane_h[0] = (unsigned)vheight;
    vplane_h[1] = vplane_h[2] = (unsigned)vheight / 2;
    for (s = 0; s < VQ_SLOTS; s++)
        for (p = 0; p < 3; p++) {
            vq[s].plane[p] = av_malloc(vlinesize[p] * vplane_h[p]);
            if (!vq[s].plane[p])
                return -1;
        }
    return 0;
}

static void vq_enqueue(const AVFrame *frame) {
    vslot_t *slot = &vq[(vq_head + vq_count) % VQ_SLOTS];
    uint64_t t0 = timer_us_gettime64();
    uint64_t dt;
    unsigned p, row;

    for (p = 0; p < 3; p++)
        for (row = 0; row < vplane_h[p]; row++)
            memcpy(slot->plane[p] + row * vlinesize[p],
                   frame->data[p] + row * frame->linesize[p], vlinesize[p]);
    slot->idx = vframes;
#ifdef MPEG_AV_FRAME_TRACE
    if (slot->idx < AV_TRACE_CHECKSUM_FRAMES) {
        av_checksum_trace_t *c = &av_checksum_trace[av_checksum_trace_n++];

        c->idx = slot->idx;
        c->y = av_adler_plane(frame->data[0], frame->linesize[0], vwidth, vheight);
        c->u = av_adler_plane(frame->data[1], frame->linesize[1], vwidth / 2, vheight / 2);
        c->v = av_adler_plane(frame->data[2], frame->linesize[2], vwidth / 2, vheight / 2);
    }
#endif
    vq_count++;
    vframes++;
    dt = timer_us_gettime64() - t0;
    vcopy_us += dt;
    sec_cur()->us[ST_COPY] += dt;
    if (dt > vcopy_max)
        vcopy_max = dt;
}

#ifdef MPEG_ADDR_PRINT
/* Opt-in runtime address dump for cache-layout analysis. Deliberately uses only
 * putchar and computed digits: no string literals (.rodata) and no statics
 * (.bss), so heap addresses match the production data layout. The decoder-side
 * pointers are printed from mpegvideo.c (MPEG_ADDR_PRINT hook). */
static void ap_hex(unsigned v) {
    int i;

    for (i = 28; i >= 0; i -= 4) {
        unsigned d = (v >> i) & 15u;

        putchar(d < 10 ? (int)('0' + d) : (int)('a' + d - 10));
    }
}
static void ap(char tag, const void *v) {
    putchar('@'); putchar(tag); putchar('='); ap_hex((unsigned)(uintptr_t)v); putchar('\n');
}
static void addr_print(AVCodecContext *vc, const AVFrame *f) {
    unsigned i;

    (void)vc;
    putchar('#'); ap_hex((unsigned)vframes); putchar('\n');
    ap('a', f->data[0]); ap('b', f->data[1]); ap('c', f->data[2]);
    ap('l', (const void *)(uintptr_t)f->linesize[0]); ap('m', (const void *)(uintptr_t)f->linesize[1]);
    if (vframes == 0)
        for (i = 0; i < VQ_SLOTS; i++)
            ap((char)('0' + (i % 10)), vq[i].plane[0]);
}
#endif

static int decode_video(AVCodecContext *vc, AVFrame *frame, const uint8_t *data,
                        int size) {
    int got = 0;
    uint64_t t0 = timer_us_gettime64();
    uint64_t dt;
#ifdef MPEG_CACHE_PROFILE_EVENT
    uint64_t ev0 = perf_cntr_count(PRFC1);
#endif
    int ret = avcodec_decode_video(vc, frame, &got, data, size);
#ifdef MPEG_CACHE_PROFILE_EVENT
    {
        const uint64_t ev_delta = perf_cntr_count(PRFC1) - ev0;

        decode_event_total += ev_delta;
        event_total_by[mpeg_decode_profile_pict & 3] += ev_delta;
    }
#endif

    dt = timer_us_gettime64() - t0;
    vdec_us += dt;
#ifdef MPEG_ADDR_PRINT
    if (got && vframes < 7)
        addr_print(vc, frame);
#endif
    {
        sec_t *sc = sec_cur();

        sc->us[ST_DEC] += dt;
        sc->vdec_n++;
        if (dt > sc->dec_max)
            sc->dec_max = dt;
    }
    if (dt > vdec_max)
        vdec_max = dt;
#ifdef MPEG_DECODE_PROFILE
    if (got && frame->pict_type >= FF_I_TYPE && frame->pict_type <= FF_B_TYPE) {
        unsigned type = (unsigned)frame->pict_type;

        picture_decode_us[type] += dt;
        picture_decode_count[type]++;
        if (dt > picture_decode_max[type])
            picture_decode_max[type] = dt;
    }
#endif
    if (ret < 0)
        verrors++;
    if (got)
        vq_enqueue(frame);
    return got;
}

static int64_t frame_due_us(unsigned long idx) {
    return first_due_us + (int64_t)(idx * frame_dt_us) + sync_offset_us;
}

/* Present the newest queued frame whose time has come; frames that are
 * already superseded count as dropped. Returns 1 if a frame was presented. */
static int present_step(enum PixelFormat fmt) {
    int64_t now = (int64_t)(timer_us_gettime64() - t_start_us);
    vslot_t *slot;
    AVFrame view;
    int64_t lateness;
    uint64_t t0, dt;

    while (vq_count >= 2 && frame_due_us(vq[(vq_head + 1) % VQ_SLOTS].idx) <= now) {
#ifdef MPEG_AV_FRAME_TRACE
        if (av_drop_trace_n < AV_TRACE_MAX_DROPS) {
            av_drop_trace_t *d = &av_drop_trace[av_drop_trace_n++];

            d->idx = vq[vq_head].idx;
            d->time_us = (uint32_t)now;
            d->due_us = (uint32_t)frame_due_us(vq[vq_head].idx);
        }
#endif
        vq_head = (vq_head + 1) % VQ_SLOTS;
        vq_count--;
        dropped++;
    }
    if (vq_count == 0)
        return 0;
    slot = &vq[vq_head];
    if (frame_due_us(slot->idx) > now)
        return 0;

    lateness = now - frame_due_us(slot->idx);
    if (lateness > (int64_t)frame_dt_us && late_event_n < LATE_EVENTS) {
        late_event_t *e = &late_events[late_event_n++];

        e->t_ms = (uint32_t)(now / 1000);
        e->late_us = (uint32_t)lateness;
        e->vq = vq_count;
        e->vfifo = vfifo.count;
        e->afifo = afifo.count;
        e->prev = prev_it;
    }
    vq_depth_sum += vq_count;
    vq_depth_n++;
    if (vq_count < vq_depth_min)
        vq_depth_min = vq_count;
    if (vq_count > vq_depth_max)
        vq_depth_max = vq_count;

    memset(&view, 0, sizeof(view));
    view.data[0] = slot->plane[0];
    view.data[1] = slot->plane[1];
    view.data[2] = slot->plane[2];
    view.linesize[0] = (int)vlinesize[0];
    view.linesize[1] = (int)vlinesize[1];
    view.linesize[2] = (int)vlinesize[2];
    t0 = timer_us_gettime64();
    pvr_video_present(&view, vwidth, vheight, fmt);
    dt = timer_us_gettime64() - t0;
    cur_it.present = (uint32_t)dt;
    pres_us += dt;
    {
        sec_t *sc = sec_cur();

        sc->us[ST_PRES] += dt;
        sc->pres_n++;
    }
    if (dt > pres_max)
        pres_max = dt;

    presented++;
    late_sum_us += (uint64_t)lateness;
    if ((uint64_t)lateness > late_max_us)
        late_max_us = (uint64_t)lateness;
    if (lateness > 16667)
        late_16ms++;
    if (lateness > (int64_t)frame_dt_us)
        late_frame++;
    if (late_n < LATE_MAX) {
        late_us_log[late_n] = lateness;
        late_t_ms[late_n] = (uint32_t)(now / 1000);
        late_n++;
    }
    vq_head = (vq_head + 1) % VQ_SLOTS;
    vq_count--;
    return 1;
}

/* Least-squares slope (ms per s) of a series, skipping the first 10%. */
static double slope_ms_per_s(const int64_t *y_us, const uint32_t *t_ms, unsigned n) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    unsigned k, m = 0, skip = n / 10;

    for (k = skip; k < n; k++) {
        double x = (double)t_ms[k] / 1000.0;
        double y = (double)y_us[k] / 1000.0;

        sx += x; sy += y; sxx += x * x; sxy += x * y; m++;
    }
    if (m > 2 && (double)m * sxx - sx * sx > 0.0)
        return ((double)m * sxy - sx * sy) / ((double)m * sxx - sx * sx);
    return 0.0;
}

static char *trim(char *str) {
    char *end;

    while (*str == ' ' || *str == '\t')
        str++;
    end = str + strlen(str);
    while (end > str && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
                         end[-1] == '\t'))
        *--end = '\0';
    return str;
}

static int load_config(const char *path) {
    char line[200];
    FILE *file = fopen(path, "r");

    if (!file)
        return -1;
    while (fgets(line, sizeof(line), file)) {
        char *key = trim(line);
        char *val = strchr(key, '=');

        if (*key == '#' || *key == ';' || !val)
            continue;
        *val++ = '\0';
        key = trim(key);
        val = trim(val);
        if (!strcmp(key, "fixture") && *val && strlen(val) < sizeof(cfg_fixture))
            strcpy(cfg_fixture, val);
        else if (!strcmp(key, "sync_offset_us"))
            sync_offset_us = atoi(val);
        else if (!strcmp(key, "audio_path"))
            audio_direct = !strcmp(val, "direct");
        else
            printf("av: config: ignoring \"%s\"\n", key);
    }
    fclose(file);
    printf("av: config from %s\n", path);
    return 0;
}

int main(int argc, char **argv) {
    static av_source_t src;
    const char *path;
    AVFrame *frame;
    int16_t *samples;
    AVPacket pkt;
    int have_apts = 0, have_vpts = 0;
    int64_t first_apts = 0, first_vpts = 0;
    snd_stream_hnd_t hnd;
    uint64_t drain_deadline_us = 0;
    uint64_t buffer_us;
    int fail = 0, audio_fail, video_fail;
    int ret;

#ifdef MPEG_ISLAND_DEBUG
    mpeg_idct_private_stack_init();
#endif

    /* Controller chord (Start+A+B+X+Y) exits, so a stuck run needs no reset. */
    cont_btn_callback(0, CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y,
                      (cont_btn_callback_t)arch_exit);

    printf("dc-libavmpeg av: Slice 2C synchronized video + audio\n");
    if (load_config(AV_CONFIG_PC_PATH) < 0 && load_config(AV_CONFIG_CD_PATH) < 0)
        printf("av: no config.ini found; using defaults\n");
    snprintf(cd_path, sizeof(cd_path), "/cd/%s", cfg_fixture);
    snprintf(pc_path, sizeof(pc_path), AV_PC_FIXTURE_DIR "%s", cfg_fixture);
    printf("av: fixture=%s sync_offset_us=%d audio_path=%s\n", cfg_fixture,
           sync_offset_us, audio_direct ? "direct" : "split");
    path = argc > 1 ? argv[1] : cd_path;
    ret = av_source_open(&src, path, AV_IDCT_ALGO);
    if (ret == AV_SOURCE_NO_FILE && argc <= 1) {
        printf("av: %s unavailable; falling back to %s\n", path, pc_path);
        path = pc_path;
        ret = av_source_open(&src, path, AV_IDCT_ALGO);
    }
    if (ret != 0) {
        printf("av: cannot open %s\n", path);
        return 1;
    }
    printf("av: opened %s (%lu bytes)\n", path, (unsigned long)src.file_size);
    av_source_prefill(&src);
#ifdef MPEG_DECODE_PROFILE
#ifdef MPEG_CACHE_PROFILE_EVENT
    perf_cntr_clear(PRFC1);
    perf_cntr_start(PRFC1, (perf_cntr_event_t)MPEG_CACHE_PROFILE_EVENT, PMCR_COUNT_CPU_CYCLES);
#endif
    mpeg_decode_profile_calibrate();
#endif

    channels = src.ac->channels;
    sample_rate = src.ac->sample_rate;
    vwidth = src.vc->width;
    vheight = src.vc->height;
    if (channels < 1 || channels > 2 || src.vc->pix_fmt != PIX_FMT_YUV420P ||
        (vwidth & 1) || (vheight & 1)) {
        printf("av: unsupported format (ch %d, pix_fmt %d, %dx%d)\n", channels,
               src.vc->pix_fmt, vwidth, vheight);
        return 1;
    }
    if (src.ic->streams[src.vidx]->r_frame_rate.num > 0) {
        fps_num = src.ic->streams[src.vidx]->r_frame_rate.num;
        fps_den = src.ic->streams[src.vidx]->r_frame_rate.den;
    }
    frame_dt_us = (uint64_t)1000000 * (uint64_t)fps_den / (uint64_t)fps_num;
    printf("av: video %dx%d %d/%d fps (frame %llu us), audio %d Hz %d ch\n", vwidth,
           vheight, fps_num, fps_den, (unsigned long long)frame_dt_us, sample_rate,
           channels);

    frame = avcodec_alloc_frame();
    samples = av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);
    ring.data = aligned_alloc(32, RING_SIZE);
    if (!frame || !samples || !ring.data || vq_alloc() < 0) {
        printf("av: out of memory\n");
        return 1;
    }

    pvr_init_defaults();
    if (pvr_video_init(vwidth, vheight) < 0) {
        printf("pvr: initialization failed\n");
        return 1;
    }
    if (snd_stream_init_ex(2, AUDIO_STREAM_BUFFER) < 0) {
        printf("av: snd_stream_init_ex failed\n");
        return 1;
    }
    if (audio_direct && channels != 2) {
        printf("av: audio_path=direct needs a stereo clip; using split\n");
        audio_direct = 0;
    }
    hnd = snd_stream_alloc(audio_direct ? NULL : audio_callback, AUDIO_STREAM_BUFFER);
    if (hnd != SND_STREAM_INVALID && audio_direct)
        snd_stream_set_callback_direct(hnd, audio_callback_direct);
    if (hnd == SND_STREAM_INVALID) {
        printf("av: snd_stream_alloc failed\n");
        return 1;
    }
    buffer_us = (uint64_t)AUDIO_STREAM_BUFFER / 2 * 1000000ULL / sample_rate;
    printf("av: playing (no output until done)\n");

    while (1) {
        int progressed = 0;
        int presented_now = 0;
        uint64_t it0 = timer_us_gettime64();
        uint64_t st0;
        unsigned demux_prev = 0;
        unsigned demux_n_it = 0;
        unsigned audio_n_it = 0;

        memset(&cur_it, 0, sizeof(cur_it));

        if (started) {
            do_poll(hnd);
            presented_now = present_step(src.vc->pix_fmt);
            progressed |= presented_now;
            if (audio_eof && vflushed && vq_count == 0 && ring.wpos == ring.rpos) {
                uint64_t now = timer_us_gettime64();

                if (!drain_deadline_us)
                    drain_deadline_us = now + buffer_us + 150000;
                else if (now >= drain_deadline_us)
                    break;
            }
        }

        /* Demux: keep both FIFOs fed. */
        st0 = timer_us_gettime64();
        /* Several packets per iteration: one per iteration supplied only ~15-22
         * packets/s during 45-65 ms heavy-scene iterations against ~62/s needed
         * (24 video + 38 audio), draining both FIFOs and starving audio. */
        while (!demux_eof && vfifo.count < PF_CAP && afifo.count < PF_CAP &&
               timer_us_gettime64() - st0 < DEMUX_SLICE_US &&
               (demux_n_it == 0 || afifo.count < PACKET_LOW ||
                vfifo.count < PACKET_LOW)) {
            ret = av_read_frame(src.ic, &pkt);
            if (ret < 0) {
                demux_eof = 1;
            } else {
                if (pkt.stream_index == src.aidx && !have_apts &&
                    pkt.pts != AV_NOPTS_VALUE) {
                    first_apts = av_rescale_q(pkt.pts,
                            src.ic->streams[src.aidx]->time_base, AV_TIME_BASE_Q);
                    have_apts = 1;
                } else if (pkt.stream_index == src.vidx && !have_vpts &&
                           pkt.pts != AV_NOPTS_VALUE) {
                    first_vpts = av_rescale_q(pkt.pts,
                            src.ic->streams[src.vidx]->time_base, AV_TIME_BASE_Q);
                    have_vpts = 1;
                }
                if (have_apts && have_vpts)
                    first_due_us = first_vpts - first_apts;
                if (pkt.stream_index == src.aidx || pkt.stream_index == src.vidx) {
                    av_dup_packet(&pkt);
                    fifo_push(pkt.stream_index == src.aidx ? &afifo : &vfifo, &pkt);
                } else {
                    av_free_packet(&pkt);
                }
            }
            progressed = 1;
            cur_it.demux = (uint32_t)(timer_us_gettime64() - st0);
            demux_us += cur_it.demux - demux_prev;
            sec_cur()->us[ST_DEMUX] += cur_it.demux - demux_prev;
            demux_prev = cur_it.demux;
            demux_n++;
            demux_n_it++;
        }

        /* Audio has priority over video: keep decoding packets until the ring is
         * full or AUDIO_SLICE_US of audio work is spent this iteration. One
         * packet per iteration starved the ring in heavy stereo scenes (a 45 ms
         * iteration made only 26 ms of audio). */
        st0 = timer_us_gettime64();
        while (afifo.count && ring_free() >= 4608 &&
               timer_us_gettime64() - st0 < AUDIO_SLICE_US &&
               (audio_n_it == 0 || ring.wpos - ring.rpos < AUDIO_LOW_BYTES)) {
            audio_n_it++;
            AVPacket *ap = fifo_front(&afifo);
            const uint8_t *data = ap->data;
            int size = ap->size;

            while (size > 0) {
                int out_bytes = AVCODEC_MAX_AUDIO_FRAME_SIZE;
                uint64_t t0 = timer_us_gettime64();
                uint64_t dt;
                int len = avcodec_decode_audio2(src.ac, samples, &out_bytes, data,
                                                size);

                dt = timer_us_gettime64() - t0;
                adec_us += dt;
                {
                    sec_t *sc = sec_cur();

                    sc->us[ST_ADEC] += dt;
                    sc->adec_n++;
                }
                if (dt > adec_max)
                    adec_max = dt;
                if (len <= 0) {
                    if (len < 0)
                        aerrors++;
                    break;
                }
                data += len;
                size -= len;
                if (out_bytes > 0) {
                    if (channels == 2) {
                        unsigned long first = (unsigned long)(decoded_bytes / 4);
                        int n, cnt = out_bytes / 4;

                        for (n = 0; n < cnt; n++) {
                            unsigned long at = first + (unsigned long)n;
                            int w = at < 11025 ? 0 : (at >= 13230 && at < 83790 ? 1 : -1);

                            if (w >= 0) {
                                ch_sumsq[w][0] += (double)samples[2 * n] * samples[2 * n];
                                ch_sumsq[w][1] += (double)samples[2 * n + 1] * samples[2 * n + 1];
                                ch_count[w]++;
                            }
                        }
                    }
                    ring_put(samples, (size_t)out_bytes);
                    decoded_bytes += (uint64_t)out_bytes;
                    aframes++;
                }
            }
            fifo_pop(&afifo);
            progressed = 1;
            cur_it.audio = (uint32_t)(timer_us_gettime64() - st0);
            /* Keep the AICA fed while a longer audio burst runs. */
            if (started)
                do_poll(hnd);
        }
        audio_eof = demux_eof && afifo.count == 0;

        /* Video: one heavy decode per iteration, and not in an iteration that
         * already presented a frame, so the gap between polls stays short. */
        st0 = timer_us_gettime64();
        if (!presented_now && vq_count < VQ_SLOTS) {
            if (vfifo.count) {
                AVPacket *vp = fifo_front(&vfifo);

                decode_video(src.vc, frame, vp->data, vp->size);
                fifo_pop(&vfifo);
                progressed = 1;
            } else if (demux_eof && !vflushed) {
                if (!decode_video(src.vc, frame, NULL, 0))
                    vflushed = 1;
                progressed = 1;
            }
            cur_it.video = (uint32_t)(timer_us_gettime64() - st0);
        }

        if (!started &&
            ((ring.wpos >= PREFILL_BYTES && vq_count >= VQ_PREFILL) ||
             (demux_eof && vflushed))) {
            printf("av: prefilled %lu audio bytes, %u video frames; starting\n",
                   (unsigned long)ring.wpos, vq_count);
            snd_stream_start(hnd, (uint32_t)sample_rate, channels - 1);
            t_start_us = timer_us_gettime64();
            started = 1;
            poll_prev_us = 0;
        }

        /* Time only iterations that did work (sleeping is not a stall). */
        cur_it.total = (uint32_t)(timer_us_gettime64() - it0);
        if (progressed) {
            iters++;
            if (cur_it.total > 42000)
                iters_over_42ms++;
            if (cur_it.total > 60000)
                iters_over_60ms++;
            if (cur_it.total > it_max.total) it_max.total = cur_it.total;
            if (cur_it.demux > it_max.demux) it_max.demux = cur_it.demux;
            if (cur_it.audio > it_max.audio) it_max.audio = cur_it.audio;
            if (cur_it.video > it_max.video) it_max.video = cur_it.video;
            if (cur_it.present > it_max.present) it_max.present = cur_it.present;
        }
        if (started && !demux_eof) {
            if (vfifo.count < fifo_min_v)
                fifo_min_v = vfifo.count;
            if (afifo.count < fifo_min_a)
                fifo_min_a = afifo.count;
            if (afifo.count == 0)
                afifo_empty_iters++;
        }
        prev_it = cur_it;
        if (!progressed && !av_source_pump(&src))
            thd_sleep(2);
    }

    snd_stream_stop(hnd);
    snd_stream_destroy(hnd);
    snd_stream_shutdown();

    /* ---- report ---- */
    {
        int64_t lmin = INT64_MAX, lmax = INT64_MIN;
        unsigned k;
        double lslope = slope_ms_per_s(lead_us, lead_t_ms, lead_n);
        double dslope = slope_ms_per_s(late_us_log, late_t_ms, late_n);

        for (k = 0; k < lead_n; k++) {
            if (lead_us[k] < lmin)
                lmin = lead_us[k];
            if (lead_us[k] > lmax)
                lmax = lead_us[k];
        }
        printf("av: video decoded=%lu presented=%lu dropped=%lu errors=%lu "
               "(fixture has 720)\n", vframes, presented, dropped, verrors);
        av_source_print_stats(&src);
#ifdef MPEG_AV_FRAME_TRACE
        printf("av: FRAME_TRACE checksum_samples=%u presented=%lu drops=%u overflow=%lu\n",
               av_checksum_trace_n, presented, av_drop_trace_n,
               dropped > av_drop_trace_n ? dropped - av_drop_trace_n : 0);
        for (k = 0; k < av_checksum_trace_n; k++)
            printf("av: FRAME_CHECKSUM idx=%lu y=%08lx u=%08lx v=%08lx\n",
                   av_checksum_trace[k].idx,
                   (unsigned long)av_checksum_trace[k].y,
                   (unsigned long)av_checksum_trace[k].u,
                   (unsigned long)av_checksum_trace[k].v);
        for (k = 0; k < av_drop_trace_n; k++)
            printf("av: FRAME_DROP idx=%lu at=%luus due=%luus\n",
                   av_drop_trace[k].idx,
                   (unsigned long)av_drop_trace[k].time_us,
                   (unsigned long)av_drop_trace[k].due_us);
        if (dropped == av_drop_trace_n) {
            for (k = 0; k < vframes; k++) {
                unsigned j;
                int was_dropped = 0;

                for (j = 0; j < av_drop_trace_n; j++)
                    if (av_drop_trace[j].idx == k) {
                        was_dropped = 1;
                        break;
                    }
                if (!was_dropped)
                    printf("av: FRAME_PRESENT idx=%u\n", k);
            }
        }
#endif
        printf("av: video lateness at present: avg=%llu us max=%llu us  "
               ">16.7ms=%lu >1 frame=%lu\n",
               (unsigned long long)(presented ? late_sum_us / presented : 0),
               (unsigned long long)late_max_us, late_16ms, late_frame);
        printf("av: lateness trend slope=%.4f ms/s (first 10%% excluded), "
               "last frame lateness=%.1f ms\n", dslope,
               late_n ? (double)late_us_log[late_n - 1] / 1000.0 : 0.0);
        for (k = 0; k < 10 && late_n; k++) {
            unsigned idx = (unsigned)((uint64_t)k * (late_n - 1) / 9);

            printf("av:   lateness @%6.2f s = %.1f ms\n",
                   (double)late_t_ms[idx] / 1000.0, (double)late_us_log[idx] / 1000.0);
        }
        printf("av: video queue depth at present: min=%u avg=%.2f max=%u (slots %d)\n",
               vq_depth_n ? vq_depth_min : 0,
               vq_depth_n ? (double)vq_depth_sum / (double)vq_depth_n : 0.0,
               vq_depth_max, VQ_SLOTS);
        printf("av: video decode total=%llu us avg=%.3f ms/frame max=%llu us; "
               "queue copy avg=%.3f ms max=%llu us\n", (unsigned long long)vdec_us,
               vframes ? (double)vdec_us / (double)vframes / 1000.0 : 0.0,
               (unsigned long long)vdec_max,
               vframes ? (double)vcopy_us / (double)vframes / 1000.0 : 0.0,
               (unsigned long long)vcopy_max);
#ifdef MPEG_DECODE_PROFILE
        {
            unsigned stage;

#ifdef MPEG_CACHE_PROFILE_EVENT
            {
                unsigned pt;

                printf("av: CACHE_EVENT code=0x%02x frames=%lu whole-decode events=%llu (%.0f/frame)\n",
                       (unsigned)MPEG_CACHE_PROFILE_EVENT, vframes, decode_event_total,
                       vframes ? (double)decode_event_total / vframes : 0.0);
                for (pt = FF_I_TYPE; pt <= FF_B_TYPE; pt++) {
                    const char tc = pt == FF_I_TYPE ? 'I' : pt == FF_P_TYPE ? 'P' : 'B';
                    const unsigned long nf = picture_decode_count[pt];

                    printf("av: CACHE_EVENT code=0x%02x pict=%c frames=%lu decode_events=%llu per_frame=%.1f\n",
                           (unsigned)MPEG_CACHE_PROFILE_EVENT, tc, nf, event_total_by[pt],
                           nf ? (double)event_total_by[pt] / nf : 0.0);
                    for (stage = 0; stage < 4; stage++)
                        printf("av: CACHE_EVENT code=0x%02x pict=%c stage=%s est=%llu per_frame=%.1f "
                               "per_mb=%.3f share_of_pict_decode=%.1f%% samples=%lu\n",
                               (unsigned)MPEG_CACHE_PROFILE_EVENT, tc, decode_stage_name[stage],
                               event_stage[pt][stage],
                               nf ? (double)event_stage[pt][stage] / nf : 0.0,
                               nf ? (double)event_stage[pt][stage] / nf / 300.0 : 0.0,
                               event_total_by[pt] ? 100.0 * event_stage[pt][stage] / event_total_by[pt] : 0.0,
                               event_samples[pt][stage]);
                }
            }
#else
            for (stage = 0; stage < 4; stage++)
                printf("av: sampled %s est=%llu us avg=%.3f ms/frame "
                       "%.1f%% decode, samples=%lu (1/4 macroblocks)\n",
                       decode_stage_name[stage],
                       (unsigned long long)decode_stage_us[stage],
                       vframes ? (double)decode_stage_us[stage] / vframes / 1000.0 : 0.0,
                       vdec_us ? 100.0 * decode_stage_us[stage] / vdec_us : 0.0,
                       decode_stage_samples[stage]);
#endif
            for (stage = FF_I_TYPE; stage <= FF_B_TYPE; stage++)
                printf("av: picture %c decode count=%lu avg=%.3f ms max=%.3f ms\n",
                       stage == FF_I_TYPE ? 'I' : stage == FF_P_TYPE ? 'P' : 'B',
                       picture_decode_count[stage],
                       picture_decode_count[stage] ? (double)picture_decode_us[stage] /
                           picture_decode_count[stage] / 1000.0 : 0.0,
                       picture_decode_max[stage] / 1000.0);
        }
#endif
        printf("av: packet FIFO max depth video=%u audio=%u (cap %d); min depth after "
               "start video=%u audio=%u; iterations with empty audio FIFO=%lu\n",
               vfifo.max_count, afifo.max_count, PF_CAP, fifo_min_v, fifo_min_a,
               afifo_empty_iters);
        printf("av: iterations with work=%lu  >42ms=%lu >60ms=%lu; max total=%u us "
               "(demux %u audio %u video %u present %u); demux avg=%llu us\n", iters,
               iters_over_42ms, iters_over_60ms, it_max.total, it_max.demux,
               it_max.audio, it_max.video, it_max.present,
               (unsigned long long)(demux_n ? demux_us / demux_n : 0));
        for (k = 0; k < late_event_n; k++) {
            const late_event_t *e = &late_events[k];

            printf("av:   LATE @%6.2f s by %.1f ms  vq=%u vfifo=%u afifo=%u  prev iter "
                   "total=%u demux=%u audio=%u video=%u present=%u us\n",
                   (double)e->t_ms / 1000.0, (double)e->late_us / 1000.0, e->vq,
                   e->vfifo, e->afifo, e->prev.total, e->prev.demux, e->prev.audio,
                   e->prev.video, e->prev.present);
        }
        {
            unsigned q;

            printf("av: per-second stage cost (ms total in that second; dec/present "
                   "per-frame avg; dec max)\n");
            for (q = 0; q <= sec_last; q++) {
                const sec_t *sc = &secs[q];

                printf("av:   sec %2u: dec=%6.1f (%2u fr, avg %5.2f, max %5.2f) "
                       "copy=%5.1f pres=%6.1f (%2u fr, avg %5.2f) adec=%5.1f (%2u) "
                       "demux=%6.1f\n", q, sc->us[ST_DEC] / 1000.0, sc->vdec_n,
                       sc->vdec_n ? sc->us[ST_DEC] / 1000.0 / sc->vdec_n : 0.0,
                       sc->dec_max / 1000.0, sc->us[ST_COPY] / 1000.0,
                       sc->us[ST_PRES] / 1000.0, sc->pres_n,
                       sc->pres_n ? sc->us[ST_PRES] / 1000.0 / sc->pres_n : 0.0,
                       sc->us[ST_ADEC] / 1000.0, sc->adec_n, sc->us[ST_DEMUX] / 1000.0);
            }
        }
        {
            pvr_video_prof_t pp;
            double n;

            pvr_video_get_prof(&pp);
            n = pp.frames ? (double)pp.frames : 1.0;
            printf("av: present split (avg/max ms): wait_ready %.3f/%.3f  "
                   "blocks %.3f/%.3f  scene submit %.3f/%.3f  (%lu frames)\n",
                   (double)pp.wait_us / n / 1000.0, (double)pp.wait_max / 1000.0,
                   (double)pp.blocks_us / n / 1000.0, (double)pp.blocks_max / 1000.0,
                   (double)pp.submit_us / n / 1000.0, (double)pp.submit_max / 1000.0,
                   pp.frames);
            printf("av: present blocks: fast=%lu pad=%lu generic=%lu (per frame %lu/%lu/%lu)\n",
                   pp.blk_fast, pp.blk_pad, pp.blk_generic, pp.blk_fast / (pp.frames ? pp.frames : 1),
                   pp.blk_pad / (pp.frames ? pp.frames : 1), pp.blk_generic / (pp.frames ? pp.frames : 1));
        }
        printf("av: pvr present avg=%.3f ms max=%llu us\n",
               presented ? (double)pres_us / (double)presented / 1000.0 : 0.0,
               (unsigned long long)pres_max);
        printf("av: audio decoded frames=%lu samples/ch=%llu errors=%lu; MP2 decode "
               "avg=%.3f ms/frame max=%llu us\n", aframes,
               (unsigned long long)(decoded_bytes / (2ULL * channels)), aerrors,
               aframes ? (double)adec_us / (double)aframes / 1000.0 : 0.0,
               (unsigned long long)adec_max);
        if (channels == 2) {
            unsigned w;

            for (w = 0; w < 2; w++)
                printf("av: decoded stereo check, %s: even(L) rms=%.1f odd(R) rms=%.1f "
                       "(expect beeps in R)\n", w ? "no-beep 0.30-1.90 s" : "beep 0-0.25 s",
                       ch_count[w] ? sqrt(ch_sumsq[w][0] / (double)ch_count[w]) : 0.0,
                       ch_count[w] ? sqrt(ch_sumsq[w][1] / (double)ch_count[w]) : 0.0);
        }
        printf("av: audio submitted samples/ch=%llu callbacks=%lu underruns(NULL "
               "before EOS)=%lu short=%lu wrap_short=%lu NULL_after_EOS=%lu "
               "poll(-3)=%lu poll_err=%d\n",
               (unsigned long long)(bytes_submitted / (2ULL * channels)), cb_calls,
               cb_null_underrun, cb_short, cb_wrap_short, cb_null_eos, poll_minus3,
               poll_err_first);
        printf("av: audio ring depth bytes min=%llu avg=%llu max=%llu (cap %d)\n",
               (unsigned long long)(depth_n ? depth_min : 0),
               (unsigned long long)(depth_n ? depth_sum / depth_n : 0),
               (unsigned long long)depth_max, RING_SIZE);
        printf("av: poll calls=%lu gap avg=%llu us max=%llu us >46ms=%lu >93ms=%lu; "
               "poll dur avg=%llu us max=%llu us\n", poll_calls,
               (unsigned long long)(poll_gaps ? poll_gap_sum / poll_gaps : 0),
               (unsigned long long)poll_gap_max, poll_gap_over_46ms,
               poll_gap_over_93ms,
               (unsigned long long)(poll_calls ? poll_dur_sum / poll_calls : 0),
               (unsigned long long)poll_dur_max);
        printf("av: audio lead (submitted - elapsed) n=%u min=%.1f ms max=%.1f ms "
               "slope=%.4f ms/s\n", lead_n, lead_n ? (double)lmin / 1000.0 : 0.0,
               lead_n ? (double)lmax / 1000.0 : 0.0, lslope);
        printf("av: sync offset sync_offset_us=%d first_due=%lld us\n",
               sync_offset_us, (long long)first_due_us);
    }

    /* Relaxed 2D gate (a single-threaded player cannot avoid an occasional drop
     * when one ~46 ms decode crosses a 41.7 ms presentation boundary):
     *   audio: 0 underruns/short returns/errors, poll gap < 93 ms, all PCM delivered
     *   video: 720 decoded, no presented frame over one frame late,
     *          <= 1% dropped, presented + dropped == decoded
     * Lead/lateness trends are printed for judgement, not gated. */
    audio_fail = cb_null_underrun || cb_short || aerrors || poll_err_first ||
                 poll_gap_over_93ms || bytes_submitted != decoded_bytes;
    video_fail = vframes != 720 || verrors || late_max_us > frame_dt_us ||
                 dropped * 100 > vframes || presented + dropped != vframes;
    fail = audio_fail || video_fail;
    printf("av: audio gate %s, video gate %s -> %s\n", audio_fail ? "FAIL" : "PASS",
           video_fail ? "FAIL" : "PASS", fail ? "FAIL (see counts above)" : "PASS");
#ifdef MPEG_IDCT_BLOCK_COUNTS
    {
        static const char *picture_name[3] = { "I", "P", "B" };
        static const char *class_name[6] = { "-1/empty", "0/DC", "1-3", "4-7", "8-15", ">15" };
        unsigned picture, path, cls;

        printf("av: IDCT block_last_index counts (add includes skipped -1 candidates)\n");
        for (picture = 0; picture < 3; picture++) {
            for (path = 0; path < 2; path++) {
                printf("av:   %s %s:", picture_name[picture], path ? "put" : "add");
                for (cls = 0; cls < 6; cls++)
                    printf(" %s=%lu", class_name[cls],
                           mpeg_idct_block_counts[picture][path][cls]);
                printf("\n");
            }
        }
    }
#endif
    printf("av: (look and listen: picture correct, lips/audio in sync?)\n");

#ifdef MPEG_ISLAND_DEBUG
    printf("av: IDCT island guard=%s highwater=%08lx used=%lu/512 bytes\n",
           mpeg_idct_private_stack_fault ? "FAIL" : "PASS",
           (unsigned long)mpeg_idct_private_stack_highwater,
           (unsigned long)((uintptr_t)mpeg_idct_private_stack_top -
                           mpeg_idct_private_stack_highwater));
#endif

    pvr_video_shutdown();
    av_source_close(&src);
    av_free(samples);
    av_free(frame);
    free(ring.data);
    return fail;
}
