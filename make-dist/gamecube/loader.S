# make-cd/gamecube/loader.S - Bootstrap for kosload (GameCube disc boot)
#
# Based on the Dreamcast loader approach (make-cd/dreamcast/loader.S).
#
# This code is loaded by the GameCube IPL/apploader from disc to 0x80003100.
# It copies the embedded kosload binary to high memory (GC_LOADER_BASE),
# flushes caches, and jumps to the loader entry point.
#
# Build with: powerpc-eabi-gcc -DGC_LOADER_BASE=0x... -nostartfiles
#             -nostdlib -T loader.ld -Wa,-I<bindir>,-mregnames loader.S -o loader.elf
#
# The binary is included via .incbin and must be named
# "kosload.bin" in the assembler include path.

    .section .text.start
    .global start

start:
    # Disable interrupts, enable FPU, keep address translation ON.
    # The IPL/bootrom BATs must remain active for virtual addresses
    # (0x80xxxxxx, 0xCCxxxxxx) to map to physical memory.
    # MSR: FP=1 (bit 18), IR=1 (bit 26), DR=1 (bit 27)
    li      r0, 0x2030
    mtmsr   r0
    isync

    # Source: embedded kosload binary
    lis     r3, kosload@ha
    addi    r3, r3, kosload@l

    # Destination: GC_LOADER_BASE (top of MEM1)
    lis     r4, (GC_LOADER_BASE >> 16)
    ori     r4, r4, (GC_LOADER_BASE & 0xFFFF)

    # Size in bytes
    lis     r5, kosload_size@ha
    lwz     r5, kosload_size@l(r5)

    # Save dest and size for cache flush
    mr      r7, r5      # r7 = size
    mr      r8, r4      # r8 = dest

    # Word-aligned copy (size is always 4-byte aligned from objcopy)
    addi    r5, r5, 3
    srwi    r5, r5, 2   # size in words
    mtctr   r5

.Lcopy:
    lwz     r0, 0(r3)
    stw     r0, 0(r4)
    addi    r3, r3, 4
    addi    r4, r4, 4
    bdnz    .Lcopy

    # Flush D-cache and invalidate I-cache over destination range
    mr      r3, r8
    addi    r5, r7, 31
    srwi    r5, r5, 5       # number of 32-byte cache lines
    mtctr   r5

.Lflush:
    dcbf    0, r3
    icbi    0, r3
    addi    r3, r3, 32
    bdnz    .Lflush
    sync
    isync

    # Jump to loader entry point
    mtctr   r8
    bctr

    .balign 4
kosload_size:
    .long   kosload_end - kosload

    # Embedded kosload binary (32-byte aligned for cache operations)
    .section .rodata
    .balign 32
kosload:
    .incbin "kosload.bin"
    .balign 4
kosload_end:
