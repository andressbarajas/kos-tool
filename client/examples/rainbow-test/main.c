/* examples/rainbow-test/main.c */
/*
 * rainbow-test - Display 7 ROYGBIV rainbow color bands
 *
 * Fills the screen with 7 horizontal bands (Red, Orange, Yellow,
 * Green, Blue, Indigo, Violet), waits ~10 seconds, then exits.
 *
 * Tests video output by writing directly to the framebuffer.
 *
 * Load and run:
 *   kos-tool -x rainbow-test.elf
 */

/* ===== Platform-specific framebuffer configuration ===== */

#if defined(__sh__) || defined(__SH4_SINGLE__)

/* DC: 640x480 RGB565 little-endian, VRAM at 0xa5000000 (P2 uncached) */
#define KOSLOAD_BASE 0x8c004000
#define FB_ADDR      0xa5000000
#define SCREEN_W     640
#define SCREEN_H     480

/* Two RGB565 pixels packed into one 32-bit word (little-endian) */
#define COLOR_RED    0xF800F800 /* RGB(255,0,0) */
#define COLOR_ORANGE 0xFD20FD20 /* RGB(255,165,0) */
#define COLOR_YELLOW 0xFFE0FFE0 /* RGB(255,255,0) */
#define COLOR_GREEN  0x07E007E0 /* RGB(0,255,0) */
#define COLOR_BLUE   0x001F001F /* RGB(0,0,255) */
#define COLOR_INDIGO 0x48104810 /* RGB(75,0,130) */
#define COLOR_VIOLET 0x901A901A /* RGB(148,0,211) */

#elif defined(__PPC__) || defined(__powerpc__)

/* GC: 640x480i YCbYCr big-endian, XFB at 0xC0050000 (uncached MEM1) */
#if defined(WII_KOSLOAD_BASE)
#define KOSLOAD_BASE WII_KOSLOAD_BASE
#elif defined(GC_KOSLOAD_BASE)
#define KOSLOAD_BASE GC_KOSLOAD_BASE
#else
#define KOSLOAD_BASE 0x817EC000
#endif
#define FB_ADDR      0xC0050000
#define SCREEN_W     640
#define SCREEN_H     480

/*
 * YCbYCr pixel pairs: (Y0 << 24) | (Cb << 16) | (Y1 << 8) | Cr
 * BT.601 studio range.
 */
#define COLOR_RED    0x525B52F0 /* RGB(255,0,0) */
#define COLOR_ORANGE 0xA52AA5B3 /* RGB(255,165,0) */
#define COLOR_YELLOW 0xD210D292 /* RGB(255,255,0) */
#define COLOR_GREEN  0x91379123 /* RGB(0,255,0) */
#define COLOR_BLUE   0x29F0296E /* RGB(0,0,255) */
#define COLOR_INDIGO 0x30AE3098 /* RGB(75,0,130) */
#define COLOR_VIOLET 0x4BC64BB2 /* RGB(148,0,211) */

#else
#error "Unsupported architecture"
#endif

/* ===== Kosload syscall interface ===== */

#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
#define KOSLOAD_MAGIC        0xdeadbeef

#define SYSCALL_EXIT 15

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);

/* ===== Entry point ===== */

void start(void) __attribute__((section(".text.start")));
void start(void) {
    volatile unsigned int *fb = (volatile unsigned int *)FB_ADDR;
    /* Both DC and GC: 2 pixels per 32-bit word, so 320 words per line */
    int words_per_line = SCREEN_W / 2;
    int lines_per_band = SCREEN_H / 7;  /* 68 lines each */
    int words_per_band = words_per_line * lines_per_band;
    unsigned int colors[7] = {
        COLOR_RED, COLOR_ORANGE, COLOR_YELLOW, COLOR_GREEN,
        COLOR_BLUE, COLOR_INDIGO, COLOR_VIOLET
    };
    int band, i, offset;

    offset = 0;
    for(band = 0; band < 7; band++) {
        for(i = 0; i < words_per_band; i++)
            fb[offset + i] = colors[band];
        offset += words_per_band;
    }

    /* Wait ~10 seconds (spin loop).
     * DC runs with cache disabled (dc_execute calls disable_cache),
     * so each iteration is ~200 CPU cycles at 200MHz uncached.
     * GC runs with cache enabled at 486MHz, ~10 cycles/iteration. */
    volatile int j;
#if defined(__sh__) || defined(__SH4_SINGLE__)
    for(j = 0; j < 10000000; j++)
#else
    for(j = 0; j < 400000000; j++)
#endif
        ;

    /* Exit via kosload syscall */
    if(KOSLOAD_MAGIC_ADDR == KOSLOAD_MAGIC) {
        kosload_syscall_fn sc = (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
        sc(SYSCALL_EXIT, 0, 0, 0);
    }
}
