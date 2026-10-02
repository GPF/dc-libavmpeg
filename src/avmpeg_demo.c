/* Exercises the avmpeg library API only (no PVR, no sound hardware): decodes a whole
 * clip, prints per-stage cost, checksums the first frames in the same format as
 * av_main.c's FRAME_CHECKSUM trace so the two outputs can be diffed, then seeks and
 * decodes again. The clip is the `fixture=` line of /pc/config.ini under
 * /pc/fixtures/ (run from the repo root with kos-tool -m .). */
#include <kos.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avmpeg.h"

#define DEFAULT_FIXTURE "lair_320_23976_30s_spec.mpg"
#define CHECKSUM_FRAMES 30
#define SEEK_TO_S 5.0
#define SEEK_FRAMES 48
#define MAX_FRAMES 1024

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

static void print_checksum(const char *tag, const avmpeg_frame_t *f) {
    printf("%s: FRAME_CHECKSUM idx=%lu y=%08lx u=%08lx v=%08lx\n", tag, f->index,
           (unsigned long)adler_plane(f->plane[0], f->stride[0], f->width, f->height),
           (unsigned long)adler_plane(f->plane[1], f->stride[1], f->width / 2, f->height / 2),
           (unsigned long)adler_plane(f->plane[2], f->stride[2], f->width / 2, f->height / 2));
}

/* Drain all audio the library can hand out right now. */
static unsigned long drain_audio(avmpeg_t *m, int16_t *buf, int max_frames) {
    unsigned long total = 0;
    int n;

    while ((n = avmpeg_audio_read(m, buf, max_frames)) > 0)
        total += (unsigned long)n;
    return total;
}

static void print_stats(avmpeg_t *m, const char *tag, unsigned long vframes, unsigned long asamples) {
    const avmpeg_stats_t *s = avmpeg_stats(m);
    const avmpeg_info_t *info = avmpeg_info(m);

    printf("%s: %lu video frames, %lu audio samples (%.2f s)\n", tag, vframes, asamples,
           (double)asamples / info->sample_rate);
    printf("%s: video decode %.3f ms/frame, audio decode %.3f ms/chunk, demux %.1f ms total\n",
           tag, vframes ? s->video_decode_us / 1000.0 / vframes : 0.0,
           s->audio_chunks ? s->audio_decode_us / 1000.0 / s->audio_chunks : 0.0,
           s->demux_us / 1000.0);
    printf("%s: errors video=%lu audio=%lu, max queued packets video=%u audio=%u\n", tag,
           s->video_errors, s->audio_errors, s->video_queue_max, s->audio_queue_max);
}

