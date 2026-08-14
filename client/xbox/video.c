/* client/xbox/video.c — Xbox dumb-framebuffer text console.
 *
 * Bare-metal, with no kernel Av or Mm thunk.  We REUSE the display mode the
 * kernel already programmed for this console's cable + encoder (which we cannot
 * synthesise: the encoder — Conexant / Focus / Xcalibur — varies by board
 * revision, and its pixel clock is per-mode encoder data, not an NV2A PLL).  So
 * xbox_video_init() just halts the NV2A pushbuffer, reuses the live scanout
 * buffer, and un-blanks the sequencer; xbox_video_kick() keeps it pinned.  This
 * is encoder-agnostic (we never touch the encoder) and therefore revision-
 * agnostic.  The mode programming was recovered clean-room.
 *
 * CPU pixel writes go through the write-combined RAM alias (0xF0000000 | phys),
 * with a WBINVD first to evict stale write-back lines for the reused page.  The
 * scanout stride and height are read back from the mode (CRTC pitch / FP timing)
 * so any resolution/region works — HDMI mods run 720x480, not 640x480.  32bpp
 * XRGB8888.
 */

#include <stdint.h>
#include "video.h"
#include "../common/font_12x24.h"

/* ===== NV2A MMIO (NV20-family; base 0xFD000000). ===== */
#define NV2A_BASE            0xFD000000u
#define NV2A_R32(off)        (*(volatile uint32_t *)(NV2A_BASE + (off)))
#define NV2A_R8(off)         (*(volatile uint8_t  *)(NV2A_BASE + (off)))

#define NV_PCRTC_START       0x600800u   /* scanout base (physical) */

/* NV2A PFIFO — the GPU command pushbuffer engine.  Halting it is part of the
 * kernel takeover: crt0's cli stops the kernel's CPU-side threads, but the NV2A
 * keeps executing the dashboard's queued commands autonomously (clearing /
 * compositing the framebuffer) until PFIFO is disabled. */
#define NV_PFIFO_CACHES          0x002500u
#define NV_PFIFO_CACHE1_DMA_PUSH 0x001220u
#define NV_PFIFO_CACHE1_PUSH0    0x003200u
#define NV_PFIFO_CACHE1_PULL0    0x003250u

#define XBOX_WC_ALIAS        0xF0000000u /* write-combined RAM alias */
#define XBOX_CACHED_ALIAS    0x80000000u /* cached RAM alias (what MmAllocateContiguousMemoryEx returns; NV2A snoops it) */

/* VGA sequencer in the NV2A PRMVIO aperture (RE @ AvSendTVEncoderOption opt 9 /
 * AvSetDisplayMode phase F).  NOTE: the sequencer is at 0x0C03C4/0x0C03C5, NOT
 * 0x6013C4 — that offset is the PRMCIO aperture, where a screen-on write
 * silently has no effect. */
#define NV_VGA_SEQ_IDX       0x0C03C4u   /* VGA sequencer index (PRMVIO) */
#define NV_VGA_SEQ_DAT       0x0C03C5u   /* VGA sequencer data  (PRMVIO) */
#define XBOX_RAM_PHYS_MASK   0x03FFFFFFu /* 64 MB */
#define XBOX_FB_PHYS         0x03C00000u /* fallback framebuffer if PCRTC reads 0 */

/* MCPX SMBus (x86 port I/O) — used to program the video encoder. */
#define SMBUS_STATUS         0xC000u
#define SMBUS_CONTROL        0xC002u
#define SMBUS_ADDRESS        0xC004u
#define SMBUS_DATA           0xC006u
#define SMBUS_COMMAND        0xC008u
#define SMBUS_BLOCK_DATA     0xC009u
#define SMBUS_CTL_BYTE       0x1Au
#define SMBUS_CTL_DWORD_WRITE 0x1Du
#define SMBUS_CTL_DWORD_READ  0x0Du
#define SMBUS_STATUS_DONE    0x10u
#define SMBUS_STATUS_ERROR   0x26u
#define SMBUS_TIMEOUT        100000u

/* System Management Controller (PIC16LC) on the same SMBus — drives the
 * front-panel LED.  Independent of NV2A video, so it works as a boot
 * breadcrumb even when the display path is dead. */
#define SMBUS_SMC_ADDR       0x20u   /* SMC write address */
#define SMC_CMD_LED_MODE     0x07u   /* 1 = use the custom sequence in reg 0x08 */
#define SMC_CMD_LED_SEQ      0x08u   /* pattern: R3 R2 R1 R0 G3 G2 G1 G0 */

static volatile uint32_t *fb;      /* framebuffer, XRGB8888  */
static uint32_t fb_phys;           /* physical scanout base  */
static uint32_t fb_stride = XBOX_SCREEN_WIDTH;  /* pixels per scanline (=pitch/4); read from the mode in xbox_video_init() */
static uint32_t fb_height = XBOX_SCREEN_HEIGHT;  /* visible scanlines; read from the mode (480 NTSC / 576 PAL) */

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port));
}

static int smbus_write_byte(uint8_t addr, uint8_t command, uint8_t value) {
    unsigned int t;
    outw(SMBUS_STATUS, inw(SMBUS_STATUS));      /* clear status */
    outb(SMBUS_ADDRESS, (uint8_t)(addr & 0xFEu));
    outb(SMBUS_COMMAND, command);
    outw(SMBUS_DATA, value);
    outb(SMBUS_CONTROL, SMBUS_CTL_BYTE);
    for(t = 0; t < SMBUS_TIMEOUT; t++) {
        uint16_t status = inw(SMBUS_STATUS);
        if(status & (SMBUS_STATUS_DONE | SMBUS_STATUS_ERROR)) {
            outw(SMBUS_STATUS, status);
            return (status & SMBUS_STATUS_DONE) ? 0 : -1;
        }
    }
    return -1;
}


/* Boot breadcrumb: force the front-panel LED to a solid colour over the SMBus.
 * pattern is a bitfield across the SMC's 4 blink phases — high nibble red, low
 * nibble green (so XBOX_LED_GREEN/RED/ORANGE in video.h give solid colours).
 * Writing reg 0x07 = 1 tells the SMC to use our custom sequence instead of the
 * kernel boot-animation's.  Best-effort: no retry, ignore SMBus errors. */
void xbox_smc_led(uint8_t pattern) {
    smbus_write_byte(SMBUS_SMC_ADDR, SMC_CMD_LED_SEQ, pattern);
    smbus_write_byte(SMBUS_SMC_ADDR, SMC_CMD_LED_MODE, 0x01u);
}

/* Hardware reset — the mechanism the kernel's HalReturnToFirmware uses (RE @
 * 0x80015278): signal the SMC that the reset is intentional, then pulse the
 * chipset reset-control register (port 0xCF9 = SYS_RST|RST_CPU|FULL_RST).  A CPU
 * triple-fault (the previous reboot approach) can hang once the loader owns the
 * IDT/GDT — this direct chipset reset always takes.  The reset re-runs the flash
 * bootloader, which reloads the kernel and boots the dashboard (with no disc). */
void xbox_reset(void) {
    smbus_write_byte(SMBUS_SMC_ADDR, 0x02u, 0x01u); /* SMC: intentional reset */
    outb(0xCF9u, 0x0Eu);                            /* chipset full reset      */
    for(;;)
        __asm__ volatile("cli; hlt");
}

/* VGA CRTC index/data ports (NV2A PRMCIO aperture); used to read the mode's
 * pitch back in xbox_read_fb_stride(). */
#define NV_CRTC_IDX    0x6013D4u   /* VGA CRTC index port (NV2A MMIO) */
#define NV_CRTC_DAT    0x6013D5u   /* VGA CRTC data port  */

/* Recover the scanout pitch (pixels/line) the kernel programmed, from the VGA
 * CRTC.  The kernel packs (pitch_bytes>>3) as CRTC 0x13 = bits0..7, 0x19 =
 * bits8..10 (masked 0xE0), 0x25 = bit11 (masked 0x20) (RE: AvSetDisplayMode
 * Pitch encode).  We draw 32bpp, so pixels/line = pitch_bytes/4.  This matters
 * because HDMI-modded boxes run the dashboard at 720x480 (pitch 2880), not
 * 640x480 (2560) — drawing at the wrong stride shears/tiles the image. */
static uint32_t xbox_read_fb_stride(void) {
    uint32_t c13, c19, c25, pitch, stride;
    NV2A_R8(NV_CRTC_IDX) = 0x13u; c13 = NV2A_R8(NV_CRTC_DAT);
    NV2A_R8(NV_CRTC_IDX) = 0x19u; c19 = NV2A_R8(NV_CRTC_DAT);
    NV2A_R8(NV_CRTC_IDX) = 0x25u; c25 = NV2A_R8(NV_CRTC_DAT);
    pitch  = (c13 | ((c19 & 0xE0u) << 3) | ((c25 & 0x20u) << 6)) << 3;
    stride = pitch / 4u;   /* 32bpp */
    if(stride < XBOX_SCREEN_WIDTH || stride > 2048u)
        stride = XBOX_SCREEN_WIDTH;   /* sanity fallback */
    return stride;
}

/* Recover the visible scanline count (height) from the NV2A PRAMDAC FP timing.
 * 0x680800 = vertical display-end (last visible line, 0-based): 0x1df = 479 →
 * 480 lines (NTSC), 0x23f = 575 → 576 (PAL).  (RE: AvSetDisplayMode FP timing
 * table, V group 0x680800-0x68081c.)  Keeps a PAL/odd-resolution console from
 * clipping to 480. */
static uint32_t xbox_read_fb_height(void) {
    uint32_t h = (NV2A_R32(0x680800u) & 0x0FFFu) + 1u;
    if(h < 400u || h > 1200u)
        h = XBOX_SCREEN_HEIGHT;   /* sanity fallback (480) */
    return h;
}

/* AvSendTVEncoderOption option 9 (RE @ 0x80030018): clear VGA Sequencer SR1 bit5
 * (un-blank) and enable AV output — the screen-on the modeset/teardown omits. */
static void xbox_video_unblank(void) {
    NV2A_R8(NV_VGA_SEQ_IDX) = 0x01u;  /* sequencer index = 1 (Clocking Mode)  */
    NV2A_R8(NV_VGA_SEQ_DAT) = 0x01u;  /* SR1 = 0x01: bit5 clear = screen ON   */
    outb(0x80D3u, 0x04u);             /* AV output enable                     */
}

/* Periodic platform hook (main loop + exception paths): keep scanout pinned to
 * our framebuffer, the pushbuffer halted, and the sequencer un-blanked so a late
 * kernel teardown can't move scanout, resume clearing, or blank the display. */
void xbox_video_kick(void) {
    NV2A_R32(NV_PCRTC_START)  = fb_phys;
    NV2A_R32(NV_PFIFO_CACHES) = 0;
    xbox_video_unblank();
    __asm__ volatile("sfence" ::: "memory");   /* drain WC buffers to RAM */
}

void xbox_video_init(void) {
    /* ===== Thunk-free display bring-up =====
     * We cannot synthesise this box's mode: the encoder (Conexant / Focus /
     * Xcalibur) varies by board revision, and AvSetDisplayMode programs the pixel
     * clock as per-mode ENCODER data, not an NV2A PLL.  So REUSE the mode the
     * kernel already programmed (correct for this cable + encoder) and:
     *   - halt the NV2A pushbuffer so the boot animation stops clearing the
     *     framebuffer (crt0 already cli'd the CPU threads);
     *   - reuse the live scanout buffer, drawing through the write-combined alias
     *     0xF0000000|P.  WBINVD first to evict stale dirty write-back lines for P
     *     (else they clobber our pixels in RAM), then draw ONLY via WC and SFENCE
     *     to drain.  (The cached alias 0x80000000|P is 4 KB-paged and page-faults
     *     on an arbitrary scanout page; the WC alias is a 4 MB PSE identity page.)
     *   - un-blank the VGA sequencer — a modeset/teardown leaves SR1 bit5 SET;
     *     this is what AvSendTVEncoderOption option 9 does.
     * Stride and height are read back from the mode (CRTC pitch / FP V-display-
     * end), so any resolution/region works — HDMI mods run 720x480, not 640x480.
     * Encoder-agnostic, so revision-agnostic.  Recovered clean-room.  No
     * MmAllocateContiguousMemoryEx / AvSetDisplayMode
     * / AvSendTVEncoderOption — zero kernel imports. */
    NV2A_R32(NV_PFIFO_CACHE1_DMA_PUSH) = 0;
    NV2A_R32(NV_PFIFO_CACHE1_PUSH0)    = 0;
    NV2A_R32(NV_PFIFO_CACHE1_PULL0)    = 0;
    NV2A_R32(NV_PFIFO_CACHES)          = 0;

    fb_phys = NV2A_R32(NV_PCRTC_START) & XBOX_RAM_PHYS_MASK;
    if(fb_phys == 0)
        fb_phys = XBOX_FB_PHYS;
    NV2A_R32(NV_PCRTC_START) = fb_phys;                     /* pin scanout       */
    fb_stride = xbox_read_fb_stride();                      /* real pixels/line  */
    fb_height = xbox_read_fb_height();                      /* real scanlines    */

    __asm__ volatile("wbinvd" ::: "memory");               /* evict stale lines */
    fb = (volatile uint32_t *)(XBOX_WC_ALIAS | fb_phys);   /* write-combined    */
    xbox_video_clear(0);
    __asm__ volatile("sfence" ::: "memory");               /* drain WC buffers  */

    xbox_video_unblank();                                  /* screen ON         */
    xbox_smc_led(XBOX_LED_GREEN);
}

static inline uint32_t to_xrgb(uint32_t rgb) {
    /* Common code passes 0x00RRGGBB; emit opaque A8R8G8B8 pixels. */
    return 0xff000000u | (rgb & 0x00ffffffu);
}

void xbox_video_clear(uint32_t color) {
    uint32_t c = to_xrgb(color);
    /* Clear the whole visible surface at the real mode-derived stride (e.g. 720
     * on HDMI mods, 640 stock) so no padding column is left as garbage. */
    int n = (int)(fb_stride * fb_height);
    if(!fb)
        return;
    for(int i = 0; i < n; i++)
        fb[i] = c;
}

void xbox_video_fill_rect(int x, int y, int w, int h, uint32_t color) {
    uint32_t c = to_xrgb(color);
    if(!fb)
        return;
    for(int row = 0; row < h; row++) {
        int py = y + row;
        if(py < 0 || py >= (int)fb_height)
            continue;
        volatile uint32_t *line = fb + py * fb_stride;
        for(int col = 0; col < w; col++) {
            int px = x + col;
            if(px < 0 || px >= XBOX_SCREEN_WIDTH)
                continue;
            line[px] = c;
        }
    }
}

static void draw_char(int x, int y, char ch, uint32_t color) {
    if(ch < 32 || ch > 126)
        return;
    const uint16_t *glyph = font_12x24[(int)(ch - 32)];
    for(int row = 0; row < XBOX_CHAR_HEIGHT; row++) {
        int py = y + row;
        if(py < 0 || py >= (int)fb_height)
            continue;
        uint16_t bits = glyph[row];
        if(!bits)
            continue;
        volatile uint32_t *line = fb + py * fb_stride;
        for(int col = 0; col < XBOX_CHAR_WIDTH; col++) {
            if(bits & (0x8000 >> col)) {
                int px = x + col;
                if(px >= 0 && px < XBOX_SCREEN_WIDTH)
                    line[px] = color;
            }
        }
    }
}

void xbox_video_draw_string(int x, int y, const char *str, uint32_t color) {
    /* Force white text: the shared code passes 16-bit-style colour values that
     * don't map cleanly to our 32-bit XRGB8888 framebuffer, so render all text
     * as solid white for legibility. */
    uint32_t c = 0xffffffffu;
    (void)color;
    if(!fb)
        return;
    int cx = x;
    for(const char *p = str; *p; p++) {
        if(*p == '\n') {
            cx = x;
            y += XBOX_CHAR_HEIGHT;
            continue;
        }
        draw_char(cx, y, *p, c);
        cx += XBOX_CHAR_WIDTH;
    }
}

void xbox_video_draw_bitmap(int x, int y, int w, int h, const uint32_t *bits, uint32_t color) {
    uint32_t c = to_xrgb(color);
    int words_per_row = (w + 31) / 32;
    if(!fb)
        return;
    for(int row = 0; row < h; row++) {
        int py = y + row;
        if(py < 0 || py >= (int)fb_height)
            continue;
        const uint32_t *row_bits = bits + row * words_per_row;
        volatile uint32_t *line = fb + py * fb_stride;
        for(int col = 0; col < w; col++) {
            int word = col / 32;
            int bit = 31 - (col % 32);
            if((row_bits[word] >> bit) & 1) {
                int px = x + col;
                if(px >= 0 && px < XBOX_SCREEN_WIDTH)
                    line[px] = c;
            }
        }
    }
}

/* ===== Names expected by shared common code + the kosload header ===== */

void setup_video(uint32_t mode, uint32_t bg_color) {
    (void)mode;
    (void)bg_color;
    if(!fb)
        xbox_video_init();
}

void clear_screen(uint32_t color) {
    xbox_video_clear(color);
}

void draw_string(int x, int y, const char *str, uint32_t color) {
    xbox_video_draw_string(x, y, str, color);
}

void clear_lines(int y, int height, unsigned int color) {
    xbox_video_fill_rect(0, y, XBOX_SCREEN_WIDTH, height, color);
}

/* Decimal uint -> string (shared exception/status display helper). */
void uint_to_string(uint32_t val, unsigned char *buf) {
    char tmp[11];
    int i = 0;
    if(val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while(val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    while(i > 0)
        buf[j++] = tmp[--i];
    buf[j] = '\0';
}

const char *exception_code_to_string(uint32_t code) {
    switch(code) {
        case 0:  return "Divide Error";
        case 1:  return "Debug";
        case 3:  return "Breakpoint";
        case 4:  return "Overflow";
        case 5:  return "BOUND Range";
        case 6:  return "Invalid Opcode";
        case 7:  return "Device Not Available";
        case 8:  return "Double Fault";
        case 10: return "Invalid TSS";
        case 11: return "Segment Not Present";
        case 12: return "Stack Fault";
        case 13: return "General Protection";
        case 14: return "Page Fault";
        case 16: return "x87 FP Error";
        case 17: return "Alignment Check";
        case 18: return "Machine Check";
        case 19: return "SIMD FP Exception";
        default: return "Exception";
    }
}
