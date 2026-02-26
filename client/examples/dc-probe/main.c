/* examples/dc-probe/main.c */
/*
 * dc-probe - Dreamcast hardware probe utility
 *
 * Probes and displays key DC hardware information:
 *   - Cable type (VGA/RGB/Composite)
 *   - RAM size (16MB/32MB)
 *   - SCIF serial port status
 *   - Expansion port (BBA/LAN/Modem)
 *   - Maple bus devices (all 4 ports)
 *
 * DC-only example.
 *
 * Load and run:
 *   kos-tool -x dc-probe.elf
 */

#if !defined(__sh__) && !defined(__SH4_SINGLE__)
#error "dc-probe is Dreamcast-only"
#endif

#define SYSCALL_WRITE     1
#define SYSCALL_EXIT      15

#define KOSLOAD_BASE        0x8c004000
#define KOSLOAD_MAGIC_ADDR  (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC       0xdeadbeef

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

static kosload_syscall_fn get_syscall(void)
{
    if (KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kosload_syscall_fn)0;
    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

static int kl_write(int fd, const void *buf, int count)
{
    kosload_syscall_fn syscall = get_syscall();
    if (!syscall) return -1;
    return syscall(SYSCALL_WRITE, fd, (int)buf, count);
}

static void kl_exit(void)
{
    kosload_syscall_fn syscall = get_syscall();
    if (syscall)
        syscall(SYSCALL_EXIT, 0, 0, 0);
}

static int slen(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

static void print(const char *msg)
{
    kl_write(1, msg, slen(msg));
}

static void uint_to_hex(unsigned int val, char *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hex[(val >> (28 - i * 4)) & 0xf];
    buf[10] = '\0';
}

static void print_dec(unsigned int val)
{
    char buf[12];
    int i = 0;
    if (val == 0) {
        print("0");
        return;
    }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    /* reverse */
    {
        char out[12];
        int j;
        for (j = 0; j < i; j++)
            out[j] = buf[i - 1 - j];
        out[i] = '\0';
        print(out);
    }
}

static void print_hex_byte(unsigned char b)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hex[(b >> 4) & 0xf];
    buf[1] = hex[b & 0xf];
    buf[2] = '\0';
    print(buf);
}

static void print_mac(const unsigned char *mac)
{
    int i;
    for (i = 0; i < 6; i++) {
        if (i > 0) print(":");
        print_hex_byte(mac[i]);
    }
}

/* ===== SCIF serial port ===== */
/*
 * SH4 SCIF registers. Always present on Dreamcast.
 * We just read status - don't touch configuration since
 * dcload-serial is using SCIF for our own I/O.
 */
#define SCBRR2   (*(volatile unsigned char  *)0xffe80004)
#define SCSCR2   (*(volatile unsigned short *)0xffe80008)
#define SCFSR2   (*(volatile unsigned short *)0xffe80010)
#define SCFDR2   (*(volatile unsigned short *)0xffe8001c)

static void probe_scif(void)
{
    unsigned short fsr, fdr, scr;
    unsigned char brr;

    print("--- SCIF Serial ---\n");

    scr = SCSCR2;
    brr = SCBRR2;

    print("  Clock:     ");
    if (scr & 0x02) {
        print("External");
    } else {
        /* Internal clock, Pclk=50MHz: baud = 50M / (32*(brr+1)) */
        /* Common BRR values for 50 MHz Pclk */
        static const struct { unsigned char brr; const char *name; } bauds[] = {
            {  0, "1562500" },
            {  2, "520833" },
            { 12, "120192 (~115200)" },
            { 26, "57870 (~57600)" },
        };
        int i, found = 0;
        for (i = 0; i < 4; i++) {
            if (brr == bauds[i].brr) {
                print(bauds[i].name);
                found = 1;
                break;
            }
        }
        if (!found)
            print("(unknown)");
    }
    print(" BRR=");
    print_dec(brr);
    print("\n");

    fsr = SCFSR2;
    print("  Status:    ");
    print((fsr & 0x40) ? "TEND " : "");
    print((fsr & 0x20) ? "TDFE " : "");
    print((fsr & 0x02) ? "RDF " : "");
    print((fsr & 0x01) ? "DR " : "");
    {
        char hex[12];
        print("[");
        uint_to_hex(fsr, hex);
        print(hex);
        print("]\n");
    }

    fdr = SCFDR2;
    print("  TX FIFO:   ");
    print_dec((fdr >> 8) & 0x1f);
    print("/16  RX FIFO: ");
    print_dec(fdr & 0x1f);
    print("/16\n");

    print("\n");
}

/* ===== Expansion port (BBA / LAN Adapter / Modem) ===== */
/*
 * The rear expansion port can hold:
 *   - Broadband Adapter (BBA): RTL8139 via GAPS PCI bridge
 *   - LAN Adapter (HIT-0300): MB86967 Ethernet
 *   - 56K Modem: Conexant (stock in most DC models)
 *   - Empty (some late revisions)
 *
 * BBA is on the G2 PCI bus with a GAPS bridge.
 * LAN adapter and modem share the same G2 base address.
 */

#define GAPS_IDENT_BASE     0xa1001400  /* 16-byte ASCII string */
#define NIC8139_BASE        0xa1001700  /* RTL8139 PCI I/O space */
#define RT_IDR0             0x00        /* MAC address offset (6 bytes) */

#define LA_BASE             0xa0600000
#define LA_REG(x)           (*(volatile unsigned char *)(LA_BASE + (x)*4 + 0x400))
#define LA_RESET            (*(volatile unsigned char *)(LA_BASE + 0x0480))

static int check_gaps_bridge(void)
{
    /* GAPSPCI_BRIDGE_2 as 4 x uint32 (big-endian ASCII) */
    static const unsigned int gaps_sig[4] = {
        0x47415053, 0x5043495f, 0x42524944, 0x47455f32
    };
    volatile unsigned int *ident = (volatile unsigned int *)GAPS_IDENT_BASE;
    int i;

    for (i = 0; i < 4; i++) {
        if (ident[i] != gaps_sig[i])
            return 0;
    }
    return 1;
}

static void probe_bba(void)
{
    volatile unsigned char *nic = (volatile unsigned char *)NIC8139_BASE;
    unsigned char mac[6];
    int i;

    print("  Type:      Broadband Adapter (RTL8139)\n");

    /* Read MAC address from RTL8139 ID registers */
    for (i = 0; i < 6; i++)
        mac[i] = nic[RT_IDR0 + i];

    print("  MAC:       ");
    print_mac(mac);
    print("\n");
}

/* Simple busy-wait delay (~ms at 200MHz SH4) */
static void delay_ms(unsigned int ms)
{
    volatile unsigned int i;
    while (ms--) {
        for (i = 0; i < 20000; i++)
            ;
    }
}

static void probe_expansion_port(void)
{
    unsigned char chip_type;

    print("--- Expansion Port ---\n");

    /* Check for BBA (GAPS PCI bridge) first */
    if (check_gaps_bridge()) {
        probe_bba();
        print("\n");
        return;
    }

    /* No GAPS bridge. Try LAN adapter / modem area.
     * Reset the expansion device and read chip type. */
    LA_RESET = 0;
    LA_RESET = 1;
    LA_RESET = 0;
    delay_ms(100);

    chip_type = (LA_REG(7) >> 6) & 3;

    if (chip_type == 2) {
        print("  Type:      LAN Adapter (HIT-0300 / MB86967)\n");
    } else {
        /* Read a few bytes from expansion area to check if anything is there */
        unsigned char r0 = LA_REG(0);
        unsigned char r1 = LA_REG(1);
        unsigned char r7 = LA_REG(7);
        char hex[12];

        if (r0 == 0xff && r1 == 0xff && r7 == 0xff) {
            print("  Type:      (empty)\n");
        } else {
            print("  Type:      Modem / Unknown\n");
            print("  Regs:      R0=");
            uint_to_hex(r0, hex);
            print(hex);
            print(" R1=");
            uint_to_hex(r1, hex);
            print(hex);
            print(" R7=");
            uint_to_hex(r7, hex);
            print(hex);
            print("\n");
        }
    }
    print("  Chip type: ");
    print_dec(chip_type);
    print("\n\n");
}

/* ===== Cable type detection ===== */
/*
 * Reads SH4 GPIO PORT8/PORT9 to determine A/V cable type.
 * Same technique as video.S _check_cable.
 */
static int detect_cable(void)
{
    volatile unsigned int *pctra = (volatile unsigned int *)0xff80002c;
    volatile unsigned short *pdtra = (volatile unsigned short *)0xff800030;
    unsigned int old_pctra;
    unsigned short val;
    int cable;

    old_pctra = *pctra;

    /* Configure PORT8 and PORT9 as inputs */
    *pctra = (old_pctra & 0xfff0ffff) | 0x000a0000;

    val = *pdtra;
    cable = (val >> 8) & 3;

    /* Restore original setting */
    *pctra = old_pctra;

    return cable;
}

/* ===== RAM size detection ===== */
/*
 * Mirror test: write different values to top of 16MB and 32MB ranges
 * using P2 (uncached) addresses. If they differ, upper 16MB is real.
 */
static unsigned int detect_ram(void)
{
    volatile unsigned char *addr_16m = (volatile unsigned char *)0xACFFFFFF;
    volatile unsigned char *addr_32m = (volatile unsigned char *)0xADFFFFFF;

    *addr_16m = 0xBA;
    *addr_32m = 0xAB;

    if (*addr_16m != *addr_32m)
        return 32;
    return 16;
}

/* ===== Maple bus probing ===== */

#define MAPLE_BASE      0xa05f6c00
#define MAPLE_DMA_ADDR  (MAPLE_BASE + 0x04)
#define MAPLE_DMA_CTRL  (MAPLE_BASE + 0x10)
#define MAPLE_ENABLE    (MAPLE_BASE + 0x14)
#define MAPLE_DMA_STAT  (MAPLE_BASE + 0x18)
#define MAPLE_BITRATE   (MAPLE_BASE + 0x80)
#define MAPLE_MAGIC     (MAPLE_BASE + 0x8C)

#define MAPLE_CMD_DEVINFO   1
#define MAPLE_RESP_DEVINFO  5

/* Function code bits */
#define FUNC_CONTROLLER 0x001
#define FUNC_MEMCARD    0x002
#define FUNC_LCD        0x004
#define FUNC_CLOCK      0x008
#define FUNC_MIC        0x010
#define FUNC_ARGUN      0x020
#define FUNC_KEYBOARD   0x040
#define FUNC_LIGHTGUN   0x080
#define FUNC_PURUPURU   0x100

struct maple_devinfo {
    unsigned int func;
    unsigned int function_data[3];
    unsigned char area_code;
    unsigned char connector_direction;
    char product_name[30];
    char product_license[60];
    unsigned short standby_power;
    unsigned short max_power;
};

/* DMA buffer must be 32-byte aligned */
static unsigned char dma_buf[2048] __attribute__((aligned(32)));

static void maple_hw_init(void)
{
    volatile unsigned int *enable = (volatile unsigned int *)MAPLE_ENABLE;
    volatile unsigned int *magic = (volatile unsigned int *)MAPLE_MAGIC;
    volatile unsigned int *bitrate = (volatile unsigned int *)MAPLE_BITRATE;

    *enable = 0;
    *magic = 0x6155404f;
    *bitrate = 0xc3500000;  /* timeout + bitrate for 2Mbps */
    *enable = 1;
}

static int maple_query(int port, int unit)
{
    volatile unsigned int *dma_addr = (volatile unsigned int *)MAPLE_DMA_ADDR;
    volatile unsigned int *dma_ctrl = (volatile unsigned int *)MAPLE_DMA_CTRL;
    volatile unsigned int *dma_stat = (volatile unsigned int *)MAPLE_DMA_STAT;
    unsigned int *send = (unsigned int *)dma_buf;
    unsigned int *recv = (unsigned int *)(dma_buf + 1024);
    unsigned int from, to;
    int timeout;

    from = port << 6;
    to = (port << 6) | (unit > 0 ? (1 << (unit - 1)) : 0x20);

    /* Build send frame */
    send[0] = (1 << 31)                /* last frame */
            | ((unsigned int)(dma_buf + 1024) & 0x1fffffff); /* recv addr (physical) */
    send[1] = (0 << 24)                /* data length (0 words for DEVINFO) */
            | (from << 8)              /* sender address */
            | (to << 16)               /* recipient address */
            | MAPLE_CMD_DEVINFO;       /* command */

    /* Clear receive area */
    recv[0] = 0xFFFFFFFF;

    /* Start DMA */
    *dma_addr = ((unsigned int)dma_buf) & 0x1fffffff;
    *dma_ctrl = 1;

    /* Wait for completion */
    timeout = 1000000;
    while ((*dma_stat & 1) && --timeout > 0)
        ;

    if (timeout <= 0)
        return -1;

    /* Check response code (bits 7:0 of first word) */
    return (int)(recv[0] & 0xFF);
}

static struct maple_devinfo *maple_get_devinfo(void)
{
    unsigned int *recv = (unsigned int *)(dma_buf + 1024);
    return (struct maple_devinfo *)&recv[1];
}

static void print_funcs(unsigned int func)
{
    int first = 1;
    static const struct { unsigned int bit; const char *name; } funcs[] = {
        { FUNC_CONTROLLER, "Controller" },
        { FUNC_MEMCARD,    "Memory Card" },
        { FUNC_LCD,        "LCD" },
        { FUNC_CLOCK,      "Clock" },
        { FUNC_MIC,        "Microphone" },
        { FUNC_ARGUN,      "AR Gun" },
        { FUNC_KEYBOARD,   "Keyboard" },
        { FUNC_LIGHTGUN,   "Light Gun" },
        { FUNC_PURUPURU,   "Vibration" },
    };
    int i;

    for (i = 0; i < 9; i++) {
        if (func & funcs[i].bit) {
            if (!first) print(", ");
            print(funcs[i].name);
            first = 0;
        }
    }
    if (first) {
        char hex[12];
        uint_to_hex(func, hex);
        print("Unknown (");
        print(hex);
        print(")");
    }
}

static void print_name(const char *name, int maxlen)
{
    int i;
    /* Trim trailing spaces */
    int len = maxlen;
    while (len > 0 && (name[len-1] == ' ' || name[len-1] == '\0'))
        len--;

    for (i = 0; i < len; i++) {
        char c[2] = { name[i], '\0' };
        print(c);
    }
}

static void probe_maple_port(int port)
{
    int resp;
    struct maple_devinfo *info;
    int unit;
    char portname[2] = { 'A' + port, '\0' };

    print("--- Maple Port ");
    print(portname);
    print(" ---\n");

    resp = maple_query(port, 0);
    if (resp != MAPLE_RESP_DEVINFO) {
        print("  (empty)\n\n");
        return;
    }

    info = maple_get_devinfo();
    print("  Device:    ");
    print_name(info->product_name, 30);
    print("\n");
    print("  Functions: ");
    print_funcs(info->func);
    print("\n");

    /* Scan sub-devices (1-5) */
    for (unit = 1; unit <= 5; unit++) {
        resp = maple_query(port, unit);
        if (resp == MAPLE_RESP_DEVINFO) {
            info = maple_get_devinfo();
            print("  Sub-");
            {
                char unitstr[2] = { '0' + unit, '\0' };
                print(unitstr);
            }
            print(":     ");
            print_name(info->product_name, 30);
            print(" [");
            print_funcs(info->func);
            print("]\n");
        }
    }
    print("\n");
}

/* ===== Entry point ===== */

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    char hex[12];
    int cable;
    unsigned int ram;

    print("\n");
    print("=== Dreamcast Hardware Probe ===\n\n");

    /* Cable type */
    cable = detect_cable();
    print("Cable type:  ");
    switch (cable) {
    case 0: print("VGA (D-Sub 15)"); break;
    case 1: print("Reserved (1)"); break;
    case 2: print("RGB (SCART)"); break;
    case 3: print("Composite / S-Video"); break;
    }
    print("  [");
    uint_to_hex(cable, hex);
    print(hex);
    print("]\n");

    /* RAM size */
    ram = detect_ram();
    print("RAM size:    ");
    print_dec(ram);
    print(" MB\n");

    /* Hardware ID */
    {
        volatile unsigned char *hwid = (volatile unsigned char *)0xa021a056;
        print("Hardware ID: ");
        uint_to_hex((hwid[0] << 16) | (hwid[1] << 8) | hwid[2], hex);
        print(hex);
        print("\n");
    }

    /* AICA RTC */
    {
        volatile unsigned int *rtc_h = (volatile unsigned int *)0xa0710000;
        volatile unsigned int *rtc_l = (volatile unsigned int *)0xa0710004;
        unsigned int secs = ((*rtc_h & 0xFFFF) << 16) | (*rtc_l & 0xFFFF);
        /* AICA epoch is 1950-01-01, Unix is 1970-01-01 = 631152000 sec diff */
        unsigned int unix_ts = secs - 631152000;
        print("RTC (unix):  ");
        uint_to_hex(unix_ts, hex);
        print(hex);
        print("\n");
    }

    print("\n");

    /* SCIF serial port */
    probe_scif();

    /* Expansion port (BBA / LAN / Modem) */
    probe_expansion_port();

    /* Maple bus */
    maple_hw_init();
    probe_maple_port(0);
    probe_maple_port(1);
    probe_maple_port(2);
    probe_maple_port(3);

    print("=== Done ===\n\n");

    kl_exit();
}
