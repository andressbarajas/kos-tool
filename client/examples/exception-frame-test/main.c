/* examples/exception-frame-test/main.c */
/*
 * exception-frame-test - synthetic exception frame test
 *
 * Constructs a synthetic exception frame with known register values
 * and writes it via SYSCALL_WRITE to test the host's exception
 * frame parsing and formatting (handle_dc_exception / handle_gc_exception
 * in console.c).
 *
 * Unlike exception-test (which triggers a real hardware exception caught
 * by the firmware), this test crafts a frame with predictable values so
 * you can verify the host output is correct.
 *
 * Expected host output:
 *   - Exception type banner with correct code string
 *   - All registers printed with recognizable pattern values
 *   - addr2line resolution of PC/SRR0 pointing to crash_site()
 *
 * Load and run:
 *   kostool -x exception-frame-test.elf
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

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

static int slen(const char *s) {
    int n = 0;
    while(*s++)
        n++;
    return n;
}

static int kl_write(int fd, const void *buf, int count) {
    kosload_syscall_fn sc;
    if(KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return -1;
    sc = (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
    return sc(SYSCALL_WRITE, fd, (int)buf, count);
}

static void print(const char *msg) {
    kl_write(1, msg, slen(msg));
}

static void kl_exit(void) {
    kosload_syscall_fn sc;
    if(KOSLOAD_MAGIC_ADDR != KOSLOAD_MAGIC)
        return;
    sc = (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
    sc(SYSCALL_EXIT, 0, 0, 0);
}

/*
 * Dummy function whose address we put in PC/SRR0.
 * addr2line should resolve this to "crash_site" in the host output.
 */
void start(void) __attribute__((section(".text.start")));
void crash_site(void) __attribute__((noinline, used));
void crash_site(void) {
    /* Prevent optimization; never actually called */
    __asm__ volatile("" ::: "memory");
}

#if defined(__sh__) || defined(__SH4_SINGLE__)
/*
 * SH4 exception frame layout (272 bytes):
 *   [0]     "EXPT" tag (4 bytes)
 *   [1]     expt_code
 *   [2..67] 66 registers: PC, PR, SR, GBR, VBR, DBR, MACH, MACL,
 *           R0B0-R7B0, R0B1-R7B1, R8-R15,
 *           FPSCR, FR0-FR15, FPUL, XF0-XF15
 */
#define FRAME_WORDS 68
#define FRAME_BYTES 272
#define NUM_REGS    66

static void send_dc_frame(void) {
    unsigned int   frame[FRAME_WORDS];
    unsigned char *tag = (unsigned char *)&frame[0];
    int i;

    /* Clear */
    for(i = 0; i < FRAME_WORDS; i++)
        frame[i] = 0;

    /* EXPT tag */
    tag[0] = 'E';
    tag[1] = 'X';
    tag[2] = 'P';
    tag[3] = 'T';

    /* Exception code: Address Error (read) = 0x0E0 */
    frame[1] = 0x0E0;

    /* Registers (word index 2 = first register = PC) */
    frame[2] = (unsigned int)&crash_site; /* PC */
    frame[3] = 0xDEAD0001;                /* PR */
    frame[4] = 0x40000000;                /* SR */
    frame[5] = 0x00000000;                /* GBR */
    frame[6] = 0x8C00F400;                /* VBR */
    frame[7] = 0x00000000;                /* DBR */
    frame[8] = 0x00000000;                /* MACH */
    frame[9] = 0x00000000;                /* MACL */

    /* R0B0-R7B0 (bank 0 GPRs) */
    for(i = 0; i < 8; i++)
        frame[10 + i] = 0xB0000000 | i;

    /* R0B1-R7B1 (bank 1 GPRs) */
    for(i = 0; i < 8; i++)
        frame[18 + i] = 0xB1000000 | i;

    /* R8-R15 */
    for(i = 0; i < 7; i++)
        frame[26 + i] = 0xAA000008 + i;
    frame[33] = 0x8CFFF000; /* R15 = SP */

    /* FPSCR */
    frame[34] = 0x00040001;

    /* FR0-FR15: pattern */
    for(i = 0; i < 16; i++)
        frame[35 + i] = 0xF0000000 | i;

    /* FPUL */
    frame[51] = 0x00000042;

    /* XF0-XF15: pattern */
    for(i = 0; i < 16; i++)
        frame[52 + i] = 0x0F000000 | i;

    print("Sending DC exception frame (272 bytes)...\n");
    kl_write(1, frame, FRAME_BYTES);
    print("Done.\n");
}
#endif

#if defined(__PPC__) || defined(__powerpc__)
/*
 * PPC 750 exception frame layout (168 bytes):
 *   [0]     "EXPT" tag (4 bytes)
 *   [1]     expt_code
 *   [2..41] 40 registers: SRR0, SRR1, R0-R31, LR, CTR, XER, CR, DSISR, DAR
 *   [42..107] FPU: FPSCR (2 words) + f0-f31 (2 words each)
 */
