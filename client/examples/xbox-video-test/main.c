/* Xbox guest-facing video API smoke test. */

#define KOSLOAD_BASE 0x00011000u
#define KOSLOAD_MAGIC 0xdeadbeefu

#define SYSCALL_WRITE 1
#define SYSCALL_EXIT  15

typedef int  (*syscall_fn)(int, int, int, int);
typedef void (*clear_screen_fn)(unsigned int);
typedef void (*draw_string_fn)(int, int, const char *, unsigned int);

static int slen(const char *s) {
    int n = 0;
    while(s[n])
        n++;
    return n;
}

void start(void) __attribute__((section(".text.start")));
void start(void) {
    volatile unsigned int *header = (volatile unsigned int *)KOSLOAD_BASE;
    syscall_fn syscall;
    clear_screen_fn clear_screen;
    draw_string_fn draw_string;
    const char *message = "Xbox guest video API works";

    if(header[0] != KOSLOAD_MAGIC)
        return;

    syscall = (syscall_fn)header[1];
    clear_screen = (clear_screen_fn)header[4];
    draw_string = (draw_string_fn)header[5];

    clear_screen(0x00202080u);
    draw_string(120, 180, "XBOX VIDEO TEST", 0x00ffffffu);
    draw_string(120, 228, message, 0x00ffffffu);
    syscall(SYSCALL_WRITE, 1, (int)message, slen(message));
    syscall(SYSCALL_WRITE, 1, (int)"\n", 1);

    for(volatile unsigned int i = 0; i < 250000000u; i++)
        __asm__ volatile("pause");

    syscall(SYSCALL_EXIT, 0, 0, 0);
}