int main(void) {
    static int16_t pcm[2304 * 2];
    char fixture[200], path[256];
    avmpeg_frame_t f;
    avmpeg_t *m;
    const avmpeg_info_t *info;
    unsigned long vframes = 0, asamples = 0;
    int r;

    fixture_name(fixture, sizeof(fixture));
    snprintf(path, sizeof(path), "/pc/fixtures/%s", fixture);
    printf("avmpeg_demo: %s\n", path);
    m = avmpeg_open(path, NULL);
    if (!m) {
        printf("avmpeg_demo: open failed\n");
        return 1;
    }
    info = avmpeg_info(m);
    printf("avmpeg_demo: video %dx%d %d/%d fps (frame %lld us), audio %d Hz %d ch, "
           "video leads audio by %lld us\n", info->width, info->height, info->fps_num,
           info->fps_den, (long long)info->frame_us, info->sample_rate, info->channels,
           (long long)info->av_offset_us);

    /* full pass, video and audio consumed alternately like a player would */
    for (;;) {
        r = avmpeg_video_next(m, &f);
        if (r == AVMPEG_AGAIN) {
            asamples += drain_audio(m, pcm, 1152);
            continue;
        }
        if (r != AVMPEG_OK)
            break;
        if (f.index < CHECKSUM_FRAMES)
            print_checksum("avmpeg_demo", &f);
        vframes++;
        /* roughly one video frame's worth of audio per frame */
        {
            int want = info->sample_rate * info->fps_den / info->fps_num, n;

            while (want > 0 && (n = avmpeg_audio_read(m, pcm, want > 1152 ? 1152 : want)) > 0) {
                asamples += (unsigned long)n;
                want -= n;
            }
        }
    }
    asamples += drain_audio(m, pcm, 1152);
    print_stats(m, "avmpeg_demo pass", vframes, asamples);

    avmpeg_close(m);

    /* Seek check. Pass 2 checksums every frame of a fresh decode from the start; pass 3
     * seeks, decodes SEEK_FRAMES frames, finds which frame of pass 2 the first one is
     * and compares the following ones. (Checksumming is kept out of the timing pass
     * above because it pollutes the caches.) */
    {
        static uint32_t sums[MAX_FRAMES][3];
        unsigned long total = 0, k, land = (unsigned long)-1, bad = 0, first_bad = 0;
        avmpeg_frame_t g;

        m = avmpeg_open(path, NULL);
        if (!m) {
            printf("avmpeg_demo: reopen failed\n");
            return 1;
        }
        while (total < MAX_FRAMES) {
            r = avmpeg_video_next(m, &f);
            if (r == AVMPEG_AGAIN) {
                drain_audio(m, pcm, 1152);
                continue;
            }
            if (r != AVMPEG_OK)
                break;
            sums[total][0] = adler_plane(f.plane[0], f.stride[0], f.width, f.height);
            sums[total][1] = adler_plane(f.plane[1], f.stride[1], f.width / 2, f.height / 2);
            sums[total][2] = adler_plane(f.plane[2], f.stride[2], f.width / 2, f.height / 2);
            total++;
        }
        avmpeg_close(m);

        m = avmpeg_open(path, NULL);
        if (!m || avmpeg_seek(m, SEEK_TO_S) != 0) {
            printf("avmpeg_demo: seek to %.1f s failed (input cannot seek)\n", SEEK_TO_S);
            avmpeg_close(m);
            return 1;
        }
        for (k = 0; k < SEEK_FRAMES;) {
            uint32_t c[3];

            r = avmpeg_video_next(m, &g);
            if (r == AVMPEG_AGAIN) {
                drain_audio(m, pcm, 1152);
                continue;
            }
            if (r != AVMPEG_OK)
                break;
            c[0] = adler_plane(g.plane[0], g.stride[0], g.width, g.height);
            c[1] = adler_plane(g.plane[1], g.stride[1], g.width / 2, g.height / 2);
            c[2] = adler_plane(g.plane[2], g.stride[2], g.width / 2, g.height / 2);
            if (k == 0) {
                unsigned long j;

                for (j = 0; j < total; j++)
                    if (!memcmp(sums[j], c, sizeof(c))) {
                        land = j;
                        break;
                    }
                printf("avmpeg_demo seek: to %.1f s, first frame type %d %s", SEEK_TO_S,
                       g.pict_type, land == (unsigned long)-1 ? "matches NO frame of the full pass\n" : "");
                if (land != (unsigned long)-1)
                    printf("= frame %lu of the full pass (%.3f s)\n", land,
                           (double)land * avmpeg_info(m)->frame_us / 1e6);
            }
            if (land != (unsigned long)-1 && land + k < total &&
                memcmp(sums[land + k], c, sizeof(c))) {
                if (!bad)
                    first_bad = k;
                bad++;
            }
            k++;
        }
        if (land != (unsigned long)-1)
            printf("avmpeg_demo seek: %lu of %lu frames after the seek point differ from the "
                   "full pass%s\n", bad, k, bad ? " (first at +" : "");
        if (bad)
            printf("avmpeg_demo seek: ...%lu)\n", first_bad);
        print_stats(m, "avmpeg_demo seek", k, 0);
    }
    avmpeg_close(m);
    printf("avmpeg_demo: done\n");
    return 0;
}
