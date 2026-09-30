/* Slice 2 milestone 2A: MPEG-PS demux + MP2/MPEG-1 decode correctness.
 * No AICA, no PVR, no sync. Opens the original program stream from memory via
 * the vendored FFmpeg 0.5 libavformat, decodes every packet and reports stream
 * selection, packet/frame/sample counts and PTS ranges. */
#include <kos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/mem.h"
#include "ffmpeg_av.h"

#ifndef PROBE_CD_PATH
#define PROBE_CD_PATH "/cd/lair_320_23976_30s_spec.mpg"
#endif

#ifndef PROBE_PC_PATH
#define PROBE_PC_PATH \
    "/pc/fixtures/lair_320_23976_30s_spec.mpg"
#endif

#ifndef PROBE_EXPECT_VIDEO_FRAMES
#define PROBE_EXPECT_VIDEO_FRAMES 720
#endif

#ifndef PROBE_IDCT_ALGO
#define PROBE_IDCT_ALGO FF_IDCT_SIMPLE
#endif

typedef struct {
    unsigned long packets;
    unsigned long no_pts;
    int64_t first_pts;
    int64_t last_pts;
    int64_t min_pts;
    int64_t max_pts;
    int64_t first_dts;
    int64_t last_dts;
    int64_t prev_pts;
    int64_t max_gap;        /* largest |pts delta| between consecutive packets */
    unsigned long bytes;
} pkt_stats_t;

static void stats_init(pkt_stats_t *s) {
    memset(s, 0, sizeof(*s));
    s->first_pts = s->last_pts = s->min_pts = s->max_pts = AV_NOPTS_VALUE;
    s->first_dts = s->last_dts = s->prev_pts = AV_NOPTS_VALUE;
}

static void stats_add(pkt_stats_t *s, const AVPacket *pkt) {
    s->packets++;
    s->bytes += (unsigned long)pkt->size;
    if (pkt->dts != AV_NOPTS_VALUE) {
        if (s->first_dts == AV_NOPTS_VALUE)
            s->first_dts = pkt->dts;
        s->last_dts = pkt->dts;
    }
    if (pkt->pts == AV_NOPTS_VALUE) {
        s->no_pts++;
        return;
    }
    if (s->first_pts == AV_NOPTS_VALUE)
        s->first_pts = pkt->pts;
    s->last_pts = pkt->pts;
    if (s->min_pts == AV_NOPTS_VALUE || pkt->pts < s->min_pts)
        s->min_pts = pkt->pts;
    if (s->max_pts == AV_NOPTS_VALUE || pkt->pts > s->max_pts)
        s->max_pts = pkt->pts;
    if (s->prev_pts != AV_NOPTS_VALUE) {
        int64_t gap = pkt->pts - s->prev_pts;
        if (gap < 0)
            gap = -gap;
        if (gap > s->max_gap)
            s->max_gap = gap;
    }
    s->prev_pts = pkt->pts;
}

static double pts_sec(int64_t pts, AVRational tb) {
    if (pts == AV_NOPTS_VALUE)
        return -1.0;
    return (double)pts * (double)tb.num / (double)tb.den;
}

