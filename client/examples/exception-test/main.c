/* examples/exception-test/main.c */
/*
 * exception-test - kosload exception handler test
 *
 * Deliberately triggers an exception to verify kosload's exception
 * handler displays a proper register dump on screen.
 *
 * On DC: triggers an Address Error by doing a misaligned 32-bit read.
 *        This works regardless of MMU state (kosload runs with MMU off).
 * On GC: triggers a DSI (data storage interrupt) by reading from
 *         an invalid address
 *
 * The expected result is the exception handler displaying register
 * values on the video output, then returning to kosload.
 *
 * Load and run:
 *   kostool -x exception-test.elf
 */

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

#define SYSCALL_WRITE     1
#define SYSCALL_EXIT     22

static int slen(const char *s) { int n = 0; while (*s++) n++; return n; }

static void print(const char *msg)
{
    kosload_syscall_fn sc;
    if (KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC) return;
    sc = (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
    sc(SYSCALL_WRITE, 1, (int)msg, slen(msg));
}

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    volatile unsigned int *bad_addr;

    print("\n");
    print("=== kosload exception handler test ===\n");
    print("\n");
    print("About to trigger an exception...\n");
    print("You should see a register dump on the console screen.\n");
    print("\n");

    /*
     * Trigger an exception.
     *
     * On SH4 (DC): A misaligned 32-bit read causes an Address Error
     *              exception (EXPEVT = 0x0E0). This works with the MMU
     *              disabled (kosload doesn't enable the MMU, so TLB miss
     *              exceptions cannot fire). Same approach as the legacy
     *              dcload-serial exception test.
     *
     * On PPC (GC): Reading from 0xC0000000 causes a DSI exception
     *              (unmapped virtual address)
     */
#if defined(__sh__) || defined(__SH4_SINGLE__)
    bad_addr = (volatile unsigned int *)0x8c000002;  /* misaligned */
#elif defined(__PPC__) || defined(__powerpc__)
    bad_addr = (volatile unsigned int *)0xC0000000;
#endif

    /* This read should trigger the exception */
    (void)*bad_addr;

    /* Should not reach here if exception handler works */
    print("ERROR: Exception was not caught!\n");

    kosload_syscall_fn sc;
    if (KOSLOAD_MAGIC_ADDR == KOSLOAD_MAGIC) {
        sc = (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
        sc(SYSCALL_EXIT, 0, 0, 0);
    }
}
