#ifdef MPEG_CACHE_PROFILE_ADDR

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cache_profile.h"

/* SH7750/SH7091, KOS CCR_DEFAULT (OIX=IIX=ORA=0): both caches are direct
 * mapped with 32-byte lines. I-cache 8 KB = 256 lines, operand cache 16 KB =
 * 512 lines (SH7750 Hardware Manual v6.0, Table 4.1). */
#define LINE_SHIFT 5
#define IC_LINES 256u
#define OC_LINES 512u
#define MAX_OBJS 96

typedef struct {
    char kind;
    const char *name;
    uintptr_t addr;
    unsigned size, stride;
} obj_t;

int mpeg_cache_addr_armed;
static obj_t objs[MAX_OBJS];
static unsigned nobjs;

void mpeg_cache_addr_note(char kind, const char *name, const void *addr,
                          unsigned size, unsigned stride) {
    if (nobjs < MAX_OBJS) {
        obj_t *o = &objs[nobjs++];

        o->kind = kind;
        o->name = name;
        o->addr = (uintptr_t)addr;
        o->size = size;
        o->stride = stride;
    }
}

static unsigned nlines(const obj_t *o) {
    unsigned first = o->addr >> LINE_SHIFT;
    unsigned last = (o->addr + (o->size ? o->size : 1) - 1) >> LINE_SHIFT;

    return last - first + 1;
}

/* Number of cache lines (mod `mod`) shared by the first pieces of a and b. */
static unsigned overlap(const obj_t *a, const obj_t *b, unsigned mod) {
    unsigned i, j, n = 0, na = nlines(a), nb = nlines(b);
    unsigned ia = (a->addr >> LINE_SHIFT) % mod, ib = (b->addr >> LINE_SHIFT) % mod;

    if (na > mod) na = mod;
    if (nb > mod) nb = mod;
    for (i = 0; i < na; i++)
        for (j = 0; j < nb; j++)
            if ((ia + i) % mod == (ib + j) % mod)
                n++;
    return n;
}

/* Distinct cache lines used by all repeated pieces of one object (rows/blocks). */
static unsigned strided_distinct(const obj_t *o, unsigned pieces, unsigned mod, unsigned *worst) {
    unsigned char seen[512];
    unsigned p, l, distinct = 0, total = 0;

    memset(seen, 0, sizeof(seen));
    for (p = 0; p < pieces; p++) {
        obj_t piece = *o;

        piece.addr = o->addr + (uintptr_t)p * o->stride;
        for (l = 0; l < nlines(&piece); l++) {
            unsigned idx = ((piece.addr >> LINE_SHIFT) + l) % mod;

            total++;
            if (!seen[idx]) {
                seen[idx] = 1;
                distinct++;
            }
        }
    }
    *worst = total - distinct;
    return distinct;
}

void mpeg_cache_addr_report(void) {
    unsigned i, j;

    printf("CACHE_ADDR ---- object table (I-cache: 8KB/32B/256 lines; operand: 16KB/32B/512 lines)\n");
    printf("CACHE_ADDR kind name addr size stride  mod8192 mod16384  ic_line oc_line lines\n");
    for (i = 0; i < nobjs; i++) {
        const obj_t *o = &objs[i];

        printf("CACHE_ADDR %c %-34s 0x%08lx %6u %6u  %5lu %5lu  %3lu %3lu  %u\n",
               o->kind, o->name, (unsigned long)o->addr, o->size, o->stride,
               (unsigned long)(o->addr & 8191u), (unsigned long)(o->addr & 16383u),
               (unsigned long)((o->addr >> LINE_SHIFT) % IC_LINES),
               (unsigned long)((o->addr >> LINE_SHIFT) % OC_LINES), nlines(o));
    }
    printf("CACHE_ADDR ---- operand-cache overlaps (shared 32B lines, first piece of each object)\n");
    for (i = 0; i < nobjs; i++)
        for (j = i + 1; j < nobjs; j++)
            if (objs[i].kind == 'D' && objs[j].kind == 'D') {
                unsigned n = overlap(&objs[i], &objs[j], OC_LINES);

                if (n)
                    printf("CACHE_ADDR OC-OVERLAP %-30s x %-30s %u lines\n",
                           objs[i].name, objs[j].name, n);
            }
    printf("CACHE_ADDR ---- instruction-cache overlaps (shared 32B lines)\n");
    for (i = 0; i < nobjs; i++)
        for (j = i + 1; j < nobjs; j++)
            if (objs[i].kind == 'C' && objs[j].kind == 'C') {
                unsigned n = overlap(&objs[i], &objs[j], IC_LINES);

                if (n)
                    printf("CACHE_ADDR IC-OVERLAP %-30s x %-30s %u lines\n",
                           objs[i].name, objs[j].name, n);
            }
    printf("CACHE_ADDR ---- strided objects: distinct operand lines over 16 pieces, self-collisions\n");
    for (i = 0; i < nobjs; i++)
        if (objs[i].kind == 'D' && objs[i].stride) {
            unsigned worst, d = strided_distinct(&objs[i], 16, OC_LINES, &worst);

            printf("CACHE_ADDR STRIDED %-30s distinct=%u colliding=%u (16 pieces of %u B, stride %u)\n",
                   objs[i].name, d, worst, objs[i].size, objs[i].stride);
        }
    printf("CACHE_ADDR ---- end\n");
}

#endif
