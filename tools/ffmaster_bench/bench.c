/* Decode benchmark for UNMODIFIED current FFmpeg (master) libraries, modern API.
 * Same clip, same measurement idea as dc-libavmpeg's av_main.c "video decode" and
 * audiobench: the MPEG-PS is read from /pc/ through a custom AVIOContext, and only the avcodec_send_packet() /
 * avcodec_receive_frame() calls are timed. First 30 output frames are Adler-32
 * checksummed per plane with the same function as av_main.c's FRAME_TRACE, so the
 * output can be compared with the vendored FFmpeg 0.5 player's FRAME_CHECKSUM lines.
 *
 * The clip is the `fixture=` line of /pc/config.ini under /pc/fixtures/ (run from
 * the repo root with kos-tool -m .). Build: tools/ffmaster_bench/Makefile. */
#include <kos.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>

#define PASSES 3
#define CHECKSUM_FRAMES 30
#define DEFAULT_FIXTURE "lair_320_23976_30s_spec.mpg"

/* File-backed AVIO: the 16 MB DC heap cannot hold the clip plus the decoders. */
typedef struct { FILE *f; int64_t size; } membuf_t;

static int mem_read(void *opaque, uint8_t *buf, int size) {
    membuf_t *m = opaque;
    size_t n = fread(buf, 1, (size_t)size, m->f);

    return n ? (int)n : AVERROR_EOF;
}

static int64_t mem_seek(void *opaque, int64_t off, int whence) {
    membuf_t *m = opaque;

    if (whence & AVSEEK_SIZE)
        return m->size;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: case SEEK_CUR: case SEEK_END: break;
    default: return -1;
    }
    if (fseek(m->f, (long)off, whence & ~AVSEEK_FORCE) != 0)
        return -1;
    return ftell(m->f);
}

static void fixture_name(char *out, size_t n) {
    char line[256];
    FILE *f = fopen("/pc/config.ini", "r");

    snprintf(out, n, "%s", DEFAULT_FIXTURE);
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "fixture=", 8)) {
            size_t len = strcspn(line + 8, "\r\n");

            if (len > 0 && len < n) {
                memcpy(out, line + 8, len);
                out[len] = 0;
            }
        }
    fclose(f);
}

static uint32_t adler_plane(const uint8_t *src, int stride, int w, int h) {
    uint32_t a = 1, b = 0;
    int x, y;

    for (y = 0; y < h; y++, src += stride)
        for (x = 0; x < w; x++) {
            a = (a + src[x]) % 65521;
            b = (b + a) % 65521;
        }
    return (b << 16) | a;
}

static int run_pass(const char *path, int pass) {
    membuf_t mb = { fopen(path, "rb"), 0 };
    AVFormatContext *fmt = avformat_alloc_context();
    uint8_t *iobuf = av_malloc(32 * 1024);
    AVIOContext *io = avio_alloc_context(iobuf, 32 * 1024, 0, &mb, mem_read, NULL, mem_seek);
    AVCodecContext *vc = NULL, *ac = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVCodecParserContext *vpar = NULL, *apar = NULL;
    AVPacket *opkt = NULL;
    int vidx = -1, aidx = -1, ret;
    unsigned i;
    uint64_t vdec_us = 0, vdec_max = 0, adec_us = 0, adec_max = 0;
    unsigned long vframes = 0, aframes = 0, vpk = 0, apk = 0, aerr = 0;
    unsigned long long asamples = 0;
    static uint32_t sums[CHECKSUM_FRAMES][3];

    if (mb.f) {
        fseek(mb.f, 0, SEEK_END);
        mb.size = ftell(mb.f);
        fseek(mb.f, 0, SEEK_SET);
    }
    if (!mb.f || !fmt || !io || !pkt || !frame) {
        printf("ffbench: out of memory\n");
        return -1;
    }
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    /* default probesize (5 MB) would buffer most of the clip in RAM */
    fmt->probesize = 256 * 1024;
    fmt->max_analyze_duration = 500000;
    if ((ret = avformat_open_input(&fmt, "mem.mpg", NULL, NULL)) < 0 ||
        (ret = avformat_find_stream_info(fmt, NULL)) < 0) {
        printf("ffbench: open/probe failed (%d)\n", ret);
        return -1;
    }
    for (i = 0; i < fmt->nb_streams; i++) {
        enum AVMediaType t = fmt->streams[i]->codecpar->codec_type;

        if (pass == 1)
            printf("ffbench: stream %u type=%d codec_id=%d %dx%d\n", i, (int)t,
                   (int)fmt->streams[i]->codecpar->codec_id,
                   fmt->streams[i]->codecpar->width, fmt->streams[i]->codecpar->height);

        if (t == AVMEDIA_TYPE_VIDEO && vidx < 0)
            vidx = (int)i;
        else if (t == AVMEDIA_TYPE_AUDIO && aidx < 0)
            aidx = (int)i;
    }
    if (vidx < 0) {
        printf("ffbench: no video stream\n");
        return -1;
    }
    if (fmt->streams[vidx]->codecpar->codec_id == AV_CODEC_ID_NONE)
        fmt->streams[vidx]->codecpar->codec_id = AV_CODEC_ID_MPEG1VIDEO;
    {
        const AVCodec *c = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);

        vc = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(vc, fmt->streams[vidx]->codecpar);
        vc->thread_count = 1;
        int oret = c ? avcodec_open2(vc, c, NULL) : -1;

        if (oret < 0) {
            char eb[80];

            av_strerror(oret, eb, sizeof(eb));
            printf("ffbench: video decoder open failed: codec_id=%d decoder=%s ret=%d (%s) %dx%d\n",
                   (int)fmt->streams[vidx]->codecpar->codec_id, c ? c->name : "NULL", oret, eb,
                   vc->width, vc->height);
            return -1;
        }
    }
    if (aidx >= 0) {
        const AVCodec *c = avcodec_find_decoder(fmt->streams[aidx]->codecpar->codec_id);

        ac = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(ac, fmt->streams[aidx]->codecpar);
        ac->thread_count = 1;
        int oret = c ? avcodec_open2(ac, c, NULL) : -1;

        if (oret < 0) {
            printf("ffbench: audio decoder open failed: ret=%d decoder=%s\n", oret, c ? c->name : "NULL");
            return -1;
        }
    }

    printf("ffbench: pass %d decoding\n", pass);
    fflush(stdout);
    /* The PS demuxer hands out PES payloads, not whole frames: run the stream
     * through the codec parsers (as libavformat's own need_parsing would). */
    vpar = av_parser_init(vc->codec_id);
    apar = ac ? av_parser_init(ac->codec_id) : NULL;
    if (!vpar || (ac && !apar)) {
        printf("ffbench: parser init failed\n");
        return -1;
    }
    opkt = av_packet_alloc();
    for (;;) {
        AVCodecContext *dc;
        AVCodecParserContext *pc;
        const uint8_t *pd;
        int ps;

        ret = av_read_frame(fmt, pkt);
        if (ret < 0)
            break;
        dc = pkt->stream_index == vidx ? vc : (pkt->stream_index == aidx ? ac : NULL);
        if (!dc) {
            av_packet_unref(pkt);
            continue;
        }
        pc = dc == vc ? vpar : apar;
        pd = pkt->data;
        ps = pkt->size;
        while (ps > 0) {
            uint8_t *od = NULL;
            int os = 0, used;
            uint64_t t0 = timer_us_gettime64(), dt;

            used = av_parser_parse2(pc, dc, &od, &os, pd, ps, pkt->pts, pkt->dts, pkt->pos);
            pd += used;
            ps -= used;
            if (os > 0) {
                opkt->data = od;
                opkt->size = os;
                if (avcodec_send_packet(dc, opkt) < 0) {
                    if (dc == ac) aerr++;
                } else {
                    while (avcodec_receive_frame(dc, frame) >= 0) {
                        if (dc == vc) {
                            if (vframes < CHECKSUM_FRAMES) {
                                /* checksum time is excluded from the timing */
                                uint64_t c0 = timer_us_gettime64();

                                sums[vframes][0] = adler_plane(frame->data[0], frame->linesize[0], vc->width, vc->height);
                                sums[vframes][1] = adler_plane(frame->data[1], frame->linesize[1], vc->width / 2, vc->height / 2);
                                sums[vframes][2] = adler_plane(frame->data[2], frame->linesize[2], vc->width / 2, vc->height / 2);
                                t0 += timer_us_gettime64() - c0;
                            }
                            vframes++;
                            if (vframes % 100 == 0) {
                                printf("ffbench: pass %d: %lu video frames, %lu audio frames\n", pass, vframes, aframes);
                                fflush(stdout);
                            }
                        } else {
                            aframes++;
                            asamples += (unsigned long long)frame->nb_samples;
                        }
                        av_frame_unref(frame);
                    }
                }
            }
            dt = timer_us_gettime64() - t0;
            if (dc == vc) {
                vdec_us += dt;
                if (os > 0) vpk++;
                if (dt > vdec_max) vdec_max = dt;
            } else {
                adec_us += dt;
                if (os > 0) apk++;
                if (dt > adec_max) adec_max = dt;
            }
        }
        av_packet_unref(pkt);
    }
    /* flush the video decoder (delayed B/P frames) */
    {
        uint64_t t0 = timer_us_gettime64();

        avcodec_send_packet(vc, NULL);
        while (avcodec_receive_frame(vc, frame) >= 0) {
            vframes++;
            av_frame_unref(frame);
        }
        vdec_us += timer_us_gettime64() - t0;
    }

    printf("ffbench: pass %d: %s %dx%d, %lu video frames from %lu packets\n", pass,
           vc->codec->name, vc->width, vc->height, vframes, vpk);
    printf("ffbench:   video decode %.3f ms/frame  (slowest packet %.2f ms)  total %.1f ms\n",
           vframes ? vdec_us / 1000.0 / vframes : 0.0, vdec_max / 1000.0, vdec_us / 1000.0);
    if (ac) {
        double secs = ac->sample_rate ? (double)asamples / ac->sample_rate : 0.0;

        printf("ffbench:   audio %s %d Hz: %lu frames (%lu packets, %lu errors), %.2f s\n",
               ac->codec->name, ac->sample_rate, aframes, apk, aerr, secs);
        printf("ffbench:   audio decode %.3f ms/frame = %.1f ms per second of audio\n",
               aframes ? adec_us / 1000.0 / aframes : 0.0, secs > 0 ? adec_us / 1000.0 / secs : 0.0);
    }
    if (pass == 1) {
        unsigned k;

        for (k = 0; k < CHECKSUM_FRAMES && k < vframes; k++)
            printf("ffbench: FRAME_CHECKSUM idx=%u y=%08lx u=%08lx v=%08lx\n", k,
                   (unsigned long)sums[k][0], (unsigned long)sums[k][1], (unsigned long)sums[k][2]);
    }
    avcodec_free_context(&vc);
    avcodec_free_context(&ac);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    opkt->data = NULL; opkt->size = 0;
    av_packet_free(&opkt);
    av_parser_close(vpar);
    if (apar) av_parser_close(apar);
    avformat_close_input(&fmt);
    av_freep(&io->buffer);
    avio_context_free(&io);
    fclose(mb.f);
    return 0;
}

int main(void) {
    char fixture[200], path[256];
    int pass;

    printf("ffbench: unmodified FFmpeg (%s), %d passes\n", av_version_info(), PASSES);
    fixture_name(fixture, sizeof(fixture));
    snprintf(path, sizeof(path), "/pc/fixtures/%s", fixture);
    printf("ffbench: streaming %s\n", path);
    {
        struct mallinfo mi = mallinfo();
        printf("ffbench: heap at start: arena=%d inuse=%d\n", mi.arena, mi.uordblks);
    }
    av_log_set_level(AV_LOG_ERROR);
    for (pass = 1; pass <= PASSES; pass++)
        if (run_pass(path, pass) != 0)
            return 1;
    printf("ffbench: done\n");
    return 0;
}
