/* client/playstation2/video.c */
/*
 * PlayStation 2 GS video output.
 *
 * Minimal GS driver for NTSC 640x480 text display using GIF DMA.
 * Uses the GS SPRITE primitive for filled rectangles and individual
 * POINT primitives for font glyph rendering.
 *
 * The framebuffer lives in GS local memory (VRAM), not main RAM.
 * All drawing is done by sending GIF packets via DMA channel 2.
 */

#include <stdint.h>

#include "video.h"
#include "cache.h"
#include "ee_cop0.h"

/* ===== GS Privileged Registers (KSEG1 mapped, 64-bit) ===== */

#define GS_PRIV_BASE    0xB2000000

#define GS_PMODE        (*(volatile uint64_t *)(GS_PRIV_BASE + 0x00))
#define GS_SMODE2       (*(volatile uint64_t *)(GS_PRIV_BASE + 0x20))
#define GS_DISPFB1      (*(volatile uint64_t *)(GS_PRIV_BASE + 0x70))
#define GS_DISPLAY1     (*(volatile uint64_t *)(GS_PRIV_BASE + 0x80))
#define GS_BGCOLOR      (*(volatile uint64_t *)(GS_PRIV_BASE + 0xE0))
#define GS_CSR          (*(volatile uint64_t *)(GS_PRIV_BASE + 0x1000))

/* ===== GIF DMA Channel (DMA channel 2) ===== */

#define GIF_DMA_BASE    0xB000A000

#define GIF_CHCR        (*(volatile uint32_t *)(GIF_DMA_BASE + 0x00))
#define GIF_MADR        (*(volatile uint32_t *)(GIF_DMA_BASE + 0x10))
#define GIF_QWC         (*(volatile uint32_t *)(GIF_DMA_BASE + 0x20))

/* DMA control registers */
#define DMA_CTRL        (*(volatile uint32_t *)0xB000E000)
#define DMA_STAT        (*(volatile uint32_t *)0xB000E010)
#define DMA_PCR         (*(volatile uint32_t *)0xB000E020)
#define DMA_SQWC        (*(volatile uint32_t *)0xB000E030)

#define DMA_CTRL_DMAE       0x00000001u
#define DMA_STAT_GIF        0x00000004u
#define DMA_PCR_CDE_GIF     0x00040000u
#define DMA_CHCR_STR        0x00000100u

/* DMAC global hold (CPND) — used to force-abort a wedged channel.
 * Same registers/bit ee_sif.c's dmac_force_stop() uses for SIF. */
#define DMAC_ENABLER    (*(volatile uint32_t *)0xB000F520)
#define DMAC_ENABLEW    (*(volatile uint32_t *)0xB000F590)
#define DMAC_CPND       0x00010000u

/* GIF unit control register (NOT the GIF DMA channel).
 * Writing bit 0 (RST) resets the GIF path controller. */
#define GIF_UNIT_CTRL   (*(volatile uint32_t *)0xB0003000)
#define GIF_UNIT_STAT   (*(volatile uint32_t *)0xB0003020)

/* ===== GIF Tag and GS Register Macros ===== */

/* GIF tag: 128 bits total. Low 64 bits contain control fields. */
#define GIF_TAG(nloop, eop, pre, prim, flg, nreg) \
    ((uint64_t)(nloop) | ((uint64_t)(eop) << 15) | \
     ((uint64_t)(pre) << 46) | ((uint64_t)(prim) << 47) | \
     ((uint64_t)(flg) << 58) | ((uint64_t)(nreg) << 60))

/* GIF register descriptor for A+D mode */
#define GIF_AD  0x0EUL

/* GS general-purpose register addresses */
#define GS_REG_PRIM        0x00
#define GS_REG_RGBAQ       0x01
#define GS_REG_XYZ2        0x05
#define GS_REG_FRAME_1     0x4C
#define GS_REG_SCISSOR_1   0x40
#define GS_REG_XYOFFSET_1  0x18
#define GS_REG_PRMODECONT  0x1A

/* GS primitive types */
#define GS_PRIM_POINT   0
#define GS_PRIM_SPRITE  6

/* Pack a PRIM register value */
#define GS_SET_PRIM(prim, iip, tme, fge, abe, aa1, fst, ctxt, fix) \
    ((uint64_t)(prim) | ((uint64_t)(iip) << 3) | ((uint64_t)(tme) << 4) | \
     ((uint64_t)(fge) << 5) | ((uint64_t)(abe) << 6) | ((uint64_t)(aa1) << 7) | \
     ((uint64_t)(fst) << 8) | ((uint64_t)(ctxt) << 9) | ((uint64_t)(fix) << 10))

/* Pack an RGBAQ register value */
#define GS_SET_RGBAQ(r, g, b, a, q) \
    ((uint64_t)(r) | ((uint64_t)(g) << 8) | ((uint64_t)(b) << 16) | \
     ((uint64_t)(a) << 24) | ((uint64_t)(q) << 32))

/* Pack an XYZ2 register value.
 * GS uses 12.4 fixed-point for X/Y coordinates:
 *   pixel X -> (X << 4), pixel Y -> (Y << 4) */
#define GS_SET_XYZ(x, y, z) \
    ((uint64_t)(x) | ((uint64_t)(y) << 16) | ((uint64_t)(z) << 32))

/* Pack a FRAME_1 register value.
 * FBP = framebuffer base pointer (in 2048-pixel units / 32 = 64-byte pages)
 * FBW = framebuffer width (in 64-pixel units)
 * PSM = pixel storage mode (0 = PSMCT32) */
#define GS_SET_FRAME(fbp, fbw, psm, fbmsk) \
    ((uint64_t)(fbp) | ((uint64_t)(fbw) << 16) | \
     ((uint64_t)(psm) << 24) | ((uint64_t)(fbmsk) << 32))

/* Pack a SCISSOR_1 register value */
#define GS_SET_SCISSOR(scax0, scax1, scay0, scay1) \
    ((uint64_t)(scax0) | ((uint64_t)(scax1) << 16) | \
     ((uint64_t)(scay0) << 32) | ((uint64_t)(scay1) << 48))

/* Pack a XYOFFSET_1 register value (16.4 fixed point) */
#define GS_SET_XYOFFSET(ofx, ofy) \
    ((uint64_t)(ofx) | ((uint64_t)(ofy) << 32))

/* Physical address mask (strip KSEG bits) */
#define PHYSADDR(x) ((uint32_t)(x) & 0x1FFFFFFF)
#define UNCACHED_ADDR(x) ((void *)(PHYSADDR(x) | 0xA0000000))

/* ===== Shared 12x24 font ===== */
#include "../common/font_12x24.h"

/* ===== GIF DMA transfer ===== */

/* Bounded budget for the ch2 STR-clear wait.  A healthy GIF DMA of a few
 * quadwords completes in microseconds; the old debug code already flagged
 * a spin count of 0x100000 (~1M) as pathological.  4M gives any real
 * transfer an enormous margin while still capping a wedged channel to a
 * fraction of a second instead of an infinite spin.  Mirrors ee_sif.c,
 * where every hardware wait is POLL_TIMEOUT-bounded. */
#define GIF_DMA_WAIT_BUDGET  0x00400000

/* Defined later in this file; used by gif_dma_recover() below. */
static void gif_dma_stop_held(void);
static void gif_unit_reset(void);

/* Repair an inherited / wedged DMAC ch2 + GIF unit.  This is exactly the
 * sequence ps2_video_init() already trusts for -F-inherited state; kept
 * in one place so gif_dma_send()'s retry and video init cannot diverge. */
static void gif_dma_recover(void)
{
    gif_dma_stop_held();   /* DMAC hold, ch2 CHCR/QWC=0, release */
    gif_unit_reset();      /* GIF_CTRL.RST: clear PATH3/PSE inside GIF */
    DMA_CTRL = DMA_CTRL_DMAE;
    DMA_SQWC = 0;
    DMA_PCR  = DMA_PCR | DMA_PCR_CDE_GIF;
    DMA_STAT = DMA_STAT_GIF;   /* clear any latched ch2 status */
}

/*
 * Send a GIF packet via DMA channel 2.
 * data must be 16-byte aligned and in cached memory (we flush before DMA).
 * qwc = number of quadwords (128-bit units) to transfer.
 *
 * A firmware-update (-F) handoff can hand us DMAC ch2 / the GIF unit
 * mid-transfer (there is no Sony-style exiting-side teardown), so the
 * STR bit may never clear.  Bound the wait; on a wedge run the
 * inherited-state repair and retry once; if it still wedges, return
 * rather than freeze the machine.
 */
