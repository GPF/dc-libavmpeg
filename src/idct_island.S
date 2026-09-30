! SH-4 stack-switch trampoline for the integer IDCT add/put bodies.
	.section .text.idct_trampoline
	.align 2
	.global _ff_simple_idct_add
	.global _ff_simple_idct_put
	.global _mpeg_idct_island_trampoline_start
	.global _mpeg_idct_island_trampoline_end
	.extern _ff_simple_idct_add_body
	.extern _ff_simple_idct_put_body
	.extern _mpeg_idct_private_stack_top
#ifdef MPEG_ISLAND_DEBUG
	.extern _mpeg_idct_private_stack_check
#endif

_mpeg_idct_island_trampoline_start:
_ff_simple_idct_add:
	mov.l .Ladd_body,r0
	bra .Lisland_enter
	nop

_ff_simple_idct_put:
	mov.l .Lput_body,r0

.Lisland_enter:
	! Preserve the caller's callee-saved r8 on its ordinary stack. r8 then
	! holds the exact stack pointer to restore after the IDCT body returns.
	sts.l pr,@-r15
	mov.l r8,@-r15
	mov r15,r8
	! r4-r6 remain live across the switch; r1 loads the private stack top.
	mov.l .Lstack_top,r1
	mov.l @r1,r15
	jsr @r0
	nop
	mov r8,r15
	mov.l @r15+,r8
#ifdef MPEG_ISLAND_DEBUG
	mov.l .Lstack_check,r0
	jsr @r0
	nop
#endif
	lds.l @r15+,pr
	rts
	nop

	.global _mpeg_idct_island_trampoline_code_end
_mpeg_idct_island_trampoline_code_end:
	.align 2
.Ladd_body:
	.long _ff_simple_idct_add_body
.Lput_body:
	.long _ff_simple_idct_put_body
.Lstack_top:
	.long _mpeg_idct_private_stack_top
#ifdef MPEG_ISLAND_DEBUG
.Lstack_check:
	.long _mpeg_idct_private_stack_check
#endif
_mpeg_idct_island_trampoline_end:
