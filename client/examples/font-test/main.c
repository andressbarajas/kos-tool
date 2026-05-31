/* examples/font-test/main.c */
/*
 * font-test - render the GameCube loader font on screen.
 *
 * Uses the loader's video callbacks so the output is drawn with the exact
 * 12x24 font embedded in client/gamecube/video.c.
 *
 * Load and run:
 *   kos-tool -x font-test.elf
 */

#if defined(__PPC__) || defined(__powerpc__)
#if defined(WII_KOSLOAD_BASE)
#define KOSLOAD_BASE WII_KOSLOAD_BASE
#elif defined(GC_KOSLOAD_BASE)
#define KOSLOAD_BASE GC_KOSLOAD_BASE
#else
#define KOSLOAD_BASE 0x817EC000
#endif
/* GC/Wii crt0 header: +0x14 clear_screen, +0x18 draw_string. */
#define KOSLOAD_CLEAR_SCREEN_OFF 0x14
#define KOSLOAD_DRAW_STRING_OFF  0x18
#define TICKS_PER_SEC 40500000u  /* PPC timebase (mftb) */
#elif defined(__mips__) || defined(__mips)
#ifdef PS2_KOSLOAD_BASE
#define KOSLOAD_BASE PS2_KOSLOAD_BASE
#else
#define KOSLOAD_BASE 0x80000284
#endif
/* The PS2 crt0 header inserts a SIF-broker iface slot at +0x10 that
 * GC/Wii don't have, shifting the video callbacks down one slot:
 * +0x18 clear_screen, +0x1C draw_string (see client/playstation2/crt0.S). */
#define KOSLOAD_CLEAR_SCREEN_OFF 0x18
#define KOSLOAD_DRAW_STRING_OFF  0x1C
#define TICKS_PER_SEC 294912000u /* R5900 COP0 Count (mfc0 $9) */
#else
#error "font-test is GameCube/Wii/PS2-only"
#endif

#define KOSLOAD_MAGIC_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
/* Loader video callbacks.  The offsets differ per console because the PS2
 * crt0 header has an extra slot (see above); a wrong offset lands one slot
 * off — e.g. draw_string() would actually invoke clear_screen() and wipe
 * the screen on every glyph. */
#define KOSLOAD_CLEAR_SCREEN_ADDR (*(volatile unsigned int *)(KOSLOAD_BASE + KOSLOAD_CLEAR_SCREEN_OFF))
#define KOSLOAD_DRAW_STRING_ADDR  (*(volatile unsigned int *)(KOSLOAD_BASE + KOSLOAD_DRAW_STRING_OFF))

#define KOSLOAD_MAGIC            0xdeadbeef
#define KOSLOAD_MAGIC_NO_CONSOLE 0xfeedface

#define SYSCALL_WRITE 1
#define SYSCALL_EXIT  15

#define DISPLAY_SECONDS  30

#define COLOR_BG    0x00001820
#define COLOR_TITLE 0x00ffd866
#define COLOR_TEXT  0x00f2f2f2
#define COLOR_DIM   0x0096c7ff

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);
typedef void (*clear_screen_fn)(unsigned int color);
typedef void (*draw_string_fn)(int x, int y, const char *str, unsigned int color);

static int kosload_present(void) {
    unsigned int magic = KOSLOAD_MAGIC_ADDR;

    return magic == KOSLOAD_MAGIC || magic == KOSLOAD_MAGIC_NO_CONSOLE;
}

static kosload_syscall_fn get_syscall(void) {
    if(!kosload_present())
        return (kosload_syscall_fn)0;

    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

static int slen(const char *s) {
    int n = 0;

    while(*s++)
        n++;

    return n;
}

static void print(const char *msg) {
    kosload_syscall_fn syscall = get_syscall();

    if(syscall)
        syscall(SYSCALL_WRITE, 1, (int)msg, slen(msg));
}

static void kl_exit(void) {
    kosload_syscall_fn syscall = get_syscall();

    if(syscall)
        syscall(SYSCALL_EXIT, 0, 0, 0);
}

static unsigned int get_ticks(void) {
    unsigned int t;

#if defined(__PPC__) || defined(__powerpc__)
    __asm__ volatile("mftb %0" : "=r"(t));
#else /* MIPS R5900: COP0 Count register ($9) */
    __asm__ volatile("mfc0 %0, $9" : "=r"(t));
#endif
    return t;
}

/* Count elapsed seconds one at a time.  A single
 * (now - start) < TICKS_PER_SEC*seconds comparison overflows 32-bit at the
 * PS2's 294 MHz rate and can't span the COP0 Count's ~14.6 s wrap; the
 * per-second unsigned delta handles both (the tight poll never misses a
 * wrap). */
static void wait_seconds(unsigned int seconds) {
    unsigned int last = get_ticks();
    unsigned int done = 0;

    while(done < seconds) {
        if((unsigned int)(get_ticks() - last) >= TICKS_PER_SEC) {
            last += TICKS_PER_SEC;
            done++;
        }
    }
}

static void draw_font_page(void) {
    clear_screen_fn clear_screen;
    draw_string_fn  draw_string;

    if(!kosload_present())
        return;

    clear_screen = (clear_screen_fn)KOSLOAD_CLEAR_SCREEN_ADDR;
    draw_string = (draw_string_fn)KOSLOAD_DRAW_STRING_ADDR;

    clear_screen(COLOR_BG);

    draw_string(24, 24, "GC/Wii/PS2 12x24 FONT TEST", COLOR_TITLE);
    draw_string(24, 56, "Printable ASCII 32-126, drawn by kosload", COLOR_DIM);

    draw_string(24, 104, "32-47:  !\"#$%&'()*+,-./", COLOR_TEXT);
    draw_string(24, 136, "48-63: 0123456789:;<=>?", COLOR_TEXT);
    draw_string(24, 168, "64-79: @ABCDEFGHIJKLMNO", COLOR_TEXT);
    draw_string(24, 200, "80-95: PQRSTUVWXYZ[\\]^_", COLOR_TEXT);
    draw_string(24, 232, "96-111: `abcdefghijklmno", COLOR_TEXT);
    draw_string(24, 264, "112-126: pqrstuvwxyz{|}~", COLOR_TEXT);

    draw_string(24, 328, "Space is the blank glyph after the 32-47 colon.", COLOR_DIM);
    draw_string(24, 360, "Holding this screen for 30 seconds.", COLOR_DIM);
}

void start(void) __attribute__((section(".text.start")));
void start(void) {
    print("\n=== GC/Wii/PS2 font test ===\n");
    print("Displaying printable ASCII for 30 seconds.\n");

    draw_font_page();
    wait_seconds(DISPLAY_SECONDS);

    print("GC/Wii/PS2 font test complete.\n");
    kl_exit();
}
