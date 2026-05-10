/* examples/exit-test/main.c */
/*
 * exit-test - Test progexit return code reporting
 *
 * Exits with a non-zero return code to verify the host prints it.
 * Expected host output: "Program returned 42"
 *
 * Load and run:
 *   kos-tool -x exit-test.elf
 *   dc-tool-ip -t <ip> -x exit-test.elf
 */

#define SYSCALL_WRITE  1
#define SYSCALL_EXIT   15

#if defined(__sh__) || defined(__SH4_SINGLE__)
#define KOSLOAD_BASE    0x8c004000
#elif defined(__PPC__) || defined(__powerpc__)
#ifdef GC_KOSLOAD_BASE
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

static kosload_syscall_fn sc;

static kosload_syscall_fn get_syscall(void)
{
    if (KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kosload_syscall_fn)0;
    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

static int slen(const char *s) { int n = 0; while (*s++) n++; return n; }

static void print(const char *msg)
{
    sc(SYSCALL_WRITE, 1, (int)msg, slen(msg));
}

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    sc = get_syscall();
    if (!sc) return;

    print("Testing progexit return code...\n");
    print("Exiting with code 42 (host should print \"Program returned 42\")\n");

    sc(SYSCALL_EXIT, 42, 0, 0);
}
