/* examples/cdfs-test/main.c */
/*
 * cdfs-test - kosload CDFS redirection test
 *
 * Tests the GD-ROM BIOS redirect by reading sectors from an ISO image
 * via the BIOS syscall interface at 0x8c0000bc. Verifies that the
 * ISO 9660 Primary Volume Descriptor can be read correctly.
 *
 * Load and run (Dreamcast only):
 *   kos-tool -x cdfs-test.elf -i cdfs-test.iso
 *
 * Expected output:
 *   - TOC read returns correct fake TOC entries
 *   - PVD sector read returns ISO 9660 signature "CD001"
 *   - Volume ID from the ISO is printed
 */

#if defined(__sh__) || defined(__SH4_SINGLE__)

/* ===== Constants ===== */

#define SECTOR_SIZE        2048
#define PVD_SECTOR         166  /* 150 (data track LBA start) + 16 (PVD offset) */

/* GD-ROM BIOS syscall numbers (r7 value) */
#define GDROM_REQ_CMD       0
#define GDROM_GET_CMD_STAT  1
#define GDROM_EXEC_SERVER   2
#define GDROM_INIT_SYSTEM   3
#define GDROM_GET_DRV_STAT  4

/* GD-ROM command codes (first arg to gdGdcReqCmd) */
#define GDROM_CMD_READ_SECTORS  16
#define GDROM_CMD_READ_TOC      19
#define GDROM_CMD_INIT_DISC     24

/* ===== Kosload syscall interface ===== */

#define KOSLOAD_BASE         0x8c004000
#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

#define SYSCALL_WRITE     1
#define SYSCALL_EXIT     22

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

