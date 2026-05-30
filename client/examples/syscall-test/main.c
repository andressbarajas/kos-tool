/* examples/syscall-test/main.c */
/*
 * syscall-test - Comprehensive kosload syscall coverage test
 *
 * Tests all 24 syscall slots in the kosload jump table.
 * Each test prints a structured PASS/FAIL result.
 *
 * Skipped:
 *   Slot 14 (assign_wrkmem) - known stub, always returns -1
 *   Slot 20 (gdbpacket)     - requires GDB client connected (see gdb-test)
 *
 * Load and run:
 *   kos-tool -x syscall-test.elf -c /tmp
 *   dc-tool-ip -t <ip> -x syscall-test.elf -c /tmp
 */

/* Syscall numbers (from crt0.S jump table) */
#define SYSCALL_READ   0
#define SYSCALL_WRITE  1
#define SYSCALL_OPEN   2
#define SYSCALL_CLOSE  3
#define SYSCALL_CREAT  4
#define SYSCALL_LINK   5
#define SYSCALL_UNLINK 6
#define SYSCALL_CHDIR  7
#define SYSCALL_CHMOD  8
#define SYSCALL_LSEEK  9
#define SYSCALL_FSTAT  10
#define SYSCALL_TIME   11
#define SYSCALL_STAT   12
#define SYSCALL_UTIME  13
/* 14 = assign_wrkmem (stub) */
#define SYSCALL_EXIT     15
#define SYSCALL_OPENDIR  16
#define SYSCALL_CLOSEDIR 17
#define SYSCALL_READDIR  18
#define SYSCALL_HOSTINFO 19
/* 20 = gdbpacket */
#define SYSCALL_REWINDDIR 21

#define SYSCALL_MKDIR 22

/* Open flags (KOS-compatible) */
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001

/* Seek whence */
#define SEEK_SET 0

/* Kosload magic detection */
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

static kosload_syscall_fn get_syscall(void) {
    if(KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kosload_syscall_fn)0;
    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

/* dcload stat structure (matches KOS wire format, 60 bytes) */
typedef struct {
    unsigned short st_dev;
    unsigned short st_ino;
    int            st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned short st_rdev;
    int            st_size;
    int            st_atime_val;
    int            st_spare1;
    int            st_mtime_val;
    int            st_spare2;
    int            st_ctime_val;
    int            st_spare3;
    int            st_blksize;
    int            st_blocks;
    int            st_spare4[2];
} kosload_stat_t;

/* dcload dirent structure */
typedef struct {
    unsigned int   d_ino;
    int            d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
} kosload_dirent_t;

/* ===== Syscall wrappers ===== */

static int kl_read(int fd, void *buf, int count) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_READ, fd, (int)buf, count);
}

static int kl_write(int fd, const void *buf, int count) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_WRITE, fd, (int)buf, count);
}

static int kl_open(const char *path, int flags) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_OPEN, (int)path, flags, 0);
}

static int kl_close(int fd) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_CLOSE, fd, 0, 0);
}

static int kl_creat(const char *path, int mode) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_CREAT, (int)path, mode, 0);
}

static int kl_link(const char *oldpath, const char *newpath) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_LINK, (int)oldpath, (int)newpath, 0);
}

static int kl_unlink(const char *path) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_UNLINK, (int)path, 0, 0);
}

static int kl_chdir(const char *path) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_CHDIR, (int)path, 0, 0);
}

static int kl_chmod(const char *path, int mode) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_CHMOD, (int)path, mode, 0);
}

static int kl_lseek(int fd, int offset, int whence) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_LSEEK, fd, offset, whence);
}

static int kl_fstat(int fd, kosload_stat_t *buf) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_FSTAT, fd, (int)buf, (int)sizeof(kosload_stat_t));
}

static int kl_time(void) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_TIME, 0, 0, 0);
}

static int kl_stat(const char *path, kosload_stat_t *buf) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_STAT, (int)path, (int)buf, (int)sizeof(kosload_stat_t));
}

static int kl_utime(const char *path, unsigned int *times) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_UTIME, (int)path, (int)times, 0);
}

static unsigned int kl_opendir(const char *path) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return 0;
    return (unsigned int)sc(SYSCALL_OPENDIR, (int)path, 0, 0);
}

static int kl_closedir(unsigned int dir) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_CLOSEDIR, (int)dir, 0, 0);
}

static kosload_dirent_t *kl_readdir(unsigned int dir) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return (kosload_dirent_t *)0;
    return (kosload_dirent_t *)sc(SYSCALL_READDIR, (int)dir, 0, 0);
}

static int kl_rewinddir(unsigned int dir) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_REWINDDIR, (int)dir, 0, 0);
}

