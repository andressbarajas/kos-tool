/* examples/gdb-test/main.c */
/*
 * gdb-test - GDB relay smoke test
 *
 * Verifies the gdbpacket syscall (slot 20) can send and receive
 * data through the kosload GDB relay. This is a basic connectivity
 * test; full GDB debugging requires KOS with GDB stub.
 *
 * Usage:
 *   Terminal 1: kos-tool -x gdb-test.elf -g
 *   Terminal 2: sh-elf-gdb -ex "target remote :2159"
 *
 * The -g flag tells kos-tool to enable the GDB server relay.
 * The program sends a GDB status query ("?") and reports
 * whether it got a response.
 */

#define SYSCALL_WRITE      1
#define SYSCALL_EXIT      15
#define SYSCALL_GDBPACKET 20

#if defined(__sh__) || defined(__SH4_SINGLE__)
#define KOSLOAD_BASE    0x8c004000
#elif defined(__PPC__) || defined(__powerpc__)
#if defined(WII_KOSLOAD_BASE)
#define KOSLOAD_BASE    WII_KOSLOAD_BASE
#elif defined(GC_KOSLOAD_BASE)
#define KOSLOAD_BASE    GC_KOSLOAD_BASE
#else
#define KOSLOAD_BASE    0x817EC000
#endif
#elif defined(__mips__) || defined(__mips)
#ifdef PS2_KOSLOAD_BASE
#define KOSLOAD_BASE    PS2_KOSLOAD_BASE
#else
/* crt0 layout: j(+0) nop(+4) magic(+8) syscall_ptr(+12).
 * DC/GC pattern needs BASE+4=magic, BASE+8=syscall, so PS2 base
 * is _start+4 (0x80000280+4), not the real entry. */
#define KOSLOAD_BASE    0x80000284
#endif
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

static void kl_exit(void)
{
    kosload_syscall_fn sc = get_syscall();
    if (sc) sc(SYSCALL_EXIT, 0, 0, 0);
}

static void uint_to_dec(unsigned int val, char *buf)
{
    char tmp[12];
    int i = 0, j;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    for (j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

/*
 * gdbpacket syscall (slot 20):
 *   arg1 = pointer to input buffer (GDB command to send)
 *   arg2 = (in_size << 16) | out_size
 *   arg3 = pointer to output buffer (GDB response received)
 *   returns: number of bytes received in out_buf
 */
static unsigned int kl_gdbpacket(const char *in_buf, unsigned int in_size,
                                 char *out_buf, unsigned int out_size)
{
    kosload_syscall_fn sc = get_syscall();
    if (!sc) return 0;
    return (unsigned int)sc(SYSCALL_GDBPACKET, (int)in_buf,
                            (int)((in_size << 16) | out_size),
                            (int)out_buf);
}

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    char out_buf[256];
    char numbuf[12];
    unsigned int ret;
    int i;

    print("\n");
    print("=== kosload GDB relay test ===\n");
    print("\n");
    print("Ensure kos-tool was started with -g flag.\n");
    print("Connect GDB: sh-elf-gdb -ex \"target remote :2159\"\n");
    print("\n");

    /* Zero the output buffer */
    for (i = 0; i < (int)sizeof(out_buf); i++)
        out_buf[i] = 0;

    /* Send GDB halt reason query "?" */
    print("Sending GDB status query (\"?\")...\n");
    ret = kl_gdbpacket("?", 1, out_buf, sizeof(out_buf) - 1);

    print("gdbpacket returned: ");
    uint_to_dec(ret, numbuf);
    print(numbuf);
    print(" bytes\n");

    if (ret > 0) {
        out_buf[ret < sizeof(out_buf) ? ret : sizeof(out_buf) - 1] = '\0';
        print("Response: \"");
        print(out_buf);
        print("\"\n");
        print("\nGDB relay ... PASS\n");
    } else {
        print("No response (GDB client not connected?)\n");
        print("\nGDB relay ... FAIL (no response)\n");
    }

    print("\n");
    kl_exit();
}
