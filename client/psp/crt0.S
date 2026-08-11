# client/psp/crt0.S — kosload PSP (Allegrex / MIPS32, little-endian) startup.
#
# The PSP kernel loads our module and enters at _start.  This stub sets gp and
# the stack, enables the FPU (COP1), clears BSS, and calls main().  It also
# publishes the guest-facing kosload header (magic + syscall entry + info +
# video fn pointers) at the image base and the kosloadsyscall dispatcher, in
# the same layout the PS2 (also MIPS) port uses.

    .set    noreorder
    .set    noat

    .extern main
    .extern __bss_start
    .extern _end
    .extern _gp
    .extern _stack
    .extern kosload_syscall_table
    .extern kosload_syscall_count
    .extern kosload_info
    .extern setup_video
    .extern clear_screen
    .extern draw_string
    .extern uint_to_string
    .extern exception_code_to_string

    .section .text.crt0, "ax"
    .global _start
    .global kosloadsyscall

# ---- Guest-facing header (mirrors PS2 layout: BASE+4 magic, +8 syscall) ----
_start:
    j       realstart
    nop
kosloadmagic:
    .word   0xdeadbeef              # +0x08
kosload_syscall_ptr:
    .word   kosloadsyscall          # +0x0C
kosload_info_ptr:
    .word   kosload_info            # +0x10
setup_video_ptr:
    .word   setup_video             # +0x14
clear_screen_ptr:
    .word   clear_screen            # +0x18
draw_string_ptr:
    .word   draw_string             # +0x1C
uint_to_string_ptr:
    .word   uint_to_string          # +0x20
exc_to_string_ptr:
    .word   exception_code_to_string # +0x24

realstart:
    # Load global pointer.
    la      $gp, _gp

    # Enable COP1 (FPU) in Status so single-float code can run.  CU1 = bit 29.
    mfc0    $t0, $12                 # COP0 Status
    lui     $t1, 0x2000              # 1 << 29
    or      $t0, $t0, $t1
    mtc0    $t0, $12
    nop
    nop

    # Set up stack (from linker _stack), 8-byte aligned.
    la      $sp, _stack
    li      $t0, -16
    and     $sp, $sp, $t0

    # Clear BSS.
    la      $t0, __bss_start
    la      $t1, _end
    beq     $t0, $t1, bss_done
    nop
bss_loop:
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    sltu    $t2, $t0, $t1
    bne     $t2, $zero, bss_loop
    nop
bss_done:
    # Reserve a 16-byte argument slot and call main().
    addiu   $sp, $sp, -16
    jal     main
    nop

    # main() should not return; loop if it does.
    j       realstart
    nop

# ===== kosloadsyscall(nr, a1, a2, a3) — dispatch through the shared table =====
    .align  2
kosloadsyscall:
    # Validate magic.
    la      $t0, kosloadmagic
    lw      $t0, 0($t0)
    lui     $t1, 0xdead
    ori     $t1, $t1, 0xbeef
    bne     $t0, $t1, badsyscall
    nop

    # Validate syscall number (< count).
    la      $t0, kosload_syscall_count
    lw      $t0, 0($t0)
    sltu    $t1, $a0, $t0
    beq     $t1, $zero, badsyscall
    nop

    # Look up handler.
    la      $t0, kosload_syscall_table
    sll     $t1, $a0, 2
    addu    $t0, $t0, $t1
    lw      $t2, 0($t0)

    # Shift args a1->a0, a2->a1, a3->a2 and tail-call the handler.
    move    $a0, $a1
    move    $a1, $a2
    move    $a2, $a3
    jr      $t2
    nop

badsyscall:
    li      $v0, -1
    jr      $ra
    nop
