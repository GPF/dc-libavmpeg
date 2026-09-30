#include <kos.h>
#include <stdint.h>

#include "libavcodec/avcodec.h"
#include "mpeg_player.h"
#include "pvr_video.h"

#ifndef MPEG_CD_FIXTURE_PATH
#define MPEG_CD_FIXTURE_PATH "/cd/lair_320_23976_30s.m1v"
#endif

#ifndef MPEG_PC_FIXTURE_PATH
#define MPEG_PC_FIXTURE_PATH \
    "/pc/fixtures/lair_320_23976_30s.m1v"
#endif

/* Diagnostic: print Adler-32 of packed Y/U/V for the first N frames so they
 * can be compared with host `ffmpeg -f framecrc -pix_fmt yuv420p`. */
#ifndef MPEG_FRAME_CHECKSUM_COUNT
#define MPEG_FRAME_CHECKSUM_COUNT 0
#endif

static uint32_t sum_plane(const uint8_t *src, int stride, int width, int height) {
    uint32_t sum = 0;
    int x, y;

    for (y = 0; y < height; ++y, src += stride)
        for (x = 0; x < width; ++x)
            sum += src[x];
    return sum;
}

static uint32_t adler_plane(uint32_t adler, const uint8_t *src, int stride,
                            int width, int height) {
    uint32_t a = adler & 0xffff, b = adler >> 16;
    int x, y;

    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            a = (a + src[x]) % 65521;
            b = (b + a) % 65521;
        }
        src += stride;
    }
    return (b << 16) | a;
}

