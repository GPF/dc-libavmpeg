! Frame-less integer SH-4 IDCT (bit-exact with simple_idct.c ff_simple_idct_add/put).
!
! Literal, deliberately unscheduled translation of the C reference: same term
! order, same constants, same DC-only row shortcut and odd-term/sparse-column
! guards. No stack is touched: r8-r14 are parked in fr4-fr10 (caller-saved
! FP registers) for the duration of the call. Only r0-r14, MACL and FPUL are
! clobbered; PR is never modified (leaf function).
!
! void ff_simple_idct_{put,add}_asm(uint8_t *dest, int line_size, short *block)
!   r4 = dest, r5 = line_size, r6 = block
!
! Register map:
!   r0,r1,r2  scratch (r2 holds the current coefficient)
!   r3        b3
!   r4,r5     dest, line_size
!   r6        row pointer (row stage) / column pointer (column stage)
!   r7        loop counter
!   r8-r11    a0-a3        r12,r13,r14,r3  b0-b3
!
! W4*(col0 + 32) is formed as W4*col0 + (W4 << 5) because col0 + 32 may not fit
! in 16 bits; the two are identical modulo 2^32.

#ifdef ASM_UNDERSCORE
#define SYM(x) _##x
#else
#define SYM(x) x
#endif

#define W1 22725
#define W2 21407
#define W3 19266
#define W4 16383
#define W5 12873
#define W6 8867
#define W7 4520

	.macro LDX off
	mov #\off,r0
	mov.w @(r0,r6),r2
	.endm

	! acc = W * r2
	.macro TI acc, w
	mov.l \w,r1
	muls.w r1,r2
	sts macl,\acc
	.endm

	! acc += W * r2
	.macro TA acc, w
	mov.l \w,r1
	muls.w r1,r2
	sts macl,r0
	add r0,\acc
	.endm

	! acc -= W * r2
	.macro TS acc, w
	mov.l \w,r1
	muls.w r1,r2
	sts macl,r0
	sub r0,\acc
	.endm

	! even part, coefficient 2 (r2 holds it)
	.macro EVEN2 p
	TA r8,.L\p\()k2
	TA r9,.L\p\()k6
	TS r10,.L\p\()k6
	TS r11,.L\p\()k2
	.endm
	! odd part, coefficient 1 (initialises b)
	.macro ODD1 p
	TI r12,.L\p\()k1
	TI r13,.L\p\()k3
	TI r14,.L\p\()k5
	TI r3,.L\p\()k7
	.endm
	.macro ODD3 p
	TA r12,.L\p\()k3
	TS r13,.L\p\()k7
	TS r14,.L\p\()k1
	TS r3,.L\p\()k5
	.endm
	.macro EVEN4 p
	TA r8,.L\p\()k4
	TS r9,.L\p\()k4
	TS r10,.L\p\()k4
	TA r11,.L\p\()k4
	.endm
	.macro ODD5 p
	TA r12,.L\p\()k5
	TS r13,.L\p\()k1
	TA r14,.L\p\()k7
	TA r3,.L\p\()k3
	.endm
	.macro EVEN6 p
	TA r8,.L\p\()k6
	TS r9,.L\p\()k2
	TA r10,.L\p\()k2
	TS r11,.L\p\()k6
	.endm
	.macro ODD7 p
	TA r12,.L\p\()k7
	TS r13,.L\p\()k5
	TA r14,.L\p\()k3
	TS r3,.L\p\()k1
	.endm

	.macro POOL p
	.align 2
.L\p\()k1: .long W1
.L\p\()k2: .long W2
.L\p\()k3: .long W3
.L\p\()k4: .long W4
.L\p\()k5: .long W5
.L\p\()k6: .long W6
.L\p\()k7: .long W7
	.endm

	! row output: row[off/2] = (a +/- b) >> 11   (r1 must hold -11)
	.macro ROWOUT a, b, op, off
	mov \a,r2
	\op \b,r2
	shad r1,r2
	mov #\off,r0
	mov.w r2,@(r0,r6)
	.endm

	! column output; add variant reads dest, put variant does not.
	.macro COLOUT a, b, op, isadd, last, cm
	mov \a,r2
	\op \b,r2
	mov #-20,r1
	shad r1,r2
	.if \isadd
	mov.b @r4,r0
	extu.b r0,r0
	add r2,r0
	.else
	mov r2,r0
	.endif
	mov.l \cm,r1
	mov.b @(r0,r1),r0
	mov.b r0,@r4
	.if \last == 0
	add r5,r4
	.endif
	.endm

	.macro IDCT name, isadd
	.text
	.align 5
	.global SYM(\name)
