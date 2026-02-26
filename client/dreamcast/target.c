/* client/dreamcast/target.c */
/*
 * Dreamcast target_ops implementation.
 *
 * Connects the target interface to DC-specific hardware functions:
 * video (from video.S/video.c), cache (from disable.S), CDFS
 * (from cdfs_redir.S), exception handler (from exception.S),
 * and execution (from go.S).
 */

#include <stdint.h>
#include <kosload/target.h>
#include <kosload/protocol.h>

/* From video.S (compiler prepends _ to C symbols on SH-ELF) */
extern void draw_string(int x, int y, const char *str, int color);
extern void clrscr(int color);
extern int  check_cable(void);
extern void init_video(int cabletype, int pixelmode);

/* From video.c */
extern char *exception_code_to_string(unsigned int expevt);
extern void setup_video(uint32_t mode, uint32_t bg_color);

/* From disable.S */
extern void disable_cache(void);

/* From go.S */
extern void go(unsigned int addr);

/* From cdfs_redir.S */
extern void cdfs_redir_save(void);
extern void cdfs_redir_enable(void);
extern void cdfs_redir_disable(void);

/* From scif.c */
extern void scif_init(int bps);

/* From maple.c */
extern void maple_init(void);

/* From cdfs.c */
extern void cdfs_init(void);

/* Console enabled flag — affects serial transport behavior */
static volatile int console_enabled = 0;

/* Forward declaration (defined in screensaver support section below) */
static void dc_tmu2_start(void);

static int dc_init(void) {
    scif_init(57600);

    maple_init();

    dc_tmu2_start();

    /* Save CDFS BIOS vectors and disable redirection */
    cdfs_init();
    return 0;
}

static void dc_draw_string(int x, int y, const char *str, uint32_t color) {
    draw_string(x, y, str, (int)(color & 0xffff));
}

static void dc_clear_screen(uint32_t color) {
    clrscr((int)(color & 0xffff));
}

static void dc_setup_video(uint32_t mode, uint32_t bg_color) {
    setup_video(mode, bg_color);
}

static void dc_execute(uint32_t address) {
    disable_cache();
    go(address);
}

static void dc_disable_cache_op(void) {
    disable_cache();
}

static void dc_reboot(void) {
    /* Jump back to kosload entry point */
    disable_cache();
    void (*entry)(void) = (void (*)(void))0x8c004000;
    entry();
}

static const char *dc_exception_to_string(uint32_t code) {
    return exception_code_to_string(code);
}

static void dc_cdfs_redir_save_op(void) {
    cdfs_redir_save();
}

static void dc_cdfs_redir_enable_op(void) {
    cdfs_redir_enable();
}

static void dc_cdfs_redir_disable_op(void) {
    cdfs_redir_disable();
}

static void dc_set_console_enabled(int enabled) {
    console_enabled = enabled;
    /* Write magic value at firmware base+4 for loaded program to detect.
     * Use P1 (cached) address — with write-through cache (CCR=0x090b),
     * the write reaches physical memory immediately. Matches legacy. */
    if (enabled)
        *(volatile unsigned int *)0x8c004004 = 0xdeadbeef;
    else
        *(volatile unsigned int *)0x8c004004 = 0xfeedface;
}

/* AICA RTC registers (P2 uncached addresses) */
#define AICA_RTC_SECS_H  (*(volatile uint32_t *)0xa0710000)
#define AICA_RTC_SECS_L  (*(volatile uint32_t *)0xa0710004)
#define AICA_RTC_CTRL    (*(volatile uint32_t *)0xa0710008)
/* Seconds from 1950-01-01 to 1970-01-01 (AICA epoch to Unix epoch) */
#define UNIX_TO_AICA_OFFSET 631152000u
#define FLASHROM_BLOCK_SIZE 64
#define FLASHROM_BITMAP_MAX_BYTES 64
#define FLASHROM_OFFSET_CRC 62

