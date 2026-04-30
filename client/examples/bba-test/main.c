/* examples/bba-test/main.c */
/*
 * bba-test - GameCube Broadband Adapter diagnostics
 *
 * Self-contained — does NOT depend on the kosload BBA driver.
 * Performs a full Swiss-pattern init, then sweeps through several
 * NWAYC configurations and reports NWAYS after each so we can see
 * which (if any) actually gets the chip to 100Mbps full-duplex on
 * the user's network.  Also dumps a wide set of registers and
 * runs a brief TX-submission speed test.
 *
 * Run: kos-tool -x bba-test.elf
 *
 * Output goes to host stdout via the loader's syscall (fd 1).
 */

#define SYSCALL_WRITE     1
#define SYSCALL_EXIT      15

#define KOSLOAD_BASE         0x817EC000
#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

typedef int (*kl_syscall_fn)(int, int, int, int);

static kl_syscall_fn get_syscall(void)
{
    if (KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kl_syscall_fn)0;
    return (kl_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

static int slen(const char *s) { int n=0; while (*s++) n++; return n; }

static void print(const char *s)
{
    kl_syscall_fn sc = get_syscall();
    if (sc) sc(SYSCALL_WRITE, 1, (int)s, slen(s));
}

static void puthex(unsigned int v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[12];
    int i;
    for (i = 0; i < digits; i++)
        buf[i] = hex[(v >> ((digits - 1 - i) * 4)) & 0xF];
    buf[digits] = '\0';
    print(buf);
}

static void putdec(unsigned int v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { print("0"); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) {
        char s[2] = { buf[i], 0 };
        print(s);
    }
}

/* ===== Inline EXI access ===== */

#define EXI_BASE        0xCC006800
#define EXI_STATUS      0x00
#define EXI_DMA_CR      0x0C
#define EXI_DATA        0x10

#define EXI_ROMDIS      (1 << 13)
#define EXI_W1C_MASK    ((1 << 1) | (1 << 3) | (1 << 11))

#define CLK_32MHZ       (5 << 4)

#define IMM_READ        (0 << 2)
#define IMM_WRITE       (1 << 2)
#define DMA_START       (1 << 0)

static volatile unsigned int *exi_reg(int ch, int off)
{
    return (volatile unsigned int *)(EXI_BASE + ch * 0x14 + off);
}

static void exi_init(void)
{
    int ch;
    for (ch = 0; ch < 3; ch++) {
        *exi_reg(ch, EXI_DMA_CR) = 0;
        if (ch == 0)
            *exi_reg(ch, EXI_STATUS) = EXI_W1C_MASK | EXI_ROMDIS;
        else
            *exi_reg(ch, EXI_STATUS) = EXI_W1C_MASK;
    }
}

static void exi_select(int ch, int dev, int clk)
{
    unsigned int val = (1 << (7 + dev)) | (clk & (7 << 4));
    if (ch == 0) val |= EXI_ROMDIS;
    *exi_reg(ch, EXI_STATUS) = val;
}

static void exi_deselect(int ch)
{
    if (ch == 0)
        *exi_reg(ch, EXI_STATUS) = EXI_ROMDIS;
    else
        *exi_reg(ch, EXI_STATUS) = 0;
}

static unsigned int exi_imm(int ch, unsigned int data, int len, int mode)
{
    volatile unsigned int *cr = exi_reg(ch, EXI_DMA_CR);
    volatile unsigned int *imm = exi_reg(ch, EXI_DATA);
    *imm = data;
    *cr = ((len - 1) << 4) | mode | DMA_START;
    while (*cr & DMA_START) ;
    return *imm;
}

/* ===== BBA constants ===== */

#define BBA_CH      0
#define BBA_DEV     2
#define BBA_ID      0x04020200

#define BBA_CMD_RD  0x80000000u
#define BBA_CMD_WR  0xC0000000u
#define BBA_CMD(dir, addr) ((dir) | (((addr) & 0xFFFFu) << 8))

/* Register map (from Dolphin's authoritative defs) */
#define R_NCRA      0x00
#define R_NCRB      0x01
#define R_LTPS      0x04
#define R_LRPS      0x05
#define R_IMR       0x08
#define R_IR        0x09
#define R_BP        0x0a
#define R_TLBP      0x0c
#define R_RXINTT    0x14
#define R_RWP       0x16
#define R_RRP       0x18
#define R_RHBP      0x1a
#define R_PAR0      0x20
#define R_NWAYC     0x30
#define R_NWAYS     0x31
#define R_GCA       0x32
#define R_MISC      0x3d
#define R_TXFIFOCNT 0x3e
#define R_WRTXFIFOD 0x48
#define R_MISC2     0x50
#define R_SI_ACTRL  0x5c
#define R_SI_STATUS 0x5d
#define R_SI_ACTRL2 0x60

/* NCRA bits */
#define NCRA_RESET  0x01
#define NCRA_ST0    0x02
#define NCRA_ST1    0x04
#define NCRA_SR     0x08

/* NWAYC bits (Dolphin) */
#define NWAYC_FD        0x01
#define NWAYC_PS100_10  0x02
#define NWAYC_ANE       0x04
#define NWAYC_ANS_RA    0x08      /* not in Dolphin enum, our header has it */
#define NWAYC_NTTEST    0x40
#define NWAYC_LTE       0x80

/* NWAYS bits (Dolphin) */
#define NWAYS_LS10    0x01
#define NWAYS_LS100   0x02
#define NWAYS_LPNWAY  0x04
#define NWAYS_ANCLPT  0x08
#define NWAYS_100TXF  0x10
#define NWAYS_100TXH  0x20
#define NWAYS_10TXF   0x40
#define NWAYS_10TXH   0x80

/* Interrupt bits */
#define IR_FRAGI    0x01
#define IR_RI       0x02
#define IR_TI       0x04
#define IR_REI      0x08
#define IR_TEI      0x10
#define IR_FIFOEI   0x20
#define IR_BUSEI    0x40
#define IR_RBFI     0x80

/* ===== BBA primitives ===== */

static void bba_sel(void) { exi_select(BBA_CH, BBA_DEV, CLK_32MHZ); }
static void bba_desel(void) { exi_deselect(BBA_CH); }

static unsigned char bba_in8(unsigned int reg)
{
    bba_sel();
    exi_imm(BBA_CH, BBA_CMD(BBA_CMD_RD, reg), 4, IMM_WRITE);
    unsigned int v = exi_imm(BBA_CH, 0, 1, IMM_READ);
    bba_desel();
    return (v >> 24) & 0xFF;
}

static void bba_out8(unsigned int reg, unsigned char val)
{
    bba_sel();
    exi_imm(BBA_CH, BBA_CMD(BBA_CMD_WR, reg), 4, IMM_WRITE);
    exi_imm(BBA_CH, (unsigned int)val << 24, 1, IMM_WRITE);
    bba_desel();
}

static void bba_cport_w1(unsigned char op, unsigned char data)
{
    bba_sel();
    exi_imm(BBA_CH, (unsigned int)op << 24, 2, IMM_WRITE);
    exi_imm(BBA_CH, (unsigned int)data << 24, 1, IMM_WRITE);
    bba_desel();
}

static void bba_cport_w2(unsigned char op, unsigned char d0, unsigned char d1)
{
    bba_sel();
    exi_imm(BBA_CH, (unsigned int)op << 24, 2, IMM_WRITE);
    exi_imm(BBA_CH,
            ((unsigned int)d0 << 24) | ((unsigned int)d1 << 16),
            2, IMM_WRITE);
    bba_desel();
}

static unsigned char bba_cport_r1(unsigned char op)
{
    bba_sel();
    exi_imm(BBA_CH, (unsigned int)op << 24, 2, IMM_WRITE);
    unsigned int v = exi_imm(BBA_CH, 0, 1, IMM_READ);
    bba_desel();
    return (v >> 24) & 0xFF;
}

/* Coarse delays (PowerPC ~486MHz). */
static void udelay(int us)
{
    int i; for (i = 0; i < us * 100; i++) __asm__ volatile("" ::: "memory");
}

static void mdelay(int ms)
{
    int i; for (i = 0; i < ms * 100000; i++) __asm__ volatile("" ::: "memory");
}

/* ===== Init (Swiss pattern, with auto-neg restart) ===== */

static int bba_init(void)
{
    /* 1. Wakeup */
    bba_cport_w1(0x42, 0x00);

    /* 2. SI_ACTRL2 = 0 */
    bba_out8(R_SI_ACTRL2, 0x00);
    mdelay(10);

    /* 3. Chipport read 0x0F */
    (void)bba_cport_r1(0x0F);
    udelay(200);
    mdelay(10);

    /* 4. Reset */
    bba_out8(R_NCRA, NCRA_RESET);
    udelay(100);
    bba_out8(R_NCRA, 0x00);
    udelay(100);

    /* 5. Chipport read 0x01 */
    (void)bba_cport_r1(0x01);

    /* 6. Chip magic */
    bba_cport_w2(0x44, 0xD1, 0x07);
    bba_cport_w1(0x45, 0x4E);

    /* 7. SI block */
    bba_out8(0x58, 0x80);
    bba_out8(0x59, 0x00);
    bba_out8(0x5A, 0x03);
    bba_out8(0x5B, 0x83);
    bba_out8(R_SI_ACTRL,  0x32);
    bba_out8(R_SI_STATUS, 0xFE);
    bba_out8(0x5E, 0x1F);
    bba_out8(0x5F, 0x1F);
    udelay(100);

    /* 8. Main config */
    bba_out8(R_NCRB,      0x52);   /* Swiss value */
    bba_out8(R_SI_ACTRL2, 0x74);
    bba_out8(0x14,        0x00);
    bba_out8(0x15,        0x06);
    bba_out8(R_MISC2,     0x80);

    /* 9. Buffer pointers */
    bba_out8(R_TLBP,     0x00);  bba_out8(R_TLBP + 1, 0x00);
    bba_out8(R_BP,       0x01);  bba_out8(R_BP + 1,   0x00);
    bba_out8(R_RHBP,     0x10);  bba_out8(R_RHBP + 1, 0x00);
    bba_out8(R_RWP,      0x01);  bba_out8(R_RWP + 1,  0x00);
    bba_out8(R_RRP,      0x01);  bba_out8(R_RRP + 1,  0x00);

    /* 10. GCA, NCRA = SR */
    bba_out8(R_GCA,  0x08);
    bba_out8(R_NCRA, NCRA_SR);

    /* 11. NWAYC: just preserve EEPROM bits, no auto-neg yet
     *     (we'll sweep separately) */
    {
        unsigned char nwayc = bba_in8(R_NWAYC);
        bba_out8(R_NWAYC, nwayc & 0xC0);
    }

    /* 12. MAC read placeholder */
    /* 13. IR/IMR */
    bba_out8(R_IR,  0xFF);
    bba_out8(R_IMR, 0xDF);

    /* 14. Activate */
    bba_cport_w1(0x42, 0xF8);

    return 0;
}

/* ===== Test 1: register dump ===== */

static void label_pad(const char *s, int width)
{
    int n = slen(s);
    print(s);
    while (n++ < width) print(" ");
}

static void dump_regs(const char *tag)
{
    print("\n== Register dump: ");
    print(tag);
    print(" ==\n");

    static const struct { const char *name; unsigned int reg; } regs[] = {
        {"NCRA",      R_NCRA},
        {"NCRB",      R_NCRB},
        {"LTPS",      R_LTPS},
        {"LRPS",      R_LRPS},
        {"IMR",       R_IMR},
        {"IR",        R_IR},
        {"NWAYC",     R_NWAYC},
        {"NWAYS",     R_NWAYS},
        {"GCA",       R_GCA},
        {"MISC",      R_MISC},
        {"MISC2",     R_MISC2},
        {"SI_ACTRL",  R_SI_ACTRL},
        {"SI_STATUS", R_SI_STATUS},
        {"SI_ACTRL2", R_SI_ACTRL2},
    };

    int i;
    for (i = 0; i < (int)(sizeof(regs)/sizeof(regs[0])); i++) {
        print("  ");
        label_pad(regs[i].name, 10);
        print("= 0x");
        puthex(bba_in8(regs[i].reg), 2);
        print("\n");
    }

    /* MAC */
    print("  MAC        = ");
    int j;
    for (j = 0; j < 6; j++) {
        puthex(bba_in8(R_PAR0 + j), 2);
        if (j < 5) print(":");
    }
    print("\n");
}

/* ===== Test 2: NWAYC sweep ===== */

static void decode_nways(unsigned char nways)
{
    if (nways & NWAYS_LS10)   print(" LS10");
    if (nways & NWAYS_LS100)  print(" LS100");
    if (nways & NWAYS_LPNWAY) print(" LPNWAY");
    if (nways & NWAYS_ANCLPT) print(" ANCLPT");
    if (nways & NWAYS_100TXF) print(" 100TXF");
    if (nways & NWAYS_100TXH) print(" 100TXH");
    if (nways & NWAYS_10TXF)  print(" 10TXF");
    if (nways & NWAYS_10TXH)  print(" 10TXH");
}

static void test_nwayc_config(const char *desc, unsigned char value)
{
    unsigned char preserve = bba_in8(R_NWAYC) & 0xC0;
    unsigned char to_write = preserve | value;

    print("\n--- NWAYC config: ");
    print(desc);
    print(" -> writing 0x");
    puthex(to_write, 2);
    print(" ---\n");

    /* For ANS_RA cases do the 3-step Swiss sequence */
    if (value & NWAYC_ANS_RA) {
        bba_out8(R_NWAYC, preserve);
        udelay(100);
        bba_out8(R_NWAYC, preserve | NWAYC_ANE);
        udelay(100);
        bba_out8(R_NWAYC, to_write);   /* full target */
        udelay(100);
    } else {
        bba_out8(R_NWAYC, to_write);
    }

    /* Wait up to ~3 seconds for link to come up */
    int i;
    unsigned char nways = 0;
    for (i = 0; i < 30; i++) {
        mdelay(100);
        nways = bba_in8(R_NWAYS);
        if (nways & 0x03) break;
    }

    print("  After ");
    putdec(i + 1);
    print("00ms: NWAYS = 0x");
    puthex(nways, 2);
    print(" =");
    decode_nways(nways);
    print("\n  NWAYC readback = 0x");
    puthex(bba_in8(R_NWAYC), 2);
    print("\n");
}

/* ===== Test 3: loader-style init verification =====
 *
 * Mirrors what client/gamecube/net/bba.c bba_init() does after our
 * recent fixes: full Swiss-pattern setup, NWAYC = preserve | (FD |
 * PS100 | ANE | ANS_RA), then wait up to ~5s for LS10 or LS100. */

static void loader_init_test(void)
{
    print("\n== Loader-style init test ==\n");

    /* Reset and re-init from scratch */
    bba_init();

    /* Step 12 (loader): write the full FD|PS100|ANE|ANS_RA NWAYC */
    unsigned char preserve = bba_in8(R_NWAYC) & 0xC0;
    unsigned char target = preserve | NWAYC_FD | NWAYC_PS100_10 |
                           NWAYC_ANE | NWAYC_ANS_RA;
    bba_out8(R_NWAYC, target);
    print("  Wrote NWAYC = 0x");
    puthex(target, 2);
    print("\n");

    /* Wait up to 5s in 100ms steps */
    int i;
    unsigned char nways = 0;
    for (i = 0; i < 50; i++) {
        mdelay(100);
        nways = bba_in8(R_NWAYS);
        if (nways & 0x03) break;
    }

    print("  Link came up after ");
    putdec((i + 1) * 100);
    print(" ms: NWAYS = 0x");
    puthex(nways, 2);
    print(" =");
    decode_nways(nways);
    print("\n");

    if (nways & NWAYS_LS100)
        print("  >>> 100Mbps link confirmed <<<\n");
    else if (nways & NWAYS_LS10)
        print("  >>> ONLY 10Mbps link (slower) <<<\n");
    else
        print("  >>> NO LINK after 5s <<<\n");
}

/* ===== Test 4: TX submission speed ===== */

static __attribute__((aligned(32))) unsigned char tx_dma[1536];

static void cache_flush(const void *addr, unsigned int size)
{
    unsigned int p, start = (unsigned int)addr & ~31;
    unsigned int end = ((unsigned int)addr + size + 31) & ~31;
    for (p = start; p < end; p += 32)
        __asm__ volatile("dcbf 0,%0" :: "r"(p));
    __asm__ volatile("sync");
}

static void exi_dma_xfer(int ch, void *addr, int len, int mode)
{
    volatile unsigned int *da = exi_reg(ch, 0x04);
    volatile unsigned int *dl = exi_reg(ch, 0x08);
    volatile unsigned int *cr = exi_reg(ch, EXI_DMA_CR);
    *da = (unsigned int)addr & 0x1FFFFFFF;
    *dl = len;
    *cr = mode | 0x03;
    while (*cr & DMA_START) ;
}

#define EXI_DMA_WR  (1 << 2)

static void send_one_packet(int len)
{
    int dma_len = len & ~31;
    int imm_len = len - dma_len;
    int i;

    bba_sel();
    exi_imm(BBA_CH, BBA_CMD(BBA_CMD_WR, R_WRTXFIFOD), 4, IMM_WRITE);

    if (dma_len > 0) {
        cache_flush(tx_dma, dma_len);
        exi_dma_xfer(BBA_CH, tx_dma, dma_len, EXI_DMA_WR);
    }
    for (i = 0; i < imm_len; i++)
        exi_imm(BBA_CH, (unsigned int)tx_dma[dma_len + i] << 24, 1, IMM_WRITE);

    bba_desel();

    unsigned char ncra = bba_in8(R_NCRA);
    bba_out8(R_NCRA, (ncra & ~(NCRA_ST0 | NCRA_ST1)) | NCRA_ST1);
}

/* Return TBR (Time Base) low 32 bits — increments at 1/4 the bus
 * clock = ~40.5 MHz on the GC. */
static unsigned int read_tbr(void)
{
    unsigned int tbl;
    __asm__ volatile("mftb %0" : "=r"(tbl));
    return tbl;
}

static void test_tx_speed(int pkt_size, int count)
{
    /* Fill a dummy packet — 60-byte minimum padded. */
    int i;
    if (pkt_size < 60) pkt_size = 60;
    if (pkt_size > 1500) pkt_size = 1500;
    for (i = 0; i < pkt_size; i++) tx_dma[i] = (unsigned char)i;

    print("\n== TX speed: ");
    putdec(count);
    print(" x ");
    putdec(pkt_size);
    print("-byte packets ==\n");

    /* Wait for any prior TX */
    int wait = 100000;
    while (wait > 0 && (bba_in8(R_NCRA) & NCRA_ST1)) wait--;
    bba_out8(R_IR, IR_TI);

    unsigned int t0 = read_tbr();

    for (i = 0; i < count; i++) {
        send_one_packet(pkt_size);
        /* Wait for THIS TX to complete (TI fires after wire xmit) */
        wait = 100000;
        while (wait > 0 && !(bba_in8(R_IR) & IR_TI)) wait--;
        bba_out8(R_IR, IR_TI);
    }

    unsigned int t1 = read_tbr();
    unsigned int elapsed_ticks = t1 - t0;
    /* TBR ticks are at ~40.5MHz (1/4 of 162MHz bus on GC).
     * Approximate time in microseconds: ticks / 40.5 ~= ticks * 25/1024 */
    unsigned int us = (elapsed_ticks + 20) / 41;

    print("  Elapsed: ");
    putdec(us);
    print(" us = ");
    putdec(us / 1000);
    print(" ms\n");
    print("  Bytes:   ");
    putdec(count * pkt_size);
    print(" (");
    putdec((count * pkt_size * 1000) / (us > 0 ? us : 1));
    print(" KB/s)\n");
    print("  Per pkt: ");
    putdec(us / (count > 0 ? count : 1));
    print(" us\n");
}

/* ===== Entry ===== */

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    exi_init();

    print("\n=== GameCube BBA diagnostics ===\n");

    /* Probe BBA presence */
    exi_select(BBA_CH, BBA_DEV, (0 << 4));   /* 1MHz for ID */
    exi_imm(BBA_CH, 0, 2, IMM_WRITE);
    unsigned int id = exi_imm(BBA_CH, 0, 4, IMM_READ);
    exi_deselect(BBA_CH);

    print("EXI ID @ ch=0 dev=2: 0x");
    puthex(id, 8);
    print("\n");
    if (id != BBA_ID) {
        print("Not a BBA (expected 0x04020200).  Aborting.\n");
        kl_syscall_fn sc = get_syscall();
        if (sc) sc(SYSCALL_EXIT, 0, 0, 0);
        return;
    }
    print("BBA confirmed.\n");

    /* Loader-style init test FIRST while chip is in clean state */
    loader_init_test();
    dump_regs("after loader-style init");

    /* Re-init for sweep tests */
    bba_init();
    dump_regs("post-fresh init (NWAYC = preserve only)");

    /* Sweep NWAYC configs */
    test_nwayc_config("preserve only (no ANE, no force)", 0x00);
    test_nwayc_config("ANE only (Swiss step 1)",          NWAYC_ANE);
    test_nwayc_config("ANE + ANS_RA (Swiss step 2)",      NWAYC_ANE | NWAYC_ANS_RA);
    test_nwayc_config("force 100FD (FD | PS100, no ANE)", NWAYC_FD | NWAYC_PS100_10);
    test_nwayc_config("force 10FD (FD only)",              NWAYC_FD);
    test_nwayc_config("FD | PS100 | ANE",                  NWAYC_FD | NWAYC_PS100_10 | NWAYC_ANE);
    test_nwayc_config("ALL ANE-related (FD|PS100|ANE|ANS_RA)",
                      NWAYC_FD | NWAYC_PS100_10 | NWAYC_ANE | NWAYC_ANS_RA);

    /* Best-link config: pick whatever yielded LS100 if any, else last */
    /* For simplicity, leave at FD | PS100 | ANE for the speed test */
    {
        unsigned char preserve = bba_in8(R_NWAYC) & 0xC0;
        bba_out8(R_NWAYC, preserve);
        udelay(100);
        bba_out8(R_NWAYC, preserve | NWAYC_ANE);
        udelay(100);
        bba_out8(R_NWAYC, preserve | NWAYC_FD | NWAYC_PS100_10 | NWAYC_ANE | NWAYC_ANS_RA);
        mdelay(2000);   /* settle */
    }
    dump_regs("after settle");

    /* Speed tests at multiple sizes */
    test_tx_speed(60,   100);
    test_tx_speed(512,  100);
    test_tx_speed(1500, 100);

    print("\n=== Done ===\n");
    kl_syscall_fn sc = get_syscall();
    if (sc) sc(SYSCALL_EXIT, 0, 0, 0);
}
