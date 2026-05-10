/* client/playstation2/iop/kosdev9_irx.c
 *
 * Cleanroom kosdev9 IRX — Stage 3.5b.
 *
 * Two responsibilities, in this order:
 *   1.  Run the existing DEV9 expansion-bay register-init sequence
 *       (the dev9_init_run code from dev9_init.c, validated as
 *       PHASE1: DEV9 OK on hardware).
 *   2.  Register a "dev9" v1.9 LOADCORE library so other IRXs (in
 *       practice: smap.irx) can import #9 = ReadEEPROM and get a
 *       working PIO-bit-bang implementation.
 *
 * Stays resident (RESIDENT_END) so its export table remains alive
 * for subsequent LMB loads.
 *
 * Module name in .iopmod is "kosdev9".  The LIBRARY name registered
 * via RegisterLibraryEntries is "dev9" v1.9 — matching what Sony's
 * INET_SMAP_driver and ps2link's smap_irx import (verified in
 * AGENT/docs/re/cleanroom-iop-loader/{11,12}-*.md import tables).  IRX
 * module name and registered-library name are independent
 * namespaces; Sony does the same split (module
 * "INET_SMAP_driver" registers library "smap").
 *
 * EEPROM bit-bang protocol RE'd from the EE-side ps2-load-ip port at
 * client/playstation2/net/smap.c (smap_eeprom_*) which has been
 * hardware-validated on real PS2 NetAdapters for years.  The IOP
 * version uses the same SMAP PIO port (offsets +0x2c/+0x2e from the
 * SMAP base) but at the IOP-alias 0xb000_xxxx instead of the EE-alias
 * 0xb400_xxxx.
 */

#include "kosload_iop.h"
#include "dev9_init_shared.h"

/* Pull dev9_init.c into this translation unit. The IRX is linked
 * with -r (relocatable) for elf2irx, which leaves R_MIPS_26 (cross-
 * function JAL) relocations unresolved in the .text section. The
 * IOP-side IRX loader does not appear to apply those at load time,
 * leaving the JAL imm26 field at 0 — which then jumps to address 0
 * and faults the IOP. By including dev9_init.c here, the call from
 * _start to dev9_init_run is intra-translation-unit and the linker
 * resolves it before elf2irx ever runs. We must also drop dev9_init.o
 * from the IRX link target (otherwise duplicate symbol). */
#include "dev9_init.c"

KOSLOAD_IRX_ID("kosdev9", 1, 0);

/* ================================================================== *
 * Imports — placed above any caller so the auto-generated extern
 * declarations are in scope.  loadcore::#6 is for library
 * registration in _start; intrman::#17/#18 wrap the EEPROM bit-bang
 * critical section to prevent IRQ-preemption corrupting the PIO
 * timing.  Export numbers RE'd from call pattern in sony_dev9.irx
 * 0xa8c (jal 0x215c sets a0 = &state, jal 0x2164 sets a0 = state) —
 * signature matches CpuSuspendIntr(int *state) / CpuResumeIntr(int).
 * ================================================================== */

KOSLOAD_IMPORT_TABLE(loadcore, 1, 1);
KOSLOAD_IMPORT(loadcore, 6, RegisterLibraryEntries, int, (void *table));
KOSLOAD_IMPORT_TABLE_END(loadcore);

KOSLOAD_IMPORT_TABLE(intrman, 1, 2);
KOSLOAD_IMPORT(intrman, 17, CpuSuspendIntr, int, (int *state));
KOSLOAD_IMPORT(intrman, 18, CpuResumeIntr,  int, (int state));
KOSLOAD_IMPORT_TABLE_END(intrman);

/* ================================================================== *
 * EEPROM bit-bang — translated from EE-side smap_eeprom_* in
 * client/playstation2/net/smap.c, with three IOP-side adjustments:
 *
 *   - SMAP base: 0xb000_0000 (IOP KSEG1) instead of 0xb400_0000 (EE
 *     SBUS-bridge alias).  Same physical DEV9 register window.
 *   - udelay: busy-wait loop tuned for IOP at ~36 MHz instead of the
 *     EE-side multiplier tuned for ~295 MHz.  We do NOT use thbase
 *     DelayThread inside _start — Stage 3.4 hardware run showed that
 *     causes the LMB-load RPC to never get RPC_END (CALL NO REPLY)
 *     because the LOADFILE service thread that called us can't
 *     deschedule mid-load.  Tight-poll busy-wait sidesteps that.
 *   - Single shared `pp_state` byte instead of per-channel state —
 *     kosdev9 has no concept of multiple SMAP instances.
 *
 * Wire-level protocol matches the 93C46-class Microwire 16-bit serial
 * EEPROM the SMAP board carries: <CS+S> <start bit 1> <op2[1:0]> <addr6>
 * <data16 × N> <CS↓>.  N=4 for the MAC-read (3 halfwords MAC + 1
 * checksum).
 * ================================================================== */

#define SMAP_PIOPORT_DIR        (*(volatile unsigned char *)0xb000002cu)
#define SMAP_PIOPORT_OUT        (*(volatile unsigned char *)0xb000002eu)
#define SMAP_PIOPORT_IN         (*(volatile unsigned char *)0xb000002eu)

#define EEPROM_PP_DOUT          (1u << 4)   /* read-only — Q from EEPROM */
#define EEPROM_PP_DIN           (1u << 5)   /* write — D to EEPROM */
#define EEPROM_PP_SCLK          (1u << 6)   /* write — clock */
#define EEPROM_PP_CSEL          (1u << 7)   /* write — chip select */

#define EEPROM_OP_READ          0x2u        /* 2'b10 */

/* ---------------------------------------------------------------- *
 * Stage 3.5d / 3.6 BIT-BANG VARIANT MATRIX
 *
 * Hardware test of the cleanroom protocol against a confirmed real
 * Sony NetAdapter (verified MAC 00:04:1f:13:b2:52 via ARP) showed
 * the bit-bang reads deterministic-but-WRONG values across boots
 * even after the intrman::CpuSuspendIntr/CpuResumeIntr critical-
 * section bracket.  This variant matrix lets us A/B which protocol
 * tweak fixes correctness, by changing one knob at a time:
 *
 *   KOSDEV9_VARIANT=0  baseline (DIR=0xe0, no dummy, 12 iter/µs)
 *   KOSDEV9_VARIANT=1  + DIR=0xe1     (Sony's bit-0 = "EEPROM
 *                                       enable" suspect)
 *   KOSDEV9_VARIANT=2  + consume dummy zero between addr & data
 *                                       (matches Sony's protocol;
 *                                        non-fatal — we discard
 *                                        the sample, don't validate)
 *   KOSDEV9_VARIANT=3  + 4× slower udelay (in case our timing is
 *                                          too fast for this rev)
 *
 * Build with: `make ip KOSDEV9_VARIANT=N` (see Makefile).
 * Defaults to 0 if not specified.
 * ---------------------------------------------------------------- */

#ifndef KOSDEV9_VARIANT
#define KOSDEV9_VARIANT 0
#endif

#if KOSDEV9_VARIANT >= 1
#define KOSDEV9_DIR_BITS  (EEPROM_PP_SCLK | EEPROM_PP_CSEL | \
                           EEPROM_PP_DIN | 0x01u)
#else
#define KOSDEV9_DIR_BITS  (EEPROM_PP_SCLK | EEPROM_PP_CSEL | \
                           EEPROM_PP_DIN)
#endif

#if KOSDEV9_VARIANT >= 2
#define KOSDEV9_CONSUME_DUMMY 1
#else
#define KOSDEV9_CONSUME_DUMMY 0
#endif

#if KOSDEV9_VARIANT >= 3
#define KOSDEV9_UDELAY_ITERS_PER_USEC  48u
#else
#define KOSDEV9_UDELAY_ITERS_PER_USEC  12u
#endif

/* Shared write-byte state.  Updated incrementally by helpers below
 * and flushed to PIOPORT_OUT on each clock-out call.  The EEPROM
 * latches data + clock together, so we keep all three pins (CSEL,
 * SCLK, DIN) in one byte for atomic-ish updates. */
static unsigned char eeprom_pp_state;

