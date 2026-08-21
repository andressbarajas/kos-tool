/*
 * Independent IA-32 ELF/XBE loader startup.
 *
 * Entry is called by the Xbox kernel. This startup takes ownership of a
 * linker-reserved stack and deliberately never returns to the kernel.
 */

    .code32

    .section .text.startup, "ax", @progbits
    .p2align 4
    .globl _start
    .extern main
    .extern __bss_start__
    .extern __bss_end__
    .extern __stack_top__
_start:
    /* Establish the flag state required by this freestanding runtime. */
    cli
    cld

    /* Establish conventional x87 and SSE control state before C can use it. */
    fninit
    ldmxcsr __startup_mxcsr

    /*
     * Switch away from the kernel call stack. The linker aligns and reserves
     * this private stack inside BSS; mask again at the compiled-code boundary.
     */
    movl $__stack_top__, %esp
    andl $-16, %esp
    xorl %ebp, %ebp

    /*
     * Clear exactly the loader BSS interval, including COMMON and the private
     * stack reservation. The separately mapped guest arena is outside these
     * bounds and is not touched.
     */
    movl $__bss_start__, %edi
    movl $__bss_end__, %ecx
    subl %edi, %ecx
    movl %ecx, %edx
    xorl %eax, %eax
    shrl $2, %ecx
    rep stosl
    movl %edx, %ecx
    andl $3, %ecx
    rep stosb

    call main

    /* A returned loader main terminates locally and never executes RET. */
.Lhalt:
    cli
    hlt
    jmp .Lhalt


    .section .rodata.startup, "a", @progbits
    .p2align 2
__startup_mxcsr:
    .long 0x00001f80
