! MAC.W integer SH-4 IDCT, bit-exact with simple_idct.c ff_simple_idct_add/put
! once the coefficients are stored with FF_SSE2_IDCT_PERM.
!
! Why: measured on hardware, muls.w + sts macl costs 8 cycles per product while
! mac.w @r+,@r+ accumulates at 2 cycles per product. Each 1-D output is split
! into an even sum A_k (x0 x2 x4 x6) and an odd sum B_k (x1 x3 x5 x7):
!   y_k = A_k + B_k,  y_(7-k) = A_k - B_k
! and every sum is a 4-term dot product with a constant vector from a table.
!
! Input layout (per row of 8 shorts, FF_SSE2_IDCT_PERM):
!   x0 x2 x4 x6 | x1 x3 x5 x7    (so both dot products are contiguous)
! The row stage writes y_j of row r to scratch[8*j + p(r)] with
! p = {0,4,1,5,2,6,3,7}, so each scratch row is one column in the same split
! layout. The input block is NOT modified (the C reference leaves row-stage
! results in it; the decoder clears blocks before reuse).
!
! Frame-less: no stack. r8-r14 live in fr4-fr10, r4/r5 in fr11/fr3 during the
! row stage (fr0 holds the column-stage address); all are caller-saved FP
! registers. Clobbers r0-r7, MACH/MACL, FPUL, fr0, fr3-fr11 and sets SR.S=0. Uses one global
! 128-byte scratch buffer: not reentrant, one IDCT at a time.
!
! void ff_simple_idct_{put,add}_mac(uint8_t *dest, int line_size, short *block)
!
! Rounding: rows add 1<<10 (lds into MACL instead of clrmac); columns add
! W4*32 = 524256, which is exactly W4*(col0 + 32) of the C code.

#ifdef ASM_UNDERSCORE
#define SYM(x) _##x
#else
#define SYM(x) x
#endif

#ifndef MAC_TAB_SHIFT
#define MAC_TAB_SHIFT 0
#endif
#ifndef MAC_TEXT_PAD
#define MAC_TEXT_PAD 0
#endif
#ifndef MAC_TMP_SET
#define MAC_TMP_SET -1
#endif
	! MAC_TAB_SHIFT (bytes, multiple of 32) is a placement experiment: it moves the
	! constant table by whole cache lines.
	!
	! MAC_TMP_SET (default -1: unpinned, 32-byte aligned .bss) pins the 128-byte
	! scratch buffer to operand-cache sets MAC_TMP_SET..+3 (set = address bits 13:5
	! of the 16 KB direct-mapped cache): its section is 16 KB aligned and the buffer
	! sits MAC_TMP_SET*32 bytes into it, so link order cannot move it.
	! There is no universally good value: the best set depends on where the rest of
	! the hot data lands in a given build. Measured: sandbox build best at 408 and
	! 0.6 ms/frame worse at 268; the author's build best at 268 and 0.6 ms worse at
	! 408. To tune, read the unpinned set from `nm` and pass -DMAC_TMP_SET=<set>.
	.section .rodata
	.align 5
#if MAC_TAB_SHIFT
	.space MAC_TAB_SHIFT
#endif
SYM(idct_mac_tab):
	! even sums A0..A3 over (x0 x2 x4 x6)
	.short 16383, 21407, 16383, 8867
	.short 16383, 8867, -16383, -21407
	.short 16383, -8867, -16383, 21407
	.short 16383, -21407, 16383, -8867
	! odd sums B0..B3 over (x1 x3 x5 x7)
	.short 22725, 19266, 12873, 4520
	.short 19266, -4520, -22725, -12873
	.short 12873, -22725, 4520, 19266
	.short 4520, -12873, 19266, -22725

	.section .bss
#if MAC_TMP_SET >= 0
	.balign 16384
#if MAC_TMP_SET
	.space MAC_TMP_SET*32
#endif
#else
	.align 5
#endif
	.global SYM(idct_mac_tmp)
SYM(idct_mac_tmp):
	.space 128

	.text

	! Dot product over the pointer pair r2 (data) / r3 (table): ACCINIT/ACCZERO
	! start the accumulator, DOT4 adds four 16x16 products and leaves the
	! 32-bit sum in \dst. With -DMAC_EMU (QEMU only; its mac.w is broken) the
	! accumulator is \dst itself and each product is muls.w + add using r0/r13.
