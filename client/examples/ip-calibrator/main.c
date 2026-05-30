/* examples/ip-calibrator/main.c */
/*
 * ip-calibrator - small, repeatable workload for host network profile tuning.
 *
 * The host-side --calibrate-ip runner uploads this program repeatedly while
 * trying different per-adapter pacing profiles. Keep the output compact and
 * machine-readable enough for future host-side parsing.
 */

#define SYSCALL_WRITE 1
#define SYSCALL_EXIT  15

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
#define KOSLOAD_BASE 0x80000284
#endif
#else
#error "Unsupported architecture"
#endif

#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

#define CONSOLE_LINES 16

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

static kosload_syscall_fn get_syscall(void) {
    if(KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return (kosload_syscall_fn)0;
    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

static int sc(int syscall, int arg1, int arg2, int arg3) {
    kosload_syscall_fn fn = get_syscall();
    if(!fn)
        return -1;
    return fn(syscall, arg1, arg2, arg3);
}

static int slen(const char *s) {
    int n = 0;
    while(*s++)
        n++;
    return n;
}

static void print(const char *s) {
    sc(SYSCALL_WRITE, 1, (int)s, slen(s));
}

static void print_u(unsigned int v) {
    char buf[11];
    int  pos = 10;
    buf[pos] = '\0';
    if(v == 0) {
        print("0");
        return;
    }
    while(v && pos > 0) {
        buf[--pos] = (char)('0' + (v % 10));
        v /= 10;
    }
    print(buf + pos);
}

static void print_end(unsigned int pass, unsigned int fail) {
    char buf[32];
    int  p = 0;

    buf[p++] = 'C';
    buf[p++] = 'A';
    buf[p++] = 'L';
    buf[p++] = ' ';
    buf[p++] = 'E';
    buf[p++] = 'N';
    buf[p++] = 'D';
    buf[p++] = ' ';
    buf[p++] = 'p';
    buf[p++] = 'a';
    buf[p++] = 's';
    buf[p++] = 's';
    buf[p++] = '=';
    buf[p++] = (pass == 1) ? '1' : '0';
    buf[p++] = ' ';
    buf[p++] = 'f';
    buf[p++] = 'a';
    buf[p++] = 'i';
    buf[p++] = 'l';
    buf[p++] = '=';
    buf[p++] = fail ? '1' : '0';
    buf[p++] = '\n';

    sc(SYSCALL_WRITE, 1, (int)buf, p);
}

static int console_burst(void) {
    unsigned int i;

    print("CAL console-burst begin lines=");
    print_u(CONSOLE_LINES);
    print("\n");

    for(i = 0; i < CONSOLE_LINES; i++) {
        print("CAL console-line index=");
        print_u(i);
        print(" payload=abcdefghijklmnopqrstuvwxyz0123456789\n");
    }

    print("CAL console-burst end\n");
    return 1;
}

void start(void) __attribute__((section(".text.start")));
void start(void) {
    unsigned int pass = 0;
    unsigned int fail = 0;

    print("\nCAL BEGIN ip-calibrator 1\n");

    if(console_burst())
        pass++;
    else
        fail++;

    print_end(pass, fail);

    sc(SYSCALL_EXIT, fail ? 1 : 0, 0, 0);
}
