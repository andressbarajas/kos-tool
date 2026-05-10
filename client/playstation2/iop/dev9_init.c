/* client/playstation2/iop/dev9_init.c
 *
 * Initializes DEV9, the PS2 expansion-bay block behind the Broadband
 * Adapter.
 *
 * This runs on the IOP because DEV9 needs 16-bit register writes, and those
 * are reliable from the IOP side.  The EE-side bridge can drop some of them.
 *
 * The sequence is based on Sony's retail DEV9 drivers:
 *   1. Read the DEV9 controller revision.
 *   2. Choose timing values for the controller family:
 *        0x2x = CXD9566, older/phat style
 *        0x3x = CXD9611, slim style
 *   3. Confirm the SPEED chip responds and mask its interrupts.
 *
 * The EE waits for a status word in a small IOP-RAM mailbox.
 *
 * Built with: mipsel-ps2-elf-gcc (IOP = MIPS R3000A, little-endian)
 */

#include "dev9_init_shared.h"

/* DEV9 controller registers.  Use IOP KSEG1 addresses so reads/writes are
 * uncached hardware accesses. */
#define DEV9_R_1460  (*(volatile unsigned short *)0xBF801460)
#define DEV9_R_1462  (*(volatile unsigned short *)0xBF801462)
#define DEV9_R_1464  (*(volatile unsigned short *)0xBF801464)
#define DEV9_R_1466  (*(volatile unsigned short *)0xBF801466)
#define DEV9_R_1468  (*(volatile unsigned short *)0xBF801468)
#define DEV9_R_146A  (*(volatile unsigned short *)0xBF80146A)
#define DEV9_R_146C  (*(volatile unsigned short *)0xBF80146C)
#define DEV9_R_146E  (*(volatile unsigned short *)0xBF80146E)
#define DEV9_R_1474  (*(volatile unsigned short *)0xBF801474)
#define DEV9_R_147A  (*(volatile unsigned short *)0xBF80147A)
#define DEV9_R_147C  (*(volatile unsigned short *)0xBF80147C)
#define DEV9_R_147E  (*(volatile unsigned short *)0xBF80147E)

/* SSBUS bus-1 timing registers (32-bit), KSEG1. */
#define SSBUS_R_1418  (*(volatile unsigned int *)0xBF801418)
#define SSBUS_R_141C  (*(volatile unsigned int *)0xBF80141C)
#define SSBUS_R_1420  (*(volatile unsigned int *)0xBF801420)

/* Per-silicon SSBUS_R_1418 values. */
#define SSBUS_R_1420_VAL          0x00051011u
#define SSBUS_R_1418_VAL_CXD9566  0x001A00BBu
#define SSBUS_R_1418_VAL_CXD9611  0xE01A3043u
#define SSBUS_R_141C_VAL          0xEF1A3043u

/* DEV9_R_REV upper-nibble values that identify the controller chip. */
#define DEV9C_REV_CXD9566  0x2u
#define DEV9C_REV_CXD9611  0x3u

/* SPEED chip on the other side of SSBUS bus 1.
 * EE-side base 0x10000000 = IOP KSEG1 alias 0xB0000000. */
#define SPD_R_REV       (*(volatile unsigned short *)0xB0000000)
#define SPD_R_REV_1     (*(volatile unsigned short *)0xB0000002)
#define SPD_R_REV_3     (*(volatile unsigned short *)0xB0000004)
#define SPD_R_INTR_STAT (*(volatile unsigned short *)0xB0000028)
#define SPD_R_INTR_MASK (*(volatile unsigned short *)0xB000002A)

#define SPEED_ATTR_BASE      0xB0000000u
#define SPEED_CIS_BYTES      0x800u
#define SPEED_MANFID_ID      0x00F1u
#define SPEED_CARD_ID        0x5300u

#define CISTPL_NULL          0x00u
#define CISTPL_MANFID        0x20u
#define CISTPL_END           0xFFu

static unsigned char speed_attr_u8(unsigned int cis_off)
{
    return *(volatile unsigned char *)(SPEED_ATTR_BASE + (cis_off << 1));
}

static unsigned short speed_attr_u16(unsigned int cis_off)
{
    return (unsigned short)(speed_attr_u8(cis_off) |
                            ((unsigned short)speed_attr_u8(cis_off + 1u) << 8));
}

static int speed_cis_find_manfid(unsigned int *packed_id, unsigned int *walk_diag)
{
    unsigned int off = 0u;
    unsigned int tuples = 0u;

    *packed_id = (unsigned int)speed_attr_u8(0u) |
                 ((unsigned int)speed_attr_u8(1u) << 8) |
                 ((unsigned int)speed_attr_u8(2u) << 16) |
                 ((unsigned int)speed_attr_u8(3u) << 24);

    while (off < SPEED_CIS_BYTES) {
        unsigned char tag = speed_attr_u8(off++);
        unsigned char len;
        unsigned int body;
        unsigned int next;

        if (tag == CISTPL_NULL) continue;
        if (tag == CISTPL_END || off >= SPEED_CIS_BYTES) break;

        len = speed_attr_u8(off++);
        if (len == CISTPL_END) break;

        body = off;
        next = body + (unsigned int)len;
        if (next > SPEED_CIS_BYTES) break;

        tuples++;
        if (tag == CISTPL_MANFID && len >= 4u) {
            unsigned short manf = speed_attr_u16(body);
            unsigned short card = speed_attr_u16(body + 2u);
            *packed_id = ((unsigned int)manf << 16) | (unsigned int)card;
            if (manf == SPEED_MANFID_ID && card == SPEED_CARD_ID) {
                *walk_diag = (tuples << 16) | (off & 0xFFFFu);
                return 1;
            }
        }

        off = next;
    }

    *walk_diag = (tuples << 16) | (off & 0xFFFFu);
    return 0;
}

/* Mailbox: EE sets cmd=1, IOP writes result here when done. */
#define MBOX_CMD_INIT_DEV9          1
#define MBOX_RESULT_OK              0
#define MBOX_RESULT_NO_DEV          (-1)
#define MBOX_RESULT_BAD_REV         (-2)
#define MBOX_RESULT_NO_PWR          (-3)
#define MBOX_RESULT_NO_BUS          (-4)
#define MBOX_RESULT_NO_MAP          (-5)
#define MBOX_RESULT_UNSUPPORTED_REV (-6)
#define MBOX_RESULT_NO_SPEED        (-7)

static __attribute__((noinline, noipa))
void dev9_publish_word(volatile unsigned int *dst, unsigned int value)
{
    *dst = value;
    __asm__ volatile ("" ::: "memory");
    (void)*dst;
    __asm__ volatile ("" ::: "memory");
}

static __attribute__((noinline, noipa))
void dev9_publish_result(volatile int *dst, int value)
{
    *dst = value;
    __asm__ volatile ("" ::: "memory");
    (void)*dst;
    __asm__ volatile ("" ::: "memory");
}

/* Force noinline so we test the inter-function jal/HI16+LO16 path.
 * If apply_lmb_patch() ran successfully on the bootstrap side, the
 * relocations get applied at IRX load time and this works. If it
 * fails, see the diagnostic prompt for what's broken. */
__attribute__((noinline))
int dev9_init_run(struct iop_mailbox *mbox)
{
    /* Track which DEV9 host adapter we're talking to:
     *   is_pcmcia_slot = 1 → CXD9566 (early Japan-only PS2s with
     *                        a literal PCMCIA slot — SCPH-10000/15000/
     *                        18000).  Bring-up needs CIS scan +
     *                        the :0x1548 routing unlock + the
     *                        :0x880 enable kick.
     *   is_pcmcia_slot = 0 → CXD9611 (expansion bay — every other
     *                        PS2 model that takes the SCPH-10350
     *                        network/HDD adapter).  Bring-up is
     *                        simpler: just SSBUS init + bay
     *                        power-on, then SPD_R_REV_3 read.
     *
     * Retail DEV9 behavior short-circuits the CIS scan for expansion
     * bay hardware — i.e. CIS scan does NOT run there. The earlier
     * version of this IRX always ran the scan; on EXPBAY that's
     * looking for a MANFID tuple that isn't published in the shorter
     * EXPBAY CIS chain. */
    int is_pcmcia_slot = 0;
    unsigned short rev;
    unsigned short presence;
    unsigned short power;
    unsigned short v;
    unsigned short power_state;
    unsigned short card_status;
    unsigned short spd_rev;
    unsigned short spd_rev_3;
    unsigned short spd_caps;
    unsigned int gen;
    volatile unsigned int delay_i;

    /* Sentinels are written to mbox->cmd via the IOP uncached alias
     * before each potentially-blocking access. If the IOP hangs, the
     * EE-side timeout dump shows the last sentinel reached. */
    dev9_publish_word(&mbox->cmd, 0xBA5E0000u); /* entered dev9_init_run */

    /* No explicit Phase A controller pre-init.
     *
     * On FMCB-launched ELFs (or any other "clean homebrew" launcher),
     * the IOP has been booted via the BIOS's IOPBTCONF, which auto-
     * loads BIOS-bundled modules including ATAD. BIOS ATAD's init
     * (at :0x16D4 — write SSBUS_R_1420 = 0x00051066, SSBUS_R_1418 =
     * 0x000A10FE, modify SSBUS_COM_DELAY) brings the DEV9C controller
     * far enough up that DEV9_R_146E returns a valid REV value.
     *
     * BIOS ATAD does NOT do retail dev9.irx's elaborate post-SSBUS
     * controller setup at :0x1548 (clear 0x146C/0x1460/0x1474, write
     * 0x147E=1 / 0x1468=0x10 / 0x146A=0x90 / 0x147C=1, copy 147C →
     * 147A). That sequence is what unlocks SPEED chip access; if we
     * find SPEED reads hang the IOP below, we need to add it.
     *
     * The earlier BIOS-ATAD-derived Phase A I had here is actually a
     * deinit path (clears 0x146C bit 2 = power-on), which would tear
     * down whatever ATAD already set up. Skipping it.
     *
     * IOP I/O addresses use KSEG1 (0xBF80...) for guaranteed-uncached
     * access. KUSEG (0x1F80...) reads went through D-cache and
     * returned stale 0s in earlier hardware tests. */

    /* ----------------------------------------------------------
     * Read DEV9_R_REV — controller silicon identification.
     *
     * Upper nibble: 0x2 = CXD9566, 0x3 = CXD9611.
     * ---------------------------------------------------------- */
    rev = DEV9_R_146E;
    gen = ((unsigned int)rev >> 4) & 0xFu;
    mbox->rev = rev;
    /* Stash the rev in map so the DON line's M= column shows it
     * (and presence keeps the real DEV9_R_1462 card-status read
     * below). On the UNSUPPORTED_REV early-return path, presence
     * also gets the rev so it's still visible in P=. */
    mbox->map = (unsigned int)rev;
    mbox->presence = (unsigned int)rev;
    dev9_publish_word(&mbox->cmd, 0xBA5E0001u); /* REV read OK */

    /* ------------------------------------------------------
     * SSBUS bus 1 timing (retail dev9.irx :0x1494 / :0x1830,
     * slim XDEV9 :0x15dc / :0x1978).
     *
     * Per-REV SSBUS_R_1418 picks setup/hold timing matched
     * to the controller silicon. Order: 0x1420, 0x1418, 0x141C.
     * ------------------------------------------------------ */
    SSBUS_R_1420 = SSBUS_R_1420_VAL;
    dev9_publish_word(&mbox->cmd, 0xBA5E0002u);

    if (gen == DEV9C_REV_CXD9566) {
        SSBUS_R_1418 = SSBUS_R_1418_VAL_CXD9566;
        is_pcmcia_slot = 1;
    } else if (gen == DEV9C_REV_CXD9611) {
        SSBUS_R_1418 = SSBUS_R_1418_VAL_CXD9611;
    } else {
        dev9_publish_result(&mbox->result, MBOX_RESULT_UNSUPPORTED_REV);
        return -6;
    }
    dev9_publish_word(&mbox->cmd, 0xBA5E0003u);

    SSBUS_R_141C = SSBUS_R_141C_VAL;
    dev9_publish_word(&mbox->cmd, 0xBA5E0004u); /* SSBUS init done */

    /* ----------------------------------------------------------
     * Card-status read — DEV9_R_1462 should now complete.
     *   Bits 0..1 = card type (16-bit Type II expected).
     *   Bits 2..3 = voltage (3.3V / 5V / invalid).
     * ---------------------------------------------------------- */
    presence = DEV9_R_1462;
    mbox->presence = presence;
    dev9_publish_word(&mbox->cmd, 0xBA5E0005u); /* card-status read OK */

    /* Wake-up sequence — re-added to force dev9_init_run to stay
     * a separate (non-inlined) function so we can byte-compare
     * elf2irx vs iopfixup on the cross-function-call case. */
    power = DEV9_R_146C;
    mbox->power = (unsigned int)power;
    if (!(power & 0x4u)) {
        DEV9_R_1466 = 1;
        DEV9_R_1464 = 0;
        v = DEV9_R_1464;
        DEV9_R_1460 = v;
    }
    dev9_publish_word(&mbox->cmd, 0xBA5E0006u); /* DEV9C bring-up done */

    /* ----------------------------------------------------------
     * Post-SSBUS DEV9C controller setup that unlocks SPEED-chip
     * routing on SSBUS bus 1.  Without this sequence, SPEED REV
     * at 0xB000_0000 reads back as 0x0000 / 0xFFFF on real
     * hardware.
     *
     * Reverse-engineered from retail dev9.irx (Sony's
     * "PsIIDEV9 2710" extracted from ESPN NFL 2K5) at file
     * offset :0x1548 — confirmed by binary disassembly only.
     *
     * NOTE: a previous attempt cleared 0x146C as the first step
     * (the comment in the older code said to).  On real hardware
     * that turned out to power-cycle the bay and leave SPEED dead
     * even after the rest of the writes — BIOS ATAD has already
     * powered the bay, so we must preserve that state and only
     * touch the routing registers.  Skipping the 0x146C clear
     * (and, defensively, the 0x1460 clear that pairs with it).
     *
     * For diagnostics, stash the post-unlock 0x146C value into
     * mbox->map.  When SPEED still fails, M= shows what the
     * power/status register actually reads — bit 2 set means
     * "card power good", bit 4 set means "power enabled".
     *
     * Gated on is_pcmcia_slot — this whole sequence is part of
     * the CXD9566 PCMCIA-slot bring-up.  Expansion-bay (CXD9611)
     * doesn't need any of these writes per ps2dev9.irx :0x600+. */
    if (is_pcmcia_slot) {
        DEV9_R_1474 = 0;

        DEV9_R_147E = 1;
        DEV9_R_1468 = 0x10;
        DEV9_R_146A = 0x90;
        DEV9_R_147C = 1;
        v = DEV9_R_147C;
        DEV9_R_147A = v;
        dev9_publish_word(&mbox->cmd, 0xBA5E000Bu); /* SPEED-routing unlock done */
    }

    /* Bay power-on.
     *
     * The power-on sequence:
     *
     *   Step A: 0x146C = (current & ~0x5) | 0x4
     *           → bit 2 = 1, bit 0 = 0, others preserved
     *   Wait ~13ms
     *   Step B: 0x1460 |= 0x1   (set bit 0 of 0x1460)
     *   Step C: 0x146C |= 0x1   → final 0x146C = 0x5
     *   Wait ~13ms
     *
     * Both bit 2 (`primary power good`) AND bit 0 (`secondary
     * enable`) need to be set to actually fire up the chip on
     * the other side of SSBUS.  Earlier attempts that wrote bit
     * 4 (sony_dev9.irx :0xdf4 style with $a1=always-on) saw bit
     * 2 latch on but bit 4 auto-cleared and SPEED stayed dead
     * — that variant targets a different card-type apparently.
     *
     * 13.5ms = 500000 IOP iterations at 37 MHz.  ps2dev9.irx
     * uses delay arg 0x7a120 = 500000 between each step. */

    /* Step A: 0x146C |= 0x4 (and clear bit 0) */
    v = DEV9_R_146C;
    v = (v & 0xFFFAu) | 0x4u;
    DEV9_R_146C = v;
    for (delay_i = 0; delay_i < 500000u; delay_i++) { }

    /* Step B: 0x1460 |= 0x1 */
    v = DEV9_R_1460;
    v = v | 0x1u;
    DEV9_R_1460 = v;

    /* Step C: 0x146C |= 0x1 */
    v = DEV9_R_146C;
    v = v | 0x1u;
    DEV9_R_146C = v;
    for (delay_i = 0; delay_i < 500000u; delay_i++) { }

    dev9_publish_word(&mbox->cmd, 0xBA5E000Cu); /* bay power-on done */

    /* CIS scan + per-silicon SSBUS-timing dance — CXD9566 PCMCIA
     * only. Retail ps2dev9.irx :0x74c writes SSBUS_R_1418 = 9566
     * timing before scanning attribute memory; on EXPBAY (CXD9611)
     * the scan is skipped, so we keep the already-set 9611 timing
     * and skip the walker. */
    if (is_pcmcia_slot) {
        SSBUS_R_1418 = SSBUS_R_1418_VAL_CXD9566;

        /* Walk the public PC Card CIS tuple chain in attribute memory.
         * Attribute-memory bytes sit on even bus addresses which
         * speed_attr_u8() hides. */
        unsigned int packed_id;
        unsigned int walk_diag;

        if (!speed_cis_find_manfid(&packed_id, &walk_diag)) {
            mbox->presence = packed_id;
            mbox->map = walk_diag;
            dev9_publish_result(&mbox->result, MBOX_RESULT_NO_SPEED);
            dev9_publish_word(&mbox->cmd, 0xBA5E000Eu); /* CIS walk: F15300 not found */
            return -7;
        }

        mbox->presence = packed_id;        /* 0x00F15300 */
        mbox->map = walk_diag;             /* high=count, low=CIS offset */
        dev9_publish_word(&mbox->cmd, 0xBA5E000Du); /* CIS walk OK */
    }

    /* SPEED chip enable.  RE'd from ps2dev9.irx around file
     * offset :0x8a0-:0x8d4.  After the bay regulator has come up,
     * the chip on the other side of SSBUS still needs a final
     * configuration kick before its registers respond:
     *
     *   1. Write 0x1474 = 7   (we cleared it earlier; must be 7
     *                          for the unlock to actually open)
     *   2. Re-write SSBUS_R_1418 with the per-silicon value
     *   3. Wait ~135us
     *   4. Clear bit 0 of 0x146C (single-shot toggle)
     */
    /* Full SPEED enable sequence per ps2dev9.irx :0x880-:0x8cc.
     * Only runs on CXD9566 PCMCIA-slot hardware — for EXPBAY the
     * dispatch at :0x5d8 jumps directly to :0x8d4 (the SPD read
     * below), skipping this whole block.
     *
     *   0x1460 = 2
     *   0x1474 = 5
     *   0x1460 = 1
     *   0x1474 = 7
     *   wait ~135us
     *   SSBUS_R_1418 = per-silicon value (re-write)
     *   0x146C bit 0 cleared  (single-shot toggle)
     */
    if (is_pcmcia_slot) {
        DEV9_R_1460 = 2;
        DEV9_R_1474 = 5;
        DEV9_R_1460 = 1;
        DEV9_R_1474 = 7;
        for (delay_i = 0; delay_i < 5000u; delay_i++) { }
        SSBUS_R_1418 = SSBUS_R_1418_VAL_CXD9566;
        v = DEV9_R_146C;
        v = v & 0xFFFEu;        /* clear bit 0 */
        DEV9_R_146C = v;
        dev9_publish_word(&mbox->cmd, 0xBA5E000Fu); /* SPEED enable kick done */
    }

    /* Stash post-enable 0x146C / 0x1462 in mbox->map for diag. */
    power_state = DEV9_R_146C;
    card_status = DEV9_R_1462;
    mbox->map = ((unsigned int)card_status << 16)
              | (unsigned int)power_state;

    /* ----------------------------------------------------------
     * SPEED chip pulse-check via SSBUS bus 1.
     *
     * Reads SPEED's REV (0xB000_0000) and capability bits
     * (0xB000_0002).  The post-SSBUS DEV9C setup above should have
     * unlocked routing to SPEED.  Failure here (REV reads as
     * 0x0000 or 0xFFFF) means SSBUS bus 1 isn't actually alive,
     * which would block any further SMAP/HDD/FLASH bring-up.
     *
     * Capability bits (SPD_R_REV_1):
     *   bit 0 = SMAP  (Ethernet)
     *   bit 1 = HDD   (ATA interface)
     *   bit 5 = FLASH (NAND)
     *
     * Stash SPEED REV in mbox->power and capability bits in
     * mbox->presence so the DON line shows them.
     * ---------------------------------------------------------- */
    /* Read all three SPEED REV registers — different chip
     * revisions populate different offsets:
     *   0xB0000000 (SPD_R_REV)    = silicon major revision
     *   0xB0000002 (SPD_R_REV_1)  = capability bits (older)
     *   0xB0000004 (SPD_R_REV_3)  = capability bits (ps2dev9
     *                              uses this offset for its
     *                              SMAP-detect check at :0x8d4)
     *
     * Pack two of them into mbox->presence (high 16 = REV_3,
     * low 16 = REV) so the DON line surfaces both. */
    spd_rev   = SPD_R_REV;
    spd_rev_3 = SPD_R_REV_3;

    mbox->power    = (unsigned int)spd_rev;
    mbox->presence = ((unsigned int)spd_rev_3 << 16)
                   | (unsigned int)spd_rev;
    dev9_publish_word(&mbox->cmd, 0xBA5E0007u); /* SPEED REVs read */

    /* SMAP-presence test mirrors ps2dev9.irx :0x8d4-:0x8e0 —
     * read REV_3 and check bit 0 (SMAP capability).  If both
     * REV and REV_3 are 0/0xFFFF, the chip didn't respond at
     * all (open bus). */
    if ((spd_rev == 0xFFFFu || spd_rev == 0x0000u) &&
        (spd_rev_3 == 0xFFFFu || spd_rev_3 == 0x0000u)) {
        dev9_publish_result(&mbox->result, MBOX_RESULT_NO_SPEED);
        return -7;
    }

    spd_caps = SPD_R_REV_1;
    dev9_publish_word(&mbox->cmd, 0xBA5E0008u); /* SPEED caps read OK */
    (void)spd_caps;

    /* Mask SPEED chip interrupts before continuing.  The SMAP
     * bring-up stage will set its own mask. */
    SPD_R_INTR_MASK = 0;
    dev9_publish_word(&mbox->cmd, 0xBA5E0009u); /* SPEED INTR_MASK cleared */

    dev9_publish_result(&mbox->result, MBOX_RESULT_OK);
    dev9_publish_word(&mbox->cmd, 0xBA5E000Au); /* full DEV9 + SPEED init done */
    return 0;
}

int iop_main(struct iop_mailbox *mbox)
{
    return dev9_init_run(mbox);
}
