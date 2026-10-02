/* MP2 decode-only benchmark: how much CPU does the audio decoder itself cost?
 *
 * No video decode, no AICA, no sound stream. The whole MPEG-PS is preloaded so
 * no storage I/O is timed. Each pass reopens the demuxer, then loops
 * av_read_frame() and decodes every audio packet with avcodec_decode_audio2().
 * Decode and demux time are accumulated separately, so the figure comparable to
 * pl_mpeg's plm_decode_audio() (which parses packets inside the call) is
 * "decode + demux".
 *
 * Clip: the `fixture=` line of /pc/config.ini (default
 * lair_320_23976_30s_spec.mpg), read from /pc/fixtures/. Run from the repo root
 * with `kos-tool -t <ip> -x dc-libavmpeg-audiobench.elf -m .` */
#include <kos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/mem.h"
#include "ffmpeg_av.h"

#ifndef AUDIO_BENCH_PASSES
#define AUDIO_BENCH_PASSES 4
#endif
#define DEFAULT_FIXTURE "lair_320_23976_30s_spec.mpg"

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

static void fixture_name(char *out, size_t n) {
    char line[256];
    FILE *f = fopen("/pc/config.ini", "r");

    snprintf(out, n, "%s", DEFAULT_FIXTURE);
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "fixture=", 8)) {
            size_t len = strcspn(line + 8, "\r\n");

            if (len > 0 && len < n) {
                memcpy(out, line + 8, len);
                out[len] = 0;
            }
        }
    }
    fclose(f);
}

static int run_pass(uint8_t *data, size_t size, const char *path, int pass,
                    int16_t *samples) {
    ByteIOContext pb;
    AVProbeData pd;
    AVInputFormat *fmt;
    AVFormatContext *ic = NULL;
    AVCodecContext *ac;
    AVCodec *adec;
    AVPacket pkt;
    int aidx = -1;
    unsigned i;
    unsigned long frames = 0, errors = 0, packets = 0;
    uint64_t samples_per_ch = 0, dec_us = 0, dec_max = 0, dem_us = 0;

    init_put_byte(&pb, data, (int)size, 0, NULL, NULL, NULL, NULL);
    pd.filename = path;
    pd.buf = data;
    pd.buf_size = size > 4096 ? 4096 : (int)size;
    fmt = av_probe_input_format(&pd, 1);
    if (!fmt || av_open_input_stream(&ic, &pb, path, fmt, NULL) < 0 ||
        av_find_stream_info(ic) < 0) {
        printf("audiobench: demuxer open failed\n");
        return -1;
    }
    for (i = 0; i < ic->nb_streams; i++) {
        AVCodecContext *c = ic->streams[i]->codec;

        if (c->codec_type == CODEC_TYPE_AUDIO && aidx < 0 &&
            c->codec_id == CODEC_ID_MP2)
            aidx = (int)i;
    }
    if (aidx < 0) {
        printf("audiobench: no MP2 stream\n");
        return -1;
    }
    ac = ic->streams[aidx]->codec;
    adec = avcodec_find_decoder(ac->codec_id);
    if (!adec || avcodec_open(ac, adec) < 0) {
        printf("audiobench: MP2 decoder open failed\n");
        return -1;
    }

    for (;;) {
        uint64_t t0 = timer_us_gettime64();
        int ret = av_read_frame(ic, &pkt);

        dem_us += timer_us_gettime64() - t0;
        if (ret < 0)
            break;
        if (pkt.stream_index == aidx) {
            const uint8_t *p = pkt.data;
            int left = pkt.size;

            packets++;
            while (left > 0) {
                int out_bytes = AVCODEC_MAX_AUDIO_FRAME_SIZE;
                uint64_t a = timer_us_gettime64(), dt;
                int len = avcodec_decode_audio2(ac, samples, &out_bytes, p, left);

                dt = timer_us_gettime64() - a;
                if (len <= 0) {
                    if (len < 0)
                        errors++;
                    break;
                }
                p += len;
                left -= len;
                if (out_bytes > 0) {
                    dec_us += dt;
                    if (dt > dec_max)
                        dec_max = dt;
                    frames++;
                    samples_per_ch += (uint64_t)out_bytes / (2u * ac->channels);
                }
            }
        }
        av_free_packet(&pkt);
    }

    {
        double secs = (double)samples_per_ch / ac->sample_rate;

        printf("audiobench: pass %d: %d Hz %d ch, %lu frames (%lu packets, %lu errors), "
               "%.2f s of audio\n", pass, ac->sample_rate, ac->channels, frames, packets,
               errors, secs);
        printf("audiobench:   decode %.3f ms/frame (max %.3f)  = %.1f ms per second of audio"
               "  | demux %.1f ms/s  | decode+demux %.1f ms/s = %.1f%% of one CPU\n",
               frames ? dec_us / 1000.0 / frames : 0.0, dec_max / 1000.0,
               secs > 0 ? dec_us / 1000.0 / secs : 0.0,
               secs > 0 ? dem_us / 1000.0 / secs : 0.0,
               secs > 0 ? (dec_us + dem_us) / 1000.0 / secs : 0.0,
               secs > 0 ? (dec_us + dem_us) / 10000.0 / secs : 0.0);
    }
    avcodec_close(ac);
    av_close_input_stream(ic);
    return 0;
}

int main(void) {
    char fixture[200], path[256];
    uint8_t *data;
    size_t size = 0;
    int16_t *samples;
    int pass;

    printf("dc-libavmpeg audiobench: FFmpeg MP2 decode only (%d passes)\n",
           AUDIO_BENCH_PASSES);
    fixture_name(fixture, sizeof(fixture));
    snprintf(path, sizeof(path), "/pc/fixtures/%s", fixture);
    data = load_file(path, &size);
    if (!data) {
        printf("audiobench: cannot load %s\n", path);
        return 1;
    }
    printf("audiobench: loaded %s (%lu bytes)\n", path, (unsigned long)size);
    avcodec_init();
    ffmpeg_register_mpegps_av();
    samples = av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);
    if (!samples) {
        printf("audiobench: out of memory\n");
        return 1;
    }
    for (pass = 1; pass <= AUDIO_BENCH_PASSES; pass++)
        if (run_pass(data, size, path, pass, samples) != 0)
            return 1;
    printf("audiobench: done\n");
    return 0;
}
