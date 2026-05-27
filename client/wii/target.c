/* client/wii/target.c - Wii target_ops implementation.
 *
 * This initial Wii port reuses the repo's freestanding PPC loader model and a
 * minimal VI text console.  Networking is isolated behind ios_net.c so the IOS
 * socket ABI can be filled in clean-room without contaminating common code.
 */

#include <stdint.h>
#include <string.h>
#include <kosload/target.h>
#include <kosload/protocol.h>

#include "../gamecube/video.h"
#include "../gamecube/cache.h"
#include "ios_net.h"

extern void go(unsigned int addr);
extern void exception_common(void);

static volatile bool console_enabled = false;

#define PPC_MTSPRG0_R0  0x7C1043A6
#define PPC_LIS_R0(hi)  (0x3C000000 | ((hi) & 0xFFFF))
#define PPC_ORI_R0(lo)  (0x60000000 | ((lo) & 0xFFFF))
#define PPC_MTCTR_R0    0x7C0903A6
#define PPC_LI_R0(val)  (0x38000000 | ((val) & 0xFFFF))
#define PPC_BCTR        0x4E800420

static void install_exception_stub(uint32_t vector)
{
    volatile uint32_t *dst = (volatile uint32_t *)(0x80000000 + vector);
    uint32_t handler = (uint32_t)exception_common;

    dst[0] = PPC_MTSPRG0_R0;
    dst[1] = PPC_LIS_R0(handler >> 16);
    dst[2] = PPC_ORI_R0(handler);
    dst[3] = PPC_MTCTR_R0;
    dst[4] = PPC_LI_R0(vector);
    dst[5] = PPC_BCTR;

    cache_flush_range((const void *)(0x80000000 + vector), 24);
}

extern int write(int fd, const void *buf, unsigned int count);
extern void progexit(int status);

void exception_handler_c(uint32_t type, uint32_t srr0, uint32_t *save_area)
{
    uint8_t buf[432];
    (void)type;
    (void)srr0;

    buf[0] = 'E'; buf[1] = 'X'; buf[2] = 'P'; buf[3] = 'T';
    memcpy(buf + 4, save_area, 428);
    write(1, buf, 432);
    progexit(0);
}

void exception_init(void)
{
    static const uint32_t vectors[] = {
        0x0100, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700,
        0x0800, 0x0900, 0x0C00, 0x0D00, 0x0F00, 0x1300, 0x1700,
    };
    unsigned int i;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
        install_exception_stub(vectors[i]);
}

/* ---- Wii AV Encoder (AVE) I2C bring-up ------------------------------------
 * The Wii routes video through an external AV encoder reached over a bit-banged
 * I2C bus on Hollywood GPIO (SCL=0x4000, SDA=0x8000 of HW_GPIO1B at 0xCD8000C0).
 * A System-Menu channel launch leaves the encoder unconfigured, so VI timing
 * alone yields no signal (black) — unlike GameCube, whose encoder the IPL sets
 * up.  This programs the encoder for NTSC 480i.  Protocol, register map and the
 * linear-gamma table are from the WiiBrew "AV Encoder" RE notes (clean-room
 * documentation, not SDK source), cross-checked against the Homebrew Channel
 * binary which does the same I2C writes. */
#define AVE_GPIO_OUT (*(volatile unsigned int *)0xCD8000C0)   /* HW_GPIO1BOUT */
#define AVE_GPIO_DIR (*(volatile unsigned int *)0xCD8000C4)   /* HW_GPIO1BDIR */
#define AVE_GPIO_IN  (*(volatile unsigned int *)0xCD8000C8)   /* HW_GPIO1BIN  */
#define AVE_SCL  0x4000u
#define AVE_SDA  0x8000u
#define AVE_ADDR 0xe0u                                         /* 0x70<<1 | W  */

static void ave_delay(void)
{
    unsigned int a, b;
    __asm__ volatile("mftb %0" : "=r"(a));
    do { __asm__ volatile("mftb %0" : "=r"(b)); } while ((b - a) < 300u); /* ~5us */
}
static void ave_scl(int v){ if(v) AVE_GPIO_OUT|=AVE_SCL; else AVE_GPIO_OUT&=~AVE_SCL; __asm__ volatile("eieio"); }
static void ave_sda(int v){ if(v) AVE_GPIO_OUT|=AVE_SDA; else AVE_GPIO_OUT&=~AVE_SDA; __asm__ volatile("eieio"); }
static void ave_sda_out(void){ AVE_GPIO_DIR|=AVE_SDA;  __asm__ volatile("eieio"); }
static void ave_sda_in(void){  AVE_GPIO_DIR&=~AVE_SDA; __asm__ volatile("eieio"); }
static int  ave_sda_read(void){ return (AVE_GPIO_IN & AVE_SDA) ? 1 : 0; }

static void ave_start(void)
{
    ave_sda_out(); ave_sda(1); ave_scl(1); ave_delay();
    ave_sda(0); ave_delay(); ave_scl(0); ave_delay();
}
static void ave_stop(void)
{
    ave_sda_out(); ave_sda(0); ave_delay();
    ave_scl(1); ave_delay(); ave_sda(1); ave_delay();
}
static void ave_wbyte(unsigned char v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        ave_sda((v >> i) & 1); ave_delay();
        ave_scl(1); ave_delay();
        ave_scl(0); ave_delay();
    }
    /* ACK: release SDA, pulse SCL, sample (slave pulls low on ACK) */
    ave_sda_in(); ave_delay();
    ave_scl(1); ave_delay(); (void)ave_sda_read(); ave_scl(0); ave_delay();
    ave_sda_out();
}
static void ave_w(unsigned char reg, unsigned char val)
{
    ave_start();
    ave_wbyte(AVE_ADDR);
    ave_wbyte(reg);
    ave_wbyte(val);
    ave_stop();
}

static void wii_ave_init(void)
{
    /* NTSC encoder bring-up reverse-engineered from disassembly of the user's
     * own HBC boot DOL.  Differs from the WiiBrew sequence: 0x65 = 0x03 not
     * 0x01, register 0x01 is NOT written at all, and the 0x7A and 0x0A blocks
     * are added.  Using the WiiBrew sequence here produced visibly wrong
     * colours (red looked orange, green tinted cyan) on a channel-launched
     * loader; the disassembled sequence reproduces correct colour. */
    static const unsigned char gamma[33] = {
        0x10,0x00,0x10,0x00,0x10,0x00,0x10,0x00,0x10,0x00,0x10,0x00,0x10,0x20,0x40,0x60,
        0x80,0xa0,0xeb,0x10,0x00,0x20,0x00,0x40,0x00,0x60,0x00,0x80,0x00,0xa0,0x00,0xeb,0x00
    };
    int i;

    AVE_GPIO_DIR |= (AVE_SCL | AVE_SDA); __asm__ volatile("eieio");  /* SCL,SDA outputs */
    AVE_GPIO_OUT |= (AVE_SCL | AVE_SDA); __asm__ volatile("eieio");  /* idle high       */

    ave_w(0x6a, 0x01);
    ave_w(0x65, 0x03);                    /* HBC value (was 0x01 in WiiBrew docs) */
    ave_w(0x00, 0x00);                    /* A/V timings -- HBC does NOT write 0x01 */
    ave_w(0x71, 0x8e); ave_w(0x72, 0x8e); /* audio volume */
    ave_w(0x02, 0x07);                    /* VBI */
    ave_w(0x05, 0x00); ave_w(0x06, 0x00); /* CGMS */
    ave_w(0x08, 0x00); ave_w(0x09, 0x00); /* WSS */
    ave_w(0x7a, 0x00); ave_w(0x7b, 0x00); /* HBC adds these four */
    ave_w(0x7c, 0x00); ave_w(0x7d, 0x00);
    for (i = 0x40; i <= 0x59; i++) ave_w((unsigned char)i, 0x00);   /* macrovision off */
    ave_w(0x0a, 0x00);                    /* HBC adds this */
    ave_w(0x03, 0x01);                    /* composite trap filter */
    for (i = 0; i < 33; i++) ave_w((unsigned char)(0x10 + i), gamma[i]);
    ave_w(0x04, 0x01);                    /* enable A/V output */
    ave_w(0x6e, 0x00);                    /* disable RGB filter */
}

static int wii_init(void)
{
    /* CPU/MMU bring-up is done in the channel stub; here configure the AV
     * encoder + VI and return. */
    wii_ave_init();
    gc_video_init();
    return 0;
}

static void wii_draw_string(int x, int y, const char *str, uint32_t color)
{
    gc_video_draw_string(x, y, str, color);
}

static void wii_clear_screen(uint32_t color)
{
    gc_video_clear(color);
}

static void wii_setup_video(uint32_t mode, uint32_t bg_color)
{
    (void)mode;
    (void)bg_color;
}

static void wii_execute(uint32_t address)
{
    cache_flush_range((const void *)address, 0x01800000);
    go(address);
}

/* Firmware-update handoffs are one-way: the trampoline never returns through
 * cmd_execute, so the bb->stop / init / start cycle there is unreachable and
 * the old loader's IOS UDP socket would stay bound to NET_DEFAULT_PORT.  The
 * replacement loader's bind() would then return -3 (in use).  Tearing IOS
 * down here closes the socket cleanly before the handoff. */
static void wii_fw_update_prepare(void)
{
    wii_ios_net_shutdown();
}

static void wii_disable_cache(void)
{
    cache_disable();
}

static void wii_reboot(void)
{
    cache_disable();
    void (*entry)(void) = (void (*)(void))WII_LOADER_BASE;
    entry();
}

