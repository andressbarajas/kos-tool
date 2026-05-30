/* examples/exi-probe/main.c */
/*
 * exi-probe - GameCube EXI bus probe utility
 *
 * Probes EXI channels 0 and 1 (device 0) and prints raw values
 * for device ID, USB Gecko TX status, and RX status commands.
 * Used to determine what a USB Gecko adapter actually reports
 * so detection logic can be tuned.
 *
 * GC-only example (EXI bus is GameCube/Wii specific).
 *
 * Load and run:
 *   kos-tool -x exi-probe.elf
 */

#define SYSCALL_WRITE 1
#define SYSCALL_EXIT  15

#if defined(__PPC__) || defined(__powerpc__)
#if defined(WII_KOSLOAD_BASE)
#define KOSLOAD_BASE WII_KOSLOAD_BASE
#elif defined(GC_KOSLOAD_BASE)
#define KOSLOAD_BASE GC_KOSLOAD_BASE
#else
#define KOSLOAD_BASE 0x817EC000
#endif
#else
#error "exi-probe is GameCube-only"
#endif

#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

static kosload_syscall_fn get_syscall(void) {
    if(KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kosload_syscall_fn)0;
    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
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

static int slen(const char *s) {
    int n = 0;
    while(*s++)
        n++;
    return n;
}

static void print(const char *msg) {
    kl_write(1, msg, slen(msg));
}

static void uint_to_hex(unsigned int val, char *buf) {
    static const char hex[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for(i = 0; i < 8; i++)
        buf[2 + i] = hex[(val >> (28 - i * 4)) & 0xf];
    buf[10] = '\0';
}

static void u16_to_hex(unsigned int val, char *buf) {
    static const char hex[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for(i = 0; i < 4; i++)
        buf[2 + i] = hex[(val >> (12 - i * 4)) & 0xf];
    buf[6] = '\0';
}

/* ===== Inline EXI access (no external dependencies) ===== */

#define EXI_BASE       0xCC006800
#define EXI_STATUS_OFF 0x00
#define EXI_DMA_CR_OFF 0x0C
#define EXI_DATA_OFF   0x10

#define EXI_ROMDIS   (1 << 13)
#define EXI_W1C_MASK ((1 << 1) | (1 << 3) | (1 << 11))

/* USB Gecko EXI commands */
#define GECKO_CMD_TX_STATUS 0xC0000000
#define GECKO_CMD_RX_STATUS 0xD0000000
#define GECKO_ID            0x01010000

/* Clock: 1 MHz for ID, 32 MHz for gecko commands */
#define CLK_1MHZ  (0 << 4)
#define CLK_32MHZ (5 << 4)

/* Transfer modes */
#define IMM_READ      (0 << 2)
#define IMM_WRITE     (1 << 2)
#define IMM_READWRITE (2 << 2)
#define DMA_START     (1 << 0)

static volatile unsigned int *exi_reg(int ch, int off) {
    return (volatile unsigned int *)(EXI_BASE + ch * 0x14 + off);
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
    unsigned int val = (1 << (7 + dev)) | (clk & (7 << 4));
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

static unsigned int exi_imm(int ch, unsigned int data, int len, int mode) {
    volatile unsigned int *cr = exi_reg(ch, EXI_DMA_CR_OFF);
    volatile unsigned int *imm = exi_reg(ch, EXI_DATA_OFF);

    *imm = data;
    *cr = ((len - 1) << 4) | mode | DMA_START;
    while(*cr & DMA_START)
        ;
    return *imm;
}

static unsigned int exi_get_id(int ch, int dev) {
    unsigned int id;
    exi_select(ch, dev, CLK_1MHZ);
    exi_imm(ch, 0x00000000, 2, IMM_WRITE);
    id = exi_imm(ch, 0x00000000, 4, IMM_READ);
    exi_deselect(ch);
    return id;
}

/* ===== Probe logic ===== */

static void probe_channel(int ch) {
    char hex32[12];
    char hex16[8];
    unsigned int id, tx_raw, rx_raw;
    unsigned int tx_status, rx_status;

    print(ch == 0 ? "--- EXI Channel 0 (Slot A) ---\n" : "--- EXI Channel 1 (Slot B) ---\n");

    /* 1. Standard EXI device ID */
    id = exi_get_id(ch, 0);
    print("  Device ID:      ");
    uint_to_hex(id, hex32);
    print(hex32);
    if(id == GECKO_ID)
        print("  <-- matches USBGECKO_ID");
    else if(id == 0xFFFFFFFF)
        print("  (empty slot / no device)");
    print("\n");

    /* 2. USB Gecko TX status command */
    exi_select(ch, 0, CLK_32MHZ);
    tx_raw = exi_imm(ch, GECKO_CMD_TX_STATUS, 2, IMM_READWRITE);
    exi_deselect(ch);
    tx_status = (tx_raw >> 16) & 0xFFFF;

    print("  TX status raw:  ");
    uint_to_hex(tx_raw, hex32);
    print(hex32);
    print("  upper16=");
    u16_to_hex(tx_status, hex16);
    print(hex16);
    if(tx_status & 0x0400)
        print("  TX_READY");
    print("\n");

    /* 3. USB Gecko RX status command */
    exi_select(ch, 0, CLK_32MHZ);
    rx_raw = exi_imm(ch, GECKO_CMD_RX_STATUS, 2, IMM_READWRITE);
    exi_deselect(ch);
    rx_status = (rx_raw >> 16) & 0xFFFF;

    print("  RX status raw:  ");
    uint_to_hex(rx_raw, hex32);
    print(hex32);
    print("  upper16=");
    u16_to_hex(rx_status, hex16);
    print(hex16);
    if(rx_status & 0x0400)
        print("  RX_READY");
    print("\n");

    /* 4. EXI channel status register (device present bit) */
    {
        unsigned int csr = *exi_reg(ch, EXI_STATUS_OFF);
        print("  EXI CSR:        ");
        uint_to_hex(csr, hex32);
        print(hex32);
        if(csr & (1 << 12))
            print("  EXT=1 (device present)");
        else
            print("  EXT=0 (no device)");
        print("\n");
    }

    /* 5. Summary */
    print("  Detection: ");
    if(id == GECKO_ID) {
        print("ID match -> USB Gecko confirmed\n");
    } else if((tx_status & ~0x0400) == 0 && (rx_status & ~0x0400) == 0) {
        print("status check -> likely USB Gecko (ID mismatch)\n");
    } else if(tx_status == 0xFFFF && rx_status == 0xFFFF) {
        print("empty slot\n");
    } else {
        print("unknown device\n");
    }
    print("\n");
}

void start(void) __attribute__((section(".text.start")));
void start(void) {
    exi_init();

    print("\n");
    print("=== EXI Bus Probe ===\n");
    print("Probing EXI channels for USB Gecko...\n\n");

    probe_channel(0);
    probe_channel(1);

    print("=== Done ===\n\n");

    kl_exit();
}
