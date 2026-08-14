# client/psp/stub/stub.S — stage-1 relocating stub for psp-load-usb (Allegrex).
#
# The firmware loads this file (as a PRX) into the user partition and enters at
# _start.  Its only job is to put the real loader — carried as a payload blob,
# linked at the fixed PSP_LOADER_BASE — at that address and jump to it.
#
# WHERE STAGE 2 GOES, AND WHY THE COPY IS SAFE
#
#   PSP_LOADER_BASE is 0x08000000, the bottom megabyte of main RAM, so that
#   guest memory is one contiguous 30.94 MiB extent instead of two.  Two things
#   must therefore happen before the first low store, and the firmware is
#   already dead while stage 2 does not exist yet, so this stub does both
#   itself:
#
#     - Take COP0 $25 and control $9.  The destination overlaps the firmware's
#       exception handler bodies; IE masks interrupts, not exceptions, and NMI
#       is not maskable at all, so a fault mid-copy would otherwise execute
#       what we had just overwritten.
#     - Open the Tachyon protection table, which answers nothing in its
#       inherited state (CCCCCCCC CCCCCCCC 00000000 00000000): hold the Media
#       Engine in reset, grant all four permission words, verify every
#       readback, and purge inherited cache lines while nothing of ours is
#       down there yet.
#
#   The firmware loads this PRX in its user partition, at 0x08800000 or above,
#   so source, copier and destination are three DISJOINT intervals: copy
#   direction cannot matter and no SECOND trampoline is required.  A loader base
#   inside the user partition would put the copier in the range it overwrites;
#   mk/memory.mk rejects that at build time rather than support it.
#
#   Terminology, because it has caused confusion: this stub IS a trampoline in
#   the ordinary sense -- a relocating stub that copies a payload into place and
#   jumps to it.  What the disjointness above rules out is an EXTRA hop: having
#   to relocate the copy loop ITSELF somewhere outside the region it is
#   overwriting.  That is the thing below that must not come back.
#
#   DISJOINTNESS IS NOT ASSUMED.  mk/memory.mk keeps the loader window below
#   the user partition at build time, and the copier re-proves the intervals at
#   runtime — from its own PC and the relocated payload symbols — before the
#   first store, because a PRX's load base is the firmware's choice, not ours.
#
#   Do not resurrect the scratchpad copier hop.  An earlier revision ran the
#   copy loop itself from the scratchpad (0x00010000).  That region is writable, but
#   hardware testing showed the Allegrex will not execute from it: a diagnostic
#   build that merely jumped there and spun killed the firmware, while an
#   otherwise identical build stopping one instruction earlier ran fine.

    .set    noreorder
    .set    noat

# The stage-2 image.  PAYLOAD_FILE is the raw binary of
# client/psp/build/usb/psp-load-usb.bin, already linked at the fixed
# PSP_LOADER_BASE.  It rides here as plain data so the firmware never relocates
# it: the copier below moves it to that address verbatim, which is what keeps
# the loader's guest-facing header at a compile-time address for the examples.
#
# Its own section, so the linker script can give it 64-byte alignment and
# place it ahead of .text.
    .section .rodata.payload, "a"
    .align  6
    .global kosload_payload
kosload_payload:
    .incbin PAYLOAD_FILE
    .global kosload_payload_end
kosload_payload_end:
    .align  4

# NB this file goes through cpp: a comment whose first word after "#" happens to
# be a directive name gets parsed as one.  Keep comment text indented past it.

    .section .text.stub, "ax"
    .global  _start

_start:

    /* Production takeover.  Stop the firmware before touching its Syscon bus:
     * its power worker otherwise races the raw SSP transaction and can re-arm
     * the Tachyon watchdog after we disable it.  This is the exact ordering
     * proven on PSP-1000 hardware.  Never restore IE;
     * stage 2 inherits exclusive hardware ownership through crt0.
     *
     * A failed disable is terminal.  Continuing would hand control to a loader
     * that Syscon will deliberately power off roughly ten seconds later. */
    .extern psp_syscon_init
    .extern psp_syscon_wdt_disable
    mfc0    $t0, $12
    li      $t1, -2
    and     $t0, $t0, $t1
    mtc0    $t0, $12
    nop
    nop
    jal     psp_syscon_init
    nop
    jal     psp_syscon_wdt_disable
    nop
    beq     $v0, $zero, production_wdt_ok
    nop
production_wdt_failed:
    b       production_wdt_failed
    nop
production_wdt_ok:







# ---------------------------------------------------------------------------
# STAGE-1 COPIER.  Every instruction from kosload_stage1_copier_begin to
# kosload_stage1_copier_end is needed until the jump, so this whole interval is
# what the build-time and runtime overlap proofs protect.  Do not move code out
# of it, and do not add code before it that stores to low RAM.
# ---------------------------------------------------------------------------
    .global kosload_stage1_copier_begin
    .global kosload_stage1_copier_end
