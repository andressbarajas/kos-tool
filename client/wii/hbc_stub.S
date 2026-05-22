/* client/wii/hbc_stub.S
 *
 * Homebrew Channel launch stub for wii-load-ip.
 *
 * HBC validates a DOL's section load addresses and only accepts the
 * standard low-MEM1 homebrew area (it reserves the top of MEM1 for
 * itself + the framebuffer).  The real loader links high
 * (WII_LOADER_BASE) so uploaded guests at the normal base can't
 * overwrite it — which HBC refuses to launch directly.
 *
 * So the DOL handed to HBC is this tiny stub, linked at WII_HBC_BASE
 * (HBC-valid).  It carries the real loader image as an appended blob,
 * relocates it up to WII_LOADER_BASE, makes it I-cache coherent, and
 * jumps.  Once HBC transfers control we own the machine and may live
 * anywhere — same idea as the PS2 -F handoff trampoline.
 *
 * Pure PowerPC asm; no external dependencies, no stack, no relocations needed.
 * MSR / cache-flush idioms mirror client/gamecube/crt0.S + cache.c
 * (32-byte cache lines on Broadway/750).
 */

    .section .text.stub,"ax",@progbits
    .globl  start
start:
    /* Known-good MSR: FP=1, IR=1, DR=1, EE=0 (matches gamecube/crt0.S). */
    li      0, 0x2030
    mtmsr   0
    isync

    /* r3=src(blob start), r4=src end, r5=dst (WII_LOADER_BASE),
       r6=dst start (kept for the cache pass). */
    lis     3, __blob_start@h
    ori     3, 3, __blob_start@l
    lis     4, __blob_end@h
    ori     4, 4, __blob_end@l
    lis     5, WII_LOADER_BASE@h
    ori     5, 5, WII_LOADER_BASE@l
    mr      6, 5

    /* Word copy blob -> WII_LOADER_BASE (no overlap: stub is in low
       MEM1, dst is high MEM1). */
1:  cmplw   3, 4
    bge     2f
    lwz     0, 0(3)
    stw     0, 0(5)
    addi    3, 3, 4
    addi    5, 5, 4
    b       1b
2:
    /* Cache-coherency over [r6, r8), r8 = dst end rounded up to a
       32-byte line.  dcbf (write back + invalidate) then icbi, with
       the sync/isync barriers cache.c uses. */
    addi    8, 5, 31
    rlwinm  8, 8, 0, 0, 26          /* clear low 5 bits -> & ~31 */

    mr      7, 6
3:  cmplw   7, 8
    bge     4f
    dcbf    0, 7
    addi    7, 7, 32
    b       3b
4:  sync

    mr      7, 6
5:  cmplw   7, 8
    bge     6f
    icbi    0, 7
    addi    7, 7, 32
    b       5b
6:  sync
    isync

    /* Enter the real loader at WII_LOADER_BASE. */
    lis     0, WII_LOADER_BASE@h
    ori     0, 0, WII_LOADER_BASE@l
    mtctr   0
    bctr

    /* Embedded real loader image (Stage 2, linked at WII_LOADER_BASE).
       Resolved from the build dir via -I (see Makefile). */
    .section .blob,"a",@progbits
    .balign 32
    .globl  __blob_start
__blob_start:
    .incbin "wii-load-ip.bin"
    .globl  __blob_end
__blob_end:
    .balign 32
