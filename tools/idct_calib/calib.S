! SH-4 micro-benchmarks for the IDCT multiply path. Each kernel(iters, a, b):
!   r4 = iterations, r5 = array A (shorts), r6 = array B (shorts)
! Only r0-r7 are touched. Each loop iteration executes the body once.
#define SYM(x) _##x
	.text

	.macro KSTART name
	.align 5
	.global SYM(\name)
SYM(\name):
	mov #77,r1
	mov #3,r2
	mov #0,r3
	mov #1,r7
	.align 4
1:
	.endm
	.macro KEND
	dt r4
	bf 1b
	nop
	rts
	nop
	.endm

	KSTART k_loop
	KEND

	KSTART k_nop16
	.rept 16
	nop
	.endr
	KEND

	KSTART k_add16
	.rept 16
	add r1,r3
	.endr
	KEND

	! current IDCT term minus the constant load: muls.w, sts macl, add
	KSTART k_muls_dep
	.rept 16
	muls.w r1,r2
	sts macl,r0
	add r0,r3
	.endr
	KEND

	! the literal term used in idct_sh4.S: pc-relative constant load included
	KSTART k_term
	.rept 16
	mov.l .Lcal_c,r1
	muls.w r1,r2
	sts macl,r0
	add r0,r3
	.endr
	KEND
	.align 2
.Lcal_c: .long 22725

	.macro KFILL name, n
	KSTART \name
	.rept 16
	muls.w r1,r2
	.rept \n
	add r7,r7
	.endr
	sts macl,r0
	add r0,r3
	.endr
	KEND
	.endm
	KFILL k_fill0, 0
	KFILL k_fill1, 1
	KFILL k_fill2, 2
	KFILL k_fill3, 3
	KFILL k_fill4, 4

	! 16 back-to-back muls.w then one sts: multiplier issue throughput
	KSTART k_muls_burst
	.rept 16
	muls.w r1,r2
	.endr
	sts macl,r0
	add r0,r3
	KEND

	! MAC.W dot products with post-increment memory operands
	.macro KMAC name, n
	KSTART \name
	.rept 16/\n
	mov r5,r2
	mov r6,r3
	clrmac
	.rept \n
	mac.w @r2+,@r3+
	.endr
	sts macl,r0
	add r0,r7
	.endr
	KEND
	.endm
	KMAC k_mac4, 4
	KMAC k_mac16, 16
	! same overhead, no mac.w: 4 groups / 1 group per iteration
	KSTART k_mac_ovh4
	.rept 4
	mov r5,r2
	mov r6,r3
	clrmac
	sts macl,r0
	add r0,r7
	.endr
	KEND
	KSTART k_mac_ovh1
	mov r5,r2
	mov r6,r3
	clrmac
	sts macl,r0
	add r0,r7
	KEND