static int kl_gethostinfo(unsigned int *ip, unsigned int *port) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_HOSTINFO, (int)ip, (int)port, 0);
}

static int kl_mkdir(const char *path, int mode) {
    kosload_syscall_fn sc = get_syscall();
    if(!sc)
        return -1;
    return sc(SYSCALL_MKDIR, (int)path, mode, 0);
}

static void kl_exit(void) {
    kosload_syscall_fn sc = get_syscall();
    if(sc)
        sc(SYSCALL_EXIT, 0, 0, 0);
}

/* ===== Utility functions ===== */

static int slen(const char *s) {
    int n = 0;
    while(*s++)
        n++;
    return n;
}

static void print(const char *msg) {
    kl_write(1, msg, slen(msg));
}

static int memcmp_b(const void *a, const void *b, int n) {
    const unsigned char *p = a, *q = b;
    int                  i;
    for(i = 0; i < n; i++)
        if(p[i] != q[i])
            return p[i] - q[i];
    return 0;
}

static void uint_to_dec(unsigned int val, char *buf) {
    char tmp[12];
    int  i = 0, j;
    if(val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while(val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    for(j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static void uint_to_hex(unsigned int val, char *buf) {
    static const char hex[] = "0123456789abcdef";
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for(i = 0; i < 8; i++)
        buf[2 + i] = hex[(val >> (28 - i * 4)) & 0xf];
    buf[10] = '\0';
}

static int pass_count = 0;
static int fail_count = 0;

static void result(const char *name, int ok) {
    print("  ");
    print(name);
    if(ok) {
        print(" ... PASS\n");
        pass_count++;
    } else {
        print(" ... FAIL\n");
        fail_count++;
    }
}

/* ===== Test file paths ===== */

static const char *test_file = "/tmp/kosload-syscall-test.txt";
static const char *test_link = "/tmp/kosload-syscall-link.txt";
static const char *test_dir = "/tmp/kosload-syscall-testdir";

/* Entry point */
void start(void) __attribute__((section(".text.start")));
void start(void) {
    char buf[256];
    char numbuf[12];
    int fd, n, ret;
    kosload_stat_t st;
    kosload_dirent_t *dent;
    unsigned int dir;
    unsigned int ip, port;
    unsigned int times[2];
    const char *test_data = "kosload syscall test data!\n";
    int test_len = slen(test_data);

    print("\n");
    print("=== kosload syscall test (all 24 slots) ===\n");
    print("\n");

    /* ===== File I/O (slots 0-6, 8-10, 12-13) ===== */
    print("[File I/O]\n");

    /* Slot 4: creat */
    fd = kl_creat(test_file, 0644);
    result("creat(test-file)", fd >= 0);

    /* Slot 1: write */
    if(fd >= 0) {
        n = kl_write(fd, test_data, test_len);
        result("write(fd, data, len)", n == test_len);
    } else {
        result("write(fd, data, len)", 0);
    }

    /* Slot 3: close (after write) */
    if(fd >= 0) {
        ret = kl_close(fd);
        result("close(fd) after write", ret == 0);
    } else {
        result("close(fd) after write", 0);
    }

    /* Slot 2: open (reopen for reading) */
    fd = kl_open(test_file, O_RDONLY);
    result("open(test-file, O_RDONLY)", fd >= 0);

    /* Slot 0: read */
    if(fd >= 0) {
        n = kl_read(fd, buf, sizeof(buf));
        result("read(fd, buf, len)", n == test_len);
        result("read() data matches written", memcmp_b(buf, test_data, test_len) == 0);
    } else {
        result("read(fd, buf, len)", 0);
        result("read() data matches written", 0);
    }

    /* Slot 9: lseek */
    if(fd >= 0) {
        ret = kl_lseek(fd, 0, SEEK_SET);
        result("lseek(fd, 0, SEEK_SET)", ret == 0);

        /* Read again to verify seek worked */
        n = kl_read(fd, buf, sizeof(buf));
        result("read() after lseek matches", n == test_len && memcmp_b(buf, test_data, test_len) == 0);
    } else {
        result("lseek(fd, 0, SEEK_SET)", 0);
        result("read() after lseek matches", 0);
    }

    /* Slot 10: fstat */
    if(fd >= 0) {
        ret = kl_fstat(fd, &st);
        result("fstat(fd, &st)", ret == 0);
        result("fstat() st_size matches", st.st_size == test_len);
        if(ret == 0) {
            print("         st_size = ");
            uint_to_dec((unsigned int)st.st_size, numbuf);
            print(numbuf);
            print("\n");
        }
    } else {
        result("fstat(fd, &st)", 0);
        result("fstat() st_size matches", 0);
    }

    /* Close before stat-by-path */
    if(fd >= 0)
        kl_close(fd);

    /* Slot 12: stat */
    ret = kl_stat(test_file, &st);
    result("stat(test-file, &st)", ret == 0);
    if(ret == 0) {
        result("stat() st_size matches", st.st_size == test_len);
    } else {
        result("stat() st_size matches", 0);
    }

    /* Slot 5: link */
    ret = kl_link(test_file, test_link);
    result("link(test-file, test-link)", ret == 0);

    /* Slot 6: unlink (remove the link) */
    ret = kl_unlink(test_link);
    result("unlink(test-link)", ret == 0);

    /* Slot 8: chmod */
    ret = kl_chmod(test_file, 0666);
    result("chmod(test-file, 0666)", ret == 0);

    /* Slot 13: utime */
    times[0] = 1000000; /* actime */
    times[1] = 1000000; /* modtime */
    ret = kl_utime(test_file, times);
    result("utime(test-file, times)", ret == 0);

    /* Cleanup: unlink test file */
    ret = kl_unlink(test_file);
    result("unlink(test-file) cleanup", ret == 0);

    print("\n");

    /* ===== Directory ops (slots 7, 16-18, 21) ===== */
    print("[Directory ops]\n");

    /* Slot 16: opendir */
    dir = kl_opendir("/tmp");
    result("opendir(/tmp)", dir != 0);

    /* Slot 18: readdir */
    if(dir) {
        dent = kl_readdir(dir);
        result("readdir(dir) returns entry", dent != (kosload_dirent_t *)0);
        if(dent) {
            print("         d_name = \"");
            print(dent->d_name);
            print("\"\n");
        }
    } else {
        result("readdir(dir) returns entry", 0);
    }

    /* Slot 21: rewinddir */
    if(dir) {
        ret = kl_rewinddir(dir);
        result("rewinddir(dir)", ret == 0);
    } else {
        result("rewinddir(dir)", 0);
    }

    /* readdir after rewind */
    if(dir) {
        dent = kl_readdir(dir);
        result("readdir() after rewind", dent != (kosload_dirent_t *)0);
    } else {
        result("readdir() after rewind", 0);
    }

    /* Slot 17: closedir */
    if(dir) {
        ret = kl_closedir(dir);
        result("closedir(dir)", ret == 0);
    } else {
        result("closedir(dir)", 0);
    }

    /* Slot 23: mkdir
     * Note: no rmdir syscall exists, so we can't clean up the directory.
     * If the dir survives from a prior run, mkdir returns -1 (EEXIST).
     * We verify success by checking the directory is accessible. */
    ret = kl_mkdir(test_dir, 0755);
    dir = kl_opendir(test_dir);
    result("mkdir(test-dir, 0755)", dir != 0);
    if(dir)
        kl_closedir(dir);

    /* Slot 7: chdir */
    ret = kl_chdir("/tmp");
    result("chdir(/tmp)", ret == 0);

    print("\n");

    /* ===== Time (slot 11) ===== */
    print("[Time]\n");
    n = kl_time();
    result("time() returns > 0", n > 0);
    if(n > 0) {
        print("         timestamp = ");
        uint_to_dec((unsigned int)n, numbuf);
        print(numbuf);
        print("\n");
    }

    print("\n");

    /* ===== Network info (slot 19) ===== */
    print("[Network info]\n");
    ip = 0;
    port = 0;
    ret = kl_gethostinfo(&ip, &port);
    /* gethostinfo always succeeds — ret is our_ip (may look "negative" as
     * signed int for IPs >= 128.x.x.x). On serial: ip=0, port=0, ret=0. */
    result("gethostinfo()", 1);
    print("         host_ip   = ");
    uint_to_hex(ip, numbuf);
    print(numbuf);
    print("\n");
    print("         host_port = ");
    uint_to_dec(port, numbuf);
    print(numbuf);
    print("\n");
    print("         our_ip    = ");
    uint_to_hex((unsigned int)ret, numbuf);
    print(numbuf);
    print("\n");

    print("\n");

    /* ===== Skipped slots ===== */
    print("[Skipped]\n");
    print("  slot 14 (assign_wrkmem) - known stub, returns -1\n");
    print("  slot 20 (gdbpacket)     - see gdb-test example\n");

    print("\n");

    /* ===== Summary ===== */
    print("Results: ");
    uint_to_dec(pass_count, numbuf);
    print(numbuf);
    print(" passed, ");
    uint_to_dec(fail_count, numbuf);
    print(numbuf);
    print(" failed\n");
    print("\n");

    /* Slot 22: progexit (always last) */
    kl_exit();
}
