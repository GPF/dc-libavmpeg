/* Layout-neutral whole-decode event probe, injected by post-link ELF patching.
 *
 * Any instrumentation compiled into the player or decoder moves code, and code
 * placement alone changes decode time by up to 12% (docs/STATUS.md). `--wrap`
 * was tried and is not layout-neutral under LTO. This probe is therefore built
 * as a free-standing blob (no libc link) and injected AFTER linking:
 *
 *   1. link the production ELF unchanged (optionally with the pad object);
 *   2. link this file with `sh-elf-ld -T probe_patch.ld` at 0x8c10fa00 (the gap
 *      between the end of .text and .init), with --defsym for the addresses of
 *      the real avcodec_decode_video and printf taken from that ELF;
 *   3. add the blob as a new section and patch the single literal-pool word in
 *      _decode_video that holds &avcodec_decode_video to point at
 *      _probe_wrap_decode (tools/probe_patch.sh).
 *
 * No existing function or symbol moves; the only change to the original image
 * is one 4-byte literal. PRFC1 is programmed directly (same register sequence
 * as KOS perfctr.c) with one event per run; PRFC0 stays KOS's timer.
 *
 * Output: every 120 calls, four lines "@E=<event> t<N> sum=<hi><lo> n=<count>",
 * N = 0 (no picture returned), 1 = I, 2 = P, 3 = B. The last block is the total.
 */
#include <stdint.h>
#include <stdio.h>

#include "libavcodec/avcodec.h"

extern int probe_real_decode(AVCodecContext *avctx, AVFrame *picture, int *got_picture_ptr,
                             const uint8_t *buf, int buf_size);
extern char CAL_EVENT[]; /* absolute symbol from --defsym */

#define PMCR1_CTRL (*(volatile uint16_t *)0xff000088u)
#define PMCTR1_HIGH (*(volatile uint32_t *)0xff10000cu)
#define PMCTR1_LOW (*(volatile uint32_t *)0xff100010u)

#define PMCR_RUN 0xc000u
#define PMCR_CLR 0x2000u

static uint32_t sum_hi[4], sum_lo[4], cnt[4], calls, started;

static uint64_t pmc_read(void) {
    uint32_t hi, lo, hi2;

    do {
        hi = PMCTR1_HIGH;
        lo = PMCTR1_LOW;
        hi2 = PMCTR1_HIGH;
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

static void pmc_start(unsigned event) {
    PMCR1_CTRL &= (uint16_t)~PMCR_RUN;
    PMCR1_CTRL |= (uint16_t)PMCR_CLR;
    PMCR1_CTRL = (uint16_t)(PMCR_RUN | event);
}

int probe_wrap_decode(AVCodecContext *avctx, AVFrame *picture, int *got_picture_ptr,
                      const uint8_t *buf, int buf_size) {
    uint64_t e0, e1, d;
    unsigned t;
    int ret;

    if (!started) {
        started = 1;
        pmc_start((unsigned)(uintptr_t)CAL_EVENT);
    }
    e0 = pmc_read();
    ret = probe_real_decode(avctx, picture, got_picture_ptr, buf, buf_size);
    e1 = pmc_read();
    d = e1 - e0;
    t = (got_picture_ptr && *got_picture_ptr && picture->pict_type >= FF_I_TYPE &&
         picture->pict_type <= FF_B_TYPE) ? (unsigned)picture->pict_type : 0u;
    {
        uint64_t s = (((uint64_t)sum_hi[t] << 32) | sum_lo[t]) + d;

        sum_hi[t] = (uint32_t)(s >> 32);
        sum_lo[t] = (uint32_t)s;
    }
    cnt[t]++;
    if (++calls % 120u == 0) {
        unsigned i;

        for (i = 0; i < 4; i++)
            printf("@E=%02x t%u sum=%08x%08x n=%u\n", (unsigned)(uintptr_t)CAL_EVENT, i,
                   (unsigned)sum_hi[i], (unsigned)sum_lo[i], (unsigned)cnt[i]);
    }
    return ret;
}
