! make-cd/dreamcast/loader.S - 1st_read bootstrap for kosload (Dreamcast)
!
! Based on dcload-serial: dcload-serial/target-src/1st_read/loader.s
! (dcload-ip version is nearly identical.)
!
! This code is loaded by the Dreamcast BIOS to 0x8c010000 from disc.
! It copies the embedded exception handler to 0x8c00f400 and the
! kosload binary to 0x8c004000, then jumps to it.
!
! Build with: sh-elf-gcc -o loader.elf loader.S disable.S
!             -nostartfiles -nostdlib -Ttext=0x8c010000 -Wa,-I<bindir>
!
! The binaries are included via .incbin and must be named
! "kosload.bin" and "exception.bin" in the assembler include path.

    .text
    .balign     2
    .globl      start
start:
    ! Disable interrupts
    mov.l       init_sr, r0
    ldc         r0, sr
    mov.l       disable_cache, r4
    jsr         @r4
     nop

    ! Clear the entire kosload region
    mov.l       kosload_base, r2
    mov.l       kosload_max_sz, r1
    mov         #0, r0
cl_loop:
    dt          r1
    mov.l       r0, @r2
    bf/s        cl_loop
     add         #4, r2

    ! Copy the exception handler to 0x8c00f400
    mov.l       exception_base, r2
    mov.l       exception_size, r1
    mov.l       exception_ptr, r0
ex_loop:
    mov.l       @r0+, r3
    dt          r1
    mov.l       r3, @r2
    bf/s        ex_loop
     add         #4, r2

    ! Copy the kosload binary to 0x8c004000
    mov.l       kosload_base, r2
    mov.l       kosload_size, r1
    mov.l       kosload_ptr, r0
copy_loop:
    mov.l       @r0+, r3
    dt          r1
    mov.l       r3, @r2
    bf/s        copy_loop
     add         #4, r2

    ! Jump to the kosload binary
    mov.l       kosload_base, r4
    jmp         @r4
     nop

    .balign     4
init_sr:
    .long       0x500000f0
exception_size:
    .long       (exception_end - exception) >> 2
exception_ptr:
    .long       exception
exception_base:
    .long       0x8c00f400
kosload_size:
    .long       (kosload_end - kosload) >> 2
kosload_ptr:
    .long       kosload
kosload_base:
    .long       0x8c004000
disable_cache:
    .long       _disable_cache
kosload_max_sz:
    .long       (0x8c010000 - 0x8c004000) >> 2

! Include the binaries here, making sure they're aligned to 4 byte boundaries
! and that they're a multiple of 4 bytes in size.
    .section    .rodata
    .balign     4
kosload:
    .incbin     "kosload.bin"
    .balign     4
kosload_end:

    .section    .rodata
    .balign     4
exception:
    .incbin     "exception.bin"
    .balign     4
exception_end:
