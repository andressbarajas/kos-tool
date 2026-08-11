/* examples/exception-test/main.c */
/*
 * exception-test - kosload exception handler test
 *
 * Triggers a real CPU exception to verify the loader's handler.  Each console
 * needs a different trigger:
 *
 *   DC    misaligned 32-bit read -> Address Error (inline asm; see below)
 *   GC    unmapped address read  -> DSI
 *   PS2   `break`                -> Breakpoint (the R5900 does NOT trap on the
 *                                   misaligned load DC uses)
 *
 * Expect a register dump on video plus an "EXPT" frame symbolized host-side.
 * DC/GC/PS2 return to kosload.
 *
 * Load and run:
 *   kostool -x exception-test.elf
 */

#if defined(__sh__) || defined(__SH4_SINGLE__)
#define KOSLOAD_BASE 0x8c004000
#elif defined(__PPC__) || defined(__powerpc__)
#if defined(WII_KOSLOAD_BASE)
#define KOSLOAD_BASE WII_KOSLOAD_BASE
#elif defined(GC_KOSLOAD_BASE)
#define KOSLOAD_BASE GC_KOSLOAD_BASE
#else
#define KOSLOAD_BASE 0x817EC000
#endif
#elif defined(__mips__) || defined(__mips)
#ifdef PS2_KOSLOAD_BASE
#define KOSLOAD_BASE PS2_KOSLOAD_BASE
#else
/* crt0 layout: j(+0) nop(+4) magic(+8) syscall_ptr(+12).
 * DC/GC pattern needs BASE+4=magic, BASE+8=syscall, so PS2 base
 * is _start+4 (0x80000280+4), not the real entry. */
#define KOSLOAD_BASE 0x80000284
#endif
#else
#error "Unsupported architecture"
#endif

#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

#define SYSCALL_WRITE 1
#define SYSCALL_EXIT  15

static int slen(const char *s) {
    int n = 0;
    while(*s++)
        n++;
    return n;
}

static void print(const char *msg) {
    kosload_syscall_fn sc;
    if(KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return;
    sc = (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
    sc(SYSCALL_WRITE, 1, (int)msg, slen(msg));
}

void start(void) __attribute__((section(".text.start")));
void start(void) {
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
     *
     * On MIPS (PS2): the R5900 completes the classic misaligned-load
     *              trick without trapping, so we raise a Breakpoint
     *              exception directly via `break` — it vectors
     *              unconditionally through the common handler.
     */
#if defined(__mips__) || defined(__mips)
    __asm__ volatile("break");
#elif defined(__sh__) || defined(__SH4_SINGLE__)
    /* MUST be inline asm: as a plain `(void)*(volatile unsigned int *)` GCC
     * narrowed the discarded read to a 16-bit `mov.w`, which is aligned and
     * never faults — the test silently passed while testing nothing
     * (HW-confirmed on a real DC).  Only SH4 needs this; PPC/x86 faults don't
     * depend on access width. */
    unsigned int sink;
    __asm__ volatile("mov.l @%1, %0"
                     : "=r"(sink)
                     : "r"((unsigned int)0x8c000002));
    (void)sink;
#elif defined(__PPC__) || defined(__powerpc__)
    (void)*(volatile unsigned int *)0xC0000000;   /* unmapped -> DSI */
#endif

    /* Should not reach here if exception handler works */
    print("ERROR: Exception was not caught!\n");

    kosload_syscall_fn sc;
    if(KOSLOAD_MAGIC_ADDR == KOSLOAD_MAGIC) {
        sc = (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
        sc(SYSCALL_EXIT, 0, 0, 0);
    }
}
