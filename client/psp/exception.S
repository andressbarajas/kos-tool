# client/psp/exception.S — Allegrex exception entry points.
#
# The Allegrex has no fixed memory exception vector.  Handler addresses live in
# COP0 registers, which exception.c programs:
#
#   COP0 $25          general exception handler  (mtc0)
#   COP0 control $9   NMI / reset handler        (ctc0)
#
# The NMI path is two hops: hardware jumps to the 64-byte stub at 0xBFC00000
# (fixed in silicon, outside main RAM), and that stub reads control $9 and jumps
# to whatever it finds.  Both facts were reverse engineered from 6.61
# exceptionman.prx; see AGENT/psp-memory-map.md.
#
# These entries are crash handlers, not resumable ones: they capture the state,
# hand it to C for display, and stop.  Interrupts are already masked by the time
# they are installed, so no interrupt can arrive here.
#
# Nothing here may assume $sp is valid — a bad stack pointer is one of the
# faults worth reporting — so the register file is spilled to a static frame and
# a private exception stack is installed before calling C.

    .set    noreorder
    .set    noat

    .extern psp_exception_handler_c

    .section .text
    .global  psp_exception_entry
    .global  psp_nmi_entry

# Frame word indices (must match exc_frame_t in exception.c).
#define F_CAUSE     32
#define F_EPC       33
#define F_STATUS    34
#define F_BADVADDR  35
#define F_IS_NMI    36
#define F_ERROREPC  37
#define F_NMI_MASK  38
#define F_NMI_FLAGS 39
#define F_CORE_ID   40
#define F_COP0_24   41
#define F_NMI_D14   42
#define F_USB_80    43

psp_exception_entry:
    lui     $k0, %hi(psp_exc_frame)
    addiu   $k0, $k0, %lo(psp_exc_frame)
    sw      $zero, (F_IS_NMI * 4)($k0)
    sw      $zero, (F_ERROREPC * 4)($k0)
    sw      $zero, (F_NMI_MASK * 4)($k0)
    sw      $zero, (F_NMI_FLAGS * 4)($k0)
    sw      $zero, (F_NMI_D14 * 4)($k0)
    sw      $zero, (F_USB_80 * 4)($k0)
    b       exc_common
    nop

psp_nmi_entry:
    lui     $k0, %hi(psp_exc_frame)
    addiu   $k0, $k0, %lo(psp_exc_frame)
    # ErrorEPC is the authoritative NMI PC while Status.ERL=1.  Capture it
    # before either sysreg read so the essential evidence depends on no MMIO.
    mfc0    $k1, $30
    sw      $k1, (F_ERROREPC * 4)($k0)
    li      $k1, 1
    sw      $k1, (F_IS_NMI * 4)($k0)
    # Preserve Sony's source bitmap at +0 and raw +4 telemetry before C/video
    # can touch sysreg state.  The crash-only path deliberately does not ACK.
    lui     $k1, 0xBC10
    lw      $k1, 0($k1)
    sw      $k1, (F_NMI_MASK * 4)($k0)
    lui     $k1, 0xBC10
    lw      $k1, 4($k1)
    sw      $k1, (F_NMI_FLAGS * 4)($k0)
    # Source 0x02000000 is Sony's upper-bank form of NMI index 9.  Its
    # source-detail register is BC100014.  BC100080 simultaneously exposes
    # the USB connect bit and four sysreg USB causes.  Snapshot both before
    # entering C and deliberately leave every W1C/ACK register untouched.
    lui     $k1, 0xBC10
    lw      $k1, 0x14($k1)
    sw      $k1, (F_NMI_D14 * 4)($k0)
    lui     $k1, 0xBC10
    lw      $k1, 0x80($k1)
    sw      $k1, (F_USB_80 * 4)($k0)
    b       exc_common
    nop

exc_common:
    # Spill the register file.  $k0 holds the frame pointer and $k1 is scratch,
    # so both are recorded as their pre-exception values are already lost.
    sw      $at, (1 * 4)($k0)
    sw      $v0, (2 * 4)($k0)
    sw      $v1, (3 * 4)($k0)
    sw      $a0, (4 * 4)($k0)
    sw      $a1, (5 * 4)($k0)
    sw      $a2, (6 * 4)($k0)
    sw      $a3, (7 * 4)($k0)
    sw      $t0, (8 * 4)($k0)
    sw      $t1, (9 * 4)($k0)
    sw      $t2, (10 * 4)($k0)
    sw      $t3, (11 * 4)($k0)
    sw      $t4, (12 * 4)($k0)
    sw      $t5, (13 * 4)($k0)
    sw      $t6, (14 * 4)($k0)
    sw      $t7, (15 * 4)($k0)
    sw      $s0, (16 * 4)($k0)
    sw      $s1, (17 * 4)($k0)
    sw      $s2, (18 * 4)($k0)
    sw      $s3, (19 * 4)($k0)
    sw      $s4, (20 * 4)($k0)
    sw      $s5, (21 * 4)($k0)
    sw      $s6, (22 * 4)($k0)
    sw      $s7, (23 * 4)($k0)
    sw      $t8, (24 * 4)($k0)
    sw      $t9, (25 * 4)($k0)
    sw      $gp, (28 * 4)($k0)
    sw      $sp, (29 * 4)($k0)
    sw      $fp, (30 * 4)($k0)
    sw      $ra, (31 * 4)($k0)

    # COP0 state.
    mfc0    $k1, $13                # Cause
    sw      $k1, (F_CAUSE * 4)($k0)
    mfc0    $k1, $14                # EPC
    sw      $k1, (F_EPC * 4)($k0)
    mfc0    $k1, $12                # Status
    sw      $k1, (F_STATUS * 4)($k0)
    mfc0    $k1, $8                 # BadVAddr
    sw      $k1, (F_BADVADDR * 4)($k0)
    mfc0    $k1, $22                # Allegrex core identity used by Sony NMI path
    sw      $k1, (F_CORE_ID * 4)($k0)
    mfc0    $k1, $24                # Sony checks bit0 for special NMI source 16
    sw      $k1, (F_COP0_24 * 4)($k0)

    # Private stack and $gp, then report.  Never returns.
    lui     $sp, %hi(psp_exc_stack_top)
    addiu   $sp, $sp, %lo(psp_exc_stack_top)
    lui     $gp, %hi(_gp)
    addiu   $gp, $gp, %lo(_gp)
    addiu   $sp, $sp, -32
    lui     $k1, %hi(psp_exception_handler_c)
    addiu   $k1, $k1, %lo(psp_exception_handler_c)
    jalr    $k1
    nop

exc_halt:
    b       exc_halt
    nop

    .section .bss
    .align  4
    .global psp_exc_frame
psp_exc_frame:
    .space  (F_USB_80 + 1) * 4

    .align  4
psp_exc_stack:
    .space  4096
psp_exc_stack_top:
