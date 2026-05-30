! client/dreamcast/go.S - Jump to loaded application
!
! Based on dcload-ip: dcload-ip/target-src/dcload/go.s
!
! Restores full Dreamcast state (stack, VBR, SR, FPSCR),
! clears all general-purpose registers, and jumps to the
! address passed in r4.

	.section .text
	.global _go

_go:
	mov.l	stack_addr_k,r0
	mov.l	@r0,r15
	mov.l	entry_addr_k,r0
	mov.l	@r0,r0
	lds	r0,pr
	mov.l	sr_data_k,r0
	mov.l	@r0,r0
	ldc	r0,sr
	mov.l	vbr_data_k,r0
	mov.l	@r0,r0
	ldc	r0,vbr
	mov.l	fpscr_data_k,r0
	mov.l	@r0,r0
	lds	r0,fpscr
	mov	#0,r0
	mov	r0,r1
	mov	r0,r2
	mov	r0,r3
	mov	r0,r5
	mov	r0,r6
	mov	r0,r7
	mov	r0,r8
	mov	r0,r9
	mov	r0,r10
	mov	r0,r11
	mov	r0,r12
	mov	r0,r13
	mov	r0,r14
	jmp	@r4
	mov	r0,r4

	.align 2
stack_addr_k:
	.long	stack_addr
stack_addr:
	.long	(DC_LOADER_BASE + DC_LOADER_SIZE)	/* top of loader region */
entry_addr_k:
	.long	entry_addr
entry_addr:
#ifdef KOSLOAD_SERIAL_TRANSPORT
	.long	DC_LOADER_BASE			/* P1 (cached) — matches original dcload-serial */
#else
	.long	(DC_LOADER_BASE + 0x20000000)	/* P2 (uncached) — matches original dcload-ip */
#endif
sr_data_k:
	.long	sr_data
sr_data:
	.long	0x500000f0
vbr_data_k:
	.long	vbr_data
vbr_data:
	.long	(DC_LOADER_BASE + DC_LOADER_SIZE)	/* VBR at top of loader region */
fpscr_data_k:
	.long	fpscr_data
fpscr_data:
#ifdef KOSLOAD_SERIAL_TRANSPORT
	.long	0x40001		/* round toward zero — matches original dcload-serial */
#else
	.long	0x40000		/* round to nearest — matches original dcload-ip */
#endif