/* ===== BIOS flashrom syscalls (vector at 0x8c0000b8) =====
 *
 * The BIOS flashrom handler dispatches on r7 (4th arg in SH4 ABI):
 *   0 = INFO, 1 = READ, 2 = WRITE, 3 = DELETE
 */
typedef int (*bios_flashrom_fn_t)(int, void *, int, int);

static bios_flashrom_fn_t flashrom_fn(void)
{
    return (bios_flashrom_fn_t)(*(uint32_t *)0x8c0000b8);
}

static int fr_info(int part, int *start, int *size)
{
    int info[2];
    int ret = flashrom_fn()(part, info, 0, 0);
    if (ret < 0) return -1;
    *start = info[0];
    *size = info[1];
    return 0;
}

static int fr_read(int pos, void *dest, int n)
{
    return flashrom_fn()(pos, dest, n, 1);
}

static int fr_write(int pos, void *src, int n)
{
    return flashrom_fn()(pos, src, n, 2);
}

/* CRC-16-CCITT over first 62 bytes of a 64-byte flashrom block */
static uint16_t flashrom_crc(const uint8_t *buf)
{
    int n = 0xffff;
    for (int i = 0; i < FLASHROM_OFFSET_CRC; i++) {
        n ^= buf[i] << 8;
        for (int c = 0; c < 8; c++) {
            if (n & 0x8000)
                n = (n << 1) ^ 4129;
            else
                n = (n << 1);
        }
    }
    return (uint16_t)((~n) & 0xffff);
}

/*
 * Update flashrom syscfg date to match the RTC. (Discovered by ATeam)
 *
 * Without this, the BIOS detects a mismatch between the RTC and the
 * "last set time" in flashrom on next boot and prompts the user to
 * re-enter the date/time.
 *
 * Partition 2 uses block allocation: 64-byte blocks with a bitmap at the
 * end of the partition tracking which slots are used (bit=1 unused, 0 used).
 * We find the current syscfg block (block_id=5), update its date field,
 * recalculate the CRC, and write it to a free slot.
 */
#define FLASHROM_PART_SYSCFG  2
#define FLASHROM_BLOCK_ID_SYSCFG  5

static void update_flashrom_syscfg(uint32_t aica_time)
{
    int start, size;
    if (fr_info(FLASHROM_PART_SYSCFG, &start, &size) < 0)
        return;

    if (size < FLASHROM_BLOCK_SIZE * 2)
        return;

    int num_blocks = size / FLASHROM_BLOCK_SIZE;

    /* Bitmap size: one bit per block, rounded up to 64-byte boundary.
     * For 16KB partition: 256 blocks → 32 bytes → rounds to 64 bytes. */
    int bmcnt = num_blocks;
    bmcnt = (bmcnt + (64 * 8) - 1) & ~(64 * 8 - 1);
    bmcnt = bmcnt / 8;

    /* Read bitmap from end of partition */
    uint8_t bitmap[FLASHROM_BITMAP_MAX_BYTES];
    if (bmcnt > FLASHROM_BITMAP_MAX_BYTES)
        return;  /* Sanity check */

    if (fr_read(start + size - bmcnt, bitmap, bmcnt) < 0)
        return;

    /* Find the newest valid syscfg block by scanning from high to low.
     * Block 0 is the partition magic block, so skip it. */
    uint8_t block[FLASHROM_BLOCK_SIZE];
    int found = 0;
    for (int i = num_blocks - 1; i > 0; i--) {
        if (bitmap[i / 8] & (0x80 >> (i % 8)))
            continue;  /* Bit set = unused, skip */

        if (fr_read(start + (i + 1) * FLASHROM_BLOCK_SIZE, block, FLASHROM_BLOCK_SIZE) < 0)
            continue;

        if (((uint16_t)block[0] | ((uint16_t)block[1] << 8)) != FLASHROM_BLOCK_ID_SYSCFG)
            continue;

        if (flashrom_crc(block) != ((uint16_t)block[62] | ((uint16_t)block[63] << 8)))
            continue;

        found = 1;
        break;
    }

    if (!found)
        return;  /* No existing syscfg — don't create one from scratch */

    /* Update date field at offset 2 (little-endian, 4 bytes) */
    block[2] = aica_time & 0xff;
    block[3] = (aica_time >> 8) & 0xff;
    block[4] = (aica_time >> 16) & 0xff;
    block[5] = (aica_time >> 24) & 0xff;

    /* Recalculate CRC at offset 62 */
    uint16_t crc = flashrom_crc(block);
    block[FLASHROM_OFFSET_CRC] = crc & 0xff;
    block[FLASHROM_OFFSET_CRC + 1] = (crc >> 8) & 0xff;

    /* Find first unused real slot (bit set = unused), skip bit 0. */
    int free_slot = -1;
    for (int i = 1; i < num_blocks; i++) {
        if (bitmap[i / 8] & (0x80 >> (i % 8))) {
            free_slot = i;
            break;
        }
    }

    if (free_slot < 0)
        return;  /* Partition full — BIOS will compact on next boot */

    /* Write bitmap first (mark slot as used: clear the bit).
     * If the block write fails after this, we lose one slot but no corruption. */
    uint8_t bm_byte = bitmap[free_slot / 8] & ~(0x80 >> (free_slot % 8));
    if (fr_write(start + size - bmcnt + (free_slot / 8), &bm_byte, 1) < 0)
        return;

    /* Write the updated syscfg block to the reserved slot */
    if (fr_write(start + (free_slot + 1) * FLASHROM_BLOCK_SIZE, block, FLASHROM_BLOCK_SIZE) < 0)
        return;
}

static uint32_t dc_get_rtc(void)
{
    uint32_t h1, h2, l;

    /* Read high, low, high again to detect rollover of the low 16 bits */
    do {
        h1 = AICA_RTC_SECS_H & 0xffff;
        l  = AICA_RTC_SECS_L & 0xffff;
        h2 = AICA_RTC_SECS_H & 0xffff;
    } while (h1 != h2);

    return ((h1 << 16) | l) - UNIX_TO_AICA_OFFSET;
}

static void dc_set_rtc(uint32_t unix_timestamp)
{
    uint32_t aica_time = unix_timestamp + UNIX_TO_AICA_OFFSET;
    int i;

    /* Write low first, then high; writing high locks RTC writes. */
    AICA_RTC_CTRL = 1;
    for (i = 0; i < 3; i++) {
        AICA_RTC_SECS_L = aica_time & 0xffff;
        AICA_RTC_SECS_H = (aica_time >> 16) & 0xffff;

        if ((((AICA_RTC_SECS_H & 0xffff) << 16) | (AICA_RTC_SECS_L & 0xffff)) == aica_time)
            break;
    }
    AICA_RTC_CTRL = 0;

    if (i == 3)
        return;

    /* Update flashrom syscfg date to prevent BIOS date/time prompt on next boot */
    update_flashrom_syscfg(aica_time);
}

/* ===== Screensaver support: timer, fill_rect, input polling ===== */

/* SH4 Timer Unit channel 2 */
#define TMU_TSTR   (*(volatile unsigned char  *)0xffd80004)
#define TMU_TCOR2  (*(volatile unsigned int   *)0xffd80020)
#define TMU_TCNT2  (*(volatile unsigned int   *)0xffd80024)
#define TMU_TCR2   (*(volatile unsigned short *)0xffd80028)

#define DC_TMU_TICKS_PER_SEC  48828  /* Pclk/1024 = 50MHz/1024 */

static void dc_tmu2_start(void)
{
    TMU_TSTR &= ~4;                /* Stop TMU2 */
    TMU_TCOR2 = 0xFFFFFFFF;
    TMU_TCNT2 = 0xFFFFFFFF;
    TMU_TCR2 = 4;                  /* Pclk/1024, no interrupt */
    TMU_TSTR |= 4;                 /* Start TMU2 */
}

static uint64_t dc_get_ticks(void)
{
    return (uint64_t)(0xFFFFFFFF - TMU_TCNT2);   /* Convert count-down to count-up */
}

static void dc_restart_timer(void)
{
    dc_tmu2_start();
}

static void dc_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    unsigned short c16 = (unsigned short)(color & 0xffff);
    int row, col;
    for (row = 0; row < h; row++) {
        volatile unsigned short *vram =
            (volatile unsigned short *)(0xa5000000 + (y + row) * 640 * 2 + x * 2);
        for (col = 0; col < w; col++)
            vram[col] = c16;
    }
}

static void dc_draw_bitmap(int x, int y, int w, int h,
                           const uint32_t *bits, uint32_t color)
{
    unsigned short c16 = (unsigned short)(color & 0xffff);
    int row, col;
    int words_per_row = (w + 31) / 32;

    for (row = 0; row < h; row++) {
        volatile unsigned short *vram =
            (volatile unsigned short *)(0xa5000000 + (y + row) * 640 * 2 + x * 2);
        const uint32_t *row_bits = bits + row * words_per_row;
        for (col = 0; col < w; col++) {
            int word = col / 32;
            int bit = 31 - (col % 32);
            if ((row_bits[word] >> bit) & 1)
                vram[col] = c16;
        }
    }
}

/*
 * Detect installed RAM size via mirror test (same technique as KOS).
 *
 * Write different values to the top of the 16MB and 32MB ranges
 * using P2 (uncached) addresses, then read back. If they match,
 * the upper 16MB mirrors the lower 16MB (standard 16MB console).
 * If different, the upper 16MB is real RAM (32MB mod).
 */
static uint32_t dc_detect_ram_size(void)
{
    volatile unsigned char *addr_16m = (volatile unsigned char *)0xACFFFFFF;
    volatile unsigned char *addr_32m = (volatile unsigned char *)0xADFFFFFF;

    *addr_16m = 0xBA;
    *addr_32m = 0xAB;

    if (*addr_16m != *addr_32m)
        return 32 * 1024 * 1024;

    return 16 * 1024 * 1024;
}

/*
 * Exception handler initialization.
 *
 * The full exception handler (exception.bin) is copied to 0x8c00f400 by
 * the 1st_read loader at boot time, matching legacy dcload behavior.
 * Nothing to do here at runtime.
 */
void exception_init(void)
{
}

const target_ops_t dreamcast_target_ops = {
    .name = "Dreamcast",
    .default_load = DC_DEFAULT_LOAD_ADDR,
    .init = dc_init,
    .draw_string = dc_draw_string,
    .clear_screen = dc_clear_screen,
    .setup_video = dc_setup_video,
    .execute = dc_execute,
    .disable_cache = dc_disable_cache_op,
    .reboot = dc_reboot,
    .exception_to_string = dc_exception_to_string,
    .cdfs_redir_save = dc_cdfs_redir_save_op,
    .cdfs_redir_enable = dc_cdfs_redir_enable_op,
    .cdfs_redir_disable = dc_cdfs_redir_disable_op,
    .set_console_enabled = dc_set_console_enabled,
    .set_rtc = dc_set_rtc,
    .get_rtc = dc_get_rtc,
    .get_ticks = dc_get_ticks,
    .ticks_per_second = DC_TMU_TICKS_PER_SEC,
    .fill_rect = dc_fill_rect,
    .draw_bitmap = dc_draw_bitmap,
    .restart_timer = dc_restart_timer,
    .detect_ram_size = dc_detect_ram_size,
};

const target_ops_t *target_get_ops(void) {
    return &dreamcast_target_ops;
}
