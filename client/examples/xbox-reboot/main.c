/* examples/xbox-reboot/main.c
 *
 * Reset the Xbox back to the dashboard.
 *
 * Uses the chipset reset-control register (port 0xCF9) — the same mechanism the
 * kernel's HalReturnToFirmware uses (recovered by RE @ 0x80015278): signal the
 * SMC that the reset is intentional, then pulse 0xCF9 for a full reset.  The
 * reset re-runs the flash bootloader, which reloads the kernel from ROM and
 * boots the dashboard (when no disc is inserted).
 *
 * This is reliable even after the loader has taken over the machine (its own
 * IDT/GDT, or the kernel erased from RAM) — a CPU triple-fault would hang in
 * that state because the fault-vector chain is gone.
 *
 *   kostool -t <ip> -x xbox-reboot.elf
 */

#define KOSLOAD_BASE   0x00011000u
#define KOSLOAD_MAGIC  0xdeadbeefu
#define SYSCALL_WRITE  1

typedef int  (*syscall_fn)(int, int, int, int);
typedef void (*clear_screen_fn)(unsigned int);
typedef void (*draw_string_fn)(int, int, const char *, unsigned int);

static inline unsigned short inw(unsigned short p) {
    unsigned short v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v;
}
static inline void outw(unsigned short p, unsigned short v) {
    __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p));
}
static inline void outb(unsigned short p, unsigned char v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}

static int slen(const char *s) { int n = 0; while(s[n]) n++; return n; }

/* SMBus byte write to the SMC (device 0x20) over the MCPX controller ports. */
static void smc_write(unsigned char reg, unsigned char val) {
    unsigned int t;
    outw(0xC000, inw(0xC000));   /* clear status         */
    outb(0xC004, 0x20);          /* SMC write address    */
    outb(0xC008, reg);           /* register             */
    outw(0xC006, val);           /* data                 */
    outb(0xC002, 0x1A);          /* byte transaction     */
    for(t = 0; t < 100000u; t++) {
        unsigned short s = inw(0xC000);
        if(s & (0x10u | 0x26u)) { outw(0xC000, s); break; }
    }
}

void start(void) __attribute__((section(".text.start")));
void start(void) {
    volatile unsigned int *hdr = (volatile unsigned int *)KOSLOAD_BASE;
    syscall_fn sc;
    clear_screen_fn clear_screen;
    draw_string_fn draw_string;
    const char *msg = "Resetting console -> dashboard...";
    unsigned int i;

    if(hdr[0] != KOSLOAD_MAGIC)
        return;
    sc           = (syscall_fn)hdr[1];
    clear_screen = (clear_screen_fn)hdr[4];
    draw_string  = (draw_string_fn)hdr[5];

    clear_screen(0x00000000u);
    draw_string(120, 200, msg, 0x00ffffffu);
    sc(SYSCALL_WRITE, 1, (int)msg, slen(msg));
    sc(SYSCALL_WRITE, 1, (int)"\n", 1);

    /* Brief pause so the message is visible before the reset. */
    for(i = 0; i < 800000000u; i++)
        __asm__ volatile("pause");

    /* Chipset reset -> flash bootloader -> dashboard. */
    smc_write(0x02, 0x01);       /* SMC: intentional reset  */
    outb(0x0CF9u, 0x0Eu);        /* chipset full reset      */
    for(;;)
        __asm__ volatile("cli; hlt");
}
