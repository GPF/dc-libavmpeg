/* Layout-neutral whole-decode event probe (opt-in, link-time only).
 *
 * Purpose: measure SH-4 performance-counter events around avcodec_decode_video()
 * on the *production* code layout. Any instrumentation compiled into the decoder
 * or player moves code, and code placement alone changes decode time by up to
 * 12% (docs/STATUS.md, placement sweep), so this probe adds no instructions to
 * any existing function. It is linked with
 *
 *   -Wl,--wrap=avcodec_decode_video
 *   -Wl,--section-start=.instr=0x8c10fa00 -Wl,--section-start=.instrd=0x8c10fe00
 *   -Wl,--defsym=CAL_EVENT=0x25 -Wl,-u,__wrap_avcodec_decode_video
 *
 * so the player's call site is redirected by changing a literal-pool address
 * only, and this code and its data sit in the gap between the end of .text and
 * .init (no existing symbol moves). One event per run on PRFC1 (PRFC0 stays
 * KOS's timer). Uses no string literals and no libc formatting, so no .rodata
 * or .bss is added either.
 *
 * Output (at exit): lines "@E=<event> t<N>:<sum hex16> n<count hex8>" via putchar,
 * for N = 0 (no picture returned), 1 = I, 2 = P, 3 = B.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <dc/perfctr.h>

#include "libavcodec/avcodec.h"

#define SEC_C __attribute__((section(".instr")))
#define SEC_D __attribute__((section(".instrd")))

extern char CAL_EVENT[]; /* absolute symbol from --defsym */

int __real_avcodec_decode_video(AVCodecContext *avctx, AVFrame *picture, int *got_picture_ptr,
                                const uint8_t *buf, int buf_size);

static uint64_t cal_sum[4] SEC_D;
static uint32_t cal_count[4] SEC_D;
static uint32_t cal_started SEC_D;

static void SEC_C cal_hex(uint64_t v, int digits) {
    int i;

    for (i = (digits - 1) * 4; i >= 0; i -= 4) {
        unsigned d = (unsigned)(v >> i) & 15u;

        putchar(d < 10 ? (int)('0' + d) : (int)('a' + d - 10));
    }
}

static void SEC_C cal_report(void) {
    unsigned t;

    for (t = 0; t < 4; t++) {
        putchar('@'); putchar('E'); putchar('=');
        cal_hex((unsigned)(uintptr_t)CAL_EVENT, 2);
        putchar(' '); putchar('t'); putchar((int)('0' + t)); putchar(':');
        cal_hex(cal_sum[t], 16);
        putchar(' '); putchar('n'); cal_hex(cal_count[t], 8);
        putchar('\n');
    }
}

int SEC_C __wrap_avcodec_decode_video(AVCodecContext *avctx, AVFrame *picture, int *got_picture_ptr,
                                      const uint8_t *buf, int buf_size) {
    uint64_t e0, e1;
    unsigned t;
    int ret;

    if (!cal_started) {
        cal_started = 1;
        perf_cntr_clear(PRFC1);
        perf_cntr_start(PRFC1, (perf_cntr_event_t)(unsigned)(uintptr_t)CAL_EVENT, PMCR_COUNT_CPU_CYCLES);
        atexit(cal_report);
    }
    e0 = perf_cntr_count(PRFC1);
    ret = __real_avcodec_decode_video(avctx, picture, got_picture_ptr, buf, buf_size);
    e1 = perf_cntr_count(PRFC1);
    t = (got_picture_ptr && *got_picture_ptr && picture->pict_type >= FF_I_TYPE &&
         picture->pict_type <= FF_B_TYPE) ? (unsigned)picture->pict_type : 0u;
    cal_sum[t] += e1 - e0;
    cal_count[t]++;
    return ret;
}
