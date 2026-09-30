#ifndef PVR_VIDEO_H
#define PVR_VIDEO_H

#include "libavcodec/avcodec.h"

int pvr_video_init(int width, int height);
int pvr_video_present(const AVFrame *frame, int width, int height,
                      enum PixelFormat format);
void pvr_video_shutdown(void);

/* Slice 3 diagnostic: cumulative present-stage times in us. */
typedef struct {
    unsigned long frames, blk_fast, blk_pad, blk_generic;
    unsigned long long wait_us, blocks_us, submit_us;
    unsigned long long wait_max, blocks_max, submit_max;
} pvr_video_prof_t;
void pvr_video_get_prof(pvr_video_prof_t *out);

#endif
