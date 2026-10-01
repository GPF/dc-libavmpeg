# MAC.W integer IDCT (MPEG_IDCT_ASM=2): results

Exact (bit-identical to `simple_idct.c`) frame-less SH-4 IDCT using `mac.w` dot
products. Coefficients use FF_SSE2_IDCT_PERM; the row stage writes a transposed
128-byte scratch buffer (`idct_mac_tmp`, not reentrant). Build:
`make av MPEG_IDCT_ASM=2 BUILD_DIR=build_mac2` (use a separate BUILD_DIR).

## Correctness
- QEMU (mac.w emulated with muls.w): 1M put + 1M add, 0 mismatches.
- On Dreamcast (real mac.w): 1M put + 1M add, 0 mismatches, r8-r14 preserved (v1 and v2).
- Player: 30 per-plane Adler checksums identical to the island reference (v1 and v2).

## Calibration (cycles @200 MHz)
muls.w+sts macl+add chain 8.0/product (9.06 with pc-relative constant); 0-2
independent filler instructions free in the gap; mac.w 2.0/product; 16 back-to-back
muls.w 2.4 each (single MACL: no overlap of multiplies).

## Isolated benchmark (net cycles/call, MPEG-like block mix)
| | warm put/add | cold put/add |
| C reference | 2658 / 2725 | 3509 / 3635 |
| muls.w asm | 1076 / 1204 | 4910 / 5139 |
| MAC.W v1 | 2366 / 2590 | 3210 / 3536 |
| MAC.W v2 (scheduled) | 2253 / 2469 | 3107 / 3427 |
Cold (caches flushed per call) is closer to a decoder call; the muls.w asm loses
there, which is why its player result was 19% slower.

## Player A/B (alternating, 3 runs each, mono Lair spec, decode ms/frame)
| layout | ref (island) | v2 | delta | sec19 | sec24 | drops ref / v2 |
| p0 | 20.924 | 20.265 | -0.659 (-3.15%) | -1.46 | -0.37 | 718/2 / 719/1,719/1,720/0 |
| text +32 | 21.088 | 20.376 | -0.712 (-3.38%) | -1.48 | -0.31 | 717/3 / 720/0 x3 |
| text +64 | 21.033 | 20.356 | -0.677 (-3.22%) | -1.45 | -0.34 | 718/2 / 720/0 x3 |
| text +96 | 20.960 | 20.328 | -0.632 (-3.02%) | -1.31 | -0.13 | 718/2 / 720/0 x3 |
Float idct_sh4 (not exact): 18.82 ms/frame, 720/0 every run. MAC.W v1: 20.646.
v2 spread across layouts 0.111 ms (ref 0.164 ms); runs repeat to ~0.01 ms.

## Scratch buffer placement (idct_mac_tmp)
The 128-byte scratch buffer's operand-cache set (addr bits 13:5, 16 KB direct-mapped)
moves decode time by up to ~0.6 ms/frame. `-DMAC_TMP_SET=<set>` pins it (16 KB-aligned
.bss section, buffer at set*32); default -1 leaves it unpinned (32-byte aligned).
Measured with ab_loop, 3 rounds each:
- sandbox build: unpinned (set 408) 20.264 = pinned 408 20.258; pinned 268 20.78 (+0.6).
- author build: unpinned (set 268) 20.705 = pinned 268 20.706; pinned 408 20.80 (+0.6).
No value is best for both builds, so the default stays unpinned. To tune a build, read
the set from `nm` on the unpinned ELF ((addr>>5)&511) and pin it to guard against link
changes. A sweep of other sets was not run.

## Open
- 720/0 on every run: v2 is 10/12 (two 719/1 at p0); ref is 0/12.
- Only the mono Lair fixture; stereo and 640x480 fixtures not run.
- layout_guard.py / alias_check.py not run on v2 (scratch + table placement unverified
  against ff_cropTbl and the block buffer).
- Not committed (the working checkout had no git remote).