kosload_stage1_copier_begin:
    # The PRX's load base is the firmware's choice, so take the copier's real
    # runtime address from the PC rather than trusting any assumed base.  $s1 is
    # callee-saved, so it survives the C calls below.
    bal     stage1_pc_here
    nop
stage1_pc_here:
    addiu   $s1, $ra, -8            # $s1 = runtime kosload_stage1_copier_begin

    # ---- open the low 8 MiB before the first store to it ------------------
    #
    # Ordering below is load-bearing:
    # IE is already masked and the watchdog already disabled above.
    #
    # 0. Take both exception vectors BEFORE touching low RAM.
    #
    #    PSP_LOADER_BASE is the bottom of the kernel partition, so the copy
    #    below lands on the firmware's exception handler *bodies*.  Masking
    #    Status.IE does not make that safe: IE masks interrupts, not
    #    exceptions, and the NMI path through control $9 is not maskable at
    #    all.  Without this, a bus or address error anywhere between the first
    #    store and stage 2's exception_init() would vector straight into memory
    #    we had just overwritten and execute it.
    #
    #    The stub we install lives in this image, in the firmware's user-
    #    partition allocation, which nothing overwrites until a guest is
    #    uploaded -- and stage 2 replaces both vectors long before that.  The
    #    address is derived from $s1 (our own PC) plus an assemble-time
    #    constant, so it needs no relocation.
    li      $t1, (stage1_exc_stub - kosload_stage1_copier_begin)
    addu    $t0, $s1, $t1
    mtc0    $t0, $25                # general exceptions
    nop
    nop
    ctc0    $t0, $9                 # NMI / reset dispatch
    nop
    nop

    # 1. Hold the Media Engine in reset.  It is a second CPU on the same RAM
    #    and its firmware image lands at 0x0839xxxx..0x083fefff, inside the
    #    range we are about to hand to stage 2 and the guest.  RMW so every
    #    other reset line is preserved; BC10004C bit 2 is the ME, identified
    #    from lowio's generic reset accessor.  Never write BC000048/BC100040.
    lui     $t0, 0xBC10
    lw      $t1, 0x4C($t0)
    ori     $t1, $t1, 0x0004
    sw      $t1, 0x4C($t0)
    sync
    lw      $t1, 0x4C($t0)
    andi    $t2, $t1, 0x0004
    beq     $t2, $zero, stage1_reclaim_failed
    nop

    # 2. Grant user+kernel read/write to all 32 chunks of the low 8 MiB.  Each
    #    store is ordered; F is Sony's own volatile-memory grant value.
    lui     $t0, 0xBC00
    li      $t1, -1
    sw      $t1, 0($t0)
    sync
    sw      $t1, 4($t0)
    sync
    sw      $t1, 8($t0)
    sync
    sw      $t1, 12($t0)
    sync

    # 3. Read all four back exactly.  Fail closed: a loader that continued here
    #    would turn one clear startup stop into a data bus error at the first
    #    payload store, with no output channel to explain it.
    lw      $t2, 0($t0)
    bne     $t2, $t1, stage1_reclaim_failed
    nop
    lw      $t2, 4($t0)
    bne     $t2, $t1, stage1_reclaim_failed
    nop
    lw      $t2, 8($t0)
    bne     $t2, $t1, stage1_reclaim_failed
    nop
    lw      $t2, 12($t0)
    bne     $t2, $t1, stage1_reclaim_failed
    nop

    # 4. Discard every inherited cached view of the low 8 MiB, through both the
    #    KU0 and K0 aliases, while nothing of ours lives there.  This is the
    #    only moment the sweep is unconditionally safe: once stage 2 has been
    #    copied to PSP_LOADER_BASE, writing back a stale firmware line would
    #    land on top of live loader code, stack, or USB buffers.  Stage 2's own
    #    sweep therefore skips the loader window (client/psp/exception.c).
    lui     $t0, 0x0800             # KU0 0x08000000..0x087FFFFF
    lui     $t3, 0x0880
stage1_purge_ku0:
    .set    push
    .set    mips32
    cache   0x1b, 0($t0)            # D: hit writeback + invalidate
    cache   0x08, 0($t0)            # I: hit invalidate
    .set    pop
    addiu   $t0, $t0, 64
    sltu    $t4, $t0, $t3
    bne     $t4, $zero, stage1_purge_ku0
    nop

    lui     $t0, 0x8800             # K0 0x88000000..0x887FFFFF
    lui     $t3, 0x8880
