# client/playstation2/exception.S - MIPS R5900 (EE) exception handler for kosload
#
# Provides the common exception handler that saves all registers and calls
# a C handler for display/reporting. Exception stubs are installed at vector
# addresses (0x80000000, 0x80000100, 0x80000180, 0x80000200) at runtime by
# exception_init() in target.c.
#
# Save area layout (416 bytes, 16-byte aligned):
#   Offset   Size   Contents
#   0x000    256    GPRs r0-r31 (32 * 8 bytes, low 64 bits via sd)
#   0x100     4     COP0 EPC ($14)
#   0x104     4     COP0 Status ($12)
#   0x108     4     COP0 Cause ($13)
#   0x10C     4     COP0 BadVAddr ($8)
#   0x110   128     FPU f0-f31 (32 * 4 bytes via swc1)
#   0x190     4     FPU FCR31 (control/status)
#   0x194    12     padding to 416

    .set    noat
    .set    noreorder

    .extern exception_handler_c
    .extern interrupt_handler_c
    .extern _start
    .extern _stack
    .extern _gp

    .section .text
    .global exception_common

exception_common:
    # At entry: k0 and k1 are available (not saved by hardware).
    # We use k0 as base pointer to the save area.

    la      $k0, exception_save_area

    # Distinguish interrupt vs exception via Cause.ExcCode (bits 6:2).
    # ExcCode == 0 means hardware interrupt; everything else (TLB
    # refill, address error, syscall, breakpoint, RI, FPE, etc.) is
    # a fatal exception we want to display + restart for.
    mfc0    $k1, $13                # Cause
    andi    $k1, $k1, 0x7C
    beqz    $k1, interrupt_fast_path
    nop

    # Save k1 first (it may have been clobbered by the stub)
    sd      $k1, 0xD8($k0)     # r27 = k1, offset = 27*8 = 216

    # Now save all GPRs (64-bit low half via sd)
    # r0 ($zero) is always 0 but save for completeness
    sd      $zero,  0x00($k0)  # r0
    sd      $at,    0x08($k0)  # r1 ($at)
    sd      $v0,    0x10($k0)  # r2
    sd      $v1,    0x18($k0)  # r3
    sd      $a0,    0x20($k0)  # r4
    sd      $a1,    0x28($k0)  # r5
    sd      $a2,    0x30($k0)  # r6
    sd      $a3,    0x38($k0)  # r7
    sd      $t0,    0x40($k0)  # r8
    sd      $t1,    0x48($k0)  # r9
    sd      $t2,    0x50($k0)  # r10
    sd      $t3,    0x58($k0)  # r11
    sd      $12,    0x60($k0)  # r12 (t4 in o32)
    sd      $13,    0x68($k0)  # r13 (t5 in o32)
    sd      $14,    0x70($k0)  # r14 (t6 in o32)
    sd      $15,    0x78($k0)  # r15 (t7 in o32)
    sd      $s0,    0x80($k0)  # r16
    sd      $s1,    0x88($k0)  # r17
    sd      $s2,    0x90($k0)  # r18
    sd      $s3,    0x98($k0)  # r19
    sd      $s4,    0xA0($k0)  # r20
    sd      $s5,    0xA8($k0)  # r21
    sd      $s6,    0xB0($k0)  # r22
    sd      $s7,    0xB8($k0)  # r23
    sd      $t8,    0xC0($k0)  # r24
    sd      $t9,    0xC8($k0)  # r25
    sd      $k0,    0xD0($k0)  # r26 (k0 - our base, not original but close enough)
    # k1 already saved above at 0xD8
    sd      $gp,    0xE0($k0)  # r28
    sd      $sp,    0xE8($k0)  # r29
    sd      $fp,    0xF0($k0)  # r30 (s8/fp)
    sd      $ra,    0xF8($k0)  # r31

    # Save COP0 special registers
    mfc0    $t0, $14            # EPC
    sw      $t0, 0x100($k0)
    mfc0    $t0, $12            # Status
    sw      $t0, 0x104($k0)
    mfc0    $t0, $13            # Cause
    sw      $t0, 0x108($k0)
    mfc0    $t0, $8             # BadVAddr
    sw      $t0, 0x10C($k0)

    # Enable FPU in COP0 Status so we can save FPRs
    mfc0    $t0, $12
    lui     $t1, 0x2000         # COP1 usable bit (bit 29)
    or      $t0, $t0, $t1
    mtc0    $t0, $12
    sync.p

    # Save FPU registers f0-f31 (32-bit each via swc1)
    swc1    $f0,  0x110($k0)
    swc1    $f1,  0x114($k0)
    swc1    $f2,  0x118($k0)
    swc1    $f3,  0x11C($k0)
    swc1    $f4,  0x120($k0)
    swc1    $f5,  0x124($k0)
    swc1    $f6,  0x128($k0)
    swc1    $f7,  0x12C($k0)
    swc1    $f8,  0x130($k0)
    swc1    $f9,  0x134($k0)
    swc1    $f10, 0x138($k0)
    swc1    $f11, 0x13C($k0)
    swc1    $f12, 0x140($k0)
    swc1    $f13, 0x144($k0)
    swc1    $f14, 0x148($k0)
    swc1    $f15, 0x14C($k0)
    swc1    $f16, 0x150($k0)
    swc1    $f17, 0x154($k0)
    swc1    $f18, 0x158($k0)
    swc1    $f19, 0x15C($k0)
    swc1    $f20, 0x160($k0)
    swc1    $f21, 0x164($k0)
    swc1    $f22, 0x168($k0)
    swc1    $f23, 0x16C($k0)
    swc1    $f24, 0x170($k0)
    swc1    $f25, 0x174($k0)
    swc1    $f26, 0x178($k0)
    swc1    $f27, 0x17C($k0)
    swc1    $f28, 0x180($k0)
    swc1    $f29, 0x184($k0)
    swc1    $f30, 0x188($k0)
    swc1    $f31, 0x18C($k0)

    # Save FPU control/status register (FCR31)
    cfc1    $t0, $31
    sw      $t0, 0x190($k0)

    # Set up a fresh stack for the C handler
    la      $sp, _stack
    li      $t0, ~0xF
    and     $sp, $sp, $t0
    addiu   $sp, $sp, -16

    # Load our global pointer
    la      $gp, _gp

    # Call exception_handler_c(cause, epc, save_area_ptr)
    # a0 = COP0 Cause
    mfc0    $a0, $13
    # a1 = COP0 EPC
    mfc0    $a1, $14
    # a2 = pointer to save area
    la      $a2, exception_save_area

    jal     exception_handler_c
    nop

    # After handler returns, restart the loader
    la      $t0, _start
    jr      $t0
    nop

# =============================================================
# Interrupt fast-path
# =============================================================
#
# Reached via the test in exception_common when Cause.ExcCode==0.
# Saves only what we need (caller-saves + a few callee-saves we
# actually use), runs the C handler, restores, and returns via
# ERET so the interrupted code continues.  Does NOT save FPU state
# — interrupt_handler_c doesn't touch the FPU.
#
# At entry: k0 = &exception_save_area; k1 was clobbered by check
# above and we don't need its original value.
#
# The save-area layout is reused from exception_common — but the
# two paths are mutually exclusive (fatal restarts; interrupt
# returns), so reuse is fine.

interrupt_fast_path:
    # Save GPRs we (or the C handler / its callees) might touch.
    sd      $at,    0x008($k0)
    sd      $v0,    0x010($k0)
    sd      $v1,    0x018($k0)
    sd      $a0,    0x020($k0)
    sd      $a1,    0x028($k0)
    sd      $a2,    0x030($k0)
    sd      $a3,    0x038($k0)
    sd      $t0,    0x040($k0)
    sd      $t1,    0x048($k0)
    sd      $t2,    0x050($k0)
    sd      $t3,    0x058($k0)
    sd      $12,    0x060($k0)
    sd      $13,    0x068($k0)
    sd      $14,    0x070($k0)
    sd      $15,    0x078($k0)
    sd      $t8,    0x0C0($k0)
    sd      $t9,    0x0C8($k0)
    sd      $gp,    0x0E0($k0)
    sd      $sp,    0x0E8($k0)
    sd      $ra,    0x0F8($k0)

    # Save EPC + Status (we'll need them for ERET)
    mfc0    $t0, $14
    sw      $t0, 0x100($k0)
    mfc0    $t0, $12
    sw      $t0, 0x104($k0)

    # Set up gp.  Reserve 512 bytes BELOW the interrupted code's $sp
    # for the C handler's frame.  This is critical: we must NOT
    # reset $sp to _stack (the global stack top) — the interrupted
    # code's locals live between its $sp and _stack, and our handler
    # frame would overwrite them.  Reserving below the interrupted
    # $sp means our frame uses memory the interrupted code wasn't
    # touching.
    la      $gp, _gp
    addiu   $sp, $sp, -512
    li      $t0, ~0xF
    and     $sp, $sp, $t0

    # Call interrupt_handler_c() — no args needed for step 1.
    jal     interrupt_handler_c
    nop

    # ----- Return path -----
    la      $k0, exception_save_area

    # Restore COP0 Status (still EXL=1 from interrupt entry; ERET
    # clears EXL atomically).  We don't change Status here.
    lw      $t0, 0x100($k0)
    mtc0    $t0, $14            # EPC

    # Restore GPRs
    ld      $at,    0x008($k0)
    ld      $v0,    0x010($k0)
    ld      $v1,    0x018($k0)
    ld      $a0,    0x020($k0)
    ld      $a1,    0x028($k0)
    ld      $a2,    0x030($k0)
    ld      $a3,    0x038($k0)
    ld      $t1,    0x048($k0)
    ld      $t2,    0x050($k0)
    ld      $t3,    0x058($k0)
    ld      $12,    0x060($k0)
    ld      $13,    0x068($k0)
    ld      $14,    0x070($k0)
    ld      $15,    0x078($k0)
    ld      $t8,    0x0C0($k0)
    ld      $t9,    0x0C8($k0)
    ld      $gp,    0x0E0($k0)
    ld      $sp,    0x0E8($k0)
    ld      $ra,    0x0F8($k0)

    # Restore $t0 last (used as scratch above)
    ld      $t0,    0x040($k0)

    # ERET: clears EXL, branches to EPC, allows next interrupt
    sync.p
    eret
    nop

# ===== Data =====

    .section .bss
    .align  4

# Register save area: 416 bytes (GPRs + COP0 + FPRs + FCR31 + pad)
# Must be 16-byte aligned for sd instructions
exception_save_area:
    .space  416
