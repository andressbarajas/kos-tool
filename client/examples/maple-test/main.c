/* examples/maple-test/main.c */
/*
 * maple-test - Dreamcast Maple Bus test
 *
 * Scans all 4 controller ports for devices, prints device info,
 * reads controller state, and optionally reads VMU block 0
 * (exercises datalen >= 2 code path in maple_docmd).
 *
 * DC only - Maple Bus is Dreamcast-specific hardware.
 *
 * Load and run:
 *   kos-tool -x maple-test.elf
 *   dc-tool-ip -t <ip> -x maple-test.elf
 */

#if !defined(__sh__) && !defined(__SH4_SINGLE__)
#error "maple-test is Dreamcast-only"
#endif

/* ===== Kosload syscall interface ===== */

#define KOSLOAD_BASE         0x8c004000
#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

#define SYSCALL_WRITE  1
#define SYSCALL_EXIT   15

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

static kosload_syscall_fn sc;

static kosload_syscall_fn get_syscall(void)
{
    if (KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kosload_syscall_fn)0;
    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

/* ===== Utility functions ===== */

static int slen(const char *s) { int n = 0; while (*s++) n++; return n; }

static void print(const char *msg)
{
    sc(SYSCALL_WRITE, 1, (int)msg, slen(msg));
}

static void kl_exit(void)
{
    sc(SYSCALL_EXIT, 0, 0, 0);
}

static void uint_to_hex(unsigned int val, char *buf)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xf];
        val >>= 4;
    }
    buf[8] = 0;
}

