#include "pvr_video.h"

#include <stdint.h>
#include <string.h>

#include <dc/pvr.h>
#include <arch/timer.h>

/*
 * The YUV420 macroblock submission follows the documented layout in the KOS
 * YUV420 converter example: U 8x8, V 8x8, then four Y 8x8 tiles.
 */
#define PVR_BLACK_Y 16
#define PVR_NEUTRAL_C 128

static pvr_ptr_t texture;
static pvr_poly_hdr_t poly_header;
static pvr_vertex_t vertices[4];
static unsigned texture_width;
static unsigned texture_height;
static int video_width;
static int video_height;
static pvr_video_prof_t prof;

static unsigned next_power_of_two(unsigned value) {
    unsigned result = 8;
    while (result < value)
        result <<= 1;
    return result;
}

static uint64_t load_or_fill_8(const uint8_t *src, int stride,
                               int x, int y, int width, int height,
                               uint8_t fill) {
    uint8_t bytes[8];
    memset(bytes, fill, sizeof(bytes));
    if (src && y >= 0 && y < height && x < width) {
        int count = width - x;
        if (count > 8)
            count = 8;
        if (count > 0)
            memcpy(bytes, src + y * stride + x, (size_t)count);
    }
    {
        uint64_t value;
        memcpy(&value, bytes, sizeof(value));
        return value;
    }
}

/* Macroblock that lies entirely outside the picture: the generic path below
 * would produce exactly this (U/V neutral, Y black), so prefill it once. */
static uint64_t pad_mb[48] __attribute__((aligned(32)));

static void copy_rows_8(uint8_t *dst, const uint8_t *src, int stride) {
    const uint64_t *s;
    uint64_t *d = (uint64_t *)dst;
    int row;

    for (row = 0; row < 8; ++row) {
        s = (const uint64_t *)(src + row * stride);
        d[row] = *s;
    }
}

static void convert_yuv420_block(const AVFrame *frame, int width, int height,
                                 unsigned x_block, unsigned y_block) {
    /* Assemble one 384-byte macroblock in RAM, then burst it to the converter
     * with sq_fast_cpy() the same way pl_mpeg does. */
    static uint64_t mb[48] __attribute__((aligned(32)));
    uint8_t *u_block = (uint8_t *)mb;
    uint8_t *v_block = u_block + 64;
    uint8_t *y_data_block = u_block + 128;
    int row, tile;

    if ((int)x_block >= width || (int)y_block >= height) {
        prof.blk_pad++;
#ifndef PVR_PROF_SKIP_SQ
        sq_fast_cpy(SQ_MASK_DEST(PVR_TA_YUV_CONV), pad_mb, 384 / 32);
#endif
        return;
    }

    /* Fully inside the picture and 8-byte aligned: plain 64-bit row copies. */
    if ((int)x_block + 16 <= width && (int)y_block + 16 <= height &&
        !(((uintptr_t)frame->data[0] | (uintptr_t)frame->data[1] |
           (uintptr_t)frame->data[2] | (uintptr_t)frame->linesize[0] |
           (uintptr_t)frame->linesize[1] | (uintptr_t)frame->linesize[2]) & 7)) {
        prof.blk_fast++;
        const uint8_t *cu = frame->data[1] + (y_block / 2) * frame->linesize[1] +
                            x_block / 2;
        const uint8_t *cv = frame->data[2] + (y_block / 2) * frame->linesize[2] +
                            x_block / 2;
        const uint8_t *cy = frame->data[0] + y_block * frame->linesize[0] + x_block;

        copy_rows_8(u_block, cu, frame->linesize[1]);
        copy_rows_8(v_block, cv, frame->linesize[2]);
        for (tile = 0; tile < 4; ++tile)
            copy_rows_8(y_data_block + tile * 64,
                        cy + (tile >> 1) * 8 * frame->linesize[0] + (tile & 1) * 8,
                        frame->linesize[0]);
#ifndef PVR_PROF_SKIP_SQ
        sq_fast_cpy(SQ_MASK_DEST(PVR_TA_YUV_CONV), mb, 384 / 32);
#endif
        return;
    }

    prof.blk_generic++;
    for (row = 0; row < 8; ++row) {
        uint64_t u = load_or_fill_8(frame->data[1], frame->linesize[1],
                (int)x_block / 2, (int)y_block / 2 + row,
                width / 2, height / 2, PVR_NEUTRAL_C);
        uint64_t v = load_or_fill_8(frame->data[2], frame->linesize[2],
                (int)x_block / 2, (int)y_block / 2 + row,
                width / 2, height / 2, PVR_NEUTRAL_C);
        memcpy(u_block + row * 8, &u, 8);
        memcpy(v_block + row * 8, &v, 8);
    }

    for (tile = 0; tile < 4; ++tile) {
        int tile_x = (int)x_block + (tile & 1) * 8;
        int tile_y = (int)y_block + (tile >> 1) * 8;
        for (row = 0; row < 8; ++row) {
            uint64_t y = load_or_fill_8(frame->data[0], frame->linesize[0],
                    tile_x, tile_y + row, width, height, PVR_BLACK_Y);
            memcpy(y_data_block + tile * 64 + row * 8, &y, 8);
        }
    }

#ifndef PVR_PROF_SKIP_SQ  /* diagnostic: garbage picture, isolates assembly cost */
    sq_fast_cpy(SQ_MASK_DEST(PVR_TA_YUV_CONV), mb, 384 / 32);
#endif
}

