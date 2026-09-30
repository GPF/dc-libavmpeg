#ifndef MPEG_PLAYER_H
#define MPEG_PLAYER_H

#include <stdint.h>

typedef struct mpeg_player mpeg_player_t;

mpeg_player_t *mpeg_player_open(const char *path);
int mpeg_player_decode_next(mpeg_player_t *player);
const void *mpeg_player_frame(mpeg_player_t *player);
int mpeg_player_width(const mpeg_player_t *player);
int mpeg_player_height(const mpeg_player_t *player);
int mpeg_player_pixel_format(const mpeg_player_t *player);
void mpeg_player_close(mpeg_player_t *player);

#endif
