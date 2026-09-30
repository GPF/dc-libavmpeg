#ifndef DC_LIBAVMPEG_CACHE_PROFILE_H
#define DC_LIBAVMPEG_CACHE_PROFILE_H

/* Opt-in (-DMPEG_CACHE_PROFILE_ADDR): one-shot address/cache-index dump of the
 * hot decode objects. Nothing here is compiled into default builds. */
#ifdef MPEG_CACHE_PROFILE_ADDR
/* kind: 'D' operand-cache data object, 'C' code (instruction cache).
 * size is the byte extent of one contiguous piece; stride (if non-zero) is the
 * distance between repeated pieces (rows, blocks). */
void mpeg_cache_addr_note(char kind, const char *name, const void *addr,
                          unsigned size, unsigned stride);
void mpeg_cache_addr_report(void);
extern int mpeg_cache_addr_armed; /* set by the MB hook; the IDCT hook then finishes and reports */
#endif

#endif
