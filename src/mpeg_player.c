#include "mpeg_player.h"
#include "ffmpeg_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"

#ifndef MPEG_IDCT_ALGO
#define MPEG_IDCT_ALGO FF_IDCT_SIMPLE
#endif

#define MPEG_INPUT_CHUNK_SIZE 4096

struct mpeg_player {
    uint8_t *input;
    uint8_t decode_chunk[MPEG_INPUT_CHUNK_SIZE + FF_INPUT_BUFFER_PADDING_SIZE];
    size_t input_size;
    size_t input_feed_size;
    size_t input_offset;
    AVCodecContext *codec;
    AVFrame *frame;
    int opened;
    int drained;
};

mpeg_player_t *mpeg_player_open(const char *path) {
    FILE *file;
    long file_size;
    mpeg_player_t *player;
    AVCodec *decoder;

    file = fopen(path, "rb");
    if (!file) {
        printf("mpeg: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        printf("mpeg: cannot size %s\n", path);
        fclose(file);
        return NULL;
    }

    player = calloc(1, sizeof(*player));
    if (!player) {
        fclose(file);
        return NULL;
    }

    player->input_size = (size_t)file_size;
    player->input = av_malloc((unsigned int)player->input_size + 4 +
                              FF_INPUT_BUFFER_PADDING_SIZE);
    if (!player->input ||
        fread(player->input, 1, player->input_size, file) != player->input_size) {
        printf("mpeg: cannot read %s\n", path);
        fclose(file);
        mpeg_player_close(player);
        return NULL;
    }
    fclose(file);
    player->input_feed_size = player->input_size;
    if (player->input_size < 4 ||
        player->input[player->input_size - 4] != 0x00 ||
        player->input[player->input_size - 3] != 0x00 ||
        player->input[player->input_size - 2] != 0x01 ||
        player->input[player->input_size - 1] != 0xb7) {
        player->input[player->input_feed_size++] = 0x00;
        player->input[player->input_feed_size++] = 0x00;
        player->input[player->input_feed_size++] = 0x01;
        player->input[player->input_feed_size++] = 0xb7;
    }
    memset(player->input + player->input_feed_size, 0,
           FF_INPUT_BUFFER_PADDING_SIZE);

    ffmpeg_register_mpeg1video();
    decoder = avcodec_find_decoder(CODEC_ID_MPEG1VIDEO);
    if (!decoder) {
        printf("mpeg: MPEG-1 decoder not registered\n");
        mpeg_player_close(player);
        return NULL;
    }
    player->codec = avcodec_alloc_context();
    player->frame = avcodec_alloc_frame();
    if (player->codec && (decoder->capabilities & CODEC_CAP_TRUNCATED))
        player->codec->flags |= CODEC_FLAG_TRUNCATED;
    /* Integer IDCT is bit-exact with host ffmpeg. Build with
     * -DMPEG_IDCT_ALGO=FF_IDCT_AUTO to get the faster, non-bit-exact float
     * SH-4 IDCT. */
    if (player->codec)
        player->codec->idct_algo = MPEG_IDCT_ALGO;
    if (!player->codec || !player->frame || avcodec_open(player->codec, decoder) < 0) {
        printf("mpeg: decoder initialization failed\n");
        mpeg_player_close(player);
        return NULL;
    }
    player->opened = 1;
    printf("mpeg: opened %s (%lu bytes) with %s\n", path,
           (unsigned long)player->input_size, decoder->name);
    return player;
}

int mpeg_player_decode_next(mpeg_player_t *player) {
    int got_frame = 0;
    int consumed;

    if (!player || !player->opened)
        return -1;

    while (player->input_offset < player->input_feed_size) {
        size_t remaining = player->input_feed_size - player->input_offset;
        size_t chunk_size = remaining;

        if (chunk_size > MPEG_INPUT_CHUNK_SIZE)
            chunk_size = MPEG_INPUT_CHUNK_SIZE;
        memcpy(player->decode_chunk, player->input + player->input_offset,
               chunk_size);
        memset(player->decode_chunk + chunk_size, 0,
               FF_INPUT_BUFFER_PADDING_SIZE);
        consumed = avcodec_decode_video(player->codec, player->frame, &got_frame,
                    player->decode_chunk, (int)chunk_size);
        if (consumed < 0 || (size_t)consumed > chunk_size)
            return -1;
        player->input_offset += (size_t)consumed;
        if (got_frame)
            return 1;
        if (consumed == 0)
            return -1;
    }

    if (player->drained)
        return 0;
    consumed = avcodec_decode_video(player->codec, player->frame, &got_frame,
                                    NULL, 0);
    if (consumed < 0)
        return -1;
    if (!got_frame)
        player->drained = 1;
    return got_frame ? 1 : 0;
}

const void *mpeg_player_frame(mpeg_player_t *player) {
    return player ? player->frame : NULL;
}

int mpeg_player_width(const mpeg_player_t *player) {
    return player ? player->codec->width : 0;
}

int mpeg_player_height(const mpeg_player_t *player) {
    return player ? player->codec->height : 0;
}

int mpeg_player_pixel_format(const mpeg_player_t *player) {
    return player ? player->codec->pix_fmt : PIX_FMT_NONE;
}

void mpeg_player_close(mpeg_player_t *player) {
    if (!player)
        return;
    if (player->opened)
        avcodec_close(player->codec);
    if (player->codec)
        av_free(player->codec);
    if (player->frame)
        av_free(player->frame);
    if (player->input)
        av_free(player->input);
    free(player);
}