stage1_purge_k0:
    .set    push
    .set    mips32
    cache   0x1b, 0($t0)
    cache   0x08, 0($t0)
    .set    pop
    addiu   $t0, $t0, 64
    sltu    $t4, $t0, $t3
    bne     $t4, $zero, stage1_purge_k0
    nop
    sync


    # ---- runtime interval proof -------------------------------------------
    #
    # src    = [kosload_payload, kosload_payload_end)   relocated by the loader
    # dst    = [PSP_LOADER_BASE, PSP_LOADER_BASE + len)
    # copier = [$s1, kosload_stage1_copier_end)         from the PC, above
    #
    # The two conditions below are what make the forward copy correct.  They
    # are re-derived here rather than taken from the build, because the
    # firmware, not the build, chooses where this PRX lands:
    #
    #   dst_start <  src_start   -> a forward copy never overtakes its source
    #   dst_end   <= copier_start-> the running copier is never overwritten
    #
    # Unsigned throughout, with the wrap and size cases rejected explicitly.
    # a0 = dst, a1 = src, a2 = length (rounded up to a word), a3 = entry
    move    $t9, $zero              # dst_end: reported as 0 until computed
    li      $a0, PSP_LOADER_BASE
    la      $a1, kosload_payload
    la      $t6, kosload_payload_end

    sltu    $t0, $a1, $t6           # src_end > src_start: non-empty, no wrap
    beq     $t0, $zero, stage1_guard_failed
    nop

    subu    $a2, $t6, $a1
    addiu   $a2, $a2, 3
    li      $t7, -4
    and     $a2, $a2, $t7

    li      $t7, PSP_LOADER_SIZE    # payload must fit the reserved window; this
    sltu    $t7, $t7, $a2           # also bounds the rounding above
    bne     $t7, $zero, stage1_guard_failed
    nop

    addu    $t9, $a0, $a2           # dst_end, rejecting unsigned overflow
    sltu    $t0, $t9, $a0
    bne     $t0, $zero, stage1_guard_failed
    nop

    sltu    $t0, $a0, $a1           # dst_start < src_start
    beq     $t0, $zero, stage1_guard_failed
    nop

    sltu    $t0, $s1, $t9           # copier_start >= dst_end
    bne     $t0, $zero, stage1_guard_failed
    nop

    li      $a3, PSP_LOADER_BASE
    move    $t0, $a0                # remember dst for the cache pass
    move    $t1, $a2                # remember length


    # Forward copy, proven safe by the guard above: either the intervals are
    # disjoint, or dst trails src inside this image and the loop never reads a
    # word it has already overwritten.
    beq     $a2, $zero, copy_done
    nop
copy_loop:
    lw      $t2, 0($a1)
    sw      $t2, 0($a0)
    addiu   $a1, $a1, 4
    addiu   $a0, $a0, 4
    addiu   $a2, $a2, -4
    bne     $a2, $zero, copy_loop
    nop
copy_done:


    # Push the copied image out to RAM and drop stale instruction lines over it
    # before executing from there.  0x1b + 0x08 is the exact sequence Sony uses
# to publish CPU-written code (interruptman.prx text+0x3268); 0x14 is an INDEXED
# whole-cache op and was wrong here.
    addu    $t3, $t0, $t1
flush_loop:
    .set    push
    .set    mips32
    cache   0x1b, 0($t0)            # D: hit writeback + invalidate
    cache   0x08, 0($t0)            # I: hit invalidate
    .set    pop
    addiu   $t0, $t0, 64
    sltu    $t4, $t0, $t3
    bne     $t4, $zero, flush_loop
    nop
    sync


    jr      $a3                     # enter the real loader; never returns
    nop

    # Both stop paths are inside the protected interval: they are branch targets
    # from the copier and must not be overwritten either.  A production build
    # has no output channel here, so it simply stops -- with the watchdog
    # already disabled the console hangs rather than powering off.
stage1_guard_failed:
stage1_guard_failed_spin:
    b       stage1_guard_failed_spin
    nop

stage1_reclaim_failed:
stage1_reclaim_failed_spin:
    b       stage1_reclaim_failed_spin
    nop

    # Stage-1 exception/NMI stub.  Installed before the first low store so that
    # a fault while the firmware's handler bodies are being overwritten stops
    # here instead of executing whatever we just wrote.  Not resumable and not
    # meant to be: stage 1 has no output channel and no recovery.
    #
    # It re-points $25 at the spin before doing anything else, so a second
    # fault cannot re-enter the handler.
stage1_exc_stub:
    li      $k1, (stage1_exc_spin - kosload_stage1_copier_begin)
    addu    $k1, $s1, $k1
    mtc0    $k1, $25
    nop
    nop
stage1_exc_spin:
    b       stage1_exc_spin
    nop

kosload_stage1_copier_end:
