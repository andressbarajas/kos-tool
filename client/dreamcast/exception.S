! client/dreamcast/exception.S - SH4 exception handler for kosload
!
! Based on dcload-ip: dcload-ip/target-src/dcload/exception.S
!
! Captures a full register dump into a static save area and calls
! a C handler (exception_handler_c) to transmit it to the host tool
! as an sh4_exception_frame_t. All formatting is done host-side.

	.section .text

.balign 0x100

! VBR + 0x100
! general exceptions
general_1:
! assume the stack may be corrupted
	mov.l stack_addr,r15
! save caller-clobbered registers for regdump
	sts.l pr,@-r15
	mov.l r0,@-r15
! call regdump (saves all registers to static exc_save_area)
	mova regdump,r0
	jsr @r0
	 mov.l r1,@-r15
! pop temporary pr, r0, and r1 from stack
	add #12,r15
! call exception_handler_c(expt_code, spc, save_area)
! read EXPEVT early while it's guaranteed valid
	mov.l expevt,r0
	mov.l @r0,r4
	stc spc,r5
	mov.l save_area_k,r6
	mov.l handler_k,r0
	jsr @r0
	 nop
! return to bootloader
	mov.l entry_addr,r0
	jmp @r0
	 nop

.align 2
stack_addr:
	.long 0x8d000000
save_area_k:
	.long exc_save_area
handler_k:
	.long _exception_handler_c
expevt:
	.long 0xff000024
entry_addr:
	.long DC_LOADER_BASE	/* loader base (P1 cached); from mk/memory.mk */

regdump:
! save registers into the static save area (exc_save_area).
! caller has pushed pr, r0, r1 onto the stack before calling us.
	sts.l pr,@-r15
	mov.l regdump_save_area_k,r0
! (s)pc, pr, (s)sr, gbr, vbr, dbr, mach, macl
	stc spc,r1
	mov.l r1,@r0
	add #4,r0
	mov.l @(12,r15),r1
	mov.l r1,@r0
	add #4,r0
	stc ssr,r1
	mov.l r1,@r0
	add #4,r0
	stc gbr,r1
	mov.l r1,@r0
	add #4,r0
	stc vbr,r1
	mov.l r1,@r0
	add #4,r0
	stc dbr,r1
	mov.l r1,@r0
	add #4,r0
	sts mach,r1
	mov.l r1,@r0
	add #4,r0
	sts macl,r1
	mov.l r1,@r0
	add #4,r0
! r0-r7 bank 0
	stc r0_bank,r1
	mov.l r1,@r0
	add #4,r0
	stc r1_bank,r1
	mov.l r1,@r0
	add #4,r0
	stc r2_bank,r1
	mov.l r1,@r0
	add #4,r0
	stc r3_bank,r1
	mov.l r1,@r0
	add #4,r0
	stc r4_bank,r1
	mov.l r1,@r0
	add #4,r0
	stc r5_bank,r1
	mov.l r1,@r0
	add #4,r0
	stc r6_bank,r1
	mov.l r1,@r0
	add #4,r0
	stc r7_bank,r1
	mov.l r1,@r0
	add #4,r0
! r0-r7 bank 1
	mov.l @(8,r15),r1
	mov.l r1,@r0
	add #4,r0
	mov.l @(4,r15),r1
	mov.l r1,@r0
	add #4,r0
	mov.l r2,@r0
	add #4,r0
	mov.l r3,@r0
	add #4,r0
	mov.l r4,@r0
	add #4,r0
	mov.l r5,@r0
	add #4,r0
	mov.l r6,@r0
	add #4,r0
	mov.l r7,@r0
	add #4,r0
! r8-r14
	mov.l r8,@r0
	add #4,r0
	mov.l r9,@r0
	add #4,r0
	mov.l r10,@r0
	add #4,r0
	mov.l r11,@r0
	add #4,r0
	mov.l r12,@r0
	add #4,r0
	mov.l r13,@r0
	add #4,r0
	mov.l r14,@r0
	add #4,r0
! sgr (r15)
	stc sgr,r1
	mov.l r1,@r0
	add #4,r0
! fpscr
	sts fpscr,r1
	mov.l r1,@r0
	add #4,r0
! set fpscr.fr = 0, to get fr0-fr15
	mov.l fpscr_val_fr,r1
	lds r1,fpscr
! fr0-fr15
	fmov.s fr0,@r0
	add #4,r0
	fmov.s fr1,@r0
	add #4,r0
	fmov.s fr2,@r0
	add #4,r0
	fmov.s fr3,@r0
	add #4,r0
	fmov.s fr4,@r0
	add #4,r0
	fmov.s fr5,@r0
	add #4,r0
	fmov.s fr6,@r0
	add #4,r0
	fmov.s fr7,@r0
	add #4,r0
	fmov.s fr8,@r0
	add #4,r0
	fmov.s fr9,@r0
	add #4,r0
	fmov.s fr10,@r0
	add #4,r0
	fmov.s fr11,@r0
	add #4,r0
	fmov.s fr12,@r0
	add #4,r0
	fmov.s fr13,@r0
	add #4,r0
	fmov.s fr14,@r0
	add #4,r0
	fmov.s fr15,@r0
	add #4,r0
! fpul
	sts fpul,r1
	mov.l r1,@r0
	add #4,r0
! set fpscr.fr = 1, to get xf0-xf15
	mov.l fpscr_val_xf,r1
	lds r1,fpscr
! xf0-xf15
	fmov.s fr0,@r0
	add #4,r0
	fmov.s fr1,@r0
	add #4,r0
	fmov.s fr2,@r0
	add #4,r0
	fmov.s fr3,@r0
	add #4,r0
	fmov.s fr4,@r0
	add #4,r0
	fmov.s fr5,@r0
	add #4,r0
	fmov.s fr6,@r0
	add #4,r0
	fmov.s fr7,@r0
	add #4,r0
	fmov.s fr8,@r0
	add #4,r0
	fmov.s fr9,@r0
	add #4,r0
	fmov.s fr10,@r0
	add #4,r0
	fmov.s fr11,@r0
	add #4,r0
	fmov.s fr12,@r0
	add #4,r0
	fmov.s fr13,@r0
	add #4,r0
	fmov.s fr14,@r0
	add #4,r0
	fmov.s fr15,@r0
! return
	lds.l @r15+,pr
	rts
	 nop

.align 2
regdump_save_area_k:
	.long exc_save_area
fpscr_val_fr:
	.long 0x00040001
fpscr_val_xf:
	.long 0x00240001

.balign 0x400

! VBR + 0x400
! TLB miss exceptions
general_2:
	nop
	bra general_1
	 nop

.balign 0x200

! VBR + 0x600
! interrupt handler (stub)
interrupt:
	nop
	rte
	 nop

	.section .bss
	.align 4

! Register save area: 264 bytes (66 registers)
exc_save_area:
	.space 264