/* Busy-wait for ~`usec` microseconds.  IOP runs at ~36 MHz; -O2 +
 * volatile counter loop is ~3-4 cycles per iter ≈ 100 ns.  Default
 * 12 iter/µs ≈ 1.2 µs.  Variant 3 uses 48 iter/µs ≈ 5 µs/µs —
 * intentionally generous to test whether timing is the issue. */
static void eeprom_udelay(unsigned int usec)
{
    volatile unsigned int i;
    unsigned int iters = usec * KOSDEV9_UDELAY_ITERS_PER_USEC;
    for (i = 0; i < iters; i++)
        ;
}

static void eeprom_clk_out(int clk)
{
    if (clk)
        eeprom_pp_state |= EEPROM_PP_SCLK;
    else
        eeprom_pp_state &= (unsigned char)~EEPROM_PP_SCLK;
    SMAP_PIOPORT_OUT = eeprom_pp_state;
}

static void eeprom_set_d(int d)
{
    if (d)
        eeprom_pp_state |= EEPROM_PP_DIN;
    else
        eeprom_pp_state &= (unsigned char)~EEPROM_PP_DIN;
}

static void eeprom_set_s(int s)
{
    if (s)
        eeprom_pp_state |= EEPROM_PP_CSEL;
    else
        eeprom_pp_state &= (unsigned char)~EEPROM_PP_CSEL;
}

static int eeprom_get_q(void)
{
    return (SMAP_PIOPORT_IN >> 4) & 1;
}

/* One bit out (clock low → high → low pulse with data on the rising
 * edge).  ~4 µs per bit at our delay budget — slow but well within
 * the EEPROM's max clock period (no upper bound for asynchronous
 * Microwire). */
static void eeprom_clock_dataout(int val)
{
    eeprom_set_d(val);
    eeprom_clk_out(0); eeprom_udelay(1);
    eeprom_clk_out(1); eeprom_udelay(1);
    eeprom_clk_out(0); eeprom_udelay(1);
}

static int eeprom_clock_datain(void)
{
    int q;

    eeprom_set_d(0);
    eeprom_clk_out(0); eeprom_udelay(1);
    eeprom_clk_out(1); eeprom_udelay(1);
    q = eeprom_get_q();
    eeprom_clk_out(0); eeprom_udelay(1);
    return q;
}

/* MSB-first 6-bit address shift-out. */
static void eeprom_put_addr(unsigned char addr)
{
    int i;

    addr &= 0x3fu;
    for (i = 0; i < 6; i++) {
        eeprom_clock_dataout((addr & 0x20u) ? 1 : 0);
        addr = (unsigned char)(addr << 1);
    }
}

/* MSB-first 16-bit data shift-in. */
static unsigned short eeprom_get_data(void)
{
    int i;
    unsigned short data = 0;

    for (i = 0; i < 16; i++) {
        data = (unsigned short)(data << 1);
        data |= (unsigned short)eeprom_clock_datain();
    }
    return data;
}

/* Begin an EEPROM op: configure PIO direction, raise CS, then shift
 * in the start-bit and 2-bit op-code.  DIR value is variant-
 * controlled (KOSDEV9_DIR_BITS) — variants 0 use 0xe0 (EE-side
 * ps2-load-ip pattern), variants 1+ use 0xe1 (Sony's pattern with
 * mystery bit 0 set). */
static void eeprom_start_op(int op)
{
    SMAP_PIOPORT_DIR = KOSDEV9_DIR_BITS;

    /* Idle-state pulse: CS=0, SCLK=0, DIN=0 settles for one µs. */
    eeprom_pp_state = 0;
    eeprom_set_s(0);
    eeprom_set_d(0);
    eeprom_clk_out(0);
    eeprom_udelay(1);

    /* Raise CS — chip select active. */
    eeprom_set_s(1);
    eeprom_set_d(0);
    eeprom_clk_out(0);
    eeprom_udelay(1);

    /* Start bit + 2-bit op code (MSB first). */
    eeprom_clock_dataout(1);
    eeprom_clock_dataout((op >> 1) & 1);
    eeprom_clock_dataout(op & 1);
}

/* Strict dummy-zero check (matching Sony's sony_dev9.irx 0x8bc-0x8d8)
 * was tried in an earlier Stage 3.5d iteration but rejected on the
 * user's hardware — the chip didn't drive Q to 0 on the dummy clock,
 * so the check fired false-fault `SMAP F EEPROM IO`.
 *
 * Different EEPROM revisions / adapter types have different dummy-bit
 * conventions: some emit the zero, some don't.  The EE-side
 * ps2-load-ip port doesn't include the check either, so we keep our
 * original "blind 16-bit read after addr" protocol — and instead
 * focus on making it RELIABLE via interrupt suspension (below). */

/* End op: drop CS, settle. */
static void eeprom_cs_low(void)
{
    eeprom_set_s(0);
    eeprom_set_d(0);
    eeprom_clk_out(0);
    eeprom_udelay(2);
}

/* ================================================================== *
 * EEPROM cache (matches Sony's sony_dev9.irx pattern).
 *
 * Sony's dev9::#9 ReadEEPROM at .text 0xa10 isn't a bit-bang — it's
 * a simple cache-reader.  The actual bit-bang happens once during
 * dev9.irx's _start (called from EXPBAY init at 0xa8c → 0x7d8) and
 * stores the 4 halfwords in dev9.irx's .data[0x2578..].  Subsequent
 * #9 calls just memcpy from that cache.
 *
 * Why this matters: on the user's hardware (Stage 3.5d testing),
 * doing the bit-bang inside smap.irx Phase 11 — AFTER smap.irx had
 * already issued the SMAP fabric reset (Phase 4), FIFO reset (Phase
 * 5), and EMAC3 MODE0 soft reset (Phase 6) — gave deterministic but
 * WRONG values (consistent 0x00:00:00:00:00:09 vs the real Sony OUI
 * 00:04:1f:13:b2:52 read correctly by the EE-side ps2-load-ip port
 * which bit-bangs BEFORE chip reset).
 *
 * The fix: cache the EEPROM at kosdev9._start time, before any chip
 * reset can affect the EEPROM PIO subsystem.  Mirror Sony's pattern
 * exactly — bit-bang once, cache in .bss, return cached on every
 * subsequent dev9::#9 call.
 * ================================================================== */

/* Cache state: 0 = not yet read, 1 = valid, -1 = bit-bang failed. */
static int eeprom_cache_state;
static unsigned short eeprom_cache[4];

/* Internal: bit-bang the EEPROM once and store result in
 * `eeprom_cache[]`.  Called from _start AFTER dev9_init_run sets up
 * the SPEED chip but BEFORE RegisterLibraryEntries publishes the
 * library to other IRXs.  Bracketed in CpuSuspendIntr/CpuResumeIntr
 * so IRQ preemption can't stretch SCLK / sampling DOUT at the wrong
 * time relative to chip state. */
static void eeprom_populate_cache(void)
{
    int i;
    int int_state;
#if KOSDEV9_CONSUME_DUMMY
    int q;
#endif

    CpuSuspendIntr(&int_state);

    /* Match the last known-working EE-side SMAP path: after
     * dev9_init_run has brought up the expansion bay, bit-bang the
     * EEPROM directly through SMAP_PIOPORT_DIR/OUT/IN with no extra
     * Sony-wrapper routing preamble.  The previous IOP-only path
     * wrote 0xbf801570 / 0xbf801464 / 0xb000002a here; hardware tests
     * produced stuck-high / bit-phase MACs such as ff:ff:c0:00:00:00,
     * so keep this path faithful to the EE implementation first. */

    /* Two-transaction read matches the EE-side ps2-load-ip port
     * exactly (`smap_get_node_addr` in client/playstation2/net/smap.c
     * does `smap_eeprom_read(0, macp, 3); smap_eeprom_read(3, &cksum,
     * 1)`).  EE-side reads `00:04:1f:13:b2:52` correctly on the user's
     * hardware; our earlier 4-halfword-continuous-read with auto-
     * increment gave wrong data deterministically.  This adapter's
     * 93C46 evidently doesn't honour multi-halfword auto-increment,
     * or the chip's behavior past halfword 0 inside one CS-asserted
     * window is otherwise unreliable.  CS toggle between transactions
     * forces a clean re-issue of start+op+addr each time. */

    /* Transaction 1: read 3 MAC halfwords starting at addr 0. */
    eeprom_start_op(EEPROM_OP_READ);
    eeprom_put_addr(0);

#if KOSDEV9_CONSUME_DUMMY
    /* Variant >= 2: consume dummy zero between addr-out and data-in. */
    eeprom_set_d(0);
    eeprom_clk_out(0); eeprom_udelay(1);
    eeprom_clk_out(1); eeprom_udelay(1);
    q = eeprom_get_q();
    eeprom_clk_out(0); eeprom_udelay(1);
    (void)q;
#endif

    for (i = 0; i < 3; i++)
        eeprom_cache[i] = eeprom_get_data();
    eeprom_cs_low();

    /* Transaction 2: read the 16-bit cksum at addr 3. */
    eeprom_start_op(EEPROM_OP_READ);
    eeprom_put_addr(3);

#if KOSDEV9_CONSUME_DUMMY
    eeprom_set_d(0);
    eeprom_clk_out(0); eeprom_udelay(1);
    eeprom_clk_out(1); eeprom_udelay(1);
    q = eeprom_get_q();
    eeprom_clk_out(0); eeprom_udelay(1);
    (void)q;
#endif

    eeprom_cache[3] = eeprom_get_data();
    eeprom_cs_low();

    CpuResumeIntr(int_state);

    eeprom_cache_state = 1;
}

