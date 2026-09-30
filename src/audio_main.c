/* Slice 2 milestone 2B: buffered AICA audio playback, no video.
 * Demux the original MPEG-PS, decode MP2 to interleaved S16, queue the PCM in
 * an aligned ring and play it through KOS snd_stream (interleaved callback, no
 * deinterleave). The callback only hands out PCM that is already decoded.
 *
 * Lifetime rule: the region returned by callback N stays untouched until
 * callback N+1 has returned. Mono DMA reads the returned pointer
 * asynchronously (snd_stream_fill() waits for the previous DMA only after
 * get_data() returns), so the producer may only overwrite bytes before the
 * start of the previous callback's region. */
#include <kos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/mem.h"
#include "ffmpeg_av.h"

#ifndef AUDIO_CD_PATH
#define AUDIO_CD_PATH "/cd/lair_320_23976_30s_spec.mpg"
#endif

#ifndef AUDIO_PC_PATH
#define AUDIO_PC_PATH \
    "/pc/fixtures/lair_320_23976_30s_spec.mpg"
#endif

/* Per-channel AICA stream buffer. Refills are requested in half-buffer
 * chunks: 8 KiB per channel = ~93 ms at 44.1 kHz S16. */
#ifndef AUDIO_STREAM_BUFFER
#define AUDIO_STREAM_BUFFER (16 * 1024)
#endif

/* PCM ring capacity in bytes (power of two, multiple of 32). */
#define RING_SIZE (128 * 1024)
#define RING_MASK (RING_SIZE - 1)

/* Decode this much PCM before starting the stream (>= the two prefill
 * requests snd_stream_start() makes: AUDIO_STREAM_BUFFER * channels). */
#define PREFILL_BYTES (32 * 1024)

/* A refill that starts later than this after the previous poll risks an
 * underrun (half buffer = ~93 ms). */
#define POLL_DANGER_US 93000

#define LEAD_MAX 4096

typedef struct {
    uint8_t *data;          /* RING_SIZE bytes, 32-byte aligned */
    uint64_t wpos;          /* total bytes written by the decoder */
    uint64_t rpos;          /* total bytes handed to KOS */
    uint64_t rel;           /* bytes below this are free for the decoder */
    uint64_t last_start;    /* start of the region returned by the last callback */
} pcm_ring_t;

static pcm_ring_t ring;
static int eof;                     /* demuxer hit EOF, decoder has nothing more */
static int started;                 /* snd_stream_start() has returned */
static int channels = 1;
static int sample_rate = 44100;
static uint64_t t_start_us;

/* Instrumentation */
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

