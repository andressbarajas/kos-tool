! client/dreamcast/crt0.S for kosload (Dreamcast)
!
! Based on dcload-ip: dcload-ip/target-src/dcload/dcload-crt0.s
!
! C runtime startup. Sets up cache, BSS, FPU, and calls main().
! Also provides the kosloadsyscall entry point. The syscall jump
! table is defined in C (syscall_table.c) for easy extension.

	.extern _setup_video
	.extern _clrscr
	.extern _draw_string
	.extern _uint_to_string
	.extern _exception_code_to_string
	.extern ___call_builtin_sh_set_fpscr

	.extern _kosload_info
	.extern _disable_cache

	.extern _edata
	.extern _end
	.extern _stack
	.extern _main

	! Syscall table and count (defined in syscall_table.c)
	.extern _kosload_syscall_table
	.extern _kosload_syscall_count

	.section .text.crt0
	.global	start
	.global _atexit
	.global _kosloadsyscall

start:
	bra	realstart
	 nop

! Magic value for detecting kosload
kosloadmagic:
	.long 0xdeadbeef

! Normal programs use this call
kosloadsyscall_k:
	.long _kosloadsyscall

! Exception handler uses these calls
setup_video_k:
	.long _setup_video
clrscr_k:
	.long _clrscr
draw_string_k:
	.long _draw_string
uint_to_string_k:
	.long _uint_to_string
exc_to_string_k:
	.long _exception_code_to_string

! Pointer to kosload info block (at entry+0x20)
kosload_info_k:
	.long _kosload_info

! end of kosload hardcoded stuff

realstart:
	stc	sr,r0
	mov.l	sr_mask,r1
	and	r1,r0
	or	#0xf0,r0
	ldc	r0,sr ! register banks may flip here if returning from an exception (but that's ok)

	! Disable the cache with invalidation before setting it back up to clear out any stale
	! entries from returning programs
	mov.l	disable_cache_addr,r1
	jsr	@r1
	 nop

! Set up the cache
	mov.l	setup_cache_k,r0
	mov.l	p2_mask,r1
	or	r1,r0
	jmp	@r0
	 nop

setup_cache:
	mov.l	ccr_addr,r0
	mov.w	ccr_data,r1
	mov.l	r1,@r0
	mov.l	start_2_k,r0
	mov	#0,r1
	nop
	nop
	nop
	nop
	nop
	nop
	jmp	@r0
	 mov	r1,r0

start_2:
	mov.l	stack_k,r15
	! zero out bss
	mov.l	edata_k,r0
	mov.l	end_k,r1
	cmp/eq	r0,r1 ! unless there is no bss
	bt	no_bss
	mov	#0,r2

start_l:
	mov.l	r2,@r0
	add	#4,r0
	cmp/hi	r0,r1
	bt	start_l
no_bss:

! Treat denormals as 0, round to nearest, disable FPU exceptions, FR = PR = SZ = 0
	mov.l	set_fpscr_k, r1
	mov	#4,r4
	jsr	@r1
	 shll16	r4

	! call main
	mov.l	main_k,r0
	jsr	@r0
	 or	r0,r0
	! bootloader should never exit, but loop back if it does
	bra	realstart
	 nop

_atexit:
	rts
	 nop


	.align 2
disable_cache_addr:
	.long	_disable_cache
sr_mask:
#ifdef KOSLOAD_SERIAL_TRANSPORT
	.long	0xefff7fff ! clear BL+FD, preserve RB (no exception handler in serial)
#else
	.long	0xcfff7fff ! clear BL+RB+FD (exception handler may have switched banks)
#endif
set_fpscr_k:
! __set_fpscr() is deprecated, use this wrapper for builtin instead
	.long	___call_builtin_sh_set_fpscr
stack_k:
	.long	_stack
edata_k:
	.long	_edata
end_k:
	.long	_end
main_k:
	.long	_main
setup_cache_k:
	.long	setup_cache
start_2_k:
	.long	start_2
p2_mask:
	.long	0xa0000000
ccr_addr:
	.long	0xff00001c
ccr_data:
#ifdef KOSLOAD_SERIAL_TRANSPORT
! All write-through (serial has no CacheBlockPurge, so copy-back is unsafe)
	.word	0x090b
#else
! P1 write-through, P0/P3/U0 copy-back (IP code uses CacheBlockPurge explicitly)
	.word	0x0909
#endif

_kosloadsyscall:
	mov.l	kosloadmagic_k,r1
	mov.l	@r1,r1
	mov.l	correctmagic,r0
	cmp/eq	r0,r1
	bf	badsyscall

	mov	r4,r0
	mov	r5,r4
	mov	r6,r5
	mov	r7,r6

	mov.l	syscall_count_k,r1
	mov.l	@r1,r1 ! load count from C variable
	cmp/hs	r0,r1 ! r1 >= r0?
	bf	badsyscall

	mov.l	syscall_table_k,r1
	shll2	r0
	mov.l	@(r0,r1),r0
	jmp	@r0
	 nop

badsyscall:
	mov	#-1,r0
	rts
	 nop

.align 2
kosloadmagic_k:
	.long kosloadmagic
correctmagic:
	.long 0xdeadbeef
syscall_count_k:
	.long _kosload_syscall_count
syscall_table_k:
	.long _kosload_syscall_table
