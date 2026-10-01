/* On-device IDCT micro-benchmark: cycles per call for the C reference and the
 * two asm kernels on an MPEG-like block mix, warm caches, blocks restored from a
 * pristine copy before every call (same overhead for every kernel, subtracted). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kos.h>

typedef short DCTELEM;
void ff_simple_idct_put_body(uint8_t *, int, DCTELEM *);
void ff_simple_idct_add_body(uint8_t *, int, DCTELEM *);
void ff_simple_idct_put_asm(uint8_t *, int, DCTELEM *);
void ff_simple_idct_add_asm(uint8_t *, int, DCTELEM *);
void ff_simple_idct_put_mac(uint8_t *, int, DCTELEM *);
void ff_simple_idct_add_mac(uint8_t *, int, DCTELEM *);

uint8_t crop_mem[8192 + 256 + 2 * 1024 + 8192];
__asm__(".globl _ff_cropTbl\n.set _ff_cropTbl, _crop_mem+8192");

static const uint8_t zz[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const int perm[8] = {0, 4, 1, 5, 2, 6, 3, 7};

static uint32_t rs = 0x1234567u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

#define NB 32            /* 32 blocks = 4 KB, stays in the 16 KB operand cache */
static DCTELEM nat[NB][64] __attribute__((aligned(32)));
static DCTELEM prm[NB][64] __attribute__((aligned(32)));
static DCTELEM work[64] __attribute__((aligned(32)));
static uint8_t frame[16 * 320] __attribute__((aligned(32)));
static uint8_t frame0[16 * 320];

static void gen(void) {
    for (int b = 0; b < NB; b++) {
        memset(nat[b], 0, 128);
        uint32_t r = rnd() % 100;
        int n = r < 8 ? 0 : r < 21 ? 1 + rnd() % 15 : 16 + rnd() % 48;
        nat[b][0] = (int)(rnd() % 1200) - 300;
        for (int k = 1; k <= n && k < 64; k++) {
            int mag = 1 + (int)(rnd() % (k < 10 ? 60 : 12));
            nat[b][zz[k]] = (rnd() & 1) ? mag : -mag;
        }
        for (int rr = 0; rr < 8; rr++) for (int c = 0; c < 8; c++) prm[b][rr * 8 + perm[c]] = nat[b][rr * 8 + c];
    }
}

typedef void (*fn_t)(uint8_t *, int, DCTELEM *);
static int cold;   /* flush D- and I-cache before every call (closer to a decoder call) */
static double bench(fn_t f, int permuted, int reps, uint32_t *sum) {
    memcpy(frame, frame0, sizeof frame);
    int old = irq_disable();
    uint64_t t0 = timer_ns_gettime64();
    for (int i = 0; i < reps; i++)
        for (int b = 0; b < NB; b++) {
            if (cold) { dcache_purge_all(); icache_flush_range(0x8c010000, 16384); }
            memcpy(work, permuted ? prm[b] : nat[b], 128);
            if (f) f(frame + (b & 1) * 8 + (b >> 1 & 1) * 8 * 320, 320, work);
        }
    uint64_t t1 = timer_ns_gettime64();
    irq_restore(old);
    uint32_t s = 0;
    for (size_t i = 0; i < sizeof frame; i++) s = s * 31 + frame[i];
    if (sum) *sum = s;
    return (double)(t1 - t0) / 5.0 / ((double)reps * NB);   /* cycles per call @200 MHz */
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < (int)sizeof crop_mem; i++) crop_mem[i] = 0x55;
    uint8_t *t = crop_mem + 8192 + 1024;
    for (int i = 0; i < 256; i++) t[i] = i;
    for (int i = 0; i < 1024; i++) { t[-1 - i] = 0; t[256 + i] = 255; }
    gen();
    for (size_t i = 0; i < sizeof frame0; i++) frame0[i] = 100 + (i * 7 & 31);
    struct { const char *n; fn_t f; int p; } k[6] = {
        {"C ref put ", ff_simple_idct_put_body, 0}, {"C ref add ", ff_simple_idct_add_body, 0},
        {"muls asm put", ff_simple_idct_put_asm, 0}, {"muls asm add", ff_simple_idct_add_asm, 0},
        {"mac  asm put", ff_simple_idct_put_mac, 1}, {"mac  asm add", ff_simple_idct_add_mac, 1}};
    for (cold = 0; cold <= 1; cold++) {
        const int R = cold ? 200 : 2000;      /* cold mode: flushing dominates the time */
        uint32_t s[6];
        double best[6], base = 1e18;
        for (int rep = 0; rep < 5; rep++) { double c = bench(NULL, 0, R, NULL); if (c < base) base = c; }
        printf("bench[%s]: %d blocks x %d reps, overhead (memcpy%s+loop) %.1f cyc/call\n", cold ? "cold" : "warm", NB, R, cold ? "+flush" : "", base);
        for (int i = 0; i < 6; i++) {
            best[i] = 1e18;
            for (int rep = 0; rep < 5; rep++) { double c = bench(k[i].f, k[i].p, R, &s[i]); if (c < best[i]) best[i] = c; }
            printf("bench[%s]: %-13s %8.1f cyc/call  (net %7.1f)  frame-sum %08lx\n", cold ? "cold" : "warm", k[i].n, best[i], best[i] - base, (unsigned long)s[i]);
        }
        printf("bench[%s]: results %s\n", cold ? "cold" : "warm", (s[0] == s[2] && s[0] == s[4] && s[1] == s[3] && s[1] == s[5]) ? "MATCH across kernels" : "MISMATCH");
        printf("bench[%s]: net mac vs C: put %+.1f (%+.1f%%)  add %+.1f (%+.1f%%);  net muls vs C: put %+.1f (%+.1f%%)  add %+.1f (%+.1f%%)\n", cold ? "cold" : "warm",
               best[4] - best[0], (best[4] - best[0]) / (best[0] - base) * 100, best[5] - best[1], (best[5] - best[1]) / (best[1] - base) * 100,
               best[2] - best[0], (best[2] - best[0]) / (best[0] - base) * 100, best[3] - best[1], (best[3] - best[1]) / (best[1] - base) * 100);
    }
    printf("bench: done\n");
    return 0;
}