static kosload_syscall_fn get_syscall(void)
{
    if (KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kosload_syscall_fn)0;
    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

static void kl_write(int fd, const void *buf, int count)
{
    kosload_syscall_fn sc = get_syscall();
    if (sc) sc(SYSCALL_WRITE, fd, (int)buf, count);
}

static void kl_exit(void)
{
    kosload_syscall_fn sc = get_syscall();
    if (sc) sc(SYSCALL_EXIT, 0, 0, 0);
}

static int slen(const char *s) { int n = 0; while (*s++) n++; return n; }
static void print(const char *msg) { kl_write(1, msg, slen(msg)); }

/* ===== GD-ROM BIOS call wrapper ===== */

/*
 * Call the GD-ROM BIOS via the redirect vector at 0x8c0000bc.
 *
 * The BIOS dispatch function reads the syscall number from r7.
 * SH4 calling convention places the first 4 integer args in r4-r7.
 * By using a 4-arg function pointer, arg1->r4, arg2->r5, arg3->r6,
 * syscall_num->r7 — exactly what the BIOS expects.
 */
typedef int (*gdrom_fn_t)(int, int, int, int);

static int gdrom_call(int arg1, int arg2, int arg3, int syscall_num)
{
    gdrom_fn_t fn = *(gdrom_fn_t *)0x8c0000bc;
    return fn(arg1, arg2, arg3, syscall_num);
}

static int gdrom_req_cmd(int cmd, int *params)
{
    return gdrom_call(cmd, (int)params, 0, GDROM_REQ_CMD);
}

static int gdrom_get_cmd_stat(int f, int *status)
{
    return gdrom_call(f, (int)status, 0, GDROM_GET_CMD_STAT);
}

static void gdrom_exec_server(void)
{
    gdrom_call(0, 0, 0, GDROM_EXEC_SERVER);
}

/* ===== Helpers ===== */

static void uint_to_dec(unsigned int val, char *buf)
{
    char tmp[12];
    int i = 0, j;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    for (j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static void uint_to_hex(unsigned int val, char *buf)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hex[(val >> (28 - i * 4)) & 0xf];
    buf[10] = '\0';
}

static void print_hex_dump(const unsigned char *data, int len)
{
    static const char hx[] = "0123456789abcdef";
    char hex_byte[4];
    int i;
    hex_byte[2] = ' ';
    hex_byte[3] = '\0';
    for (i = 0; i < len; i++) {
        hex_byte[0] = hx[data[i] >> 4];
        hex_byte[1] = hx[data[i] & 0xf];
        print(hex_byte);
    }
}

static int pass_count = 0;
static int fail_count = 0;

static void result(const char *name, int ok)
{
    print(name);
    if (ok) { print(" ... OK\n"); pass_count++; }
    else    { print(" ... FAIL\n"); fail_count++; }
}

/* ===== Test data ===== */

static unsigned char sector_buf[SECTOR_SIZE] __attribute__((aligned(32)));

/* ===== Entry point ===== */

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    int params[4];
    int status[4];
    char numbuf[12];
    int ret;

    struct {
        unsigned int entry[99];
        unsigned int first, last;
        unsigned int dunno;
    } toc;

    print("\n");
    print("=== kosload CDFS redirection test ===\n");
    print("(requires: kostool -x cdfs-test.elf -c -i disc.iso)\n");
    print("\n");

    /* Test 1: Init disc */
    ret = gdrom_req_cmd(GDROM_CMD_INIT_DISC, params);
    gdrom_exec_server();
    ret = gdrom_get_cmd_stat(0, status);
    result("Test 1: Init disc (cmd 24)", ret == 2);

    /* Test 2: Read TOC */
    params[0] = 0;           /* session */
    params[1] = (int)&toc;   /* TOC buffer */
    ret = gdrom_req_cmd(GDROM_CMD_READ_TOC, params);
    gdrom_exec_server();
    ret = gdrom_get_cmd_stat(0, status);
    result("Test 2: Read TOC (cmd 19)", ret == 2);

    if (ret == 2) {
        /* Verify TOC contents match the fake TOC dcload provides */
        result("Test 3: TOC entry[0] = 0x41000096",
               toc.entry[0] == 0x41000096);
        result("Test 4: TOC first = last = 0x41010000",
               toc.first == 0x41010000 && toc.last == 0x41010000);

        print("         entry[0] = ");
        uint_to_hex(toc.entry[0], numbuf);
        print(numbuf);
        print(", first = ");
        uint_to_hex(toc.first, numbuf);
        print(numbuf);
        print("\n");
    }

    /* Test 5: Read the ISO 9660 Primary Volume Descriptor
     * PVD is at sector 16 of the data track. With the fake TOC's
     * data track starting at LBA 150, the PVD is at LBA 166. */
    params[0] = PVD_SECTOR;          /* starting sector (LBA) */
    params[1] = 1;                    /* number of sectors */
    params[2] = (int)sector_buf;      /* destination buffer */
    params[3] = 0;                    /* status (set by handler) */

    ret = gdrom_req_cmd(GDROM_CMD_READ_SECTORS, params);
    gdrom_exec_server();
    ret = gdrom_get_cmd_stat(0, status);
    result("Test 5: Read PVD sector (cmd 16, LBA 166)", ret == 2);
    result("Test 6: Read completed (params[3] == 0)", params[3] == 0);

    if (ret == 2) {
        /* ISO 9660 Primary Volume Descriptor format:
         * byte 0:   type (1 = PVD)
         * byte 1-5: "CD001" standard identifier */
        int cd001_ok = (sector_buf[1] == 'C' &&
                        sector_buf[2] == 'D' &&
                        sector_buf[3] == '0' &&
                        sector_buf[4] == '0' &&
                        sector_buf[5] == '1');
        result("Test 7: PVD signature 'CD001'", cd001_ok);
        result("Test 8: PVD type byte == 1", sector_buf[0] == 1);

        if (cd001_ok) {
            /* Volume Identifier is at bytes 40-71, space-padded */
            int end = 71;
            while (end > 40 && sector_buf[end] == ' ') end--;
            print("         Volume ID: ");
            kl_write(1, &sector_buf[40], end - 40 + 1);
            print("\n");
        }

        print("         First 16 bytes: ");
        print_hex_dump(sector_buf, 16);
        print("\n");
    }

    /* Summary */
    print("\n");
    print("Results: ");
    uint_to_dec(pass_count, numbuf);
    print(numbuf);
    print(" passed, ");
    uint_to_dec(fail_count, numbuf);
    print(numbuf);
    print(" failed\n");
    print("\n");

    kl_exit();
}

#else /* Not Dreamcast */

#define KOSLOAD_BASE         0x80003100
#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

typedef int (*kosload_syscall_fn)(int, int, int, int);

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    const char *msg = "\nCDFS test: not supported on this platform\n\n";
    int n = 0;
    while (msg[n]) n++;

    if (KOSLOAD_MAGIC_ADDR == KOSLOAD_MAGIC) {
        kosload_syscall_fn sc = (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
        sc(1, 1, (int)msg, n);  /* write */
        sc(15, 0, 0, 0);        /* exit */
    }
}

#endif
