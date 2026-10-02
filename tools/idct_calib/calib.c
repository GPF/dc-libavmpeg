#include <kos.h>
#include <stdint.h>
#include <stdio.h>

typedef void (*kern_t)(int iters, const short *a, const short *b);
#define K(n) extern void n(int, const short *, const short *)
K(k_loop); K(k_nop16); K(k_add16); K(k_muls_dep); K(k_term);
K(k_fill0); K(k_fill1); K(k_fill2); K(k_fill3); K(k_fill4);
K(k_muls_burst); K(k_mac4); K(k_mac16); K(k_mac_ovh4); K(k_mac_ovh1);

static short A[64] __attribute__((aligned(32))), B[64] __attribute__((aligned(32)));
#define ITERS 200000
#define REPS 5

/* cycles per loop iteration at 200 MHz (5 ns/cycle); minimum of REPS runs */
static double run(kern_t k) {
    uint64_t best = ~0ull;
    for (int r = 0; r < REPS; r++) {
        int old = irq_disable();
        uint64_t t0 = timer_ns_gettime64();
        k(ITERS, A, B);
        uint64_t t1 = timer_ns_gettime64();
        irq_restore(old);
        if (t1 - t0 < best) best = t1 - t0;
    }
    return (double)best / 5.0 / ITERS;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < 64; i++) { A[i] = 100 + i; B[i] = 200 - i; }
    double loop = run(k_loop);
    printf("calib: iters=%d reps=%d (min), cycles assume 200 MHz\n", ITERS, REPS);
    printf("calib: loop overhead      %7.3f cyc/iter\n", loop);
#define P(n, units, label) do { double c = run(n); \
    printf("calib: %-14s %7.3f cyc/iter  %6.3f cyc/unit  (%s, %d units)\n", #n, c, (c - loop) / (units), label, units); } while (0)
    P(k_nop16, 16, "nop");
    P(k_add16, 16, "dependent add");
    P(k_muls_dep, 16, "muls.w+sts macl+add");
    P(k_term, 16, "mov.l const+muls.w+sts+add");
    P(k_fill0, 16, "muls.w+0 fill+sts+add");
    P(k_fill1, 16, "muls.w+1 fill+sts+add");
    P(k_fill2, 16, "muls.w+2 fill+sts+add");
    P(k_fill3, 16, "muls.w+3 fill+sts+add");
    P(k_fill4, 16, "muls.w+4 fill+sts+add");
    P(k_muls_burst, 16, "16 muls.w then one sts");
    double o4 = run(k_mac_ovh4), o1 = run(k_mac_ovh1);
    double m4 = run(k_mac4), m16 = run(k_mac16);
    printf("calib: mac4  group: %7.3f cyc/iter, overhead %7.3f -> %6.3f cyc per mac.w\n", m4, o4, (m4 - o4) / 16);
    printf("calib: mac16 group: %7.3f cyc/iter, overhead %7.3f -> %6.3f cyc per mac.w\n", m16, o1, (m16 - o1) / 16);
    printf("calib: done\n");
    return 0;
}