int main(int argc, char **argv) {
    mpeg_player_t *player;
#ifdef MPEG_BENCHMARK
    uint64_t decode_total_us = 0;
    uint64_t decode_min_us = UINT64_MAX;
    uint64_t decode_max_us = 0;
    uint64_t start_us;
#endif
    unsigned long frames = 0;
    unsigned long frame_types[4] = { 0, 0, 0, 0 };
    int pvr_ready = 0;
    int result;

    printf("dc-libavmpeg: MPEG-1 decode-only bring-up\n");
    if (argc > 1) {
        player = mpeg_player_open(argv[1]);
    } else {
        printf("mpeg: trying CD fixture %s\n", MPEG_CD_FIXTURE_PATH);
        player = mpeg_player_open(MPEG_CD_FIXTURE_PATH);
        if (!player) {
            printf("mpeg: CD fixture unavailable; falling back to %s\n",
                   MPEG_PC_FIXTURE_PATH);
            player = mpeg_player_open(MPEG_PC_FIXTURE_PATH);
        }
    }
    if (!player)
        return 1;
    pvr_init_defaults();

#ifdef MPEG_BENCHMARK
    start_us = timer_us_gettime64();
#endif
    while (1) {
        const AVFrame *frame;
#ifdef MPEG_BENCHMARK
        uint64_t t0 = timer_us_gettime64();
        uint64_t elapsed;
#endif
        result = mpeg_player_decode_next(player);
#ifdef MPEG_BENCHMARK
        elapsed = timer_us_gettime64() - t0;
        decode_total_us += elapsed;
#endif

        if (result < 0) {
            printf("mpeg: decode error after %lu frames\n", frames);
            mpeg_player_close(player);
            if (pvr_ready)
                pvr_video_shutdown();
            return 1;
        }
        if (result == 0)
            break;

        frame = (const AVFrame *)mpeg_player_frame(player);
#ifdef MPEG_BENCHMARK
        if (elapsed < decode_min_us)
            decode_min_us = elapsed;
        if (elapsed > decode_max_us)
            decode_max_us = elapsed;
#endif
        frames++;
        if (frames <= MPEG_FRAME_CHECKSUM_COUNT) {
            int fw = mpeg_player_width(player), fh = mpeg_player_height(player);
            printf("mpeg: frame %lu type %d y=0x%08lx u=0x%08lx v=0x%08lx\n",
                   frames - 1, frame->pict_type,
                   (unsigned long)adler_plane(1, frame->data[0], frame->linesize[0], fw, fh),
                   (unsigned long)adler_plane(1, frame->data[1], frame->linesize[1], fw / 2, fh / 2),
                   (unsigned long)adler_plane(1, frame->data[2], frame->linesize[2], fw / 2, fh / 2));
            printf("mpeg:   sum y=%lu u=%lu v=%lu  row120 y[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x"
                   "  u60[0..3]=%02x %02x %02x %02x v60[0..3]=%02x %02x %02x %02x\n",
                   (unsigned long)sum_plane(frame->data[0], frame->linesize[0], fw, fh),
                   (unsigned long)sum_plane(frame->data[1], frame->linesize[1], fw / 2, fh / 2),
                   (unsigned long)sum_plane(frame->data[2], frame->linesize[2], fw / 2, fh / 2),
                   frame->data[0][120 * frame->linesize[0] + 0], frame->data[0][120 * frame->linesize[0] + 1],
                   frame->data[0][120 * frame->linesize[0] + 2], frame->data[0][120 * frame->linesize[0] + 3],
                   frame->data[0][120 * frame->linesize[0] + 4], frame->data[0][120 * frame->linesize[0] + 5],
                   frame->data[0][120 * frame->linesize[0] + 6], frame->data[0][120 * frame->linesize[0] + 7],
                   frame->data[1][60 * frame->linesize[1] + 0], frame->data[1][60 * frame->linesize[1] + 1],
                   frame->data[1][60 * frame->linesize[1] + 2], frame->data[1][60 * frame->linesize[1] + 3],
                   frame->data[2][60 * frame->linesize[2] + 0], frame->data[2][60 * frame->linesize[2] + 1],
                   frame->data[2][60 * frame->linesize[2] + 2], frame->data[2][60 * frame->linesize[2] + 3]);
        }
        if (MPEG_FRAME_CHECKSUM_COUNT > 0 && frames > MPEG_FRAME_CHECKSUM_COUNT &&
            ((frames - 1) % 24) == 0) {
            int fw = mpeg_player_width(player), fh = mpeg_player_height(player);
            printf("mpeg: frame %lu type %d sum y=%lu u=%lu v=%lu\n", frames - 1,
                   frame->pict_type,
                   (unsigned long)sum_plane(frame->data[0], frame->linesize[0], fw, fh),
                   (unsigned long)sum_plane(frame->data[1], frame->linesize[1], fw / 2, fh / 2),
                   (unsigned long)sum_plane(frame->data[2], frame->linesize[2], fw / 2, fh / 2));
        }
        if (frames == 1) {
            printf("mpeg: first frame %dx%d, pixel format %d, type %d\n",
                   mpeg_player_width(player), mpeg_player_height(player),
                   mpeg_player_pixel_format(player), frame->pict_type);
            if (pvr_video_init(mpeg_player_width(player),
                               mpeg_player_height(player)) < 0) {
                printf("pvr: initialization failed\n");
                mpeg_player_close(player);
                pvr_video_shutdown();
                return 1;
            }
            pvr_ready = 1;
        }
        if (frame->pict_type >= 0 && frame->pict_type < 4)
            frame_types[frame->pict_type]++;
        if (pvr_video_present((const AVFrame *)mpeg_player_frame(player),
                              mpeg_player_width(player),
                              mpeg_player_height(player),
                              mpeg_player_pixel_format(player)) < 0) {
            printf("pvr: frame presentation failed at %lu\n", frames);
            mpeg_player_close(player);
            pvr_video_shutdown();
            return 1;
        }
    }

    printf("mpeg: decoded %lu frames\n", frames);
    printf("mpeg: picture types I=%lu P=%lu B=%lu\n",
           frame_types[FF_I_TYPE], frame_types[FF_P_TYPE], frame_types[FF_B_TYPE]);
#ifdef MPEG_BENCHMARK
    if (frames) {
        printf("mpeg: decode time total=%llu us avg=%.3f ms/frame min=%llu us max=%llu us\n",
               (unsigned long long)decode_total_us,
               (double)decode_total_us / (double)frames / 1000.0,
               (unsigned long long)decode_min_us,
               (unsigned long long)decode_max_us);
    }
    printf("mpeg: elapsed=%llu us (input read excluded)\n",
           (unsigned long long)(timer_us_gettime64() - start_us));
#endif
    mpeg_player_close(player);
    if (pvr_ready)
        pvr_video_shutdown();
    return 0;
}
