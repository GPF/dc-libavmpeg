/* Printing half of the stackless region probe (tools/probe2.py generates the
 * assembly half). Called from the decode-return stub once per 120 decode calls;
 * it folds the 32-bit per-period accumulators into 64-bit totals and prints.
 * Free-standing: linked at a fixed address in the .text/.init gap, calling only
 * the production printf. Layout-neutral by the same argument as probe_patch.c. */
#include <stdint.h>

extern int printf(const char *fmt, ...);
extern char CAL_EVENT[]; /* absolute symbol from --defsym */

/* Accumulators are defined by the generated assembly. */
extern uint32_t probe_acc_dec[4], probe_n_dec[4];
extern uint32_t probe_acc_mb, probe_n_mb, probe_acc_idct, probe_n_idct, probe_acc_mc, probe_n_mc;

extern uint32_t probe_desc_mb[4], probe_desc_idct[4], probe_desc_mc[4];

static uint64_t tot_dec[4], cnt_dec[4], tot_mb, cnt_mb, tot_idct, cnt_idct, tot_mc, cnt_mc;

static void line(const char *name, unsigned t, uint64_t s, uint64_t n) {
    printf("@E=%02x %s%u sum=%08x%08x n=%u\n", (unsigned)(uintptr_t)CAL_EVENT, name, t,
           (unsigned)(s >> 32), (unsigned)s, (unsigned)n);
}

void probe_print(void) {
    unsigned i;

    for (i = 0; i < 4; i++) {
        tot_dec[i] += probe_acc_dec[i]; probe_acc_dec[i] = 0;
        cnt_dec[i] += probe_n_dec[i];   probe_n_dec[i] = 0;
    }
    tot_mb += probe_acc_mb;     probe_acc_mb = 0;   cnt_mb += probe_n_mb;     probe_n_mb = 0;
    tot_idct += probe_acc_idct; probe_acc_idct = 0; cnt_idct += probe_n_idct; probe_n_idct = 0;
    tot_mc += probe_acc_mc;     probe_acc_mc = 0;   cnt_mc += probe_n_mc;     probe_n_mc = 0;
    for (i = 0; i < 4; i++)
        line("dec t", i, tot_dec[i], cnt_dec[i]);
    line("mb t", 0, tot_mb, cnt_mb);
    line("idct t", 0, tot_idct, cnt_idct);
    line("mc t", 0, tot_mc, cnt_mc);
    printf("@SP mb=%08x idct=%08x mc=%08x\n", (unsigned)probe_desc_mb[3], (unsigned)probe_desc_idct[3], (unsigned)probe_desc_mc[3]);
}
