/* client/wii/bootstrap.S
 *
 * Unified channel + Homebrew Channel boot stub for wii-load-ip.
 *
 * Architecture: ONE channel DOL launches from BOTH the System Menu (channel
 * WAD) and the Homebrew Channel (wiiload network upload).  Two loadable DOL
 * sections (matching pink's known-working shape + HBC's real channel-boot
 * DOL):
 *
 *   TEXT0 @ 0x80003400  this stub (bring-up + runtime copy)
 *   TEXT1 @ 0x80004000  the embedded loader image, .blob (SM loads it here)
 *
 * The DOL entry is the HIGH alias 0x80003400 (NOT the low alias 0x00003400 as
 * earlier iterations had it).  Reason:
 *
 *   - HBC's wiiload path ends in `loader_exec` ->`exec_stub`, which sits at
 *     the top of MEM2 and does `mtctr r3; bctr` to the DOL entry in VIRTUAL
 *     mode with libogc-standard BATs already installed (BAT0 cached MEM1,
 *     BAT4 cached MEM2, DBAT1 uncached hw, DBAT5 uncached MEM2).  A low-alias
 *     entry like 0x00003400 has no BAT match here -> ISI fault, cascading
 *     into the garbage-PC DSI we were chasing.  A high-alias entry lands
 *     under BAT0 and the first instruction fetches cleanly from phys 0x3400.
 *
 *   - The SM/IOS PPC channel launcher accepts high-alias entries (HBC's own
 *     channel uses __mem1_entry=0x80004000 high alias, see
 *     hbc/wiipax/stub/channel.ld) -- the high bit is dropped by the launcher
 *     before the real-mode entry, so a 0x80003400 entry resolves to physical
 *     0x3400 either way.
 *
 * Bring-up sequence -- cloned from HBC's channel crt0
 * (hbc/channel/channelapp/stub/crt0.S):
 *
 *   1. `b startup` first, so DOL-inspection tools see a sane head.
 *   2. Disable interrupts (works in either mode).
 *   3. Use `clrlwi r3, r3, 2` to compute the LOW alias of a real-mode landing
 *      label, then `rfi` with SRR0=that low alias and SRR1=MSR&~(DR|IR).
 *      This is the magic that lets the SAME entry block work whether we
 *      entered in real mode (SM path: stripped to low alias) or virtual mode
 *      (HBC path: BATs active at high alias) -- the rfi atomically changes
 *      PC and MSR together, so we never have an instant where MSR is real
 *      but PC is a high virtual address (= bus error).
 *   4. In real mode at the low alias: HID0 caches, invalidate all 8 BAT pairs
 *      and SR0..15, install canonical BAT layout, set HID4 SBE.
 *   5. `rfi` back to virtual mode at the HIGH alias of `post_bringup` under
 *      our BATs.
 *   6. Copy the embedded loader from 0x80004000 to WII_LOADER_BASE
 *      (0x81330000), dcbf+icbi the destination, `bctr` to WII_LOADER_BASE.
 *
 * The loader at WII_LOADER_BASE has the standard kosload header at base+0 /
 * +4 / +8 (b realstart / 0xdeadbeef / kosloadsyscall pointer), so guests
 * find VEC_KOSLOAD at WII_LOADER_BASE+0x08.
 */

    .section .text.stub,"ax",@progbits
    .globl  start
start:
    b       startup

    /* The HBC channel crt0 puts a "STUB"/"HAXX"/0 marker here (channelapp's
     * "this is a stub, don't treat me as a real loader" signature).  We keep
     * the same pad so byte-level DOL diffing against HBC produces sane
     * output -- the marker itself isn't load-bearing for our code. */
    .ascii  "STUB"
    .ascii  "HAXX"
    .long   0

startup:
    /* Disable interrupts (preserve current mode -- works whether we entered
     * in real or virtual). */
    mfmsr   3
    rlwinm  4, 3, 0, 17, 15
    rlwinm  4, 4, 0, 26, 24
    mtmsr   4

    /* Compute the LOW alias of `real_mode` and stage an rfi that drops to
     * real mode at that low alias.  The `clrlwi r3, r3, 2` is the trick:
     *   - If we entered in virtual mode at a high alias (HBC path), it
     *     strips 0x80000000 -> SRR0 = phys-equivalent low alias.
     *   - If we entered in real mode at a low alias (SM path), clrlwi is a
     *     no-op (top bits already zero) -- SRR0 unchanged.
     * Either way, rfi lands at the low alias with MSR & ~(DR|IR), which is
     * the only place a real-mode PC is valid in MEM1.  An `mtmsr` direct
     * drop instead of rfi would fault the very next instruction fetch if
     * we'd come in at a high alias. */
    isync
    lis     3, real_mode@h
    ori     3, 3, real_mode@l
    clrlwi  3, 3, 2             /* strip top 2 bits -> low alias */
    mtspr   26, 3               /* SRR0 = low_alias(real_mode) */
    mfmsr   3
    li      4, 0x30
    andc    3, 3, 4             /* MSR & ~(DR | IR) */
    mtspr   27, 3               /* SRR1 = MSR without translation */
    rfi

real_mode:
    /* Now in real mode at the LOW alias of this label.  Real-mode PC is
     * physical, so PC matches a valid MEM1 offset and fetches work without
     * an MMU. */

    /* HID0 = 0x00110c64: bus checkstops off, sleep modes off, caches off
     * (we'll re-enable), invalidate I+D caches, store gathering off, enable
     * D-cache flush assist + branch target cache + branch history table. */
    lis     3, 0x0011
    ori     3, 3, 0x0c64
    mtspr   1008, 3
    isync

    /* HID0 |= 0xc000: re-enable I and D caches. */
    ori     3, 3, 0xc000
    mtspr   1008, 3
    isync

    /* Invalidate ALL 8 BAT pairs (upper halves = 0). */
    li      0, 0
    mtibatu 0, 0
    mtibatu 1, 0
    mtibatu 2, 0
    mtibatu 3, 0
    mtdbatu 0, 0
    mtdbatu 1, 0
    mtdbatu 2, 0
    mtdbatu 3, 0
    mtspr   560, 0              /* IBAT4U */
    mtspr   562, 0              /* IBAT5U */
    mtspr   564, 0              /* IBAT6U */
    mtspr   566, 0              /* IBAT7U */
    mtspr   568, 0              /* DBAT4U */
    mtspr   570, 0              /* DBAT5U */
    mtspr   572, 0              /* DBAT6U */
    mtspr   574, 0              /* DBAT7U */
    isync

    /* Segment registers SR0-15 = 0x80000000 (libogc-standard). */
    lis     0, 0x8000
    mtsr    0, 0
    mtsr    1, 0
    mtsr    2, 0
    mtsr    3, 0
    mtsr    4, 0
    mtsr    5, 0
    mtsr    6, 0
    mtsr    7, 0
    mtsr    8, 0
    mtsr    9, 0
    mtsr    10, 0
    mtsr    11, 0
    mtsr    12, 0
    mtsr    13, 0
    mtsr    14, 0
    mtsr    15, 0
    isync

    /* BAT0 (D+I): 0x80000000, 256 MB -> phys 0x00000000 cached (MEM1). */
    li      3, 2                /* BATL = 0x00000002 (PP=10 R/W) */
    lis     4, 0x8000
    ori     4, 4, 0x1fff        /* BATU = 0x80001fff */
    mtibatl 0, 3
    mtibatu 0, 4
    mtdbatl 0, 3
    mtdbatu 0, 4
    isync

    /* BAT4 (D+I): 0x90000000 -> phys 0x10000000, cached (MEM2). */
    addis   3, 3, 0x1000        /* -> 0x10000002 */
    addis   4, 4, 0x1000        /* -> 0x90001fff */
    mtspr   561, 3              /* IBAT4L */
    mtspr   560, 4              /* IBAT4U */
    mtspr   569, 3              /* DBAT4L */
    mtspr   568, 4              /* DBAT4U */
    isync

    /* DBAT1: 0xC0000000, 256 MB -> phys 0x00000000, uncached/guarded
     * (covers VI 0xCC.., AVE GPIO 0xCD.., XFB 0xC005..). */
    li      3, 42               /* BATL = 0x0000002a (WIMG=0101 = I+G) */
    lis     4, 0xc000
    ori     4, 4, 0x1fff        /* BATU = 0xc0001fff */
    mtdbatl 1, 3
    mtdbatu 1, 4
    isync

    /* DBAT5: 0xD0000000 -> phys 0x10000000, uncached (MEM2 uncached). */
    addis   3, 3, 0x1000        /* -> 0x1000002a */
    addis   4, 4, 0x1000        /* -> 0xd0001fff */
    mtspr   571, 3              /* DBAT5L */
    mtspr   570, 4              /* DBAT5U */
    isync

    /* HID4 = H4A | SBE: activate the extended BATs (BAT4-7). */
    lis     3, 0x8200
    mtspr   1011, 3
    isync

    /* rfi to post_bringup at HIGH alias with translation re-enabled.
     * Atomically transitions to virtual mode under OUR BATs (no dependency
     * on whatever the SM or HBC left behind). */
    lis     3, post_bringup@h
    ori     3, 3, post_bringup@l
    mtspr   26, 3               /* SRR0 = virtual post_bringup */
    mfmsr   3
    ori     3, 3, 0x30          /* MSR |= IR | DR */
    mtspr   27, 3               /* SRR1 */
    rfi

post_bringup:
    /* Copy the embedded loader (TEXT1 @ 0x80004000) to WII_LOADER_BASE.
     * r3 = src (blob start), r4 = src end, r5 = dst, r6 = dst start. */
    lis     3, __blob_start@h
    ori     3, 3, __blob_start@l
    lis     4, __blob_end@h
    ori     4, 4, __blob_end@l
    lis     5, WII_LOADER_BASE@h
    ori     5, 5, WII_LOADER_BASE@l
    mr      6, 5

1:  cmplw   3, 4
    bge     2f
    lwz     0, 0(3)
    stw     0, 0(5)
    addi    3, 3, 4
    addi    5, 5, 4
    b       1b
2:

    /* Cache coherency over [r6, r8).  r8 = dst end rounded up to a 32-byte
     * cache line.  dcbf so the copy reaches RAM; icbi so the I-cache picks
     * up the new instructions. */
    addi    8, 5, 31
    rlwinm  8, 8, 0, 0, 26      /* & ~31 */

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

    /* Enter the loader at WII_LOADER_BASE.  Its first instruction is the
     * kosload header's `b realstart`, which skips the magic/ptrs at
     * +4..+0x20 and runs the standard MSR/stack/BSS/FPU/main bring-up. */
    lis     0, WII_LOADER_BASE@h
    ori     0, 0, WII_LOADER_BASE@l
    mtctr   0
    bctr

    /* Embedded loader image (Stage 2).  The linker script gives this blob
     * its own PT_LOAD segment at 0x80004000 so the final DOL has two text
     * sections -- matching pink's working shape and HBC's real boot DOL. */
    .section .blob,"ax",@progbits
    .balign 32
    .globl  __blob_start
__blob_start:
    .incbin "wii-load-ip.bin"
    .globl  __blob_end
__blob_end:
    .balign 32
