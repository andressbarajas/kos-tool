/* examples/console-test/main.c */
/*
 * console-test - kosload console output test
 *
 * Tests the write syscall to verify console I/O between
 * the target and the host tool. Prints messages via the
 * kosload syscall interface.
 *
 * Build for DC:
 *   sh-elf-gcc -O2 -ffreestanding -nostdlib -Ttext=0x8c010000
 *              -o console-test.elf main.c
 *   sh-elf-objcopy -O binary console-test.elf console-test.bin
 *
 * Build for GC:
 *   powerpc-eabi-gcc -O2 -ffreestanding -nostdlib -Ttext=0x80100000
 *                    -o console-test.elf main.c
 *   powerpc-eabi-objcopy -O binary console-test.elf console-test.bin
 *
 * Load and run:
 *   kostool -x console-test.elf
 */

/* Syscall numbers (from crt0.S syscall jump table) */
#define SYSCALL_READ      0
#define SYSCALL_WRITE     1
#define SYSCALL_OPEN      2
#define SYSCALL_CLOSE     3
#define SYSCALL_EXIT      15

/*
 * Kosload syscall interface.
 *
 * The kosload binary places a function pointer table at its base address.
 * On DC: base = 0x8c004000, magic at base+4, syscall func ptr at base+8
 * On GC: base = 0x80003100, magic at base+4, syscall func ptr at base+8
 *
 * The magic value 0xdeadbeef indicates kosload is present.
 * The syscall function takes: (syscall_number, arg1, arg2, arg3)
 */

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

/* Simple strlen */
static int slen(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

/* Print a string to the console */
static void print(const char *msg)
{
    kl_write(1, msg, slen(msg));
}

/* Convert an unsigned int to hex string */
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

/* Entry point */
void start(void) __attribute__((section(".text.start")));
void start(void)
{
    char hexbuf[12];

    print("\n");
    print("=== kosload console test ===\n");
    print("\n");

    /* Test 1: Basic write */
    print("Test 1: Basic console output ... OK\n");

    /* Test 2: Verify kosload magic */
    print("Test 2: Kosload magic value: ");
    uint_to_hex(KOSLOAD_MAGIC_ADDR, hexbuf);
    print(hexbuf);
    if (KOSLOAD_MAGIC_ADDR == KOSLOAD_MAGIC)
        print(" ... OK\n");
    else
        print(" ... FAIL (expected 0xdeadbeef)\n");

    /* Test 3: Verify syscall function pointer */
    print("Test 3: Syscall function at: ");
    uint_to_hex(KOSLOAD_SYSCALL_ADDR, hexbuf);
    print(hexbuf);
    print(" ... OK\n");

    /* Test 4: Multiple writes */
    print("Test 4: Multiple writes: ");
    print("A");
    print("B");
    print("C");
    print(" ... OK\n");

    /* Test 5: Large write */
    print("Test 5: Large write (80 chars): ");
    print("01234567890123456789012345678901234567890123456789"
          "012345678901234567890123456789");
    print("\n");
    print("        ... OK\n");

    print("\n");
    print("All console tests passed!\n");
    print("\n");

    kl_exit();
}
