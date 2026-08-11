/* client/psp/video.c — PSP dumb-framebuffer text console.
 *
 * Writes into VRAM (PSP_VRAM_BASE, uncached alias 0x44000000) using the shared
 * 12x24 font.  The screen is 480x272; the display framebuffer stride is 512
 * pixels in RGBA8888.  These are documented PSP hardware facts (screen size,
 * VRAM location, buffer stride, pixel format).
 *
 * BARE-METAL (no firmware syscall): the LCD is pointed at our VRAM buffer by
 * programming the DMACPlus "framebuffer scanout" registers directly, NOT via
 * sceDisplaySetFrameBuf.  The register offsets are from the PSP hardware docs
 * (psdevwiki, DMACPlus 0xBC800000) and were CONFIRMED against Sony's decrypted
 * sceDmacplus_driver (lowio.prx): +0x100 fb addr, +0x104 hardware pixfmt,
 * +0x108 width, +0x10C stride, and +0x110 control.  Bit 0 of +0x110 enables
 * scanout; bit 1 tracks the LCDC interrupt and is not a framebuffer latch.
 * Preserve every inherited control bit except for briefly clearing bit 0 while
 * rebinding the buffer.  Retail display_01g.prx also proves that hardware
 * pixfmt 0 is RGBA8888 after translating the public display-format enum.
 *
 * COLD-BOOT PANEL: psp_lcdc_panel_init() programs the LCDC timing controller
 * (0xBE140000) with the 480x272 PSP-1000 timing recovered from Sony's
 * sceLcdc_driver (lowio.prx, panel descriptor id 0) — register meanings from
 * psdevwiki, values from the RE.  It is idempotent when the panel is already up
 * (launched under a homebrew loader), and required at true cold boot.
 *
 * The LCD backlight is turned on via the syscon SSP (syscon.c, RE'd from Sony's
 * sceSyscon_driver).  The full cold-boot chain is now implemented: display-FIFO
 * reset (0xBE700000) -> LCDC panel timing (0xBE140000) -> scanout (0xBC800100)
 * -> backlight -- all RE'd from Sony's drivers, no firmware syscall.  Hardware
 * register access requires the module to load in KERNEL mode (module_info
 * attribute; see module_info.c).  Untested on hardware.
 */

#include <stdint.h>
#include "video.h"
#include "syscon.h"
#include "../common/font_12x24.h"

#ifndef PSP_VRAM_BASE
#define PSP_VRAM_BASE 0x04000000
#endif
#define PSP_VRAM_UNCACHED (0x40000000u | PSP_VRAM_BASE)

/* DMACPlus framebuffer-scanout registers (psdevwiki: 0xBC800000 "DmacplusLcdc").
 * 0xBCxxxxxx is the uncached, kernel-mode MMIO alias. */
#define DMACPLUS_FB_ADDR    ((volatile uint32_t *)0xBC800100) /* fb phys, bits[28:0] */
#define DMACPLUS_FB_PIXFMT  ((volatile uint32_t *)0xBC800104) /* 0=RGBA8888 */
#define DMACPLUS_FB_WIDTH   ((volatile uint32_t *)0xBC800108) /* visible width */
#define DMACPLUS_FB_STRIDE  ((volatile uint32_t *)0xBC80010C) /* line stride (pixels) */
#define DMACPLUS_FB_SCANOUT ((volatile uint32_t *)0xBC800110) /* control; bit0 enable */

#define DMACPLUS_PIXFMT_8888 0

/* LCDC panel timing controller (psdevwiki 0xBE140000; 480x272 values RE'd from
 * Sony's sceLcdc_driver in lowio.prx, panel descriptor id 0). */
