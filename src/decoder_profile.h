#ifndef DC_LIBAVMPEG_DECODER_PROFILE_H
#define DC_LIBAVMPEG_DECODER_PROFILE_H

#include <stdint.h>

uint64_t mpeg_decode_profile_start(void);
void mpeg_decode_profile_end(unsigned stage, uint64_t start_ns);
uint64_t mpeg_decode_profile_calibrate(void);
extern int mpeg_decode_profile_sample;
extern int mpeg_decode_profile_pict; /* FF_I/P/B_TYPE of the slice being decoded */

#ifdef MPEG_IDCT_BLOCK_COUNTS
/* [I/P/B][add/put][empty/DC/1-3/4-7/8-15/>15 last-index class] */
extern unsigned long mpeg_idct_block_counts[3][2][6];
#endif

#endif