static void *audio_callback(snd_stream_hnd_t hnd, int smp_req, int *smp_recv) {
    uint64_t avail = ring.wpos - ring.rpos;
    size_t off = (size_t)(ring.rpos & RING_MASK);
    size_t contig = RING_SIZE - off;
    size_t n;

    (void)hnd;
    cb_calls++;

    /* Callback N-1's region may still be in flight; everything before its
     * start (i.e. up to callback N-2's end) is done. */
    ring.rel = ring.last_start;
    ring.last_start = ring.rpos;

    /* Callbacks after the last sample is queued (eof and ring empty) submit
     * nothing, so they say nothing about lead or depth. */
    if (started && !(eof && avail == 0)) {
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

    if (contig > avail)
        contig = (size_t)avail;
    n = contig < (size_t)smp_req ? contig : (size_t)smp_req;
    n &= ~(size_t)31;

    if (n == 0) {
        *smp_recv = 0;
        if (started) {
            if (eof && avail == 0)
                cb_null_eos++;
            else
                cb_null_underrun++;
        }
        return NULL;
    }
    if (started && n < (size_t)smp_req) {
        if (avail >= (uint64_t)smp_req)
            cb_wrap_short++;        /* ring end cut the region; expected */
        else if (!(eof && n == avail))
            cb_short++;             /* decoder had less than requested */
    }

    ring.rpos += n;
    bytes_submitted += n;
    *smp_recv = (int)n;
    return ring.data + off;
}

static uint8_t *load_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;

    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = av_malloc((unsigned int)size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        av_free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    memset(data + size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    *size_out = (size_t)size;
    return data;
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

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : AUDIO_CD_PATH;
    uint8_t *file_data;
    size_t file_size = 0;
    ByteIOContext pb;
    AVProbeData pd;
    AVInputFormat *fmt;
    AVFormatContext *ic = NULL;
    AVCodecContext *ac = NULL;
    AVCodec *adec;
    int16_t *samples;
    int vidx = -1, aidx = -1;
    unsigned i;
    int ret;
    snd_stream_hnd_t hnd;
    unsigned long aframes = 0, aerrors = 0;
    uint64_t decoded_bytes = 0;
    uint64_t adec_us = 0, adec_max = 0;
    uint64_t drain_deadline_us = 0;
    uint64_t buffer_us;
    int fail = 0;

    printf("dc-libavmpeg audio: Slice 2B buffered AICA playback (no video)\n");
    file_data = load_file(path, &file_size);
    if (!file_data && argc <= 1) {
        printf("audio: %s unavailable; falling back to %s\n", path, AUDIO_PC_PATH);
        path = AUDIO_PC_PATH;
        file_data = load_file(path, &file_size);
    }
    if (!file_data) {
        printf("audio: cannot load %s\n", path);
        return 1;
    }
    printf("audio: loaded %s (%lu bytes)\n", path, (unsigned long)file_size);

    avcodec_init();
    ffmpeg_register_mpegps_av();
    init_put_byte(&pb, file_data, (int)file_size, 0, NULL, NULL, NULL, NULL);
    pd.filename = path;
    pd.buf = file_data;
    pd.buf_size = file_size > 4096 ? 4096 : (int)file_size;
    fmt = av_probe_input_format(&pd, 1);
    if (!fmt || av_open_input_stream(&ic, &pb, path, fmt, NULL) < 0 ||
        av_find_stream_info(ic) < 0) {
        printf("audio: demuxer open failed\n");
        return 1;
    }
    for (i = 0; i < ic->nb_streams; i++) {
        AVCodecContext *c = ic->streams[i]->codec;

        if (c->codec_type == CODEC_TYPE_VIDEO && vidx < 0)
            vidx = (int)i;
        else if (c->codec_type == CODEC_TYPE_AUDIO && aidx < 0 &&
                 c->codec_id == CODEC_ID_MP2)
            aidx = (int)i;
    }
    if (aidx < 0) {
        printf("audio: no MP2 stream\n");
        return 1;
    }
    ac = ic->streams[aidx]->codec;
    adec = avcodec_find_decoder(ac->codec_id);
    if (!adec || avcodec_open(ac, adec) < 0) {
        printf("audio: MP2 decoder open failed\n");
        return 1;
    }
    channels = ac->channels;
    sample_rate = ac->sample_rate;
    if (channels < 1 || channels > 2) {
        printf("audio: unsupported channel count %d\n", channels);
        return 1;
    }
    printf("audio: MP2 %d Hz, %d channel(s); video stream %d is skipped\n",
           sample_rate, channels, vidx);

    ring.data = aligned_alloc(32, RING_SIZE);
    samples = av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);
    if (!ring.data || !samples) {
        printf("audio: out of memory\n");
        return 1;
    }

    if (snd_stream_init_ex(2, AUDIO_STREAM_BUFFER) < 0) {
        printf("audio: snd_stream_init_ex failed\n");
        return 1;
    }
    hnd = snd_stream_alloc(audio_callback, AUDIO_STREAM_BUFFER);
    if (hnd == SND_STREAM_INVALID) {
        printf("audio: snd_stream_alloc failed\n");
        return 1;
    }
    buffer_us = (uint64_t)AUDIO_STREAM_BUFFER / 2 * 1000000ULL / sample_rate;

    /* Producer/consumer loop. Decode one audio packet per iteration when the
     * ring has room, polling before every decode step. */
    while (1) {
        AVPacket pkt;

        if (started) {
            do_poll(hnd);
            if (eof && ring.wpos == ring.rpos) {
                uint64_t now = timer_us_gettime64();

                /* Everything is queued in KOS/AICA; let the stream buffer
                 * play out before stopping. */
                if (!drain_deadline_us)
                    drain_deadline_us = now + buffer_us + 150000;
                else if (now >= drain_deadline_us)
                    break;
            }
        }

        if (!eof && ring_free() >= 4608) {
            ret = av_read_frame(ic, &pkt);
            if (ret < 0) {
                eof = 1;
                printf("audio: demux EOF (ret %d, url_feof %d) at %lu frames\n",
                       ret, url_feof(ic->pb), aframes);
            } else {
                if (pkt.stream_index == aidx) {
                    const uint8_t *data = pkt.data;
                    int size = pkt.size;

                    while (size > 0) {
                        int out_bytes = AVCODEC_MAX_AUDIO_FRAME_SIZE;
                        uint64_t t0 = timer_us_gettime64();
                        uint64_t dt;
                        int len = avcodec_decode_audio2(ac, samples, &out_bytes,
                                                        data, size);

                        dt = timer_us_gettime64() - t0;
                        adec_us += dt;
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
                            ring_put(samples, (size_t)out_bytes);
                            decoded_bytes += (uint64_t)out_bytes;
                            aframes++;
                        }
                    }
                }
                av_free_packet(&pkt);
            }
        } else if (started && !eof) {
            thd_sleep(5);   /* ring is full: wait for the AICA to drain it */
        } else if (started && eof) {
            thd_sleep(5);
        }

        if (!started && (eof || ring.wpos >= PREFILL_BYTES)) {
            printf("audio: prefilled %lu bytes, starting stream\n",
                   (unsigned long)ring.wpos);
            snd_stream_start(hnd, (uint32_t)sample_rate, channels - 1);
            t_start_us = timer_us_gettime64();
            started = 1;
            poll_prev_us = 0;
        }
    }

    snd_stream_stop(hnd);
    snd_stream_destroy(hnd);
    snd_stream_shutdown();

    /* Report */
    {
        int64_t lmin = INT64_MAX, lmax = INT64_MIN;
        double sx = 0, sy = 0, sxx = 0, sxy = 0, slope = 0;
        unsigned k, skip = lead_n / 10;    /* ignore the first 10%: start-up */
        unsigned m = 0;

        for (k = 0; k < lead_n; k++) {
            if (lead_us[k] < lmin)
                lmin = lead_us[k];
            if (lead_us[k] > lmax)
                lmax = lead_us[k];
        }
        for (k = skip; k < lead_n; k++) {
            double x = (double)lead_t_ms[k] / 1000.0;
            double y = (double)lead_us[k] / 1000.0;

            sx += x; sy += y; sxx += x * x; sxy += x * y; m++;
        }
        if (m > 2 && (double)m * sxx - sx * sx > 0.0)
            slope = ((double)m * sxy - sx * sy) / ((double)m * sxx - sx * sx);

        printf("audio: decoded frames=%lu bytes=%llu samples/ch=%llu errors=%lu\n",
               aframes, (unsigned long long)decoded_bytes,
               (unsigned long long)(decoded_bytes / (2ULL * channels)), aerrors);
        printf("audio: submitted bytes=%llu samples/ch=%llu (%.4f s) callbacks=%lu\n",
               (unsigned long long)bytes_submitted,
               (unsigned long long)(bytes_submitted / (2ULL * channels)),
               (double)(bytes_submitted / (2ULL * channels)) / sample_rate, cb_calls);
        printf("audio: underruns(NULL before EOS)=%lu short_returns=%lu "
               "wrap_short=%lu NULL_after_EOS=%lu poll(-3)=%lu poll_err=%d\n",
               cb_null_underrun, cb_short, cb_wrap_short, cb_null_eos, poll_minus3,
               poll_err_first);
        printf("audio: ring depth at callback bytes min=%llu avg=%llu max=%llu "
               "(cap %d)\n", (unsigned long long)(depth_n ? depth_min : 0),
               (unsigned long long)(depth_n ? depth_sum / depth_n : 0),
               (unsigned long long)depth_max, RING_SIZE);
        printf("audio: poll calls=%lu gap avg=%llu us max=%llu us  >46ms=%lu "
               ">93ms=%lu\n", poll_calls,
               (unsigned long long)(poll_gaps ? poll_gap_sum / poll_gaps : 0),
               (unsigned long long)poll_gap_max, poll_gap_over_46ms,
               poll_gap_over_93ms);
        printf("audio: poll duration avg=%llu us max=%llu us\n",
               (unsigned long long)(poll_calls ? poll_dur_sum / poll_calls : 0),
               (unsigned long long)poll_dur_max);
        printf("audio: MP2 decode total=%llu us avg=%.3f ms/frame max=%llu us\n",
               (unsigned long long)adec_us,
               aframes ? (double)adec_us / (double)aframes / 1000.0 : 0.0,
               (unsigned long long)adec_max);
        printf("audio: lead (submitted - elapsed) samples=%u min=%.1f ms max=%.1f ms "
               "slope=%.4f ms/s (first 10%% excluded)\n", lead_n,
               lead_n ? (double)lmin / 1000.0 : 0.0,
               lead_n ? (double)lmax / 1000.0 : 0.0, slope);
        for (k = 0; k < 10 && lead_n; k++) {
            unsigned idx = (unsigned)((uint64_t)k * (lead_n - 1) / 9);

            printf("audio:   lead @%6.2f s = %.1f ms\n",
                   (double)lead_t_ms[idx] / 1000.0, (double)lead_us[idx] / 1000.0);
        }
    }

    if (cb_null_underrun || cb_short || aerrors || poll_err_first || poll_gap_over_93ms ||
        bytes_submitted != decoded_bytes || !eof)
        fail = 1;
    printf("audio: %s\n", fail ? "FAIL (see counts above)" : "PASS");
    printf("audio: (listen: whole clip continuous and correct? that part is yours)\n");

    avcodec_close(ac);
    av_close_input_stream(ic);
    av_free(samples);
    free(ring.data);
    av_free(file_data);
    return fail;
}