static void gif_dma_send(void *data, unsigned int qwc)
{
    unsigned int attempt;

    /* Flush cached packet writes so DMA sees them. Packets written through
     * KSEG1 are already uncached and must not be written back from stale
     * cached lines. */
    if (((uint32_t)data & 0xE0000000) != 0xA0000000)
        cache_flush_dc(data, qwc * 16);

    for (attempt = 0; attempt < 2; attempt++) {
        uint32_t spin = 0;

        /* (Re)enable DMA + arm ch2.  Repeated on the retry path because
         * gif_dma_recover() tears the channel down. */
        if ((DMAC_ENABLER & DMAC_CPND) != 0) {
            DMAC_ENABLEW = DMAC_ENABLER & ~DMAC_CPND;
            (void)DMAC_ENABLER;
        }
        DMA_CTRL = DMA_CTRL_DMAE;
        DMA_PCR  = DMA_PCR | DMA_PCR_CDE_GIF;

        /* Source address (physical) + quadword count, then start:
         * normal mode, DIR=0 (from memory), STR=1. */
        GIF_MADR = PHYSADDR(data);
        GIF_QWC  = qwc;
        GIF_CHCR = DMA_CHCR_STR;

        while (GIF_CHCR & DMA_CHCR_STR) {
            if (++spin >= GIF_DMA_WAIT_BUDGET)
                break;
        }
        if ((GIF_CHCR & DMA_CHCR_STR) == 0)
            return;                       /* transfer completed */

        /* Wedged — repair the channel and retry once. */
        gif_dma_recover();
    }
    /* Both attempts wedged: GS/DMAC is unrecoverable from here.  Don't
     * hang — let bring-up continue. */
}

/* ===== GIF packet buffer ===== */

/* Maximum GIF packet: GIFtag (1 QW) + PRIM (1 QW) + RGBAQ (1 QW) +
 * up to ~120 XYZ2 entries (1 QW each) for densest 12x24 glyphs.
 * 124 QWs covers any character with up to 121 foreground pixels. */
#define PKT_MAX_QWORDS  124
static uint64_t pkt_buf[PKT_MAX_QWORDS * 2] __attribute__((aligned(16)));

/* ===== Drawing primitives ===== */

/*
 * Draw a filled rectangle using the GS SPRITE primitive.
 * Color is 0x00RRGGBB format.
 */
static void gs_fill_rect(int x, int y, int w, int h,
                          uint8_t r, uint8_t g, uint8_t b)
{
    uint64_t *p = (uint64_t *)pkt_buf;

    /* GIFtag: NLOOP=4, EOP=1, PACKED mode, NREG=1, REG=A+D */
    p[0] = GIF_TAG(4, 1, 0, 0, 0, 1);
    p[1] = GIF_AD;

    /* PRIM = SPRITE, no texture, no alpha blend */
    p[2] = GS_SET_PRIM(GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0);
    p[3] = GS_REG_PRIM;

    /* RGBAQ = color */
    p[4] = GS_SET_RGBAQ(r, g, b, 0x80, 0);
    p[5] = GS_REG_RGBAQ;

    /* XYZ2 = top-left corner (12.4 fixed point) */
    p[6] = GS_SET_XYZ(x << 4, y << 4, 0);
    p[7] = GS_REG_XYZ2;

    /* XYZ2 = bottom-right corner */
    p[8] = GS_SET_XYZ((x + w) << 4, (y + h) << 4, 0);
    p[9] = GS_REG_XYZ2;

    gif_dma_send(p, 5);  /* 5 quadwords = 10 uint64 */
}

/* ===== Color normalization ===== */

/*
 * Normalize color input: accepts both RGB565 (0x0000-0xFFFF) and
 * RGB888 (0x00RRGGBB with any byte > 0 in bits 23-16).
 * This lets common code pass DC-style RGB565 values while PS2-specific
 * code can use full 0x00RRGGBB colors.
 */
static uint32_t normalize_color(uint32_t color)
{
    if (color <= 0xFFFF) {
        /* Treat as RGB565: RRRRRGGGGGGBBBBB */
        unsigned int r5 = (color >> 11) & 0x1F;
        unsigned int g6 = (color >> 5) & 0x3F;
        unsigned int b5 = color & 0x1F;
        return ((r5 << 19) | (r5 << 14)) |   /* R: 5-bit to 8-bit */
               ((g6 << 10) | (g6 << 4)) |     /* G: 6-bit to 8-bit */
               ((b5 << 3) | (b5 >> 2));        /* B: 5-bit to 8-bit */
    }
    return color;  /* Already 0x00RRGGBB */
}

/* Extract RGB components from normalized color */
static void color_to_rgb(uint32_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    color = normalize_color(color);
    *r = (color >> 16) & 0xFF;
    *g = (color >> 8) & 0xFF;
    *b = color & 0xFF;
}

/* ===== Public API ===== */

static void gif_dma_stop_held(void)
{
    uint32_t status = ee_cop0_read_status();
    uint32_t saved;

    ee_cop0_write_status(status & ~EE_COP0_STATUS_IE);
    saved = DMAC_ENABLER;
    DMAC_ENABLEW = saved | DMAC_CPND;
    (void)DMAC_ENABLER;
    (void)GIF_CHCR;
    (void)DMA_CTRL;
    GIF_CHCR = 0;
    GIF_QWC = 0;
    (void)GIF_CHCR;            /* MMIO read-back drains the EE write buffer */
    /* This is early hardware ownership, not a temporary pause/resume.  If a
     * previous context left the whole DMAC held, release it before video DMA. */
    DMAC_ENABLEW = saved & ~DMAC_CPND;
    (void)DMAC_ENABLER;
    ee_cop0_write_status(status);
}


static void gif_unit_reset(void)
{
    GIF_UNIT_CTRL = 1;         /* RST */
    (void)GIF_UNIT_STAT;
    GIF_UNIT_CTRL = 0;         /* PSE=0: ensure transfer processing is live */
    (void)GIF_UNIT_STAT;
}

void ps2_video_init(void)
{
    uint64_t *p;

    /* Initialize DMAC for bare-metal operation.
     * A firmware update enters here with EE hardware inherited from the
     * previous loader.  Repair the global state that controls whether ch2 can
     * execute at all, not just the ch2 STR bit:
     *   - DMA_CTRL clears any stale stall/MFIFO mode while enabling DMA.
     *   - DMA_PCR.CDE2 lets GIF run if priority control was left enabled.
     *   - GIF_CTRL.RST clears PATH3/PSE state inside the GIF unit itself.
     */
    gif_dma_stop_held();
    gif_unit_reset();
    DMA_CTRL = DMA_CTRL_DMAE;
    DMA_SQWC = 0;
    DMA_PCR = DMA_PCR | DMA_PCR_CDE_GIF;
    DMA_STAT = DMA_STAT_GIF;

    /* Fully configure display for NTSC 640x480 interlaced.
     * The BIOS/uLaunchELF may have been using circuit 2 or a different
     * resolution, so we must set everything explicitly. */

    /* SMODE2: INT=1 (interlaced), FFMD=0 (field mode).
     * We scan out a 480-line display below; using frame mode here
     * makes the output look vertically stretched on hardware. */
    GS_SMODE2 = (uint64_t)1;

    /* DISPLAY1: NTSC 640x480 interlaced field mode.
     * DX=652   - horizontal start position (VCK units, standard NTSC)
     * DY=50    - doubled NTSC vertical start position for frame mode
     * MAGH=3   - horizontal magnification 4x (640*4=2560 VCK)
     * MAGV=0   - vertical magnification 1x
     * DW=2559  - display width in VCK (640*4-1)
     * DH=479   - display height (480-1) */
    GS_DISPLAY1 = (uint64_t)652 |
                  ((uint64_t)50 << 12) |
                  ((uint64_t)3 << 23) |
                  ((uint64_t)0 << 27) |
                  ((uint64_t)2559 << 32) |
                  ((uint64_t)479 << 44);

    /* DISPFB1: framebuffer at VRAM base 0, 640 wide, PSMCT32.
     * FBP=0, FBW=10 (640/64), PSM=0 (PSMCT32) */
    GS_DISPFB1 = (uint64_t)0 |           /* FBP */
                 ((uint64_t)10 << 9) |    /* FBW */
                 ((uint64_t)0 << 15);     /* PSM = PSMCT32 */

    /* PMODE: enable read circuit 1 only */
    GS_PMODE = (uint64_t)1 |          /* EN1 = 1 */
               ((uint64_t)1 << 2) |   /* CRTMD = 1 */
               ((uint64_t)0xFF << 8); /* ALP = 0xFF */

    /* BGCOLOR: black (visible in overscan/margin area) */
    GS_BGCOLOR = 0;

    /* Send GIF packets to configure GS drawing environment */
    p = (uint64_t *)pkt_buf;

    /* GIFtag: NLOOP=4, EOP=1, PACKED, NREG=1, REG=A+D */
    p[0] = GIF_TAG(4, 1, 0, 0, 0, 1);
    p[1] = GIF_AD;

    /* FRAME_1: FBP=0, FBW=10, PSM=0 (PSMCT32), FBMSK=0 */
    p[2] = GS_SET_FRAME(0, 10, 0, 0);
    p[3] = GS_REG_FRAME_1;

    /* SCISSOR_1: full screen */
    p[4] = GS_SET_SCISSOR(0, PS2_SCREEN_WIDTH - 1, 0, PS2_SCREEN_HEIGHT - 1);
    p[5] = GS_REG_SCISSOR_1;

    /* XYOFFSET_1: offset = 0 */
    p[6] = GS_SET_XYOFFSET(0, 0);
    p[7] = GS_REG_XYOFFSET_1;

    /* PRMODECONT: AC=1 (use PRIM register, not PRMODE) */
    p[8] = (uint64_t)1;
    p[9] = GS_REG_PRMODECONT;

    gif_dma_send(p, 5);

    /* Clear screen to the loader's standard background colour. */
    ps2_video_clear(0x001C81B3);
}

void ps2_video_clear(uint32_t color)
{
    uint8_t r, g, b;
    color_to_rgb(color, &r, &g, &b);
    gs_fill_rect(0, 0, PS2_SCREEN_WIDTH, PS2_SCREEN_HEIGHT, r, g, b);
}

/*
 * Draw a single character at pixel position (x, y).
 * Uses batched POINT primitives sent via GIF DMA.
 */
static void ps2_video_draw_char(int x, int y, char c,
                                 uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t *glyph;
    int row, col;
    int pixel_count;
    uint64_t *p;

    if (c < 32 || c > 126)
        c = '?';

    glyph = font_12x24[(int)(c - 32)];

    /* First pass: count foreground pixels */
    pixel_count = 0;
    for (row = 0; row < 24; row++) {
        uint16_t bits = glyph[row];
        for (col = 0; col < 12; col++) {
            if (bits & (0x8000 >> col))
                pixel_count++;
        }
    }

    if (pixel_count == 0)
        return;

    /* Limit to what fits in our packet buffer.
     * Each pixel needs 1 A+D pair (2 uint64 = 1 quadword) for XYZ2.
     * Plus GIFtag (1 QW) + PRIM (1 QW) + RGBAQ (1 QW) = 3 QW overhead.
     * Max payload = PKT_MAX_QWORDS - 3 = 63 pixels per batch.
     * A 12x24 glyph has at most 288 pixels, typically ~100 foreground. */
    p = (uint64_t *)pkt_buf;
    int batch_count = 0;
    int total_nloop;

    /* We send PRIM + RGBAQ once, then all XYZ2 entries.
     * Total NLOOP = 2 (PRIM + RGBAQ) + pixel_count (XYZ2 entries) */
    if (pixel_count > (PKT_MAX_QWORDS - 1 - 2)) {
        /* Shouldn't happen for 12x24 font, but clamp just in case */
        pixel_count = PKT_MAX_QWORDS - 1 - 2;
    }
    total_nloop = 2 + pixel_count;

    /* GIFtag: NLOOP=total, EOP=1, PACKED, NREG=1, REG=A+D */
    p[0] = GIF_TAG(total_nloop, 1, 0, 0, 0, 1);
    p[1] = GIF_AD;
    p += 2;

    /* PRIM = POINT */
    p[0] = GS_SET_PRIM(GS_PRIM_POINT, 0, 0, 0, 0, 0, 0, 0, 0);
    p[1] = GS_REG_PRIM;
    p += 2;

    /* RGBAQ = foreground color */
    p[0] = GS_SET_RGBAQ(r, g, b, 0x80, 0);
    p[1] = GS_REG_RGBAQ;
    p += 2;

    /* Emit XYZ2 for each foreground pixel */
    for (row = 0; row < 24; row++) {
        uint16_t bits = glyph[row];
        if (bits == 0) continue;
        for (col = 0; col < 12; col++) {
            if (bits & (0x8000 >> col)) {
                p[0] = GS_SET_XYZ((x + col) << 4, (y + row) << 4, 0);
                p[1] = GS_REG_XYZ2;
                p += 2;
                batch_count++;
                if (batch_count >= pixel_count)
                    goto done;
            }
        }
    }
done:
    /* Total quadwords = 1 (GIFtag) + total_nloop (data) */
    gif_dma_send((void *)pkt_buf, 1 + total_nloop);
}

void ps2_video_draw_string(int x, int y, const char *str, uint32_t color)
{
    uint8_t r, g, b;
    color_to_rgb(color, &r, &g, &b);

    while (*str) {
        ps2_video_draw_char(x, y, *str, r, g, b);
        x += PS2_CHAR_WIDTH;
        str++;
    }
}

void ps2_video_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    uint8_t r, g, b;
    color_to_rgb(color, &r, &g, &b);
    gs_fill_rect(x, y, w, h, r, g, b);
}

void ps2_video_draw_bitmap(int x, int y, int w, int h,
                            const uint32_t *bits, uint32_t color)
{
    uint8_t r, g, b;
    color_to_rgb(color, &r, &g, &b);

    int words_per_row = (w + 31) / 32;
    int row, col;

    for (row = 0; row < h; row++) {
        const uint32_t *row_bits = bits + row * words_per_row;

        /* Count foreground pixels in this row */
        int row_pixels = 0;
        for (col = 0; col < w; col++) {
            int word = col / 32;
            int bit = 31 - (col % 32);
            if ((row_bits[word] >> bit) & 1)
                row_pixels++;
        }

        if (row_pixels == 0)
            continue;

        /* Clamp to packet buffer capacity */
        if (row_pixels > (PKT_MAX_QWORDS - 1 - 2))
            row_pixels = PKT_MAX_QWORDS - 1 - 2;

        int total_nloop = 2 + row_pixels;
        uint64_t *p = (uint64_t *)pkt_buf;

        p[0] = GIF_TAG(total_nloop, 1, 0, 0, 0, 1);
        p[1] = GIF_AD;
        p += 2;

        p[0] = GS_SET_PRIM(GS_PRIM_POINT, 0, 0, 0, 0, 0, 0, 0, 0);
        p[1] = GS_REG_PRIM;
        p += 2;

        p[0] = GS_SET_RGBAQ(r, g, b, 0x80, 0);
        p[1] = GS_REG_RGBAQ;
        p += 2;

        int emitted = 0;
        for (col = 0; col < w && emitted < row_pixels; col++) {
            int word = col / 32;
            int bit = 31 - (col % 32);
            if ((row_bits[word] >> bit) & 1) {
                p[0] = GS_SET_XYZ((x + col) << 4, (y + row) << 4, 0);
                p[1] = GS_REG_XYZ2;
                p += 2;
                emitted++;
            }
        }

        gif_dma_send((void *)pkt_buf, 1 + total_nloop);
    }
}

/* ===== Functions expected by crt0.S header and common code ===== */

void setup_video(uint32_t mode, uint32_t bg_color)
{
    (void)mode;
    (void)bg_color;
    /* Video is initialized once in ps2_video_init() via target->init() */
}

void clear_screen(uint32_t color)
{
    ps2_video_clear(color);
}

void draw_string(int x, int y, const char *str, uint32_t color)
{
    ps2_video_draw_string(x, y, str, color);
}

void clear_lines(int y, int height, unsigned int color)
{
    uint8_t r, g, b;
    color_to_rgb(color, &r, &g, &b);
    gs_fill_rect(0, y, PS2_SCREEN_WIDTH, height, r, g, b);
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

/* Exception code to string -- stub, formatting moved to host (kos-tool).
 * Must exist for crt0.S linkage (fixed offset in entry header). */
const char *exception_code_to_string(uint32_t code)
{
    (void)code;
    return "";
}
