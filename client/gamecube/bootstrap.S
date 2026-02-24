# client/gamecube/bootstrap.S - Two-stage bootstrap for kosload (GameCube)
#
# Loads at a safe high address (0x81300000) so that Swiss and other
# DOL loaders can copy it without memory conflicts.  Copies the
# embedded kosload binary down to 0x80003100, flushes caches, and
# jumps to the loader entry point.
#
# The kosload .bin is linked in as a binary blob via objcopy,
# producing symbols:
#   _binary_loader_payload_bin_start
#   _binary_loader_payload_bin_end

    .extern _binary_loader_payload_bin_start
    .extern _binary_loader_payload_bin_end

    .section .text.bootstrap
    .global start

start:
    # Disable interrupts, enable FPU, keep address translation ON.
    # The IPL/bootrom BATs must remain active for virtual addresses
    # (0x80xxxxxx, 0xCCxxxxxx) to map to physical memory.
    # MSR: FP=1 (bit 18), IR=1 (bit 26), DR=1 (bit 27)
    li      r0, 0x2030
    mtmsr   r0
    isync

    # Source: embedded loader binary (in bootstrap address space)
    lis     r3, _binary_loader_payload_bin_start@ha
    addi    r3, r3, _binary_loader_payload_bin_start@l

    # Destination: loader base address
    lis     r4, 0x8000
    ori     r4, r4, 0x3100

    # Calculate size in bytes
    lis     r5, _binary_loader_payload_bin_end@ha
    addi    r5, r5, _binary_loader_payload_bin_end@l
    sub     r5, r5, r3

    # Save size and dest for cache flush later
    mr      r7, r5      # r7 = size
    mr      r8, r4      # r8 = dest (0x80003100)

    # Word-aligned copy (size is always aligned from objcopy)
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
    # so the CPU sees the freshly-copied code.
    mr      r3, r8          # r3 = dest start (0x80003100)
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
    mtctr   r8              # r8 = 0x80003100
    bctr