static void uint_to_dec(unsigned int val, char *buf)
{
    char tmp[11];
    int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (val) { tmp[i++] = '0' + (val % 10); val /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = 0;
}

/* Print a fixed-length string field (not necessarily null-terminated) */
static void print_field(const char *data, int len)
{
    char buf[64];
    int i;
    for (i = 0; i < len && i < 63; i++)
        buf[i] = data[i];
    /* Trim trailing spaces */
    while (i > 0 && buf[i-1] == ' ') i--;
    buf[i] = 0;
    print(buf);
}

/* ===== Minimal Maple Bus implementation ===== */

#define MAPLE(x) (*((volatile unsigned int *)(0xa05f6c00 + (x))))

#define MAPLE_DMA_SIZE       (1024 + 1024 + 12)
#define MAPLE_CMD_DEVINFO    1
#define MAPLE_CMD_GETCOND    9
#define MAPLE_CMD_BREAD      11
#define MAPLE_RESP_DEVINFO   5
#define MAPLE_RESP_DATATRF   8
#define MAPLE_RESP_NONE      (-1 & 0xff)
#define MAPLE_FUNC_CONTROLLER 0x001
#define MAPLE_FUNC_MEMCARD    0x002

static __attribute__((aligned(32))) volatile unsigned char dmabuf[MAPLE_DMA_SIZE];

static void maple_init(void)
{
    MAPLE(0x8c) = 0x6155404f;
    MAPLE(0x10) = 0;
    MAPLE(0x80) = (50000 << 16) | 0;
    MAPLE(0x14) = 1;
}

static void maple_wait_dma(void)
{
    while (MAPLE(0x18) & 1)
        ;
}

/*
 * Send a Maple command and return pointer to response frame.
 *
 * port    - controller port (0-3)
 * unit    - 0 = main device, 1-5 = sub-devices
 * cmd     - Maple command number
 * datalen - parameter data length in 32-bit longwords
 * data    - parameter data (big-endian Maple format)
 */
static unsigned int *maple_cmd(int port, int unit, int cmd,
                               int datalen, const unsigned int *data)
{
    volatile unsigned int *sendbuf, *recvbuf;
    int to, from;

    port &= 3;
    from = port << 6;
    to = (port << 6) | (unit > 0 ? ((1 << (unit - 1)) & 0x1f) : 0x20);

    if (datalen > 255) datalen = 255;
    else if (datalen < 0) datalen = 0;

    recvbuf = (volatile unsigned int *)((unsigned int)dmabuf | 0xa0000000);
    sendbuf = (volatile unsigned int *)((unsigned int)recvbuf + 1024);

    maple_wait_dma();

    MAPLE(0x04) = (unsigned int)sendbuf & 0x0fffffff;

    *sendbuf++ = (unsigned int)datalen | ((unsigned int)port << 16) | 0x80000000;
    *sendbuf++ = (unsigned int)recvbuf & 0x0fffffff;
    *sendbuf++ = (unsigned int)(cmd & 0xff) | ((unsigned int)to << 8)
               | ((unsigned int)from << 16) | ((unsigned int)datalen << 24);

    /* Copy parameter data (already in uncached region, no cache flush needed) */
    for (int i = 0; i < datalen; i++)
        sendbuf[i] = data[i];

    MAPLE(0x18) = 1;
    maple_wait_dma();

    return (unsigned int *)recvbuf;
}

/* ===== Maple device info structure ===== */

struct maple_devinfo {
    unsigned int func;             /* Supported function codes */
    unsigned int func_data[3];     /* Function-specific data */
    unsigned char area_code;
    unsigned char connector_dir;
    char product_name[30];
    char product_license[60];
    unsigned short standby_power;
    unsigned short max_power;
};

/* ===== Test logic ===== */

static int pass_count = 0;
static int fail_count = 0;

static void result(const char *name, int ok)
{
    print(ok ? "  PASS: " : "  FAIL: ");
    print(name);
    print("\n");
    if (ok) pass_count++; else fail_count++;
}

static void print_hex(unsigned int val)
{
    char buf[9];
    uint_to_hex(val, buf);
    print("0x");
    print(buf);
}

static void print_dec(unsigned int val)
{
    char buf[11];
    uint_to_dec(val, buf);
    print(buf);
}

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    char buf[9];
    sc = get_syscall();
    if (!sc) return;

    print("\n=== Maple Bus Test ===\n\n");

    maple_init();

    int devices_found = 0;

    for (int port = 0; port < 4; port++) {
        print("Port ");
        buf[0] = 'A' + port;
        buf[1] = 0;
        print(buf);
        print(": ");

        unsigned int *resp = maple_cmd(port, 0, MAPLE_CMD_DEVINFO, 0, 0);

        /* Response byte 0 is the response code */
        unsigned char resp_code = ((unsigned char *)resp)[0];

        if (resp_code == MAPLE_RESP_NONE) {
            print("No device\n");
            continue;
        }

        if (resp_code != MAPLE_RESP_DEVINFO) {
            print("Unexpected response: ");
            print_hex(resp_code);
            print("\n");
            result("DEVINFO response code", 0);
            continue;
        }

        devices_found++;

        /* Device info starts at resp[1] (after the 4-byte response header) */
        struct maple_devinfo *info = (struct maple_devinfo *)&resp[1];

        print("\"");
        print_field(info->product_name, 30);
        print("\"\n");

        result("DEVINFO response code", 1);

        print("    Functions: ");
        print_hex(info->func);
        print("\n");

        /* Test GETCOND if it's a controller (datalen=1, exercises datalen=1 path) */
        if (info->func & MAPLE_FUNC_CONTROLLER) {
            unsigned int func_code = MAPLE_FUNC_CONTROLLER;
            resp = maple_cmd(port, 0, MAPLE_CMD_GETCOND, 1, &func_code);
            resp_code = ((unsigned char *)resp)[0];

            if (resp_code == MAPLE_RESP_DATATRF) {
                result("GETCOND controller", 1);

                /* resp[1] = function code, resp[2] = button state */
                unsigned int buttons = resp[2];
                print("    Buttons: ");
                print_hex(buttons);
                print("\n");
            } else {
                result("GETCOND controller", 0);
            }
        }

        /* Test BREAD if it has a memory card function (datalen=2, exercises the fix) */
        if (info->func & MAPLE_FUNC_MEMCARD) {
            /* Check sub-devices for VMU */
            for (int unit = 1; unit <= 2; unit++) {
                resp = maple_cmd(port, unit, MAPLE_CMD_DEVINFO, 0, 0);
                resp_code = ((unsigned char *)resp)[0];
                if (resp_code != MAPLE_RESP_DEVINFO) continue;

                struct maple_devinfo *sub_info = (struct maple_devinfo *)&resp[1];
                if (!(sub_info->func & MAPLE_FUNC_MEMCARD)) continue;

                print("    Sub-unit ");
                print_dec(unit);
                print(": \"");
                print_field(sub_info->product_name, 30);
                print("\"\n");

                /* BREAD: read block 0, partition 0
                 * datalen=2: func_code (1 longword) + block_number (1 longword)
                 * This is the code path that was broken before the (datalen-1)*4 fix */
                unsigned int bread_args[2];
                bread_args[0] = MAPLE_FUNC_MEMCARD;  /* function code */
                bread_args[1] = 0;                    /* block 0, partition 0 */

                resp = maple_cmd(port, unit, MAPLE_CMD_BREAD, 2, bread_args);
                resp_code = ((unsigned char *)resp)[0];

                if (resp_code == MAPLE_RESP_DATATRF) {
                    result("BREAD block 0 (datalen=2)", 1);
                    /* resp[1] = function, resp[2] = block#, resp[3..] = data */
                    print("    Block 0 first 4 bytes: ");
                    print_hex(resp[3]);
                    print("\n");
                } else {
                    print("    BREAD response: ");
                    print_hex(resp_code);
                    print("\n");
                    /* FILEERR (-5/0xfb) is expected if VMU has no save data */
                    result("BREAD block 0 (datalen=2)",
                           resp_code == (unsigned char)(-5 & 0xff));
                }

                break; /* Only test first VMU per port */
            }
        }

        print("\n");
    }

    /* Summary */
    print("--- Results ---\n");
    print("Devices found: ");
    print_dec(devices_found);
    print("\n");
    print("Passed: ");
    print_dec(pass_count);
    print("  Failed: ");
    print_dec(fail_count);
    print("\n\n");

    if (devices_found == 0) {
        print("No devices found - connect a controller and retry\n");
    }

    kl_exit();
}
