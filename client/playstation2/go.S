# client/playstation2/go.S - Jump to loaded application (PlayStation 2)
#
# Saves the loader's context (stack pointer, return address, GP,
# callee-saved registers, COP0 Status/Config) and jumps to the
# address passed in a0. If the loaded program returns, control
# passes through a trampoline that restores the loader's context.

    .set    noat
    .set    noreorder

    .extern _gp

    .section .text
    .global go

go:
    # Save loader context to __go_save_regs
    la      $t0, __go_save_regs

    # Save callee-saved registers s0-s7 (64-bit each using sd)
    sd      $s0,  0($t0)
    sd      $s1,  8($t0)
    sd      $s2, 16($t0)
    sd      $s3, 24($t0)
    sd      $s4, 32($t0)
    sd      $s5, 40($t0)
    sd      $s6, 48($t0)
    sd      $s7, 56($t0)
    sd      $s8, 64($t0)       # s8/fp

    # Save COP0 Status and Config
    mfc0    $t1, $12            # COP0 Status
    sw      $t1, 72($t0)
    mfc0    $t1, $16            # COP0 Config
    sw      $t1, 76($t0)

    # Save gp
    sw      $gp, 80($t0)

    # Save ra
    sw      $ra, 84($t0)

    # Save sp to static variable
    la      $t0, __go_save_sp
    sw      $sp, 0($t0)

    # Set ra to return trampoline so loaded program's jr $ra comes back
    la      $ra, .Lgo_return

    # Jump to loaded program (address in a0)
    move    $t0, $a0
    jr      $t0
    nop

.Lgo_return:
    # Loaded program returned from main(). Restore loader context.

    # Restore gp first so we can access our symbols
    la      $gp, _gp

    # Restore sp
    la      $t0, __go_save_sp
    lw      $sp, 0($t0)

    # Restore all saved registers
    la      $t0, __go_save_regs

    ld      $s0,  0($t0)
    ld      $s1,  8($t0)
    ld      $s2, 16($t0)
    ld      $s3, 24($t0)
    ld      $s4, 32($t0)
    ld      $s5, 40($t0)
    ld      $s6, 48($t0)
    ld      $s7, 56($t0)
    ld      $s8, 64($t0)

    # Restore COP0 Status and Config
    lw      $t1, 72($t0)
    mtc0    $t1, $12            # COP0 Status
    sync.p
    lw      $t1, 76($t0)
    mtc0    $t1, $16            # COP0 Config
    sync.p

    # Restore gp and ra
    lw      $gp, 80($t0)
    lw      $ra, 84($t0)

    # Return to caller (ps2_execute)
    jr      $ra
    nop

    .section .bss
    .align  4
__go_save_sp:
    .space  4

    .align  4
__go_save_regs:
    # s0-s8: 9 * 8 = 72 bytes (64-bit saves)
    # COP0 Status + Config: 2 * 4 = 8 bytes
    # gp + ra: 2 * 4 = 8 bytes
    # Total: 88 bytes
    .space  88
