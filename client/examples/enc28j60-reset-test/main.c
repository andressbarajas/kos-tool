/* examples/enc28j60-reset-test/main.c */
/*
 * ENC28J60 reset/wake diagnostic for GameCube Slot A.
 *
 * Load this through gc-load-serial with the USB Gecko in Slot B and the
 * GCNet/ENC28J60 adapter in Slot A. The program talks to the ENC directly
 * over EXI, without using the normal network stack, so reset failures are
 * easier to separate from DHCP or packet code.
 */

#define SYSCALL_WRITE     1
#define SYSCALL_EXIT      15

#if defined(__PPC__) || defined(__powerpc__)
#ifdef GC_KOSLOAD_BASE
#define KOSLOAD_BASE    GC_KOSLOAD_BASE
#else
#define KOSLOAD_BASE    0x817EC000
#endif
#else
#error "enc28j60-reset-test is GameCube-only"
#endif

#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

static kosload_syscall_fn get_syscall(void) {
    if(KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kosload_syscall_fn)0;
    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

static int slen(const char *s) {
    int n = 0;
    while(*s++)
        n++;
    return n;
}

static int kl_write(int fd, const void *buf, int count) {
    kosload_syscall_fn syscall = get_syscall();
    if(!syscall)
        return -1;
    return syscall(SYSCALL_WRITE, fd, (int)buf, count);
}

static void kl_exit(void) {
    kosload_syscall_fn syscall = get_syscall();
    if(syscall)
        syscall(SYSCALL_EXIT, 0, 0, 0);
}

static void print(const char *msg) {
    kl_write(1, msg, slen(msg));
}

static void hex8(u8 val, char *buf) {
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    buf[2] = hex[(val >> 4) & 0x0f];
    buf[3] = hex[val & 0x0f];
    buf[4] = '\0';
}

static void hex16(u16 val, char *buf) {
    static const char hex[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for(i = 0; i < 4; i++)
        buf[2 + i] = hex[(val >> (12 - i * 4)) & 0x0f];
    buf[6] = '\0';
}

static void hex32(u32 val, char *buf) {
    static const char hex[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for(i = 0; i < 8; i++)
        buf[2 + i] = hex[(val >> (28 - i * 4)) & 0x0f];
    buf[10] = '\0';
}

static void print_hex8(u8 val) {
    char buf[5];
    hex8(val, buf);
    print(buf);
}

static void print_hex16(u16 val) {
    char buf[7];
    hex16(val, buf);
    print(buf);
}

static void print_hex32(u32 val) {
    char buf[11];
    hex32(val, buf);
    print(buf);
}

static void print_bool(int value) {
    print(value ? "yes" : "no");
}

static int failures;
static int exi_timeouts;

static void result(const char *name, int pass) {
    if(pass)
        print("[PASS] ");
    else {
        print("[FAIL] ");
        failures++;
    }
    print(name);
    print("\n");
}

static void info_hex8(const char *name, u8 val) {
    print("  ");
    print(name);
    print("=");
    print_hex8(val);
}

static void info_hex16(const char *name, u16 val) {
    print("  ");
    print(name);
    print("=");
    print_hex16(val);
}

/* ===== Inline EXI access ===== */

#define EXI_BASE        0xCC006800
#define EXI_STATUS_OFF  0x00
#define EXI_DMA_CR_OFF  0x0C
#define EXI_DATA_OFF    0x10

#define EXI_ROMDIS      (1 << 13)
#define EXI_W1C_MASK    ((1 << 1) | (1 << 3) | (1 << 11))

#define CLK_1MHZ        (0 << 4)
#define CLK_16MHZ       (4 << 4)

#define IMM_READ        (0 << 2)
#define IMM_WRITE       (1 << 2)
#define IMM_READWRITE   (2 << 2)
#define DMA_START       (1 << 0)
#define EXI_WAIT_SPINS  1000000

#define ENC_CH          0
#define ENC_DEV         0
#define ENC_CLK         CLK_16MHZ

static volatile u32 *exi_reg(int ch, int off) {
    return (volatile u32 *)(EXI_BASE + ch * 0x14 + off);
}

static void exi_init(void) {
    int ch;
    for(ch = 0; ch < 3; ch++) {
        *exi_reg(ch, EXI_DMA_CR_OFF) = 0;
        if(ch == 0)
            *exi_reg(ch, EXI_STATUS_OFF) = EXI_W1C_MASK | EXI_ROMDIS;
        else
            *exi_reg(ch, EXI_STATUS_OFF) = EXI_W1C_MASK;
    }
}

static void exi_select(int ch, int dev, int clk) {
    u32 val = (1 << (7 + dev)) | (clk & (7 << 4));
    if(ch == 0)
        val |= EXI_ROMDIS;
    *exi_reg(ch, EXI_STATUS_OFF) = val;
}

static void exi_deselect(int ch) {
    if(ch == 0)
        *exi_reg(ch, EXI_STATUS_OFF) = EXI_ROMDIS;
    else
        *exi_reg(ch, EXI_STATUS_OFF) = 0;
}

static u32 exi_imm(int ch, u32 data, int len, int mode) {
    volatile u32 *cr = exi_reg(ch, EXI_DMA_CR_OFF);
    volatile u32 *imm = exi_reg(ch, EXI_DATA_OFF);
    int timeout = EXI_WAIT_SPINS;

    *imm = data;
    *cr = ((len - 1) << 4) | mode | DMA_START;
    while(*cr & DMA_START) {
        if(--timeout == 0) {
            *cr = 0;
            exi_timeouts++;
            return 0;
        }
    }
    return *imm;
}

static u32 exi_get_id(int ch, int dev) {
    u32 id;
    exi_select(ch, dev, CLK_1MHZ);
    exi_imm(ch, 0x00000000, 2, IMM_WRITE);
    id = exi_imm(ch, 0x00000000, 4, IMM_READ);
    exi_deselect(ch);
    return id;
}

/* ===== ENC28J60 constants ===== */

#define ENC28J60_EXI_ID             0xFA050000
#define ENC28J60_PHID1_EXPECTED     0x0083
#define ENC28J60_PHID2_EXPECTED     0x1400

#define ENC_CMD_RCR(reg)            (((0x00 | ((reg) & 0x1F)) << 24))
#define ENC_CMD_WCR(reg)            (((0x40 | ((reg) & 0x1F)) << 24))
#define ENC_CMD_BFS(reg)            (((0x80 | ((reg) & 0x1F)) << 24))
#define ENC_CMD_BFC(reg)            (((0xA0 | ((reg) & 0x1F)) << 24))
#define ENC_CMD_SRC                 (0xFF << 24)

#define ENC_BANK(addr)              (((addr) >> 5) & 0x03)
#define ENC_ADDR(addr)              ((addr) & 0x1F)
#define ENC_IS_MAC_MII(addr)        ((addr) & 0x80)

#define ERDPTL   0x00
#define ERDPTH   0x01
#define EWRPTL   0x02
#define EWRPTH   0x03
#define ETXSTL   0x04
#define ETXSTH   0x05
#define ETXNDL   0x06
#define ETXNDH   0x07
#define ERXSTL   0x08
#define ERXSTH   0x09
#define ERXNDL   0x0A
#define ERXNDH   0x0B
#define ERXRDPTL 0x0C
#define ERXRDPTH 0x0D
#define ERXFCON  0x38
#define EPKTCNT  0x39
#define MACON1   (0x40 | 0x80)
#define MACON2   (0x41 | 0x80)
#define MACON3   (0x42 | 0x80)
#define MACON4   (0x43 | 0x80)
#define MABBIPG  (0x44 | 0x80)
#define MAIPGL   (0x46 | 0x80)
#define MAIPGH   (0x47 | 0x80)
#define MAMXFLL  (0x4A | 0x80)
#define MICMD    (0x52 | 0x80)
#define MIREGADR (0x54 | 0x80)
#define MIWRL    (0x56 | 0x80)
#define MIWRH    (0x57 | 0x80)
#define MIRDL    (0x58 | 0x80)
#define MIRDH    (0x59 | 0x80)
#define MAADR5   (0x60 | 0x80)
#define MAADR6   (0x61 | 0x80)
#define MAADR3   (0x62 | 0x80)
#define MAADR4   (0x63 | 0x80)
#define MAADR1   (0x64 | 0x80)
#define MAADR2   (0x65 | 0x80)
#define MISTAT   (0x6A | 0x80)
#define EREVID   0x72
#define ECOCON   0x75
#define EIE      0x1B
#define EIR      0x1C
#define ESTAT    0x1D
#define ECON2    0x1E
#define ECON1    0x1F

#define ECON1_BSEL0    (1 << 0)
#define ECON1_BSEL1    (1 << 1)
#define ECON1_RXEN     (1 << 2)
#define ECON1_TXRTS    (1 << 3)
#define ECON1_RXRST    (1 << 6)
#define ECON1_TXRST    (1 << 7)
#define ECON2_PWRSV    (1 << 5)
#define ECON2_AUTOINC  (1 << 7)
#define ESTAT_CLKRDY   (1 << 0)
#define EIR_RXERIF     (1 << 0)
#define EIR_TXERIF     (1 << 1)
#define EIR_TXIF       (1 << 3)
#define EIR_LINKIF     (1 << 4)
#define EIR_DMAIF      (1 << 5)
#define EIR_PKTIF      (1 << 6)
#define ERXFCON_BCEN   (1 << 0)
#define ERXFCON_CRCEN  (1 << 5)
#define ERXFCON_UCEN   (1 << 7)
#define MACON1_MARXEN  (1 << 0)
#define MACON1_RXPAUS  (1 << 2)
#define MACON1_TXPAUS  (1 << 3)
#define MACON3_FRMLNEN (1 << 1)
#define MACON3_TXCRCEN (1 << 4)
#define MACON3_PADCFG0 (1 << 5)
#define MACON4_DEFER   (1 << 6)
#define MICMD_MIIRD    (1 << 0)
#define MISTAT_BUSY    (1 << 0)

#define PHCON1  0x00
#define PHID1   0x02
#define PHID2   0x03
#define PHCON2  0x10
#define PHSTAT2 0x11
#define PHIE    0x12
#define PHLCON  0x14

#define PHCON1_PPWRSV  (1 << 11)
#define PHCON1_PLOOPBK (1 << 14)
#define PHCON2_HDLDIS  (1 << 8)
#define PHSTAT2_LSTAT  (1 << 10)
#define PHIE_PGEIE     (1 << 1)
#define PHIE_PLNKIE    (1 << 4)

#define ENC_RX_START  0x0000
#define ENC_RX_END    0x0FFF
#define ENC_TX_START  0x1000
#define ENC_MAX_FRAME 1518

static u8 current_bank;
static u8 runtime_mac[6] = { 0x00, 0x09, 0xBF, 0x12, 0x34, 0x56 };

/* GameCube Time Base Register: 40.5 MHz. Low 32 bits are enough for
 * the short delays used by this diagnostic. */
#define TB_TICKS_PER_MS  40500
#define TB_TICKS_PER_US  41

static u32 get_tbl(void) {
    u32 tbl;
    __asm__ volatile("mftb %0" : "=r"(tbl));
    return tbl;
}

static void delay_ticks(u32 ticks) {
    u32 start = get_tbl();
    while((u32)(get_tbl() - start) < ticks)
        ;
}

static void delay_us(u32 us) {
    delay_ticks(us * TB_TICKS_PER_US);
}

static void delay_ms(u32 ms) {
    while(ms-- > 0)
        delay_ticks(TB_TICKS_PER_MS);
}

static void enc_select(void) {
    exi_select(ENC_CH, ENC_DEV, ENC_CLK);
}

static void enc_deselect(void) {
    exi_deselect(ENC_CH);
}

static void enc_raw_clear_bank_bits(void) {
    enc_select();
    exi_imm(ENC_CH, ENC_CMD_BFC(ECON1) | ((u32)(ECON1_BSEL0 | ECON1_BSEL1) << 16), 2, IMM_WRITE);
    enc_deselect();
    current_bank = 0;
}

static void enc_set_bank(u8 reg) {
    u8 addr = ENC_ADDR(reg);
    u8 bank;

    if(addr >= 0x1B)
        return;

    bank = ENC_BANK(reg);
    if(bank == current_bank)
        return;

    enc_raw_clear_bank_bits();

    if(bank) {
        enc_select();
        exi_imm(ENC_CH, ENC_CMD_BFS(ECON1) | ((u32)bank << 16), 2, IMM_WRITE);
        enc_deselect();
    }

    current_bank = bank;
}

static u8 enc_read_reg(u8 reg) {
    u32 val;
    int len;

    enc_set_bank(reg);
    len = ENC_IS_MAC_MII(reg) ? 3 : 2;

    enc_select();
    val = exi_imm(ENC_CH, ENC_CMD_RCR(ENC_ADDR(reg)), len, IMM_READWRITE);
    enc_deselect();

    if(len == 3)
        return (val >> 8) & 0xff;
    return (val >> 16) & 0xff;
}

static void enc_write_reg(u8 reg, u8 val) {
    enc_set_bank(reg);
    enc_select();
    exi_imm(ENC_CH, ENC_CMD_WCR(ENC_ADDR(reg)) | ((u32)val << 16), 2, IMM_WRITE);
    enc_deselect();
}

static void enc_set_bits(u8 reg, u8 mask) {
    enc_set_bank(reg);
    enc_select();
    exi_imm(ENC_CH, ENC_CMD_BFS(ENC_ADDR(reg)) | ((u32)mask << 16), 2, IMM_WRITE);
    enc_deselect();
}

static void enc_clear_bits(u8 reg, u8 mask) {
    enc_set_bank(reg);
    enc_select();
    exi_imm(ENC_CH, ENC_CMD_BFC(ENC_ADDR(reg)) | ((u32)mask << 16), 2, IMM_WRITE);
    enc_deselect();
}

static u16 enc_read_reg16(u8 reg_l) {
    u16 val = enc_read_reg(reg_l);
    val |= (u16)enc_read_reg(reg_l + 1) << 8;
    return val;
}

static void enc_capture_runtime_mac(void) {
    u8 mac[6];
    int i;
    int useful = 0;

    enc_raw_clear_bank_bits();
    mac[0] = enc_read_reg(MAADR1);
    mac[1] = enc_read_reg(MAADR2);
    mac[2] = enc_read_reg(MAADR3);
    mac[3] = enc_read_reg(MAADR4);
    mac[4] = enc_read_reg(MAADR5);
    mac[5] = enc_read_reg(MAADR6);

    for(i = 0; i < 6; i++) {
        if(mac[i] != 0x00 && mac[i] != 0xff)
            useful = 1;
    }

    if(useful) {
        for(i = 0; i < 6; i++)
            runtime_mac[i] = mac[i];
    }
}

static void enc_write_runtime_mac(void) {
    enc_write_reg(MAADR1, runtime_mac[0]);
    enc_write_reg(MAADR2, runtime_mac[1]);
    enc_write_reg(MAADR3, runtime_mac[2]);
    enc_write_reg(MAADR4, runtime_mac[3]);
    enc_write_reg(MAADR5, runtime_mac[4]);
    enc_write_reg(MAADR6, runtime_mac[5]);
}

static void enc_write_reg16(u8 reg_l, u16 val) {
    enc_write_reg(reg_l, val & 0xff);
    enc_write_reg(reg_l + 1, val >> 8);
}

static void enc_raw_soft_reset(void) {
    exi_select(ENC_CH, ENC_DEV, CLK_1MHZ);
    exi_imm(ENC_CH, ENC_CMD_SRC, 1, IMM_WRITE);
    exi_deselect(ENC_CH);
    delay_ms(5);
    current_bank = 0;
}

static void enc_wake_and_reset(void) {
    exi_select(ENC_CH, ENC_DEV, CLK_1MHZ);
    exi_imm(ENC_CH, ENC_CMD_BFC(ECON2) | ((u32)ECON2_PWRSV << 16), 2, IMM_WRITE);
    exi_deselect(ENC_CH);

    delay_ms(2);
    enc_raw_soft_reset();
}

static int enc_phy_read(u8 reg, u16 *out) {
    int timeout;

    enc_write_reg(MIREGADR, reg);
    enc_write_reg(MICMD, MICMD_MIIRD);
    delay_us(15);

    timeout = 10000;
    while(enc_read_reg(MISTAT) & MISTAT_BUSY) {
        if(--timeout == 0) {
            enc_write_reg(MICMD, 0x00);
            return 0;
        }
        delay_us(10);
    }

    enc_write_reg(MICMD, 0x00);
    delay_us(1);
    *out = enc_read_reg(MIRDL);
    *out |= (u16)enc_read_reg(MIRDH) << 8;
    return 1;
}

static int enc_phy_write(u8 reg, u16 val) {
    int timeout;

    enc_write_reg(MIREGADR, reg);
    enc_write_reg(MIWRL, val & 0xff);
    enc_write_reg(MIWRH, val >> 8);
    delay_us(15);

    timeout = 10000;
    while(enc_read_reg(MISTAT) & MISTAT_BUSY) {
        if(--timeout == 0)
            return 0;
        delay_us(10);
    }

    return 1;
}

static int enc_phy_id_ok(u16 *phid1, u16 *phid2) {
    if(!enc_phy_read(PHID1, phid1))
        return 0;
    if(!enc_phy_read(PHID2, phid2))
        return 0;
    return *phid1 == ENC28J60_PHID1_EXPECTED && (*phid2 & 0xFC00) == (ENC28J60_PHID2_EXPECTED & 0xFC00);
}

static void dump_snapshot(const char *label) {
    u16 phid1 = 0;
    u16 phid2 = 0;
    u16 phcon1 = 0;
    u16 phcon2 = 0;
    u16 phstat2 = 0;

    enc_raw_clear_bank_bits();

    print(label);
    print("\n  exi_id=");
    print_hex32(exi_get_id(ENC_CH, ENC_DEV));
    info_hex8("ESTAT", enc_read_reg(ESTAT));
    info_hex8("EIR", enc_read_reg(EIR));
    info_hex8("ECON1", enc_read_reg(ECON1));
    info_hex8("ECON2", enc_read_reg(ECON2));
    info_hex8("EPKTCNT", enc_read_reg(EPKTCNT));
    info_hex8("EREVID", enc_read_reg(EREVID));
    print("\n");

    info_hex16("ERDPT", enc_read_reg16(ERDPTL));
    info_hex16("ERXRDPT", enc_read_reg16(ERXRDPTL));
    info_hex16("EWRPT", enc_read_reg16(EWRPTL));
    print("\n");

    if(enc_phy_read(PHID1, &phid1) && enc_phy_read(PHID2, &phid2) && enc_phy_read(PHCON1, &phcon1) &&
       enc_phy_read(PHCON2, &phcon2) && enc_phy_read(PHSTAT2, &phstat2)) {
        info_hex16("PHID1", phid1);
        info_hex16("PHID2", phid2);
        info_hex16("PHCON1", phcon1);
        info_hex16("PHCON2", phcon2);
        info_hex16("PHSTAT2", phstat2);
        print("  link=");
        print_bool((phstat2 & PHSTAT2_LSTAT) != 0);
        print("\n");
    } else {
        print("  PHY read timed out\n");
    }
}

static int enc_wait_link(void) {
    int timeout = 1200;
    u16 phstat2 = 0;

    while(timeout-- > 0) {
        if(enc_phy_read(PHSTAT2, &phstat2) && (phstat2 & PHSTAT2_LSTAT))
            return 1;
        delay_ms(1);
    }

    return 0;
}

static int enc_driver_like_init(int *link_up) {
    u16 phid1 = 0;
    u16 phid2 = 0;

    current_bank = 0;
    enc_wake_and_reset();
    enc_raw_clear_bank_bits();

    if(!enc_phy_id_ok(&phid1, &phid2))
        return 0;

    enc_write_reg16(ERXSTL, ENC_RX_START);
    enc_write_reg16(ERXNDL, ENC_RX_END);
    enc_write_reg16(ERXRDPTL, ENC_RX_END);
    enc_write_reg16(ERDPTL, ENC_RX_START);
    enc_write_reg16(ETXSTL, ENC_TX_START);
    enc_write_reg16(EWRPTL, ENC_TX_START);
    enc_set_bits(ECON2, ECON2_AUTOINC);

    enc_write_reg(ERXFCON, ERXFCON_UCEN | ERXFCON_BCEN | ERXFCON_CRCEN);
    enc_write_reg(MACON1, MACON1_MARXEN | MACON1_TXPAUS | MACON1_RXPAUS);
    enc_write_reg(MACON2, 0x00);
    enc_write_reg(MACON3, MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN);
    enc_write_reg(MACON4, MACON4_DEFER);
    enc_write_reg16(MAMXFLL, ENC_MAX_FRAME);
    enc_write_reg(MABBIPG, 0x12);
    enc_write_reg(MAIPGL, 0x12);
    enc_write_reg(MAIPGH, 0x0C);

    enc_write_runtime_mac();

    if(!enc_phy_write(PHCON1, 0x0000))
        return 0;
    if(!enc_phy_write(PHCON2, PHCON2_HDLDIS))
        return 0;
    if(!enc_phy_write(PHLCON, 0x3C12))
        return 0;
    if(!enc_phy_write(PHIE, PHIE_PLNKIE | PHIE_PGEIE))
        return 0;

    enc_clear_bits(EIR, EIR_RXERIF | EIR_TXERIF | EIR_TXIF | EIR_LINKIF | EIR_DMAIF | EIR_PKTIF);
    enc_write_reg(ECOCON, 0x00);

    *link_up = enc_wait_link();
    enc_raw_clear_bank_bits();
    enc_set_bits(ECON1, ECON1_RXEN);
    return 1;
}

static void enc_stop_runtime(void) {
    enc_raw_clear_bank_bits();
    enc_clear_bits(ECON1, ECON1_RXEN | ECON1_TXRTS);
}

static void test_dirty_id_reset(void) {
    u32 id_fresh;
    u32 id_dirty;
    u32 id_reset;
    int restore_ok;
    int link_up = 0;

    print("\n-- Dirty ERDPT / EXI ID reset --\n");
    enc_wake_and_reset();
    enc_raw_clear_bank_bits();
    id_fresh = exi_get_id(ENC_CH, ENC_DEV);
    enc_write_reg16(ERDPTL, 0x1234);
    id_dirty = exi_get_id(ENC_CH, ENC_DEV);
    enc_wake_and_reset();
    enc_raw_clear_bank_bits();
    id_reset = exi_get_id(ENC_CH, ENC_DEV);
    restore_ok = enc_driver_like_init(&link_up);

    print("  fresh id=");
    print_hex32(id_fresh);
    print(" dirty id=");
    print_hex32(id_dirty);
    print(" reset id=");
    print_hex32(id_reset);
    print(" restore=");
    print_bool(restore_ok);
    print(" link=");
    print_bool(link_up);
    print("\n");

    result("wake reset restores EXI ID after dirty ERDPT", restore_ok && id_reset == ENC28J60_EXI_ID);
}

static void test_power_save_reset(void) {
    u8 econ2_after_raw;
    u16 phid1 = 0;
    u16 phid2 = 0;
    int recover_ok;
    int restore_ok;
    int link_up = 0;

    print("\n-- ECON2.PWRSV reset behavior --\n");
    enc_wake_and_reset();
    enc_raw_clear_bank_bits();
    enc_set_bits(ECON2, ECON2_PWRSV);
    delay_ms(2);
    enc_raw_soft_reset();
    enc_raw_clear_bank_bits();
    econ2_after_raw = enc_read_reg(ECON2);

    enc_wake_and_reset();
    enc_raw_clear_bank_bits();
    recover_ok = enc_phy_id_ok(&phid1, &phid2);
    restore_ok = enc_driver_like_init(&link_up);

    print("  ECON2 after raw SRC while asleep=");
    print_hex8(econ2_after_raw);
    print("  PWRSV=");
    print_bool((econ2_after_raw & ECON2_PWRSV) != 0);
    print("\n");

    print("  recovered PHY=");
    print_bool(recover_ok);
    print(" restore=");
    print_bool(restore_ok);
    print(" link=");
    print_bool(link_up);
    print("\n");

    result("wake reset recovers PHY after PWRSV", recover_ok && restore_ok);
}

static void test_phy_cleanup(void) {
    int link_up = 0;
    u16 phcon1 = 0;
    u16 phcon2 = 0;
    int init_ok;

    print("\n-- PHY power-save / loopback cleanup --\n");
    enc_wake_and_reset();
    enc_raw_clear_bank_bits();

    enc_phy_write(PHCON1, PHCON1_PPWRSV | PHCON1_PLOOPBK);
    delay_ms(2);
    enc_phy_read(PHCON1, &phcon1);

    init_ok = enc_driver_like_init(&link_up);
    enc_phy_read(PHCON1, &phcon1);
    enc_phy_read(PHCON2, &phcon2);

    print("  after init PHCON1=");
    print_hex16(phcon1);
    print(" PHCON2=");
    print_hex16(phcon2);
    print(" link=");
    print_bool(link_up);
    print("\n");

    result("driver-like init clears PHCON1 power-save/loopback",
           init_ok && ((phcon1 & (PHCON1_PPWRSV | PHCON1_PLOOPBK)) == 0));
    result("driver-like init disables half-duplex loopback", init_ok && ((phcon2 & PHCON2_HDLDIS) != 0));
    enc_stop_runtime();
}

static void test_reinit_cycles(void) {
    int cycle;
    int all_init = 1;
    int all_stop = 1;
    int any_link = 0;
    int all_restore = 1;

    print("\n-- Repeated init / stop cycles --\n");

    for(cycle = 0; cycle < 6; cycle++) {
        int link_up = 0;
        int restore_link = 0;
        int restore_ok;
        int init_ok;
        u8 econ1_live;
        u8 econ2_live;
        u8 econ1_stopped;
        u16 phcon1 = 0;

        print("  cycle ");
        print_hex8((u8)(cycle + 1));
        print(": ");

        init_ok = enc_driver_like_init(&link_up);
        econ1_live = enc_read_reg(ECON1);
        econ2_live = enc_read_reg(ECON2);
        enc_phy_read(PHCON1, &phcon1);
        enc_stop_runtime();
        econ1_stopped = enc_read_reg(ECON1);
        restore_ok = enc_driver_like_init(&restore_link);

        if(!init_ok || (econ1_live & ECON1_RXEN) == 0 ||
           (econ1_live & (ECON1_TXRTS | ECON1_TXRST | ECON1_RXRST)) != 0 || (econ2_live & ECON2_PWRSV) != 0 ||
           (phcon1 & (PHCON1_PPWRSV | PHCON1_PLOOPBK)) != 0) {
            all_init = 0;
        }

        if(econ1_stopped & (ECON1_RXEN | ECON1_TXRTS))
            all_stop = 0;

        if(!restore_ok)
            all_restore = 0;

        if(link_up)
            any_link = 1;

        print("init=");
        print_bool(init_ok);
        print(" live ECON1=");
        print_hex8(econ1_live);
        print(" ECON2=");
        print_hex8(econ2_live);
        print(" PHCON1=");
        print_hex16(phcon1);
        print(" link=");
        print_bool(link_up);
        print(" stopped ECON1=");
        print_hex8(econ1_stopped);
        print(" restore=");
        print_bool(restore_ok);
        print("\n");
    }

    result("all repeated init cycles leave runtime state sane", all_init);
    result("all stop cycles clear RXEN/TXRTS", all_stop);
    result("all cycles restore console-safe runtime state", all_restore);
    if(any_link)
        result("link observed during at least one cycle", 1);
    else
        print("[WARN] link was never observed; check cable/router if "
              "unexpected\n");
}

void start(void) __attribute__((section(".text.start")));
void start(void) {
    u32 csr;
    int final_link = 0;

    failures = 0;
    exi_timeouts = 0;
    exi_init();

    print("\n");
    print("=== ENC28J60 Reset/Wake Diagnostic ===\n");
    print("Target: EXI channel 0 device 0 (GameCube Slot A)\n");

    csr = *exi_reg(ENC_CH, EXI_STATUS_OFF);
    print("Initial EXI CSR=");
    print_hex32(csr);
    print(" EXT=");
    print_bool((csr & (1 << 12)) != 0);
    print("\n");

    enc_capture_runtime_mac();
    dump_snapshot("Initial snapshot");
    test_dirty_id_reset();
    test_power_save_reset();
    test_phy_cleanup();
    test_reinit_cycles();
    enc_driver_like_init(&final_link);
    dump_snapshot("Final restored snapshot");
    result("no EXI transfer timeouts", exi_timeouts == 0);

    print("\nSummary failures=");
    print_hex8((u8)failures);
    print(" EXI timeouts=");
    print_hex8((u8)exi_timeouts);
    print("\n");
    if(failures == 0)
        print("=== ENC28J60 diagnostic PASS ===\n\n");
    else
        print("=== ENC28J60 diagnostic FAIL ===\n\n");

    kl_exit();
}