#define FRAME_WORDS 108
#define FRAME_BYTES 432
#define NUM_REGS    40

static void send_gc_frame(void) {
    unsigned int   frame[FRAME_WORDS];
    unsigned char *tag = (unsigned char *)&frame[0];
    int i;

    /* Clear */
    for(i = 0; i < FRAME_WORDS; i++)
        frame[i] = 0;

    /* EXPT tag */
    tag[0] = 'E';
    tag[1] = 'X';
    tag[2] = 'P';
    tag[3] = 'T';

    /* Exception code: DSI = 0x0300 */
    frame[1] = 0x0300;

    /* SRR0 = PC at exception */
    frame[2] = (unsigned int)&crash_site;

    /* SRR1 = saved MSR */
    frame[3] = 0x00002032;

    /* R0-R31 */
    for(i = 0; i < 32; i++)
        frame[4 + i] = 0xAA000000 | i;
    frame[4 + 1] = 0x817E0000; /* R1 = SP */
    frame[4 + 2] = 0x80003100; /* R2 = typical TOC/SDA */

    /* LR */
    frame[36] = (unsigned int)&start;

    /* CTR */
    frame[37] = 0x00000000;

    /* XER */
    frame[38] = 0x00000000;

    /* CR */
    frame[39] = 0x22000482;

    /* DSISR */
    frame[40] = 0x40000000;

    /* DAR = faulting address */
    frame[41] = 0xC0000000;

    /* FPSCR (8 bytes as two uint32: hi=reserved, lo=FPSCR bits) */
    frame[42] = 0x00000000;
    frame[43] = 0x000000F8;

    /* f0-f31 (8 bytes each as two uint32: hi, lo) */
    for(i = 0; i < 32; i++) {
        frame[44 + i * 2] = 0x3FF00000 + i;  /* hi word */
        frame[44 + i * 2 + 1] = (unsigned int)i * 0x11111111; /* lo word */
    }

    print("Sending GC exception frame (432 bytes)...\n");
    kl_write(1, frame, FRAME_BYTES);
    print("Done.\n");
}
#endif

#if defined(__mips__) || defined(__mips)
/*
 * R5900 (EE) exception frame layout (420 bytes), matching
 * ps2_exception_frame_t on the host:
 *   [0]        "EXPT" tag (4 bytes)
 *   [1..64]    32 GPRs, each 64-bit: low word then high word
 *   [65]       COP0 EPC
 *   [66]       COP0 Status
 *   [67]       COP0 Cause
 *   [68]       COP0 BadVAddr
 *   [69..100]  FPU f0-f31 (one word each)
 *   [101]      FCR31
 *   [102..104] padding to the 416-byte save area
 */
#define PS2_FRAME_WORDS 105
#define PS2_FRAME_BYTES 420

static void send_ps2_frame(void) {
    unsigned int   frame[PS2_FRAME_WORDS];
    unsigned char *tag = (unsigned char *)&frame[0];
    int i;

    for(i = 0; i < PS2_FRAME_WORDS; i++)
        frame[i] = 0;

    /* EXPT tag */
    tag[0] = 'E';
    tag[1] = 'X';
    tag[2] = 'P';
    tag[3] = 'T';

    /* GPRs r0-r31: low word at [1 + i*2], high word at [1 + i*2 + 1]. */
    for(i = 0; i < 32; i++)
        frame[1 + i * 2] = 0xAA000000 | (unsigned int)i; /* low 32 bits */
    frame[1 + 28 * 2] = 0x00001000;          /* gp */
    frame[1 + 29 * 2] = 0x001FFFF0;          /* sp */
    frame[1 + 31 * 2] = (unsigned int)&start; /* ra */

    /* COP0: Cause.ExcCode (bits 6:2) = 4 → Address error (load/fetch). */
    frame[65] = (unsigned int)&crash_site;   /* EPC */
    frame[66] = 0x00400000;                  /* Status */
    frame[67] = 4u << 2;                     /* Cause (ExcCode=4) */
    frame[68] = 0x00000002;                  /* BadVAddr (misaligned) */

    /* FPU f0-f31 + FCR31 patterns */
    for(i = 0; i < 32; i++)
        frame[69 + i] = 0xF0000000 | (unsigned int)i;
    frame[101] = 0x0000000E;                 /* FCR31 */

    print("Sending PS2 exception frame (420 bytes)...\n");
    kl_write(1, frame, PS2_FRAME_BYTES);
    print("Done.\n");
}
#endif

void start(void) {
    print("\n");
    print("=== kosload exception frame test ===\n");
    print("\n");
    print("This sends a synthetic exception frame to the host.\n");
    print("Check the host terminal for the formatted register dump.\n");
    print("\n");

#if defined(__sh__) || defined(__SH4_SINGLE__)
    send_dc_frame();
#elif defined(__PPC__) || defined(__powerpc__)
    send_gc_frame();
#elif defined(__mips__) || defined(__mips)
    send_ps2_frame();
#endif

    print("\n");
    print("Exception frame test complete.\n");
    print("\n");

    kl_exit();
}
