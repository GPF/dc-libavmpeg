#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef short DCTELEM;
void ff_simple_idct_put_body(uint8_t *dest, int line_size, DCTELEM *block);
void ff_simple_idct_add_body(uint8_t *dest, int line_size, DCTELEM *block);
#ifdef TEST_MAC
/* MAC.W kernel: coefficients in FF_SSE2_IDCT_PERM order, block left unmodified */
#define IDCT_PUT ff_simple_idct_put_mac
#define IDCT_ADD ff_simple_idct_add_mac
#else
#define IDCT_PUT ff_simple_idct_put_asm
#define IDCT_ADD ff_simple_idct_add_asm
#endif
void IDCT_PUT(uint8_t *dest, int line_size, DCTELEM *block);
void IDCT_ADD(uint8_t *dest, int line_size, DCTELEM *block);

uint8_t crop_mem[8192 + 256 + 2 * 1024 + 8192];
/* ff_cropTbl sits mid-buffer so out-of-range crop indices read defined bytes in both implementations */
#ifdef ASM_UNDERSCORE
#define U "_"
#else
#define U ""
#endif
__asm__(".globl " U "ff_cropTbl\n.set " U "ff_cropTbl, " U "crop_mem+8192");
int abi_call(void (*fn)(uint8_t *, int, DCTELEM *), uint8_t *dest, int line, DCTELEM *block);

