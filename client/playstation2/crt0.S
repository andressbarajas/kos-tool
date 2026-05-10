# client/playstation2/crt0.S for kosload (PlayStation 2)
#
# MIPS R5900 (Emotion Engine) runtime startup.
# Sets up COP0 Status, clears BSS, initializes stack and GP, calls main().
# Also provides the kosloadsyscall entry point. The syscall jump
# table is defined in C (syscall_table.c) for easy extension.

    .set    noat
    .set    noreorder

    .extern main
    .extern __bss_start
    .extern _end
    .extern _gp
    .extern _stack

    # Syscall table and count (defined in syscall_table.c)
    .extern kosload_syscall_table
    .extern kosload_syscall_count

    .extern kosload_info

    # Video/display functions for exception handler
    .extern setup_video
    .extern clear_screen
    .extern draw_string
    .extern uint_to_string
    .extern exception_code_to_string

    .section .text.crt0
    .global _start
    .global _atexit
    .global kosloadsyscall

_start:
    j       realstart
    nop

# Magic value for detecting kosload (at entry+0x08).
# The nop delay slot at entry+0x04 is the console-flag word toggled by
# ps2_set_console_enabled() between 0xdeadbeef and 0xfeedface.
# KOSLOAD_BASE = entry+0x04, so loaded programs see BASE+4=magic, BASE+8=syscall,
# matching the DC/GC layout.
ps2loadmagic:
    .word   0xdeadbeef

# Syscall entry pointer (at entry+0x0C)
kosloadsyscall_ptr:
    .word   kosloadsyscall

# Function pointers for exception handler (at entry+0x10, +0x14, +0x18, +0x1C, +0x20)
setup_video_ptr:
    .word   setup_video
clear_screen_ptr:
    .word   clear_screen
draw_string_ptr:
    .word   draw_string
uint_to_string_ptr:
    .word   uint_to_string
exc_to_string_ptr:
    .word   exception_code_to_string

# Pointer to kosload info block (at entry+0x24)
kosload_info_ptr:
    .word   kosload_info

# End of kosload hardcoded header

realstart:
    # Load global pointer
    la      $gp, _gp

    # Set COP0 Status register:
    # 0x70030C00 = COP0 usable (bit 28) + COP1 usable (bit 29) +
    #              COP2 usable (bit 30) + EIE/EDI set +
    #              INTC/DMAC mask bits set + BEV=0 + kernel mode.
    #              Interrupts are still globally disabled because IE=0.
    #
    # Bit layout:
    #   [30] COP2 usable (VU0 macro mode)
    #   [29] COP1 usable (FPU)
    #   [28] COP0 usable
    #   [17] EDI (EI/DI instruction enable) = 1
    #   [16] EIE (EE master interrupt enable) = 1
    #   [11] INT1/DMAC mask = 1
    #   [10] INT0/INTC mask = 1
    #   [2:0] ERL=0, EXL=0, IE=0
    lui     $t0, 0x7003
    ori     $t0, $t0, 0x0C00
    mtc0    $t0, $12            # COP0 Status
    sync.p

    # Set up stack pointer (from linker script _stack symbol)
    la      $sp, _stack
    # Align stack to 16-byte boundary (EE ABI requirement)
    li      $t0, ~0xF
    and     $sp, $sp, $t0

    # Clear BSS section
    la      $t0, __bss_start
    la      $t1, _end
    beq     $t0, $t1, .Lbss_done
    nop
.Lbss_loop:
    sw      $zero, 0($t0)
    addiu   $t0, $t0, 4
    sltu    $t2, $t0, $t1
    bne     $t2, $zero, .Lbss_loop
    nop
.Lbss_done:

    # Create initial stack frame (16 bytes, back chain = 0)
    addiu   $sp, $sp, -16
    sw      $zero, 0($sp)

    # Call main()
    jal     main
    nop

    # Bootloader should never exit, but loop back if it does
    j       realstart
    nop

_atexit:
    jr      $ra
    nop

# ===== Kosload syscall entry point =====
# a0 = syscall number, a1 = arg1, a2 = arg2, a3 = arg3

    .align  2
kosloadsyscall:
    # Check magic value at ps2loadmagic
    la      $t0, ps2loadmagic
    lw      $t0, 0($t0)
    lui     $t1, 0xdead
    ori     $t1, $t1, 0xbeef
    bne     $t0, $t1, .Lbadsyscall
    nop

    # Validate syscall number (must be < count)
    la      $t0, kosload_syscall_count
    lw      $t0, 0($t0)
    sltu    $t1, $a0, $t0
    beq     $t1, $zero, .Lbadsyscall
    nop

    # Load function pointer from C table
    la      $t0, kosload_syscall_table
    sll     $t1, $a0, 2            # syscall_number * 4
    addu    $t0, $t0, $t1
    lw      $t2, 0($t0)            # t2 = handler address

    # Shift arguments: a1->a0, a2->a1, a3->a2
    move    $a0, $a1
    move    $a1, $a2
    move    $a2, $a3

    # Save caller's gp and ra on stack
    addiu   $sp, $sp, -16
    sw      $gp, 0($sp)
    sw      $ra, 4($sp)

    # Load our _gp (in case caller changed gp)
    la      $gp, _gp

    # Call the syscall handler
    jalr    $t2
    nop

    # Restore caller's gp and ra
    lw      $gp, 0($sp)
    lw      $ra, 4($sp)
    addiu   $sp, $sp, 16

    # Return to caller (v0 has return value from handler)
    jr      $ra
    nop

.Lbadsyscall:
    li      $v0, -1
    jr      $ra
    nop