int pvr_video_init(int width, int height) {
    pvr_poly_cxt_t context;

    if (width <= 0 || height <= 0 || (width & 1) || (height & 1))
        return -1;
    video_width = width;
    video_height = height;
    memset(pad_mb, PVR_NEUTRAL_C, 128);
    memset((uint8_t *)pad_mb + 128, PVR_BLACK_Y, 256);
    texture_width = next_power_of_two((unsigned)width);
    texture_height = next_power_of_two((unsigned)height);
    texture = pvr_mem_malloc(texture_width * texture_height * 2);
    if (!texture)
        return -1;

    PVR_SET(PVR_YUV_ADDR, ((uintptr_t)texture) & 0x00ffffff);
    PVR_SET(PVR_YUV_CFG, (((texture_height / 16) - 1) << 8) |
                         ((texture_width / 16) - 1));
    PVR_GET(PVR_YUV_CFG);

    pvr_poly_cxt_txr(&context, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_YUV422 | PVR_TXRFMT_NONTWIDDLED,
                     texture_width, texture_height, texture,
                     PVR_FILTER_BILINEAR);
    pvr_poly_compile(&poly_header, &context);

    vertices[0].x = 0.0f; vertices[0].y = 0.0f;
    vertices[1].x = 640.0f; vertices[1].y = 0.0f;
    vertices[2].x = 0.0f; vertices[2].y = 480.0f;
    vertices[3].x = 640.0f; vertices[3].y = 480.0f;
    vertices[0].u = vertices[2].u = 0.0f;
    vertices[1].u = vertices[3].u = (float)width / texture_width;
    vertices[0].v = vertices[1].v = 0.0f;
    vertices[2].v = vertices[3].v = (float)height / texture_height;
    for (int i = 0; i < 4; ++i) {
        vertices[i].z = 1.0f;
        vertices[i].argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
        vertices[i].oargb = 0;
        vertices[i].flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    }
    return 0;
}

int pvr_video_present(const AVFrame *frame, int width, int height,
                      enum PixelFormat format) {
    unsigned x, y;
    uint64_t t0, t1, t2, t3;

    if (!frame || format != PIX_FMT_YUV420P || width != video_width ||
        height != video_height)
        return -1;

    t0 = timer_us_gettime64();
    pvr_wait_ready();
    t1 = timer_us_gettime64();
    sq_lock((void *)PVR_TA_YUV_CONV);
    for (y = 0; y < texture_height; y += 16) {
        for (x = 0; x < texture_width; x += 16)
            convert_yuv420_block(frame, width, height, x, y);
    }
    sq_unlock();
    t2 = timer_us_gettime64();

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&poly_header, sizeof(poly_header));
    for (int i = 0; i < 4; ++i)
        pvr_prim(&vertices[i], sizeof(vertices[i]));
    pvr_list_finish();
    pvr_scene_finish();
    t3 = timer_us_gettime64();
    prof.frames++;
    prof.wait_us += t1 - t0;
    prof.blocks_us += t2 - t1;
    prof.submit_us += t3 - t2;
    if (t1 - t0 > prof.wait_max) prof.wait_max = t1 - t0;
    if (t2 - t1 > prof.blocks_max) prof.blocks_max = t2 - t1;
    if (t3 - t2 > prof.submit_max) prof.submit_max = t3 - t2;
    return 0;
}

void pvr_video_get_prof(pvr_video_prof_t *out) {
    *out = prof;
}

void pvr_video_shutdown(void) {
    if (texture) {
        pvr_mem_free(texture);
        texture = NULL;
    }
}
