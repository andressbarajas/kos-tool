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

# entry+0x04 is the mandatory `j realstart` branch-delay slot: a real
# nop. It is NOT the console flag (a stale comment used to claim that;
# the flag is ps2loadmagic at entry+0x08, see below).
#
# ps2loadmagic (entry+0x08) is BOTH the kosload-detection magic AND the
# console-enabled flag: ps2_set_console_enabled() writes 0xdeadbeef
# (console on) / 0xfeedface (console off) here. Detectors must treat
# BOTH values as "loader present". KOSLOAD_BASE = entry+0x04, so loaded
# programs see BASE+4=magic, BASE+8=syscall, matching the DC/GC layout.
ps2loadmagic:
    .word   0xdeadbeef

# Syscall entry pointer (entry+0x0C)
kosload_syscall_ptr:
    .word   kosloadsyscall

# Pointer to kosload info block (entry+0x10)
kosload_info_ptr:
    .word   kosload_info

# Function pointers for exception handler (entry+0x18, +0x1C, +0x20, +0x24, +0x28)
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

# End of kosload hardcoded header

realstart:
    # ---- -F stale instruction-cache fix ----------------------------
    # On a firmware self-update (-F) the host trampoline copies this new
    # loader image into RAM and purges the D-cache, but nothing
    # invalidates the EE *instruction* cache.  The old and new loader
    # both link at 0x80000280, so the I-cache still holds the OLD
    # loader's lines for these addresses.  crt0 is layout-stable (always
    # first in .text) so it runs correctly from stale lines, but
    # main()/memset()/etc. sit at addresses whose stale I-cache contents
    # differ from this build -> Reserved-Instruction fault (observed as
    # excode 0xA, epc=memset, on -F; clean on cold boot, and
    # intermittent exactly because it is a cache-refill race).
    #
    # Hit-invalidate the whole image range [_start, _end) with the same
    # `cache 0x0b` (IHIN) + sync.p pattern as arch_icache_inval_range().
    # Self-contained inline asm on purpose: a C cache routine's own
    # lines could be stale too (chicken-and-egg).  Cold boot is
    # unaffected — invalidating an already-coherent I-cache just forces
    # harmless refills.  Needs no $gp/stack, so it runs first.
    la      $t0, _start
    la      $t1, _end
    sync.p
.Licache_inval:
    cache   0x0b, 0($t0)
    addiu   $t0, $t0, 64
    sltu    $t2, $t0, $t1
    bne     $t2, $zero, .Licache_inval
    nop
    sync.p

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

    # Start with no pending COP0 timer interrupt from the previous EE
    # context.  Writing Compare acknowledges the timer pending bit.
    mtc0    $zero, $9            # COP0 Count
    li      $t0, -1
    mtc0    $t0, $11             # COP0 Compare
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
