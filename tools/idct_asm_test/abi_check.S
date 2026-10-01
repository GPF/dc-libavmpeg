! int abi_call(fn, dest, line_size, block): calls fn(dest, line_size, block)
! with sentinels in r8-r14 and returns a bitmask of callee-saved registers
! (bit k = r(8+k)) that fn failed to preserve. 0 means the ABI was respected.
#ifdef ASM_UNDERSCORE
#define SYM(x) _##x
#else
#define SYM(x) x
#endif
	.text
	.align 2
	.global SYM(abi_call)
SYM(abi_call):
	sts.l pr,@-r15
	mov.l r8,@-r15
	mov.l r9,@-r15
	mov.l r10,@-r15
	mov.l r11,@-r15
	mov.l r12,@-r15
	mov.l r13,@-r15
	mov.l r14,@-r15
	mov r4,r1
	mov r5,r4
	mov r6,r5
	mov r7,r6
	mov #0x11,r8
	mov #0x22,r9
	mov #0x33,r10
	mov #0x44,r11
	mov #0x55,r12
	mov #0x66,r13
	mov #0x77,r14
	jsr @r1
	nop
	mov #0,r1
	mov r8,r0
	cmp/eq #0x11,r0
	bt 1f
	add #1,r1
1:	mov r9,r0
	cmp/eq #0x22,r0
	bt 1f
	add #2,r1
1:	mov r10,r0
	cmp/eq #0x33,r0
	bt 1f
	add #4,r1
1:	mov r11,r0
	cmp/eq #0x44,r0
	bt 1f
	add #8,r1
1:	mov r12,r0
	cmp/eq #0x55,r0
	bt 1f
	add #16,r1
1:	mov r13,r0
	cmp/eq #0x66,r0
	bt 1f
	add #32,r1
1:	mov r14,r0
	cmp/eq #0x77,r0
	bt 1f
	add #64,r1
1:	mov r1,r0
	mov.l @r15+,r14
	mov.l @r15+,r13
	mov.l @r15+,r12
	mov.l @r15+,r11
	mov.l @r15+,r10
	mov.l @r15+,r9
	mov.l @r15+,r8
	lds.l @r15+,pr
	rts
	nop