#define LCDC_BASE           0xBE140000u
#define LCDC(off)           (*(volatile uint32_t *)(LCDC_BASE + (uint32_t)(off)))
#define LCDC_ENABLE         0x00u  /* bits[1:0] = 3 to enable */
#define LCDC_SYNC_DIFF      0x04u
#define LCDC_MODE           0x08u
#define LCDC_X_BACKPORCH    0x10u
#define LCDC_X_SYNCWIDTH    0x14u
#define LCDC_X_FRONTPORCH   0x18u
#define LCDC_X_RES          0x1Cu
#define LCDC_Y_BACKPORCH    0x20u
#define LCDC_Y_SYNCWIDTH    0x24u
#define LCDC_Y_FRONTPORCH   0x28u
#define LCDC_Y_RES          0x2Cu
#define LCDC_Y_SHIFT        0x40u
#define LCDC_X_SHIFT        0x44u
#define LCDC_SCALED_X       0x48u
#define LCDC_SCALED_Y       0x4Cu
#define LCDC_INIT_RESUME    0x70u  /* = 1 on Init/Resume */

/* Display-FIFO / DMACPlus unit at 0xBE700000 (psdevwiki "Display" block; reset
 * sequence + values RE'd from Sony's sceDmacplus_driver, lowio.prx @0x7dd8).
 * Must be reset+enabled before the scanout DMA is turned on. */
#define DISPFIFO_BASE       0xBE700000u
#define DISPFIFO(off)       (*(volatile uint32_t *)(DISPFIFO_BASE + (uint32_t)(off)))
#define DISPFIFO_STATUS     0x20u  /* per-write "busy" handshake bits */

/* Sysreg clock/reset gate for the display-FIFO unit: 0xBC100060, device index 0,
 * 3-bit field (4 = assert reset, 6 = run). */
#define SYSREG_CLKGATE60    (*(volatile uint32_t *)0xBC100060u)

static volatile uint32_t *fb = (volatile uint32_t *)PSP_VRAM_UNCACHED;

/* Wait while the given DISPFIFO_STATUS busy bit(s) are set (bounded). */
static void dispfifo_wait(uint32_t mask) {
    unsigned int t = 1000000;
    while(t-- && (DISPFIFO(DISPFIFO_STATUS) & mask))
        ;
}

/* Reset + enable the 0xBE700000 display-FIFO unit.  Exact write order/values and
 * the per-write status-bit handshake are from lowio.prx (see banner). */
static void psp_dmacplus_fifo_init(void) {
    SYSREG_CLKGATE60 = (SYSREG_CLKGATE60 & ~7u) | 4u; /* gate: assert reset */
    dispfifo_wait(0x04); DISPFIFO(0x00) = 0;
    dispfifo_wait(0x04);
    dispfifo_wait(0x02); DISPFIFO(0x04) = 0;
    dispfifo_wait(0x08); DISPFIFO(0x0C) = 0xFFFFFFFFu;
    dispfifo_wait(0x10); DISPFIFO(0x10) = 0;
    dispfifo_wait(0x20); DISPFIFO(0x14) = 0;
    dispfifo_wait(0x3A); DISPFIFO(0x00) = 1;
    dispfifo_wait(0x04);
    SYSREG_CLKGATE60 = (SYSREG_CLKGATE60 & ~7u) | 6u; /* gate: run */
    DISPFIFO(0x24) = 1;                               /* go / enable */
}

/* Cold-boot LCD panel timing bring-up for the 480x272 PSP-1000 panel.  Values
 * and write order are from Sony's sceLcdc_driver (see file banner).  Idempotent
 * when the panel is already running.  Does NOT power the backlight (syscon). */
static void psp_lcdc_panel_init(void) {
    LCDC(LCDC_MODE) = 0x00000300;               /* mode word (descriptor id 0) */
    LCDC(LCDC_SYNC_DIFF) = 0;                    /* sync difference (zoom 1:1)  */
    LCDC(LCDC_X_BACKPORCH) = 41;
    LCDC(LCDC_X_SYNCWIDTH) = 2;
    LCDC(LCDC_X_FRONTPORCH) = 2;
    LCDC(LCDC_X_RES) = PSP_SCREEN_WIDTH;         /* 480 */
    LCDC(LCDC_Y_BACKPORCH) = 2;
    LCDC(LCDC_Y_SYNCWIDTH) = 2;
    LCDC(LCDC_Y_FRONTPORCH) = 10;
    LCDC(LCDC_Y_RES) = PSP_SCREEN_HEIGHT;        /* 272 */
    LCDC(LCDC_Y_SHIFT) = 0;                      /* native, no scaling */
    LCDC(LCDC_X_SHIFT) = 0;
    LCDC(LCDC_SCALED_X) = PSP_SCREEN_WIDTH;
    LCDC(LCDC_SCALED_Y) = PSP_SCREEN_HEIGHT;
    LCDC(LCDC_ENABLE) = LCDC(LCDC_ENABLE) | 3;   /* enable last (bits[1:0]=3) */
    LCDC(LCDC_INIT_RESUME) = 1;
}