/* ================================================================== *
 * Public export #9: ReadEEPROM
 *
 * Sony's INET_SMAP_driver and ps2link's smap_irx both call this as
 * `dev9::#9(buf)` with a single buffer pointer.  Contract: write 4
 * halfwords to *out — 3 halfwords of MAC (canonical bytes 0..1,
 * 2..3, 4..5 in low-byte-first halfword order) and 1 halfword of
 * 16-bit checksum (sum of the 3 MAC halfwords, & 0xffff).
 *
 * Returns 0 on success, -1 if the cache hasn't been populated
 * (= dev9.irx _start hadn't run by the time the call arrived,
 * which shouldn't happen for properly-ordered IRX loads).
 *
 * Must be a non-static file-scope symbol — KOSLOAD_EXPORT below
 * references it by name from the export-table .data section.
 * ================================================================== */

int dev9_read_eeprom(unsigned short *out)
{
    int i;

    if (eeprom_cache_state != 1)
        return -1;

    for (i = 0; i < 4; i++)
        out[i] = eeprom_cache[i];

    return 0;
}

/* Stub for unused export slots.  Sony's INET_SMAP_driver "smap"
 * library does the same thing for its 4 exposed slots — they're
 * registered for the netdev framework's library-walk but don't
 * implement anything callable. */
int dev9_unused(void)
{
    return 0;
}

/* ================================================================== *
 * Library "dev9" v1.9 — slot 9 = dev9_read_eeprom.
 *
 * Slots 0-8 point at the dev9_unused return-stub.  CRITICAL: do NOT
 * leave NULL pointers in the middle of funcs[].  funcs[] is NULL-
 * terminated, so a NULL slot before the real exports causes LOADCORE's
 * import-resolution walker to treat that slot as the end-of-array and
 * never find later slots — smap.irx's `dev9::#9 ReadEEPROM` import
 * went unresolved (or worse, hung) when slots 0..8 were NULL on Stage
 * 3.5c's first hardware run.
 *
 * Sony's INET_SMAP_driver does the same — all 4 of its `smap` library
 * slots point at a `jr ra; nop` return-stub even though only the
 * netdev path actually drives the chip.
 *
 * Stage 3.6 will replace dev9_unused at slot #8 with the real
 * RegisterIntrCb implementation when we add IRQ wiring.
 * ================================================================== */

KOSLOAD_EXPORT_TABLE(dev9, 1, 9);
KOSLOAD_EXPORT(dev9, dev9_unused);      /* 0 */
KOSLOAD_EXPORT(dev9, dev9_unused);      /* 1 */
KOSLOAD_EXPORT(dev9, dev9_unused);      /* 2 */
KOSLOAD_EXPORT(dev9, dev9_unused);      /* 3 */
KOSLOAD_EXPORT(dev9, dev9_unused);      /* 4 */
KOSLOAD_EXPORT(dev9, dev9_unused);      /* 5 */
KOSLOAD_EXPORT(dev9, dev9_unused);      /* 6 */
KOSLOAD_EXPORT(dev9, dev9_unused);      /* 7 */
KOSLOAD_EXPORT(dev9, dev9_unused);      /* 8 */
KOSLOAD_EXPORT(dev9, dev9_read_eeprom); /* 9 */
KOSLOAD_EXPORT_TABLE_END(dev9);

