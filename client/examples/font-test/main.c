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
#ifdef GC_KOSLOAD_BASE
#define KOSLOAD_BASE    GC_KOSLOAD_BASE
#else
#define KOSLOAD_BASE    0x817EC000
#endif
#else
#error "font-test is GameCube-only"
#endif

#define KOSLOAD_MAGIC_ADDR         (*(volatile unsigned int *)(KOSLOAD_BASE + 4))
#define KOSLOAD_SYSCALL_ADDR       (*(volatile unsigned int *)(KOSLOAD_BASE + 8))
/* crt0 header layout: +0x10 setup_video, +0x14 clear_screen,
 * +0x18 draw_string (see client/gamecube/crt0.S).  These were off
 * by one slot, so draw_string() actually invoked clear_screen() —
 * every glyph call wiped the screen and nothing was rendered. */
#define KOSLOAD_CLEAR_SCREEN_ADDR  (*(volatile unsigned int *)(KOSLOAD_BASE + 0x14))
#define KOSLOAD_DRAW_STRING_ADDR   (*(volatile unsigned int *)(KOSLOAD_BASE + 0x18))

#define KOSLOAD_MAGIC              0xdeadbeef
#define KOSLOAD_MAGIC_NO_CONSOLE   0xfeedface

#define SYSCALL_WRITE              1
#define SYSCALL_EXIT               15

#define TB_TICKS_PER_SEC           40500000u
#define DISPLAY_SECONDS            60u

#define COLOR_BG                   0x00001820u
#define COLOR_TITLE                0x00ffd866u
#define COLOR_TEXT                 0x00f2f2f2u
#define COLOR_DIM                  0x0096c7ffu

typedef int (*kosload_syscall_fn)(int syscall, int arg1, int arg2, int arg3);
typedef void (*clear_screen_fn)(unsigned int color);
typedef void (*draw_string_fn)(int x, int y, const char *str, unsigned int color);

static int kosload_present(void)
{
    unsigned int magic = KOSLOAD_MAGIC_ADDR;

    return magic == KOSLOAD_MAGIC || magic == KOSLOAD_MAGIC_NO_CONSOLE;
}

static kosload_syscall_fn get_syscall(void)
{
    if (!kosload_present())
        return (kosload_syscall_fn)0;

    return (kosload_syscall_fn)KOSLOAD_SYSCALL_ADDR;
}

static int slen(const char *s)
{
    int n = 0;

    while (*s++)
        n++;

    return n;
}

static void print(const char *msg)
{
    kosload_syscall_fn syscall = get_syscall();

    if (syscall)
        syscall(SYSCALL_WRITE, 1, (int)msg, slen(msg));
}

static void kl_exit(void)
{
    kosload_syscall_fn syscall = get_syscall();

    if (syscall)
        syscall(SYSCALL_EXIT, 0, 0, 0);
}

static unsigned int get_tbl(void)
{
    unsigned int tbl;

    __asm__ volatile("mftb %0" : "=r"(tbl));
    return tbl;
}

static void wait_seconds(unsigned int seconds)
{
    unsigned int start = get_tbl();
    unsigned int ticks = TB_TICKS_PER_SEC * seconds;

    while ((unsigned int)(get_tbl() - start) < ticks)
        ;
}

static void draw_font_page(void)
{
    clear_screen_fn clear_screen;
    draw_string_fn draw_string;

    if (!kosload_present())
        return;

    clear_screen = (clear_screen_fn)KOSLOAD_CLEAR_SCREEN_ADDR;
    draw_string = (draw_string_fn)KOSLOAD_DRAW_STRING_ADDR;

    clear_screen(COLOR_BG);

    draw_string(24, 24,  "GC 12x24 FONT TEST", COLOR_TITLE);
    draw_string(24, 56,  "Printable ASCII 32-126, drawn by kosload", COLOR_DIM);

    draw_string(24, 104, "32-47:  !\"#$%&'()*+,-./", COLOR_TEXT);
    draw_string(24, 136, "48-63: 0123456789:;<=>?", COLOR_TEXT);
    draw_string(24, 168, "64-79: @ABCDEFGHIJKLMNO", COLOR_TEXT);
    draw_string(24, 200, "80-95: PQRSTUVWXYZ[\\]^_", COLOR_TEXT);
    draw_string(24, 232, "96-111: `abcdefghijklmno", COLOR_TEXT);
    draw_string(24, 264, "112-126: pqrstuvwxyz{|}~", COLOR_TEXT);

    draw_string(24, 328, "Space is the blank glyph after the 32-47 colon.", COLOR_DIM);
    draw_string(24, 360, "Holding this screen for 60 seconds.", COLOR_DIM);
}

void start(void) __attribute__((section(".text.start")));
void start(void)
{
    print("\n=== GC font test ===\n");
    print("Displaying printable ASCII for 60 seconds.\n");

    draw_font_page();
    wait_seconds(DISPLAY_SECONDS);

    print("GC font test complete.\n");
    kl_exit();
}
