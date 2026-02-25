# client/gamecube/go.S - Jump to loaded application (GameCube)
#
# Clears all general-purpose registers and jumps to the address
# passed in r3. Cache flushing should be done by the caller
# (target_ops->execute calls cache_flush then go).
#
# Sets ArenaHi (OS low mem 0x80000034) to the loader base so
# that libogc/devkitPPC heaps stay below the loader, then sets
# r1 (stack pointer) just below the loader region.
#
# Saves the loader's stack pointer and return address so that if
# the loaded program returns from main(), control passes through
# a trampoline that restores the loader's context and returns to
# gc_execute normally.

    .section .text
    .global go

go:
    # Save loader's stack pointer and return address (LR)
    lis     r11, go_saved_sp@ha
    stw     r1, go_saved_sp@l(r11)
    mflr    r0
    lis     r11, go_saved_lr@ha
    stw     r0, go_saved_lr@l(r11)

    # Save target address in CTR
    mtctr   r3

    # Set LR to return trampoline so loaded program's blr comes back here
    lis     r11, .Lgo_return@ha
    addi    r11, r11, .Lgo_return@l
    mtlr    r11

    # Set ArenaHi (OS low mem 0x80000034) = GC_LOADER_BASE so that
    # libogc heaps stay below the loader reservation.
    lis     r11, (GC_LOADER_BASE >> 16)
    ori     r11, r11, (GC_LOADER_BASE & 0xFFFF)
    lis     r12, 0x8000
    stw     r11, 0x0034(r12)

    # Set up stack just below loader region (GC_LOADER_BASE - 32)
    addi    r1, r11, -32

    # Clear all other GPRs
    li      r0, 0
    li      r2, 0
    li      r3, 0
    li      r4, 0
    li      r5, 0
    li      r6, 0
    li      r7, 0
    li      r8, 0
    li      r9, 0
    li      r10, 0
    li      r11, 0
    li      r12, 0
    li      r13, 0
    li      r14, 0
    li      r15, 0
    li      r16, 0
    li      r17, 0
    li      r18, 0
    li      r19, 0
    li      r20, 0
    li      r21, 0
    li      r22, 0
    li      r23, 0
    li      r24, 0
    li      r25, 0
    li      r26, 0
    li      r27, 0
    li      r28, 0
    li      r29, 0
    li      r30, 0
    li      r31, 0

    # Jump to loaded application
    bctr

.Lgo_return:
    # Loaded program returned from main(). Restore loader context.
    lis     r11, go_saved_sp@ha
    lwz     r1, go_saved_sp@l(r11)
    lis     r11, go_saved_lr@ha
    lwz     r0, go_saved_lr@l(r11)
    mtlr    r0
    blr

    .section .bss
    .align 2
go_saved_sp:
    .space 4
go_saved_lr:
    .space 4