static uint32_t rs = 0x9e3779b9u;
static uint32_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return rs;
}
static int rng(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

static void init_crop(void) {
    for (int i = 0; i < (int)sizeof crop_mem; i++) crop_mem[i] = (uint8_t)(rnd() | 0x01);
    uint8_t *t = crop_mem + 8192 + 1024;
    for (int i = 0; i < 256; i++) t[i] = i;
    for (int i = 0; i < 1024; i++) { t[-1 - i] = 0; t[256 + i] = 255; }
}

static int extreme(void) {
    static const int v[] = {-32768, 32767, -2048, 2047, 2048, -2049, 0, 1, -1, 255, -255};
    return v[rnd() % (sizeof v / sizeof v[0])];
}
static int coef(int mode) {
    switch (mode) {
    case 0: return rng(-2048, 2047);
    case 1: return (int16_t)rnd();
    case 2: return extreme();
    default: return rng(-8, 8);
    }
}

static void gen_block(DCTELEM *b) {
    int style = rnd() % 12, mode = rnd() % 4;
    memset(b, 0, 128);
    switch (style) {
    case 0: for (int i = 0; i < 64; i++) b[i] = coef(mode); break;
    case 1: for (int i = 0; i < 64; i++) if (rnd() % 8 == 0) b[i] = coef(mode); break;
    case 2: for (int i = 0; i < 64; i++) if (rnd() % 24 == 0) b[i] = coef(mode); break;
    case 3: b[0] = (rnd() & 1) ? extreme() : coef(mode); break;
    case 4: for (int r = 0; r < 8; r++) {                 /* row-structured */
                int k = rnd() % 5;
                for (int c = 0; c < 8; c++) {
                    int on = k == 0 ? c == 0 : k == 1 ? c < 4 : k == 2 ? c == 1 : k == 3 ? c >= 4 : 1;
                    if (on) b[r * 8 + c] = coef(mode);
                }
            } break;
    case 5: for (int i = 0; i < 64; i++) b[i] = (rnd() & 1) ? 32767 : -32768; break;
    case 6: for (int i = 0; i < 64; i++) b[i] = (rnd() % 3) ? 0 : ((rnd() & 1) ? 32767 : -32768); break;
    case 7: break;                                         /* all zero */
    case 8: for (int c = 0; c < 8; c++) {                 /* column-structured */
                int k = rnd() % 4;
                for (int r = 0; r < 8; r++) {
                    int on = k == 0 ? r == 0 : k == 1 ? r < 4 : k == 2 ? r >= 4 : 1;
                    if (on) b[r * 8 + c] = coef(mode);
                }
            } break;
    case 9: for (int i = 0; i < 8; i++) b[i] = coef(mode); break;  /* row 0 only */
    case 10: for (int i = 0; i < 64; i += 8) b[i] = coef(mode); break; /* col 0 only */
    default: for (int i = 0; i < 64; i++) b[i] = coef(mode) >> (rnd() % 12); break;
    }
}

#ifdef TEST_MAC
#define TEST_MAC_NO 1
#else
#define TEST_MAC_NO 0
#endif
#define CAN 16
int main(int argc, char **argv) {
#ifndef DEFAULT_CASES
#define DEFAULT_CASES 1000000
#endif
    long n = argc > 1 ? atol(argv[1]) : DEFAULT_CASES;
    int abi = 0;
    long bad = 0, stat[12] = {0};
    enum { DSZ = 400 * 8 + 2 * CAN + 64 };
    static uint8_t dref[DSZ] __attribute__((aligned(4))), dasm[DSZ] __attribute__((aligned(4))), dorig[DSZ] __attribute__((aligned(4)));
    init_crop();
    for (int mode = 0; mode < 2; mode++) {
        const char *nm = mode ? "add" : "put";
        long mism = 0;
        for (long i = 0; i < n; i++) {
            if (i % 10000 == 0) { printf("%s %ld/%ld\n", nm, i, n); fflush(stdout); }
            DCTELEM bo[64] __attribute__((aligned(4))), br[64] __attribute__((aligned(4))), ba[64] __attribute__((aligned(4))), bp[64];
            gen_block(bo);
            int line = rng(8, 400);
            int off = CAN + (int)(rnd() % 3);   /* misaligned dest too */
            /* only the bytes this case can touch: rows 0..7 plus a canary margin */
            int span = (off + 7 * line + 8 + CAN + 3) & ~3;
            for (int k = 0; k < span; k += 4) *(uint32_t *)(dorig + k) = rnd();
            memcpy(br, bo, 128);
#ifdef TEST_MAC
            { static const int p[8] = {0, 4, 1, 5, 2, 6, 3, 7};
              for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) ba[r * 8 + p[c]] = bo[r * 8 + c]; }
            memcpy(bp, ba, 128);
#else
            memcpy(ba, bo, 128);
#endif
            memcpy(dref, dorig, span); memcpy(dasm, dorig, span);
            if (mode) { ff_simple_idct_add_body(dref + off, line, br); abi |= abi_call(IDCT_ADD, dasm + off, line, ba); }
            else      { ff_simple_idct_put_body(dref + off, line, br); abi |= abi_call(IDCT_PUT, dasm + off, line, ba); }
            #ifdef TEST_MAC
            if (memcmp(bp, ba, 128)) { if (mism++ < 5) printf("MAC kernel modified its input block (case %ld)\n", i); }
            if (memcmp(dref, dasm, span)) {
#else
            if (memcmp(br, ba, 128) || memcmp(dref, dasm, span)) {
#endif
                if (mism++ < 5) {
                    printf("MISMATCH %s case %ld line %d off %d\n", nm, i, line, off);
                    for (int k = 0; k < 64 && !TEST_MAC_NO; k++) if (br[k] != ba[k]) { printf("  block[%d] ref %d asm %d (in %d)\n", k, br[k], ba[k], bo[k]); break; }
                    for (int k = 0; k < span; k++) if (dref[k] != dasm[k]) { printf("  dest[%d] ref %d asm %d\n", k, dref[k], dasm[k]); break; }
                }
            }
        }
        printf("%s: %ld cases, %ld mismatches\n", nm, n, mism);
        bad += mism;
    }
    /* directed: the DC-only corner from NEXT_TASK (row[0]=2048 -> 16384 not 16383) */
    (void)stat;
    printf("callee-saved r8-r14 violation mask: 0x%x\n", abi);
    bad += abi != 0;
    printf(bad ? "FAIL\n" : "PASS\n");
    fflush(stdout);
    return bad != 0;
}
