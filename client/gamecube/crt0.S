# client/gamecube/crt0.S for kosload (GameCube)
#
# PowerPC 750 (Gekko) runtime startup.
# Sets up MSR, clears BSS, initializes stack and FPU, calls main().
# Also provides the kosloadsyscall entry point. The syscall jump
# table is defined in C (syscall_table.c) for easy extension.

    .extern main
    .extern __bss_start
    .extern _end
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
    .global start
    .global _atexit
    .global kosloadsyscall

start:
    b       realstart

# Magic value for detecting kosload (at entry+0x04).
# Also serves as console flag: gc_set_console_enabled() overwrites
# this to 0xdeadbeef (enabled) or 0xfeedface (disabled).
# Layout matches DC: magic at base+4, syscall ptr at base+8.
kosloadmagic:
    .long   0xdeadbeef

# Syscall entry pointer (entry+0x08)
kosload_syscall_ptr:
    .long   kosloadsyscall

# Pointer to kosload info block (entry+0x0C)
kosload_info_ptr:
    .long   kosload_info

# Function pointers for exception handler (entry+0x10, +0x14, +0x18, +0x1C, +0x20)
setup_video_ptr:
    .long   setup_video
clear_screen_ptr:
    .long   clear_screen
draw_string_ptr:
    .long   draw_string
uint_to_string_ptr:
    .long   uint_to_string
exc_to_string_ptr:
    .long   exception_code_to_string

# End of kosload hardcoded header

realstart:
    # Disable interrupts, enable FPU, keep address translation ON.
    # MSR: FP=1 (bit 18), IR=1 (bit 26), DR=1 (bit 27)
    #
    # BATs set up by the IPL/bootrom map:
    #   0x80000000 -> physical 0x00000000 (cached MEM1)
    #   0xC0000000 -> physical 0x00000000 (uncached MEM1)
    #   0xCC000000 -> physical 0x0C000000 (hardware registers)
    # Without the MMU, these virtual addresses have no valid physical
    # mapping and any access causes a bus error.
    li      r0, 0x2030
    mtmsr   r0
    isync

    # Set up stack pointer
    lis     r1, _stack@ha
    addi    r1, r1, _stack@l
    # Ensure 8-byte alignment (EABI requirement)
    clrrwi  r1, r1, 3

    # Clear BSS
    lis     r3, __bss_start@ha
    addi    r3, r3, __bss_start@l
    lis     r4, _end@ha
    addi    r4, r4, _end@l
    li      r0, 0
    cmplw   r3, r4
    bge     .Lbss_done
.Lbss_loop:
    stw     r0, 0(r3)
    addi    r3, r3, 4
    cmplw   r3, r4
    blt     .Lbss_loop
.Lbss_done:

    # Initialize FPSCR: round to nearest, no exceptions
    mtfsfi  0, 0
    mtfsfi  1, 0
    mtfsfi  2, 0
    mtfsfi  3, 0
    mtfsfi  4, 0
    mtfsfi  5, 0
    mtfsfi  6, 0
    mtfsfi  7, 0

    # Create initial stack frame (back chain = 0)
    li      r0, 0
    stwu    r0, -16(r1)

    # Call main()
    bl      main

    # Bootloader should never exit, but loop back if it does
    b       realstart

_atexit:
    blr

# ===== Kosload syscall entry point =====
# r3 = syscall number, r4 = arg1, r5 = arg2, r6 = arg3

    .align 2
kosloadsyscall:
    # Check magic value at kosloadmagic
    lis     r11, kosloadmagic@ha
    lwz     r11, kosloadmagic@l(r11)
    lis     r12, 0xdead
    ori     r12, r12, 0xbeef
    cmplw   r11, r12
    bne     .Lbadsyscall

    # Validate syscall number (must be < count)
    lis     r11, kosload_syscall_count@ha
    lwz     r11, kosload_syscall_count@l(r11)
    cmplw   r3, r11
    bge     .Lbadsyscall

    # Load function pointer from C table
    lis     r11, kosload_syscall_table@ha
    addi    r11, r11, kosload_syscall_table@l
    slwi    r0, r3, 2
    lwzx    r12, r11, r0

    # Shift arguments: r4->r3, r5->r4, r6->r5
    mr      r3, r4
    mr      r4, r5
    mr      r5, r6

    # Jump to syscall handler
    mtctr   r12
    bctr

.Lbadsyscall:
    li      r3, -1
    blr
