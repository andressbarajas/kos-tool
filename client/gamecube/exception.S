# client/gamecube/exception.S - PowerPC 750 (Gekko) exception handler for kosload
#
# Provides the common exception handler that saves registers and calls
# a C handler for display. Exception stubs are installed at vector
# addresses (0x80000100-0x80001700) at runtime by exception_init() in
# target.c.

    .section .text
    .global exception_common
    .global exc_save_area

# ===== Common exception handler =====
# Called from exception vector stubs. At entry:
#   r0 = exception type (vector offset, e.g. 0x300 for DSI)
#   SPRG0 = original r0
#   All other registers unchanged from exception context.

exception_common:
    # Save r3 to SPRG1 so we have a work register
    mtsprg  1, r3

    # Point r3 at the register save area
    lis     r3, exc_save_area@ha
    addi    r3, r3, exc_save_area@l

    # Save exception type (from r0)
    stw     r0, 0(r3)          # [0] exception type

    # Save SRR0 and SRR1
    mfsrr0  r0
    stw     r0, 4(r3)          # [4] SRR0 (PC at exception)
    mfsrr1  r0
    stw     r0, 8(r3)          # [8] SRR1 (saved MSR)

    # Save original r0 (from SPRG0)
    mfsprg  r0, 0
    stw     r0, 12(r3)         # [12] r0

    # Save r1, r2
    stw     r1, 16(r3)         # [16] r1 (SP)
    stw     r2, 20(r3)         # [20] r2

    # Save original r3 (from SPRG1)
    mfsprg  r0, 1
    stw     r0, 24(r3)         # [24] r3

    # Save r4-r31
    stw     r4,  28(r3)
    stw     r5,  32(r3)
    stw     r6,  36(r3)
    stw     r7,  40(r3)
    stw     r8,  44(r3)
    stw     r9,  48(r3)
    stw     r10, 52(r3)
    stw     r11, 56(r3)
    stw     r12, 60(r3)
    stw     r13, 64(r3)
    stw     r14, 68(r3)
    stw     r15, 72(r3)
    stw     r16, 76(r3)
    stw     r17, 80(r3)
    stw     r18, 84(r3)
    stw     r19, 88(r3)
    stw     r20, 92(r3)
    stw     r21, 96(r3)
    stw     r22, 100(r3)
    stw     r23, 104(r3)
    stw     r24, 108(r3)
    stw     r25, 112(r3)
    stw     r26, 116(r3)
    stw     r27, 120(r3)
    stw     r28, 124(r3)
    stw     r29, 128(r3)
    stw     r30, 132(r3)
    stw     r31, 136(r3)

    # Save SPRs
    mflr    r0
    stw     r0, 140(r3)        # [140] LR
    mfctr   r0
    stw     r0, 144(r3)        # [144] CTR
    mfxer   r0
    stw     r0, 148(r3)        # [148] XER
    mfcr    r0
    stw     r0, 152(r3)        # [152] CR
    mfdsisr r0
    stw     r0, 156(r3)        # [156] DSISR
    mfdar   r0
    stw     r0, 160(r3)        # [160] DAR

    # Set up a fresh stack for the C handler
    lis     r1, exc_stack_top@ha
    addi    r1, r1, exc_stack_top@l
    stwu    r1, -16(r1)

    # Enable FPU in MSR so we can save FPRs and call C code
    li      r0, 0x2000
    mtmsr   r0
    isync

    # Save FPU state: FPSCR + f0-f31 (264 bytes at offset 164)
    # Must save f0 first since mffs clobbers it
    stfd    f0,  172(r3)
    mffs    f0
    stfd    f0,  164(r3)       # FPSCR (8 bytes, low word = FPSCR bits)
    stfd    f1,  180(r3)
    stfd    f2,  188(r3)
    stfd    f3,  196(r3)
    stfd    f4,  204(r3)
    stfd    f5,  212(r3)
    stfd    f6,  220(r3)
    stfd    f7,  228(r3)
    stfd    f8,  236(r3)
    stfd    f9,  244(r3)
    stfd    f10, 252(r3)
    stfd    f11, 260(r3)
    stfd    f12, 268(r3)
    stfd    f13, 276(r3)
    stfd    f14, 284(r3)
    stfd    f15, 292(r3)
    stfd    f16, 300(r3)
    stfd    f17, 308(r3)
    stfd    f18, 316(r3)
    stfd    f19, 324(r3)
    stfd    f20, 332(r3)
    stfd    f21, 340(r3)
    stfd    f22, 348(r3)
    stfd    f23, 356(r3)
    stfd    f24, 364(r3)
    stfd    f25, 372(r3)
    stfd    f26, 380(r3)
    stfd    f27, 388(r3)
    stfd    f28, 396(r3)
    stfd    f29, 404(r3)
    stfd    f30, 412(r3)
    stfd    f31, 420(r3)

    # Call C exception handler: exception_handler_c(type, srr0, save_area)
    lwz     r3, 0(r3)          # r3 = exception type
    lis     r5, exc_save_area@ha
    addi    r5, r5, exc_save_area@l
    lwz     r4, 4(r5)          # r4 = SRR0 (PC)
    # r5 = save area pointer

    .extern exception_handler_c
    bl      exception_handler_c

    # Reboot back to kosload
    .extern start
    lis     r3, start@ha
    addi    r3, r3, start@l
    mtctr   r3
    bctr

# ===== Data =====

    .section .bss
    .align 4

# Register save area: 428 bytes (GPRs + SPRs + FPRs)
exc_save_area:
    .space  428

# Exception handler stack (1KB)
    .space  1024
exc_stack_top:
