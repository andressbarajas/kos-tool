/* client/gamecube/video.c */
/*
 * GameCube video output.
 *
 * Minimal VI driver for NTSC 480i text display using a YUY2 XFB.
 * Includes an embedded 12x24 bitmap font for text rendering.
 */

#include "video.h"
#include "cache.h"

/* ===== VI Registers ===== */

#define VI_VTR      (*(volatile uint16_t *)(VI_BASE + 0x00))
#define VI_DCR      (*(volatile uint16_t *)(VI_BASE + 0x02))
#define VI_HTR0     (*(volatile uint32_t *)(VI_BASE + 0x04))
#define VI_HTR1     (*(volatile uint32_t *)(VI_BASE + 0x08))
#define VI_VTO      (*(volatile uint32_t *)(VI_BASE + 0x0C))
#define VI_VTE      (*(volatile uint32_t *)(VI_BASE + 0x10))
#define VI_BBEI     (*(volatile uint32_t *)(VI_BASE + 0x14))
#define VI_BBOI     (*(volatile uint32_t *)(VI_BASE + 0x18))
#define VI_TFBL     (*(volatile uint32_t *)(VI_BASE + 0x1C))
#define VI_TFBR     (*(volatile uint32_t *)(VI_BASE + 0x20))
#define VI_BFBL     (*(volatile uint32_t *)(VI_BASE + 0x24))
#define VI_BFBR     (*(volatile uint32_t *)(VI_BASE + 0x28))
#define VI_DPV      (*(volatile uint16_t *)(VI_BASE + 0x2C))
#define VI_DPH      (*(volatile uint16_t *)(VI_BASE + 0x2E))
#define VI_DI0      (*(volatile uint32_t *)(VI_BASE + 0x30))
#define VI_DI1      (*(volatile uint32_t *)(VI_BASE + 0x34))
#define VI_DI2      (*(volatile uint32_t *)(VI_BASE + 0x38))
#define VI_DI3      (*(volatile uint32_t *)(VI_BASE + 0x3C))
#define VI_HSW      (*(volatile uint16_t *)(VI_BASE + 0x48))
#define VI_HSR      (*(volatile uint16_t *)(VI_BASE + 0x4A))
#define VI_FCT0     (*(volatile uint32_t *)(VI_BASE + 0x4C))
#define VI_FCT1     (*(volatile uint32_t *)(VI_BASE + 0x50))
#define VI_FCT2     (*(volatile uint32_t *)(VI_BASE + 0x54))
#define VI_FCT3     (*(volatile uint32_t *)(VI_BASE + 0x58))
#define VI_FCT4     (*(volatile uint32_t *)(VI_BASE + 0x5C))
#define VI_FCT5     (*(volatile uint32_t *)(VI_BASE + 0x60))
#define VI_FCT6     (*(volatile uint32_t *)(VI_BASE + 0x64))
#define VI_VICLK    (*(volatile uint16_t *)(VI_BASE + 0x6C))
#define VI_VISEL    (*(volatile uint16_t *)(VI_BASE + 0x6E))
#define VI_SRCWID   (*(volatile uint16_t *)(VI_BASE + 0x70))

/* ===== Framebuffer ===== */

/* XFB in MEM1: 640*480*2 = 614400 bytes. Placed at a fixed high address. */
#define XFB_ADDR    0xC0050000
static uint8_t *xfb = (uint8_t *)XFB_ADDR;

/* ===== Embedded 12x24 font ===== */
/* Shared with the PS2 loader so both ports render identical glyphs. */
#include "../common/font_12x24.h"

/* ===== RGB to YUV conversion ===== */

/* Normalize color input: accepts both RGB565 (0x0000-0xFFFF) and
 * RGB888 (0x00RRGGBB with any byte > 0 in bits 23-16).
 * This lets common code pass DC-style RGB565 values while GC-specific
 * code can use full 0x00RRGGBB colors. */
static uint32_t normalize_color(uint32_t color)
{
    if (color <= 0xFFFF) {
        /* Treat as RGB565: RRRRRGGGGGGBBBBB */
        unsigned int r5 = (color >> 11) & 0x1F;
        unsigned int g6 = (color >>  5) & 0x3F;
        unsigned int b5 =  color        & 0x1F;
        unsigned int r8 = (r5 << 3) | (r5 >> 2);
        unsigned int g8 = (g6 << 2) | (g6 >> 4);
        unsigned int b8 = (b5 << 3) | (b5 >> 2);
        return (r8 << 16) | (g8 << 8) | b8;
    }
    return color;  /* Already 0x00RRGGBB */
}

/* Convert 0x00RRGGBB to a YCbYCr pixel pair (Y0 Cb Y1 Cr).
 * The GC VI XFB uses Y0CbY1Cr byte order on big-endian PPC.
 * Uses ITU-R BT.601 coefficients with studio-range output (Y: 16-235). */
uint32_t gc_color_to_yuy2(uint32_t rgb)
{
    rgb = normalize_color(rgb);
    int r = (rgb >> 16) & 0xFF;
    int g = (rgb >> 8) & 0xFF;
    int b = rgb & 0xFF;

    /* BT.601 studio range: Y = 16..235, Cb/Cr = 16..240 */
    int y  = ((  66 * r + 129 * g +  25 * b + 128) >> 8) + 16;
    int cb = (( -38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
    int cr = (( 112 * r -  94 * g -  18 * b + 128) >> 8) + 128;

    /* Clamp to valid range */
    if (y < 16) y = 16; else if (y > 235) y = 235;
    if (cb < 16) cb = 16; else if (cb > 240) cb = 240;
    if (cr < 16) cr = 16; else if (cr > 240) cr = 240;

    /* Y0CbY1Cr byte order (big-endian PPC) */
    return ((uint32_t)y << 24) | ((uint32_t)cb << 16) |
           ((uint32_t)y << 8) | (uint32_t)cr;
}

/* Cached background YUV values (set by gc_video_clear) */
static uint8_t bg_yuv_y  = 16;
static uint8_t bg_yuv_cb = 128;
static uint8_t bg_yuv_cr = 128;

/* ===== Private helpers ===== */

/* Write a YCbYCr pixel pair to the XFB.
 * XFB format: Y0 Cb Y1 Cr (4 bytes per 2 pixels).
 * Uses a single 32-bit write — byte writes to cache-inhibited
 * memory on Gekko can silently fail. */
static void xfb_set_pixel_pair(int x, int y,
                               uint8_t y0, uint8_t y1,
                               uint8_t cb, uint8_t cr)
{
    uint32_t *fb32 = (uint32_t *)(xfb + (y * GC_SCREEN_WIDTH + x) * 2);
    *fb32 = ((uint32_t)y0 << 24) | ((uint32_t)cb << 16) |
            ((uint32_t)y1 << 8) | (uint32_t)cr;
}

/* ===== Public API ===== */

void gc_video_init(void)
{
    /* VICLK = 0 means 27 MHz; VICLK = 1 means 54 MHz and is used for progressive mode. */
    VI_VICLK = 0x0000;   /* 27 MHz video clock. */

    /* Wait for the raster beam to enter the vertical blanking region, but ONLY if
     * VI is currently enabled (DCR bit 0). */
    if (VI_DCR & 0x0001)
        while (VI_DPV > 20);

    /* Configure VI for NTSC 480i */
    VI_VTR  = 0x0F06;         /* ACV=240, EQU=6 */
    VI_HTR0 = 0x476901AD;     /* HCS=0x47, HCE=0x69, HLW=0x01AD */
    VI_HTR1 = 0x02EA5140;     /* HBS=0x02EA, HBE+HSY */
    VI_VTO  = 0x00030018;     /* PSB_odd=3, PRB_odd=24 */
    VI_VTE  = 0x00020019;     /* PSB_even=2, PRB_even=25 */
    VI_BBEI = 0x410C410C;
    VI_BBOI = 0x40ED40ED;

    /* Set framebuffer base (physical address).
     * TFBL/BFBL bit 28 = POFF (page offset flag):
     *   POFF=0: FBB = raw physical address (for addr < 16MB)
     *   POFF=1: FBB = physical address >> 5 (for addr >= 16MB)
     * Our XFB at 0x00050000 is well under 16MB, so use POFF=0. */
    uint32_t xfb_phys = (uint32_t)xfb & 0x1FFFFFFF;
    VI_TFBL = xfb_phys;
    VI_BFBL = xfb_phys + GC_SCREEN_WIDTH * 2;

    /* Use standard NTSC 640 scaling. */
    VI_HSW = 0x2850;
    VI_HSR = 0x0100;

    /* Anti-aliasing filter coefficients */
    VI_FCT0 = 0x1AE771F0;
    VI_FCT1 = 0x0DB4A574;
    VI_FCT2 = 0x00C1188E;
    VI_FCT3 = 0xC4C0CBE2;
    VI_FCT4 = 0xFCECDECF;
    VI_FCT5 = 0x13130F08;
    VI_FCT6 = 0x00080C0F;

    VI_VISEL = 0x0000;
    VI_SRCWID = 0x0280;       /* Source width: 640 pixels */

    /* Select NTSC interlaced output and enable the VI. */
    VI_DCR = 0x0001;

    /* Clear the framebuffer to black */
    gc_video_clear(0);
}

void gc_video_set_xfb(void *new_xfb)
{
    xfb = (uint8_t *)new_xfb;
    uint32_t phys = (uint32_t)xfb & 0x1FFFFFFF;
    VI_TFBL = phys;
    VI_BFBL = phys + GC_SCREEN_WIDTH * 2;
}

void gc_video_clear(uint32_t color)
{
    uint32_t yuy2 = gc_color_to_yuy2(color);
    uint32_t *fb32 = (uint32_t *)xfb;
    int count = (GC_SCREEN_WIDTH * GC_SCREEN_HEIGHT * 2) / 4;
    int i;

    /* Cache the background YUV components for draw_char (Y0CbY1Cr packing) */
    bg_yuv_y  = (yuy2 >> 24) & 0xFF;
    bg_yuv_cb = (yuy2 >> 16) & 0xFF;
    bg_yuv_cr = yuy2 & 0xFF;

    for (i = 0; i < count; i++)
        fb32[i] = yuy2;

    cache_flush_dc(xfb, GC_SCREEN_WIDTH * GC_SCREEN_HEIGHT * 2);
}

void gc_video_draw_char(int x, int y, char c, uint32_t color)
{
    const uint16_t *glyph;
    uint32_t fg_yuy2 = gc_color_to_yuy2(color);
    uint8_t fg_y  = (fg_yuy2 >> 24) & 0xFF;
    uint8_t fg_cb = (fg_yuy2 >> 16) & 0xFF;
    uint8_t fg_cr = fg_yuy2 & 0xFF;
    int row, col;

    if (c < 32 || c > 126)
        c = '?';

    glyph = font_12x24[(int)(c - 32)];

    for (row = 0; row < 24; row++) {
        uint16_t bits = glyph[row];
        /* Process 2 pixels at a time (YUY2 constraint) */
        for (col = 0; col < 12; col += 2) {
            int p0 = (bits & (0x8000 >> col)) != 0;
            int p1 = (bits & (0x8000 >> (col + 1))) != 0;
            uint8_t y0 = p0 ? fg_y : bg_yuv_y;
            uint8_t y1 = p1 ? fg_y : bg_yuv_y;
            /* Use fg chroma if either pixel is foreground, else bg */
            uint8_t cb = (p0 || p1) ? fg_cb : bg_yuv_cb;
            uint8_t cr = (p0 || p1) ? fg_cr : bg_yuv_cr;
            xfb_set_pixel_pair(x + col, y + row, y0, y1, cb, cr);
        }
    }
}

void gc_video_draw_string(int x, int y, const char *str, uint32_t color)
{
    while (*str) {
        gc_video_draw_char(x, y, *str, color);
        x += GC_CHAR_WIDTH;
        str++;
    }
    /* Flush the modified framebuffer region */
    cache_flush_dc(xfb + y * GC_SCREEN_WIDTH * 2,
                   GC_CHAR_HEIGHT * GC_SCREEN_WIDTH * 2);
}

/* ===== Functions expected by crt0.S header and common code ===== */

void setup_video(uint32_t mode, uint32_t bg_color)
{
    (void)mode;
    (void)bg_color;
    /* Video is initialized once in gc_video_init() via target->init() */
}

void clear_screen(uint32_t color)
{
    gc_video_clear(color);
}

void draw_string(int x, int y, const char *str, uint32_t color)
{
    gc_video_draw_string(x, y, str, color);
}

void uint_to_string(unsigned int val, unsigned char *buf)
{
    int i;
    static const char hex[] = "0123456789ABCDEF";

    for (i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
}

/* Exception code to string — stub, formatting moved to host (kos-tool).
 * Must exist for crt0.S linkage (fixed offset in entry header). */
const char *exception_code_to_string(uint32_t code)
{
    (void)code;
    return "";
}

void clear_lines(int y, int height, uint32_t color)
{
    uint32_t yuy2 = gc_color_to_yuy2(color);
    uint32_t *fb32 = (uint32_t *)(xfb + y * GC_SCREEN_WIDTH * 2);
    int count = (height * GC_SCREEN_WIDTH * 2) / 4;
    int i;
    for (i = 0; i < count; i++)
        fb32[i] = yuy2;
    cache_flush_dc(xfb + y * GC_SCREEN_WIDTH * 2, height * GC_SCREEN_WIDTH * 2);
}