/* (loadcore + intrman import tables are at the top of the file so
 * their auto-emitted extern declarations are in scope for any caller
 * — see comment block near KOSLOAD_IRX_ID.) */

/* ================================================================== *
 * _start
 *
 * Lifecycle:
 *   1. Pre-touch the dev9 mailbox at 0xA01F8000 (settling write —
 *      see the Stage 1 dev9_init_irx history for why a 4-field
 *      pre-write was empirically needed for the EE poll to see
 *      subsequent writes).
 *   2. Run dev9_init_run() — full DEV9 + SPEED expansion-bay setup.
 *   3. RegisterLibraryEntries(&dev9_export_table) so smap.irx's
 *      "dev9" v1.9 import block resolves at load time.
 *   4. Return RESIDENT_END so the library stays available.
 *
 * argv parsing is deliberately bypassed — the dev9 mailbox address
 * is fixed at 0x001F8000 (the system-reserved IOP scratch slot).
 * The bootstrap doesn't hand us argv[1] because there's no per-load
 * configuration (the mailbox is always the same place).
 * ================================================================== */

int _start(int argc, char **argv)
{
    struct iop_mailbox *mbox;
    volatile struct iop_mailbox *probe =
        (volatile struct iop_mailbox *)0xA01F8000u;
    unsigned int runtime_pc;

    (void)argc;
    (void)argv;

    __asm__ volatile(
        ".set push\n"
        ".set noreorder\n"
        "bgezal $zero, 1f\n"
        "nop\n"
        "1:\n"
        "addiu %0, $ra, 0\n"
        ".set pop\n"
        : "=r"(runtime_pc)
        :
        : "$ra");

    /* Pre-touch the mailbox via the IOP uncached alias.  Empirically
     * this 4-field settling write is what makes dev9_init_run's later
     * writes reach IOP RAM in time for the EE to poll them.  Diagnostic
     * values; the EE doesn't depend on the specific bits. */
    probe->cmd      = 0xDEAD0001u;
    probe->rev      = (unsigned int)argc;
    probe->presence = (unsigned int)((unsigned int)argv >> 16);
    probe->map      = (unsigned int)argv;
    probe->power    = 0xE0E1E0E1u;

    probe->power    = 0xE0E10013u;
    probe->rev      = runtime_pc;
    probe->map      = (unsigned int)(void *)dev9_init_run;
    probe->presence = (unsigned int)(void *)_start;

    /* DEV9 register init — same path as Stage 1 / 2 / 3.4. */
    mbox = (struct iop_mailbox *)0xA01F8000u;
    (void)dev9_init_run(mbox);

    /* Bit-bang the EEPROM and cache the result NOW — before any
     * subsequent IRX (smap.irx) gets to load and reset chip state.
     * Sony's sony_dev9.irx does the bit-bang here too, in its
     * _start, and caches into .data[0x2578..]; dev9::#9 ReadEEPROM
     * is then a simple memcpy from that cache.  Reading the EEPROM
     * after smap.irx had reset EMAC3/FIFO/MODE0 produced wrong but
     * deterministic values on hardware (Stage 3.5d).  Fixing the
     * order means dev9_read_eeprom returns cached good data on
     * every call. */
    eeprom_populate_cache();

    /* Register the "dev9" v1.9 library so other IRXs can import.
     * RegisterLibraryEntries returns 0 on success, non-zero if the
     * library is already registered (LOADCORE rejects duplicates).
     * If registration fails we return NO_RESIDENT_END — keeping the
     * module loaded with an unregistered library would just waste
     * IOP RAM. */
    if (RegisterLibraryEntries(KOSLOAD_EXPORT_TABLE_PTR(dev9)) != 0)
        return KOSLOAD_MODULE_NO_RESIDENT_END;

    /* Stay resident — our export table must remain alive for
     * subsequent LMB loads to find. */
    return KOSLOAD_MODULE_RESIDENT_END;
}