/* Point the LCD scanout at our VRAM framebuffer via the DMACPlus registers.
 * The framebuffer-address register takes the low 29 bits of the physical VRAM
 * address (0x04000000); we render through the 0x44000000 uncached alias of the
 * same memory, so scanout sees our pixels immediately. */
static void psp_display_present(void) {
    uint32_t control = *DMACPLUS_FB_SCANOUT;

    /* Same handoff-safe binding sequence as the hardware-proven stage-1 UI:
     * stop scanout without clobbering its interrupt/control state, program a
     * known buffer, then restore the inherited state with scanout enabled. */
    *DMACPLUS_FB_SCANOUT = control & ~1u;
    *DMACPLUS_FB_PIXFMT = DMACPLUS_PIXFMT_8888;
    *DMACPLUS_FB_WIDTH = PSP_SCREEN_WIDTH;
    *DMACPLUS_FB_STRIDE = PSP_FB_STRIDE;
    *DMACPLUS_FB_ADDR = PSP_VRAM_BASE & 0x1FFFFFFFu;
    *DMACPLUS_FB_SCANOUT = control | 1u;
    __asm__ volatile("sync" ::: "memory");
}

static inline uint32_t to_rgba(uint32_t rgb) {
    /* Two conventions meet here.  The shared client code (serial transport,
     * screensaver) speaks the Dreamcast/GameCube RGB565 convention in which
     * 0xffff is white; PSP-only code passes 0x00RRGGBB.  Taking 0xffff as
     * 24-bit gave R=0,G=255,B=255 -- cyan -- for every string the loader drew.
     * Values that fit in 16 bits are expanded from 565, anything larger is
     * read as 24-bit, and 0 is black under both readings.  Channel expansion
     * replicates the high bits rather than dividing, so 0x1F maps to 0xFF. */
    uint32_t r, g, b;

    if(rgb <= 0xFFFFu) {
        uint32_t r5 = (rgb >> 11) & 0x1Fu;
        uint32_t g6 = (rgb >> 5) & 0x3Fu;
        uint32_t b5 = rgb & 0x1Fu;
        r = (r5 << 3) | (r5 >> 2);
        g = (g6 << 2) | (g6 >> 4);
        b = (b5 << 3) | (b5 >> 2);
    } else {
        r = (rgb >> 16) & 0xff;
        g = (rgb >> 8) & 0xff;
        b = rgb & 0xff;
    }
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

void psp_video_init(void) {
    fb = (volatile uint32_t *)PSP_VRAM_UNCACHED;
    psp_dmacplus_fifo_init();    /* reset+enable the display FIFO (before scanout) */
    psp_lcdc_panel_init();       /* cold-boot panel timing (idempotent if up)      */
    psp_display_present();       /* point LCD scanout at our VRAM buffer, enable    */
    psp_syscon_backlight(true);  /* turn the LCD backlight on (cold boot)           */
}

void psp_video_attach_existing(void) {
    /* Adopt the scanout psp_video_init() already set up, without touching
     * FIFO/LCDC/Syscon again.  The exception handler calls this: re-running
     * panel bring-up from a crash context is how you lose the crash screen. */
    fb = (volatile uint32_t *)PSP_VRAM_UNCACHED;
}

void psp_video_clear(uint32_t color) {
    uint32_t c = to_rgba(color);
    for(int y = 0; y < PSP_SCREEN_HEIGHT; y++) {
        volatile uint32_t *line = fb + y * PSP_FB_STRIDE;
        for(int x = 0; x < PSP_SCREEN_WIDTH; x++)
            line[x] = c;
    }
}

void psp_video_fill_rect(int x, int y, int w, int h, uint32_t color) {
    uint32_t c = to_rgba(color);
    for(int row = 0; row < h; row++) {
        int py = y + row;
        if(py < 0 || py >= PSP_SCREEN_HEIGHT)
            continue;
        volatile uint32_t *line = fb + py * PSP_FB_STRIDE;
        for(int col = 0; col < w; col++) {
            int px = x + col;
            if(px >= 0 && px < PSP_SCREEN_WIDTH)
                line[px] = c;
        }
    }
}

static void draw_char(int x, int y, char ch, uint32_t c) {
    if(ch < 32 || ch > 126)
        return;
    const uint16_t *glyph = font_12x24[(int)(ch - 32)];
    for(int row = 0; row < PSP_CHAR_HEIGHT; row++) {
        int py = y + row;
        if(py < 0 || py >= PSP_SCREEN_HEIGHT)
            continue;
        uint16_t bits = glyph[row];
        if(!bits)
            continue;
        volatile uint32_t *line = fb + py * PSP_FB_STRIDE;
        for(int col = 0; col < PSP_CHAR_WIDTH; col++) {
            if(bits & (0x8000 >> col)) {
                int px = x + col;
                if(px >= 0 && px < PSP_SCREEN_WIDTH)
                    line[px] = c;
            }
        }
    }
}

void psp_video_draw_string(int x, int y, const char *str, uint32_t color) {
    uint32_t c = to_rgba(color);
    int cx = x;
    for(const char *p = str; *p; p++) {
        if(*p == '\n') {
            cx = x;
            y += PSP_CHAR_HEIGHT;
            continue;
        }
        draw_char(cx, y, *p, c);
        cx += PSP_CHAR_WIDTH;
    }
}

void psp_video_draw_bitmap(int x, int y, int w, int h, const uint32_t *bits, uint32_t color) {
    uint32_t c = to_rgba(color);
    int words_per_row = (w + 31) / 32;
    for(int row = 0; row < h; row++) {
        int py = y + row;
        if(py < 0 || py >= PSP_SCREEN_HEIGHT)
            continue;
        const uint32_t *row_bits = bits + row * words_per_row;
        volatile uint32_t *line = fb + py * PSP_FB_STRIDE;
        for(int col = 0; col < w; col++) {
            int word = col / 32;
            int bit = 31 - (col % 32);
            if((row_bits[word] >> bit) & 1) {
                int px = x + col;
                if(px >= 0 && px < PSP_SCREEN_WIDTH)
                    line[px] = c;
            }
        }
    }
}

/* ===== Names expected by shared common code + kosload header ===== */

void setup_video(uint32_t mode, uint32_t bg_color) {
    (void)mode;
    (void)bg_color;
    psp_video_init();
}

void clear_screen(uint32_t color) {
    psp_video_clear(color);
}

void draw_string(int x, int y, const char *str, uint32_t color) {
    psp_video_draw_string(x, y, str, color);
}

void clear_lines(int y, int height, unsigned int color) {
    psp_video_fill_rect(0, y, PSP_SCREEN_WIDTH, height, color);
}

/* Zero-padded 8-digit hex, identical to the Dreamcast, GameCube, Wii, PS2 and
 * Xbox implementations.
 *
 * The width is a contract, not a preference: callers pass `[9]` buffers and
 * serial_transport.c's progress line positions its separators by assuming
 * exactly 8 characters.  Neither tolerates a variable-length field. */
void uint_to_string(uint32_t val, unsigned char *buf) {
    static const char hex[] = "0123456789ABCDEF";
    int i;

    for(i = 7; i >= 0; i--) {
        buf[i] = (unsigned char)hex[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
}

const char *exception_code_to_string(uint32_t code) {
    switch(code) {
        case 0:  return "Interrupt";
        case 4:  return "Address Load Error";
        case 5:  return "Address Store Error";
        case 6:  return "Bus Error (instr)";
        case 7:  return "Bus Error (data)";
        case 8:  return "Syscall";
        case 9:  return "Breakpoint";
        case 10: return "Reserved Instruction";
        case 11: return "Coprocessor Unusable";
        case 12: return "Arithmetic Overflow";
        case 13: return "Trap";
        case 15: return "FPU Exception";
        default: return "Exception";
    }
}
