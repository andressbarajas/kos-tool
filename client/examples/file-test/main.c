/* examples/file-test/main.c */
/*
 * file-test - kosload file I/O test
 *
 * Tests file syscalls (open, read, write, close, stat) to verify
 * the host fileserver functionality.
 *
 * Load and run:
 *   kostool -x file-test.elf -c  (enable console/fileserver mode)
 */

/* Syscall numbers */
#define SYSCALL_READ      0
#define SYSCALL_WRITE     1
#define SYSCALL_OPEN      2
#define SYSCALL_CLOSE     3
#define SYSCALL_CREAT     4
#define SYSCALL_UNLINK    6
#define SYSCALL_LSEEK     9
#define SYSCALL_FSTAT    10
#define SYSCALL_TIME     11
#define SYSCALL_EXIT     22
#define SYSCALL_OPENDIR  16
#define SYSCALL_CLOSEDIR 17
#define SYSCALL_READDIR  18

/* Open flags (KOS-compatible) */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0200
#define O_TRUNC     0x0400

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

static int kl_write(int fd, const void *buf, int count)
{
    kosload_syscall_fn sc = get_syscall();
    if (!sc) return -1;
    return sc(SYSCALL_WRITE, fd, (int)buf, count);
}

static int kl_read(int fd, void *buf, int count)
{
    kosload_syscall_fn sc = get_syscall();
    if (!sc) return -1;
    return sc(SYSCALL_READ, fd, (int)buf, count);
}

static int kl_open(const char *path, int flags)
{
    kosload_syscall_fn sc = get_syscall();
    if (!sc) return -1;
    return sc(SYSCALL_OPEN, (int)path, flags, 0);
}

static int kl_close(int fd)
{
    kosload_syscall_fn sc = get_syscall();
    if (!sc) return -1;
    return sc(SYSCALL_CLOSE, fd, 0, 0);
}

static int kl_creat(const char *path, int mode)
{
    kosload_syscall_fn sc = get_syscall();
    if (!sc) return -1;
    return sc(SYSCALL_CREAT, (int)path, mode, 0);
}

static int kl_unlink(const char *path)
{
    kosload_syscall_fn sc = get_syscall();
    if (!sc) return -1;
    return sc(SYSCALL_UNLINK, (int)path, 0, 0);
}

static int kl_time(void)
{
    kosload_syscall_fn sc = get_syscall();
    if (!sc) return -1;
    return sc(SYSCALL_TIME, 0, 0, 0);
}

static void kl_exit(void)
{
    kosload_syscall_fn sc = get_syscall();
    if (sc) sc(SYSCALL_EXIT, 0, 0, 0);
}

/* Helpers */
static int slen(const char *s) { int n = 0; while (*s++) n++; return n; }

static void print(const char *msg)
{
    kl_write(1, msg, slen(msg));
}

static int memcmp_b(const void *a, const void *b, int n)
{
    const unsigned char *p = a, *q = b;
    int i;
    for (i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] - q[i];
    return 0;
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

static int pass_count = 0;
static int fail_count = 0;

static void result(const char *name, int ok)
{
    print(name);
    if (ok) {
        print(" ... OK\n");
        pass_count++;
    } else {
        print(" ... FAIL\n");
        fail_count++;
    }
}

/* Entry point */
void start(void) __attribute__((section(".text.start")));
void start(void)
{
    char buf[256];
    char numbuf[12];
    int fd, n;
    const char *test_data = "Hello from kosload!\n";
    int test_len = slen(test_data);

    print("\n");
    print("=== kosload file I/O test ===\n");
    print("\n");

    /* Test 1: Create and write a file */
    fd = kl_creat("/tmp/kosload-test.txt", 0644);
    result("Test 1: creat(/tmp/kosload-test.txt)", fd >= 0);

    if (fd >= 0) {
        n = kl_write(fd, test_data, test_len);
        result("Test 2: write() returns correct count",
               n == test_len);
        kl_close(fd);
    }

    /* Test 3: Open and read the file back */
    fd = kl_open("/tmp/kosload-test.txt", O_RDONLY);
    result("Test 3: open(O_RDONLY)", fd >= 0);

    if (fd >= 0) {
        n = kl_read(fd, buf, sizeof(buf));
        result("Test 4: read() returns correct count",
               n == test_len);
        result("Test 5: read() data matches written data",
               n == test_len && memcmp_b(buf, test_data, test_len) == 0);
        kl_close(fd);
    }

    /* Test 6: Get current time */
    n = kl_time();
    result("Test 6: time() returns non-zero", n > 0);
    if (n > 0) {
        print("         timestamp = ");
        uint_to_dec((unsigned int)n, numbuf);
        print(numbuf);
        print("\n");
    }

    /* Test 7: Unlink the test file */
    n = kl_unlink("/tmp/kosload-test.txt");
    result("Test 7: unlink()", n == 0);

    /* Test 8: Verify file is gone */
    fd = kl_open("/tmp/kosload-test.txt", O_RDONLY);
    result("Test 8: open() fails after unlink", fd < 0);
    if (fd >= 0)
        kl_close(fd);

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