static void print_stats(const char *name, const pkt_stats_t *s, AVRational tb) {
    printf("probe: %s packets=%lu bytes=%lu no_pts=%lu\n", name, s->packets,
           s->bytes, s->no_pts);
    printf("probe: %s pts first=%.4f last=%.4f min=%.4f max=%.4f (s)\n", name,
           pts_sec(s->first_pts, tb), pts_sec(s->last_pts, tb),
           pts_sec(s->min_pts, tb), pts_sec(s->max_pts, tb));
    printf("probe: %s dts first=%.4f last=%.4f  max consecutive pts gap=%.4f s\n",
           name, pts_sec(s->first_dts, tb), pts_sec(s->last_dts, tb),
           pts_sec(s->max_gap, tb));
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

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : PROBE_CD_PATH;
    uint8_t *file_data;
    size_t file_size = 0;
    ByteIOContext pb;
    AVProbeData pd;
    AVInputFormat *fmt;
    AVFormatContext *ic = NULL;
    AVCodecContext *vc = NULL, *ac = NULL;
    AVCodec *vdec, *adec;
    AVFrame *frame;
    int16_t *samples;
    int vidx = -1, aidx = -1;
    unsigned i;
    int ret;
    pkt_stats_t vstat, astat;
    unsigned long vframes = 0, aframes = 0, verrors = 0, aerrors = 0;
    unsigned long long asamples = 0;
    uint32_t a_sum = 1, a_sum2 = 0;
    int a_peak = 0;
    uint64_t vdec_us = 0, adec_us = 0, vdec_max = 0, adec_max = 0;
    uint64_t start_us;
    int fail = 0;

    printf("dc-libavmpeg probe: Slice 2A MPEG-PS demux + MP2 decode\n");
    file_data = load_file(path, &file_size);
    if (!file_data && argc <= 1) {
        printf("probe: %s unavailable; falling back to %s\n", path, PROBE_PC_PATH);
        path = PROBE_PC_PATH;
        file_data = load_file(path, &file_size);
    }
    if (!file_data) {
        printf("probe: cannot load %s\n", path);
        return 1;
    }
    printf("probe: loaded %s (%lu bytes)\n", path, (unsigned long)file_size);

    avcodec_init();
    ffmpeg_register_mpegps_av();
#ifdef PROBE_VERBOSE
    av_log_set_level(AV_LOG_DEBUG);
    {
        static const enum CodecID ids[] = { CODEC_ID_MPEG1VIDEO, CODEC_ID_MPEG2VIDEO, CODEC_ID_MP2 };
        unsigned k;

        for (k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
            AVCodec *c = avcodec_find_decoder(ids[k]);
            AVCodecContext *tc = avcodec_alloc_context();
            int r = c && tc ? avcodec_open(tc, c) : -999;

            printf("probe: diag standalone open id=%d decoder=%s -> %d\n", ids[k],
                   c ? c->name : "(none)", r);
            if (r == 0)
                avcodec_close(tc);
            av_free(tc);
        }
    }
#endif

    init_put_byte(&pb, file_data, (int)file_size, 0, NULL, NULL, NULL, NULL);
    pd.filename = path;
    pd.buf = file_data;
    pd.buf_size = file_size > 4096 ? 4096 : (int)file_size;
    fmt = av_probe_input_format(&pd, 1);
    if (!fmt) {
        printf("probe: no input format matched\n");
        return 1;
    }
    printf("probe: format %s (%s)\n", fmt->name, fmt->long_name);
    ret = av_open_input_stream(&ic, &pb, path, fmt, NULL);
    if (ret < 0 || !ic) {
        printf("probe: av_open_input_stream failed (%d)\n", ret);
        return 1;
    }
    ret = av_find_stream_info(ic);
    if (ret < 0) {
        printf("probe: av_find_stream_info failed (%d)\n", ret);
        return 1;
    }

    printf("probe: container start_time=%.4f s duration=%.4f s streams=%u\n",
           ic->start_time == AV_NOPTS_VALUE ? -1.0 : (double)ic->start_time / AV_TIME_BASE,
           ic->duration == AV_NOPTS_VALUE ? -1.0 : (double)ic->duration / AV_TIME_BASE,
           ic->nb_streams);
    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        AVCodecContext *c = st->codec;

        printf("probe: stream %u id=0x%x type=%d codec_id=%d time_base=%d/%d\n", i,
               st->id, c->codec_type, c->codec_id, st->time_base.num,
               st->time_base.den);
        if (c->codec_type == CODEC_TYPE_VIDEO) {
            printf("probe:   video %dx%d pix_fmt=%d fps=%d/%d\n", c->width,
                   c->height, c->pix_fmt, st->r_frame_rate.num,
                   st->r_frame_rate.den);
            if (vidx < 0 && c->codec_id == CODEC_ID_MPEG1VIDEO)
                vidx = (int)i;
        } else if (c->codec_type == CODEC_TYPE_AUDIO) {
            printf("probe:   audio %d Hz channels=%d sample_fmt=%d bit_rate=%d\n",
                   c->sample_rate, c->channels, c->sample_fmt, c->bit_rate);
            if (aidx < 0 && c->codec_id == CODEC_ID_MP2)
                aidx = (int)i;
        }
    }
    if (vidx < 0 || aidx < 0) {
        printf("probe: FAIL missing MPEG-1 video (%d) or MP2 audio (%d) stream\n",
               vidx, aidx);
        return 1;
    }
    printf("probe: selected video stream %d, audio stream %d\n", vidx, aidx);

    vc = ic->streams[vidx]->codec;
    ac = ic->streams[aidx]->codec;
    vc->idct_algo = PROBE_IDCT_ALGO;
    vdec = avcodec_find_decoder(vc->codec_id);
    adec = avcodec_find_decoder(ac->codec_id);
    if (!vdec || !adec || avcodec_open(vc, vdec) < 0 || avcodec_open(ac, adec) < 0) {
        printf("probe: decoder open failed (video %p audio %p)\n", (void *)vdec,
               (void *)adec);
        return 1;
    }
    frame = avcodec_alloc_frame();
    samples = av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);
    if (!frame || !samples) {
        printf("probe: out of memory\n");
        return 1;
    }

    stats_init(&vstat);
    stats_init(&astat);
    start_us = timer_us_gettime64();
    while (1) {
        AVPacket pkt;
        uint64_t t0;
        uint64_t dt;

        ret = av_read_frame(ic, &pkt);
        if (ret < 0)
            break;
        if (pkt.stream_index == vidx) {
            int got = 0;

            stats_add(&vstat, &pkt);
            t0 = timer_us_gettime64();
            ret = avcodec_decode_video(vc, frame, &got, pkt.data, pkt.size);
            dt = timer_us_gettime64() - t0;
            vdec_us += dt;
            if (dt > vdec_max)
                vdec_max = dt;
            if (ret < 0)
                verrors++;
            if (got)
                vframes++;
        } else if (pkt.stream_index == aidx) {
            const uint8_t *data = pkt.data;
            int size = pkt.size;

            stats_add(&astat, &pkt);
            while (size > 0) {
                int out_bytes = AVCODEC_MAX_AUDIO_FRAME_SIZE;
                int n, len;

                t0 = timer_us_gettime64();
                len = avcodec_decode_audio2(ac, samples, &out_bytes, data, size);
                dt = timer_us_gettime64() - t0;
                adec_us += dt;
                if (dt > adec_max)
                    adec_max = dt;
                if (len < 0) {
                    aerrors++;
                    break;
                }
                data += len;
                size -= len;
                if (out_bytes > 0) {
                    const uint8_t *b = (const uint8_t *)samples;

                    aframes++;
                    asamples += (unsigned)(out_bytes / (2 * ac->channels));
                    for (n = 0; n < out_bytes; n++) {
                        a_sum = (a_sum + b[n]) % 65521;
                        a_sum2 = (a_sum2 + a_sum) % 65521;
                    }
                    for (n = 0; n < out_bytes / 2; n++) {
                        int v = samples[n] < 0 ? -samples[n] : samples[n];
                        if (v > a_peak)
                            a_peak = v;
                    }
                }
                if (len == 0)
                    break;
            }
        }
        av_free_packet(&pkt);
    }
    printf("probe: av_read_frame ended with %d, url_feof=%d\n", ret, url_feof(ic->pb));

    /* Flush delayed (B-frame reordering) pictures. */
    while (1) {
        int got = 0;
        uint64_t t0 = timer_us_gettime64();

        ret = avcodec_decode_video(vc, frame, &got, NULL, 0);
        vdec_us += timer_us_gettime64() - t0;
        if (ret < 0)
            verrors++;
        if (ret < 0 || !got)
            break;
        vframes++;
    }

    printf("probe: demux+decode wall=%llu us (file already in RAM)\n",
           (unsigned long long)(timer_us_gettime64() - start_us));
    print_stats("video", &vstat, ic->streams[vidx]->time_base);
    print_stats("audio", &astat, ic->streams[aidx]->time_base);
    printf("probe: video decoded frames=%lu (expected %d) errors=%lu\n", vframes,
           PROBE_EXPECT_VIDEO_FRAMES, verrors);
    printf("probe: video decode total=%llu us avg=%.3f ms/pkt max=%llu us\n",
           (unsigned long long)vdec_us,
           vstat.packets ? (double)vdec_us / (double)vstat.packets / 1000.0 : 0.0,
           (unsigned long long)vdec_max);
    printf("probe: audio decoded frames=%lu samples/channel=%llu channels=%d "
           "rate=%d errors=%lu\n", aframes, asamples, ac->channels,
           ac->sample_rate, aerrors);
    printf("probe: audio duration from samples=%.4f s  frames*1152=%lu\n",
           ac->sample_rate ? (double)asamples / (double)ac->sample_rate : 0.0,
           aframes * 1152UL);
    printf("probe: audio decode total=%llu us avg=%.3f ms/frame max=%llu us\n",
           (unsigned long long)adec_us,
           aframes ? (double)adec_us / (double)aframes / 1000.0 : 0.0,
           (unsigned long long)adec_max);
    printf("probe: audio pcm adler32=0x%04x%04x peak=%d\n", (unsigned)a_sum2,
           (unsigned)a_sum, a_peak);

    if (aframes != astat.packets)
        printf("probe: note: %lu audio packets produced %lu decoded frames\n",
               astat.packets, aframes);
    if (vframes != PROBE_EXPECT_VIDEO_FRAMES || verrors || aerrors)
        fail = 1;
    printf("probe: %s\n", fail ? "FAIL (see counts above)" : "PASS");

    avcodec_close(vc);
    avcodec_close(ac);
    av_close_input_stream(ic);
    av_free(frame);
    av_free(samples);
    av_free(file_data);
    return fail;
}