#ifdef MAC_EMU
	.macro ACCINIT src, dst
	mov \src,\dst
	.endm
	.macro ACCZERO dst
	mov #0,\dst
	.endm
	.macro DOT4 dst
	.rept 4
	mov.w @r2+,r0
	mov.w @r3+,r13
	muls.w r0,r13
	sts macl,r13
	add r13,\dst
	.endr
	.endm
	.macro ACCREAD dst
	.endm
#else
	.macro ACCINIT src, dst
	lds \src,macl
	.endm
	.macro ACCZERO dst
	clrmac
	.endm
	.macro DOT4 dst
	mac.w @r2+,@r3+
	mac.w @r2+,@r3+
	mac.w @r2+,@r3+
	mac.w @r2+,@r3+
	.endm
	.macro ACCREAD dst
	sts macl,\dst
	.endm
#endif

	! store r13 as the 16-bit value scratch[8*j + slot] (r14 = scratch + 2*slot)
	.macro TSTORE j
	.if \j == 0
	mov.w r13,@r14
	.else
	mov #(\j*16),r0
	mov.w r13,@(r0,r14)
	.endif
	.endm

	! Row stage groups. Anything independent of the sum (next group's pointer
	! setup, the copy of A_k, the store offset) sits between the last mac.w and
	! sts macl, where the pipeline would otherwise stall.
	.macro RA dst, nxt
	ACCINIT r4, \dst
	DOT4 \dst
	mov \nxt,r2
	ACCREAD \dst
	.endm

	.macro ROWB acc, j, nxt=
	ACCZERO r12
	DOT4 r12
	mov \acc,r13
	.if \j != 0
	mov #(\j*16),r0
	.endif
	.ifnb \nxt
	mov \nxt,r2
	.endif
	ACCREAD r12
	add r12,r13
	shad r1,r13
	.if \j == 0
	mov.w r13,@r14
	.else
	mov.w r13,@(r0,r14)
	.endif
	sub r12,\acc
	.endm

	.macro ROWLOW acc, j
	mov \acc,r13
	shad r1,r13
	TSTORE \j
	.endm

	.macro CA dst, odd
	ACCINIT r12, \dst
	DOT4 \dst
	mov r6,r2
	.if \odd
	add #8,r2
	.endif
	ACCREAD \dst
	.endm

	! one output pixel row: value in r13 (already shifted), r14 = cm table
	.macro EMIT isadd
	.if \isadd
	mov.b @r4,r0
	extu.b r0,r0
	add r13,r0
	.else
	mov r13,r0
	.endif
	mov.b @(r0,r14),r0
	mov.b r0,@r4
	.endm

	.macro COLB acc, isadd, more
	ACCZERO r12
	DOT4 r12
	mov \acc,r13
	.if \isadd
	mov.b @r4,r0
	extu.b r0,r0
	.endif
	.if \more
	mov r6,r2
	add #8,r2
	.endif
	ACCREAD r12
	add r12,r13
	shad r1,r13
	.if \isadd
	add r13,r0
	.else
	mov r13,r0
	.endif
	mov.b @(r0,r14),r0
	mov.b r0,@r4
	add r5,r4
	sub r12,\acc
	.endm

	.macro COLLOW acc, isadd, last
	mov \acc,r13
	shad r1,r13
	EMIT \isadd
	.if \last == 0
	add r5,r4
	.endif
	.endm

	! put and add share one row stage and one exit to halve the code footprint
	! (the 8 KB instruction cache is the scarce resource, see IDCT_MAC_RESULTS.md).
	! The entry points only load the address of their own column stage into r0;
	! the shared stash code parks it in fr0 and the row stage jumps there.

	.macro ENTRY name, cont
	.align 2
	.global SYM(\name)
SYM(\name):
	mov.l 1f,r0
	bra .Lrows
	nop
	.align 2
1:	.long .L\cont
	.endm

#if MAC_TEXT_PAD
	! placement experiment: moves the kernel by whole cache lines
	.space MAC_TEXT_PAD
#endif
	.align 5
	ENTRY ff_simple_idct_put_mac, colput
	ENTRY ff_simple_idct_add_mac, coladd

	.align 5
