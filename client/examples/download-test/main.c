/* examples/download-test/main.c */
/*
 * download-test - Memory read-back verification test
 *
 * Writes a known pattern to memory, prints the address and expected
 * values, then spins. The user then downloads that memory region
 * using kos-tool's -d flag and verifies the pattern matches.
 *
 * Usage:
 *   1. Upload and run:
 *      kos-tool -x download-test.elf
 *
 *   2. Note the printed pattern address (e.g., 0x8c01XXXX)
 *
 *   3. Download memory (serial transport, or after reset for network):
 *      kos-tool -d /tmp/memdump.bin -a <address> -s 256
 *
 *   4. Verify with: xxd /tmp/memdump.bin
 *      Expected: 64 bytes of 0xAA, 64 bytes of 0x55, 128 incrementing (0x80-0xFF)
 *
 * Note: On network transport, the program must exit before the host can
 * send download commands (the kosload event loop must be running).
 * On serial transport, the host can interrupt at any time.
 */

#define SYSCALL_WRITE  1
#define SYSCALL_EXIT  15

#if defined(__sh__) || defined(__SH4_SINGLE__)
#define KOSLOAD_BASE    0x8c004000
#elif defined(__PPC__) || defined(__powerpc__)
#define KOSLOAD_BASE    0x80003100
#else
#error "Unsupported architecture"
#endif

#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

static kosload_syscall_fn get_syscall(void)
{
    if (KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kosload_syscall_fn)0;
    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

static int slen(const char *s) { int n = 0; while (*s++) n++; return n; }

static void print(const char *msg)
{
    kosload_syscall_fn sc = get_syscall();
    if (!sc) return;
    sc(SYSCALL_WRITE, 1, (int)msg, slen(msg));
}

static void uint_to_hex(unsigned int val, char *buf)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hex[(val >> (28 - i * 4)) & 0xf];
    buf[10] = '\0';
}

/*
 * Pattern buffer. Declared volatile so the compiler doesn't optimize
 * away the writes. The actual address depends on where BSS lands
 * relative to .text — printed at runtime.
 */
static volatile unsigned char pattern[256];

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    char hexbuf[12];
    int i;

    print("\n");
    print("=== kosload download test ===\n");
    print("\n");

    /* Write known pattern:
     *   Bytes   0- 63: 0xAA
     *   Bytes  64-127: 0x55
     *   Bytes 128-255: incrementing (0x80, 0x81, ... 0xFF)
     */
    for (i = 0; i < 64; i++)
        pattern[i] = 0xAA;
    for (i = 64; i < 128; i++)
        pattern[i] = 0x55;
    for (i = 128; i < 256; i++)
        pattern[i] = (unsigned char)i;

    /* Print the address and expected layout */
    print("Pattern written to memory.\n");
    print("Address: ");
    uint_to_hex((unsigned int)pattern, hexbuf);
    print(hexbuf);
    print("\n");
    print("Size:    256 bytes\n");
    print("Layout:\n");
    print("  [  0- 63] = 0xAA (170)\n");
    print("  [ 64-127] = 0x55 (85)\n");
    print("  [128-255] = 0x80..0xFF (incrementing)\n");
    print("\n");

    /* Verify pattern is correct by spot-checking */
    print("Spot check: pattern[0]=");
    uint_to_hex(pattern[0], hexbuf);
    print(hexbuf);
    print(" pattern[64]=");
    uint_to_hex(pattern[64], hexbuf);
    print(hexbuf);
    print(" pattern[128]=");
    uint_to_hex(pattern[128], hexbuf);
    print(hexbuf);
    print(" pattern[255]=");
    uint_to_hex(pattern[255], hexbuf);
    print(hexbuf);
    print("\n\n");

    print("Download with:\n");
    print("  kos-tool -d /tmp/memdump.bin -a ");
    uint_to_hex((unsigned int)pattern, hexbuf);
    print(hexbuf);
    print(" -s 256\n");
    print("\nVerify with:\n");
    print("  xxd /tmp/memdump.bin\n");
    print("\n");
    print("Pattern is in memory. Download now, then reset console.\n");

    kosload_syscall_fn sc = get_syscall();
    if (sc) sc(SYSCALL_EXIT, 0, 0, 0);
}