SYM(\name):
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
	mov #8,r7

	! ---- row stage: 8 rows in place ----
.L\name\()_row:
	mov.l @r6,r0
	shlr16 r0
	mov.l @(4,r6),r1
	or r1,r0
	mov.l @(8,r6),r1
	or r1,r0
	mov.l @(12,r6),r1
	or r1,r0
	tst r0,r0
	bf .L\name\()_gen
	! DC-only row: every output is (row[0] << 3) & 0xffff
	mov.w @r6,r2
	shll2 r2
	shll r2
	extu.w r2,r2
	mov r2,r0
	shll16 r0
	or r0,r2
	mov.l r2,@r6
	mov.l r2,@(4,r6)
	mov.l r2,@(8,r6)
	mov.l r2,@(12,r6)
	bra .L\name\()_rownext
	nop
.L\name\()_gen:
	LDX 0
	TI r8,.L\name\()_rk4
	mov #4,r1
	shll8 r1
	add r1,r8
	mov r8,r9
	mov r8,r10
	mov r8,r11
	LDX 4
	EVEN2 \name\()_r
	LDX 2
	ODD1 \name\()_r
	LDX 6
	ODD3 \name\()_r
	mov.l @(8,r6),r0
	mov.l @(12,r6),r1
	or r1,r0
	tst r0,r0
	bt .L\name\()_noodd
	LDX 8
	EVEN4 \name\()_r
	LDX 12
	EVEN6 \name\()_r
	LDX 10
	ODD5 \name\()_r
	LDX 14
	ODD7 \name\()_r
.L\name\()_noodd:
	mov #-11,r1
	ROWOUT r8,r12,add,0
	ROWOUT r8,r12,sub,14
	ROWOUT r9,r13,add,2
	ROWOUT r9,r13,sub,12
	ROWOUT r10,r14,add,4
	ROWOUT r10,r14,sub,10
	ROWOUT r11,r3,add,6
	ROWOUT r11,r3,sub,8
.L\name\()_rownext:
	add #16,r6
	dt r7
	bt .L\name\()_rowdone
	bra .L\name\()_row
	nop
.L\name\()_rowdone:
	bra .L\name\()_colstart
	nop
	POOL \name\()_r
.L\name\()_colstart:
	add #-128,r6
	mov #8,r7

	! ---- column stage: 8 columns, output to dest ----
.L\name\()_col:
	LDX 0
	TI r8,.L\name\()_ck4
	shll2 r1
	shll2 r1
	shll r1
	add r1,r8
	mov r8,r9
	mov r8,r10
	mov r8,r11
	LDX 32
	EVEN2 \name\()_c
	LDX 16
	ODD1 \name\()_c
	LDX 48
	ODD3 \name\()_c
	LDX 64
	tst r2,r2
	bt 1f
	EVEN4 \name\()_c
1:
	LDX 80
	tst r2,r2
	bt 1f
	ODD5 \name\()_c
1:
	LDX 96
	tst r2,r2
	bt 1f
	EVEN6 \name\()_c
1:
	LDX 112
	tst r2,r2
	bt 1f
	ODD7 \name\()_c
1:
	COLOUT r8,r12,add,\isadd,0,.L\name\()_ccm
	COLOUT r9,r13,add,\isadd,0,.L\name\()_ccm
	COLOUT r10,r14,add,\isadd,0,.L\name\()_ccm
	COLOUT r11,r3,add,\isadd,0,.L\name\()_ccm
	COLOUT r11,r3,sub,\isadd,0,.L\name\()_ccm
	COLOUT r10,r14,sub,\isadd,0,.L\name\()_ccm
	COLOUT r9,r13,sub,\isadd,0,.L\name\()_ccm
	COLOUT r8,r12,sub,\isadd,1,.L\name\()_ccm
	! rewind dest by 7*line_size and step to the next column
	mov r5,r2
	shll2 r2
	shll r2
	sub r5,r2
	sub r2,r4
	add #1,r4
	add #2,r6
	dt r7
	bt .L\name\()_coldone
	bra .L\name\()_col
	nop
.L\name\()_coldone:
	bra .L\name\()_end
	nop
	POOL \name\()_c
.L\name\()_ccm: .long SYM(ff_cropTbl) + 1024
.L\name\()_end:
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
	.endm

	IDCT ff_simple_idct_put_asm, 0
	IDCT ff_simple_idct_add_asm, 1
