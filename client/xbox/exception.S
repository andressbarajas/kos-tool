/* client/xbox/exception.S — x86 CPU exception ISR stubs for the Xbox loader.
 *
 * Each stub normalizes the stack to a common frame (pushing a dummy error code
 * for the vectors that don't push one) and jumps to isr_common, which saves the
 * general registers and calls exception_handler_c() with a pointer to the
 * frame.  The C handler renders a crash screen; it does not return.
 */

    .code32
    .text
    .globl  isr_common
    .extern exception_handler_c

/* Vectors that push a hardware error code: 8, 10, 11, 12, 13, 14, 17. */
.macro ISR_ERR num
    .globl  isr_\num
isr_\num:
    pushl   $\num
    jmp     isr_common
.endm

/* Vectors that do NOT push an error code: push a 0 placeholder for a uniform
   frame, then the vector number. */
.macro ISR_NOERR num
    .globl  isr_\num
isr_\num:
    pushl   $0
    pushl   $\num
    jmp     isr_common
.endm

    ISR_NOERR 0
    ISR_NOERR 1
    ISR_NOERR 2
    ISR_NOERR 3
    ISR_NOERR 4
    ISR_NOERR 5
    ISR_NOERR 6
    ISR_NOERR 7
    ISR_ERR   8
    ISR_NOERR 9
    ISR_ERR   10
    ISR_ERR   11
    ISR_ERR   12
    ISR_ERR   13
    ISR_ERR   14
    ISR_NOERR 15
    ISR_NOERR 16
    ISR_ERR   17
    ISR_NOERR 18
    ISR_NOERR 19

/* On entry the stack holds: [vector][errcode][eip][cs][eflags].
   pusha appends [edi..eax], giving the x86_frame_t the C handler reads. */
isr_common:
    pusha
    pushl   %esp                    /* &frame -> first arg */
    call    exception_handler_c
    addl    $4, %esp
1:  cli
    hlt
    jmp     1b
