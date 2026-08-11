# client/psp/go.S — jump to a loaded program (PSP / Allegrex MIPS).
#
# go(uint32_t addr) saves the loader's callee-saved registers and stack,
# calls the loaded program, and returns when it returns.  go_return is
# exported so the exception handler can resume the loader after a fault.

    .set    noreorder
    .set    noat

    .section .text
    .global  go
    .global  go_return

# void go(uint32_t addr)  — addr in $a0
go:
    la      $t0, go_save
    sw      $s0, 0($t0)
    sw      $s1, 4($t0)
    sw      $s2, 8($t0)
    sw      $s3, 12($t0)
    sw      $s4, 16($t0)
    sw      $s5, 20($t0)
    sw      $s6, 24($t0)
    sw      $s7, 28($t0)
    sw      $s8, 32($t0)
    sw      $gp, 36($t0)
    sw      $sp, 40($t0)
    sw      $ra, 44($t0)

    # Call the program; set ra = go_return so its `jr ra` comes back.
    move    $t1, $a0
    la      $ra, go_return
    jr      $t1
    nop

go_return:
    la      $t0, go_save
    lw      $s0, 0($t0)
    lw      $s1, 4($t0)
    lw      $s2, 8($t0)
    lw      $s3, 12($t0)
    lw      $s4, 16($t0)
    lw      $s5, 20($t0)
    lw      $s6, 24($t0)
    lw      $s7, 28($t0)
    lw      $s8, 32($t0)
    lw      $gp, 36($t0)
    lw      $sp, 40($t0)
    lw      $ra, 44($t0)
    jr      $ra
    nop

    .section .bss
    .align  4
# Exported so the crash handler can tell whether a context was ever saved:
# word 10 (offset 40) is the loader $sp, and it is zero until go() first runs.
    .global go_save
go_save:
    .space  48