.Lrows:
	lds r0,fpul
	fsts fpul,fr0
	clrs
	lds r8,fpul
	fsts fpul,fr4
	lds r9,fpul
	fsts fpul,fr5
	lds r10,fpul
	fsts fpul,fr6
	lds r11,fpul
	fsts fpul,fr7
	lds r12,fpul
	fsts fpul,fr8
	lds r13,fpul
	fsts fpul,fr9
	lds r14,fpul
	fsts fpul,fr10
	lds r4,fpul
	fsts fpul,fr11
	lds r5,fpul
	fsts fpul,fr3

	! ---- row stage: rows visited in slot order 0,2,4,6,1,3,5,7 ----
	mov #4,r4
	shll8 r4
	mov #-11,r1
	mov.l .Lrow_ts,r14
	mov #8,r7
.Lrow:
	! DC-only row: every output is (x0 << 3) & 0xffff
	mov.l @r6,r13
	shlr16 r13
	mov.l @(4,r6),r0
	or r0,r13
	mov.l @(8,r6),r0
	or r0,r13
	mov.l @(12,r6),r0
	or r0,r13
	tst r13,r13
	bf .Lrow_gen
	mov.w @r6,r13
	shll2 r13
	shll r13
	extu.w r13,r13
	TSTORE 0
	TSTORE 1
	TSTORE 2
	TSTORE 3
	TSTORE 4
	TSTORE 5
	TSTORE 6
	TSTORE 7
	bra .Lrow_next
	nop
.Lrow_gen:
	mov.l .Lrow_tab,r3
	mov r6,r5
	add #8,r5
	mov r6,r2
	RA r8, r6
	RA r9, r6
	RA r10, r6
	RA r11, r5
	ROWB r8, 0, r5
	ROWB r9, 1, r5
	ROWB r10, 2, r5
	ROWB r11, 3
	ROWLOW r11, 4
	ROWLOW r10, 5
	ROWLOW r9, 6
	ROWLOW r8, 7
.Lrow_next:
	add #2,r14
	add #32,r6
	dt r7
	bt .Lrow_done
	mov r7,r0
	cmp/eq #4,r0
	bf .Lrow_again
	add #-112,r6
.Lrow_again:
	bra .Lrow
	nop
.Lrow_done:
	flds fr11,fpul
	sts fpul,r4
	flds fr3,fpul
	sts fpul,r5
	flds fr0,fpul
	sts fpul,r0
	jmp @r0
	nop
	.align 2
.Lrow_ts:  .long SYM(idct_mac_tmp)
.Lrow_tab: .long SYM(idct_mac_tab)

	! ---- column stage: scratch row j is column j, in split layout ----
	.macro COLS name, isadd
	.align 5
.L\name:
	mov.l .L\name\()_ts,r6
	mov.l .L\name\()_cm,r14
	mov #-20,r1
	mov #8,r7
.L\name\()_col:
	mov.l .L\name\()_tab,r3
	mov.l .L\name\()_rnd,r12
	mov r6,r2
	CA r8, 0
	CA r9, 0
	CA r10, 0
	CA r11, 1
	COLB r8, \isadd, 1
	COLB r9, \isadd, 1
	COLB r10, \isadd, 1
	COLB r11, \isadd, 0
	COLLOW r11, \isadd, 0
	COLLOW r10, \isadd, 0
	COLLOW r9, \isadd, 0
	COLLOW r8, \isadd, 1
	! rewind dest by 7*line_size and step to the next column
	mov r5,r13
	shll2 r13
	shll r13
	sub r5,r13
	sub r13,r4
	add #1,r4
	add #16,r6
	dt r7
	bt .L\name\()_done
	bra .L\name\()_col
	nop
.L\name\()_done:
	bra .Lexit
	nop
	.align 2
.L\name\()_ts:  .long SYM(idct_mac_tmp)
.L\name\()_tab: .long SYM(idct_mac_tab)
.L\name\()_cm:  .long SYM(ff_cropTbl) + 1024
.L\name\()_rnd: .long 524256
	.endm

	COLS colput, 0
	COLS coladd, 1

.Lexit:
	flds fr4,fpul
	sts fpul,r8
	flds fr5,fpul
	sts fpul,r9
	flds fr6,fpul
	sts fpul,r10
	flds fr7,fpul
	sts fpul,r11
	flds fr8,fpul
	sts fpul,r12
	flds fr9,fpul
	sts fpul,r13
	flds fr10,fpul
	sts fpul,r14
	rts
	nop