void disable_cache(void)
{
    cache_disable();
}

static void wii_set_console_enabled(bool enabled)
{
    console_enabled = enabled;
    *(volatile unsigned int *)(WII_LOADER_BASE + 4) =
        enabled ? 0xdeadbeef : 0xfeedface;
}

static void wii_set_rtc(uint32_t unix_timestamp)
{
    (void)unix_timestamp;
}

static uint32_t wii_get_rtc(void)
{
    return 0;
}

#define WII_TBR_TICKS_PER_SEC 60750000u

static uint64_t wii_get_ticks(void)
{
    uint32_t tbl, tbu0, tbu1;

    do {
        __asm__ volatile("mftbu %0" : "=r"(tbu0));
        __asm__ volatile("mftb  %0" : "=r"(tbl));
        __asm__ volatile("mftbu %0" : "=r"(tbu1));
    } while (tbu0 != tbu1);

    return ((uint64_t)tbu0 << 32) | tbl;
}

static void wii_restart_timer(void)
{
}

#define XFB_ADDR 0xC0050000

static void wii_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    uint32_t fill = gc_color_to_yuy2(color);
    int row;

    for (row = 0; row < h; row++) {
        uint32_t *fb32 = (uint32_t *)(XFB_ADDR +
                          (y + row) * GC_SCREEN_WIDTH * 2 + (x & ~1) * 2);
        int pairs = (w + 1) / 2;
        int i;
        for (i = 0; i < pairs; i++)
            fb32[i] = fill;
    }

    cache_flush_dc((const void *)(XFB_ADDR + y * GC_SCREEN_WIDTH * 2),
                   h * GC_SCREEN_WIDTH * 2);
}

static void wii_draw_bitmap(int x, int y, int w, int h,
                            const uint32_t *bits, uint32_t color)
{
    uint32_t yuy2 = gc_color_to_yuy2(color);
    uint8_t c_y = (yuy2 >> 24) & 0xFF;
    uint8_t c_cb = (yuy2 >> 16) & 0xFF;
    uint8_t c_cr = yuy2 & 0xFF;
    int words_per_row = (w + 31) / 32;
    int row, col;

    for (row = 0; row < h; row++) {
        const uint32_t *row_bits = bits + row * words_per_row;
        for (col = 0; col < w; col += 2) {
            int word0 = col / 32;
            int bit0 = 31 - (col % 32);
            int word1 = (col + 1) / 32;
            int bit1 = 31 - ((col + 1) % 32);
            int p0 = (row_bits[word0] >> bit0) & 1;
            int p1 = (col + 1 < w) ? ((row_bits[word1] >> bit1) & 1) : 0;
            uint32_t *fb32;

            if (!p0 && !p1)
                continue;

            fb32 = (uint32_t *)(XFB_ADDR + (y + row) * GC_SCREEN_WIDTH * 2 +
                                ((x + col) & ~1) * 2);
            if (p0 && p1) {
                *fb32 = ((uint32_t)c_y << 24) | ((uint32_t)c_cb << 16) |
                        ((uint32_t)c_y << 8) | (uint32_t)c_cr;
            } else {
                uint32_t existing = *fb32;
                uint8_t y0 = p0 ? c_y : (existing >> 24) & 0xFF;
                uint8_t y1 = p1 ? c_y : (existing >> 8) & 0xFF;
                *fb32 = ((uint32_t)y0 << 24) | ((uint32_t)c_cb << 16) |
                        ((uint32_t)y1 << 8) | (uint32_t)c_cr;
            }
        }
    }

    cache_flush_dc((const void *)(XFB_ADDR + y * GC_SCREEN_WIDTH * 2),
                   h * GC_SCREEN_WIDTH * 2);
}

static uint32_t wii_detect_ram_size(void)
{
    return 88 * 1024 * 1024;
}

const target_ops_t wii_target_ops = {
    .name = "Wii",
    .default_load = WII_DEFAULT_LOAD_ADDR,
    .init = wii_init,
    .draw_string = wii_draw_string,
    .clear_screen = wii_clear_screen,
    .setup_video = wii_setup_video,
    .execute = wii_execute,
    .disable_cache = wii_disable_cache,
    .reboot = wii_reboot,
    .set_console_enabled = wii_set_console_enabled,
    .set_rtc = wii_set_rtc,
    .get_rtc = wii_get_rtc,
    .get_ticks = wii_get_ticks,
    .ticks_per_second = WII_TBR_TICKS_PER_SEC,
    .fill_rect = wii_fill_rect,
    .draw_bitmap = wii_draw_bitmap,
    .restart_timer = wii_restart_timer,
    .detect_ram_size = wii_detect_ram_size,
    .fw_update_prepare = wii_fw_update_prepare,
};

const target_ops_t *target_get_ops(void)
{
    return &wii_target_ops;
}
