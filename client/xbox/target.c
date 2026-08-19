/* client/xbox/target.c — Xbox target_ops implementation.
 *
 * Connects the shared client interface to Xbox hardware. The loader
 * runtime makes ZERO firmware/kernel calls — NIC, video, cache, exceptions and
 * reboot are all driven directly on the hardware.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <kosload/target.h>
#include <kosload/protocol.h>

#include "video.h"
#include "cache.h"

/* From go.S */
extern void go(uint32_t addr);
/* From exception.c — rebuilds the IDT using the current code selector. */
extern void exception_init(void);
extern void xbox_clean_rtc_init(void);
extern void xbox_clean_set_rtc(uint32_t unix_timestamp);
extern int  xbox_clean_get_rtc(uint32_t *unix_timestamp);

/* Xbox CPU clock (Pentium III @ 733.33 MHz) — the TSC increments at this rate.
 * Approximate; only used for coarse timeouts and the ~1 Hz status/lease
 * display, so exactness is not required. */
#define XBOX_CPU_HZ  733333333

static bool console_enabled = true;
static uint64_t monotonic_epoch_ticks;

static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ===== target_ops implementations ===== */

/* Kernel takeover: install our own GDT (the loader owns its IDT, but its gates
 * reference the KERNEL's GDT), re-point the IDT, and mask the PICs.  After this
 * the machine is entirely ours: the MS kernel is silenced (its ISRs live in the
 * replaced IDT; no interrupt reaches it) — no kernel code runs, no syscalls.
 * "Exit" is reboot-only (xbox_reset() -> flash bootloader reloads the kernel and
 * boots the dashboard).
 *
 * We deliberately DO NOT zero the kernel here.  xbox_relocate_identity() (called
 * next) physically relocates the loader on top of the kernel at phys 0x11000, so
 * the kernel image is *overwritten in place* where the loader lands; the small
 * remnants outside the loader (0x10000..0x11000 and image_end..0x3CB08) are dead
 * (silenced) and harmless.  Erasing 0x10000..0x3CB08 here was fatal on an -F
 * re-entry: by then the loader already lives at phys 0x11000 (the identity
 * region), so the erase would zero the running loader out from under itself. */
static uint64_t xbox_gdt[3] __attribute__((aligned(8))) = {
    0x0000000000000000ull, /* null                              */
    0x00CF9A000000FFFFull, /* 0x08: ring-0 32-bit 4GB flat code */
    0x00CF92000000FFFFull, /* 0x10: ring-0 32-bit 4GB flat data */
};

static void xbox_kernel_takeover(void) {
    struct __attribute__((packed)) {
        uint16_t limit;
        uint32_t base;
    } gdtr = { sizeof(xbox_gdt) - 1, (uint32_t)(uintptr_t)xbox_gdt };

    __asm__ volatile("cli");

    /* Our own GDT, then reload every segment register (a far jump reloads CS). */
    __asm__ volatile(
        "lgdt %0\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "ljmp $0x08, $1f\n\t"
        "1:\n\t"
        :: "m"(gdtr) : "eax", "memory");

    /* Re-point the IDT gates at our (now-current) 0x08 code selector. */
    exception_init();

    /* Mask the legacy PICs (belt-and-suspenders on the cli). */
    __asm__ volatile("outb %%al, $0x21" :: "a"((uint8_t)0xFF));
    __asm__ volatile("outb %%al, $0xA1" :: "a"((uint8_t)0xFF));
}

/* Linker-provided bounds of the loader image (see kosload-ip.ld.in).  The
 * __asm__ labels pin the exact linker names — PE/i386 C would otherwise prepend
 * a leading underscore (___stack_bottom__) and fail to resolve. */
extern char __loaded_image_start__[] __asm__("__loaded_image_start__"); /* 0x11000 */
extern char __stack_bottom__[]       __asm__("__stack_bottom__");
extern char __stack_top__[]          __asm__("__stack_top__");
extern char __loaded_image_end__[]   __asm__("__loaded_image_end__");

/* Cached RAM alias: VA (0x80000000 | phys) == that physical page. */
#define XBOX_RAM_ALIAS   0x80000000
/* Kernel self-mapped page tables: the PTE for VA X is at 0xC0000000 + (X>>12)*4. */
#define XBOX_SELFMAP_PT  0xC0000000

/* Relocate the running loader so it is IDENTITY-mapped at physical 0x11000 (its
 * own linked VA), making VA == PA.  After this dma_phys() is the identity and the
 * whole scattered-VA!=PA class of NIC/DMA problems disappears.  Approach B: a
 * page-table remap (no rebasing), so the kosload header stays at VA 0x11000 and
 * the guest + KOS ABIs are untouched.
 *
 * Called after xbox_kernel_takeover() has silenced the MS kernel.  The identity
 * destination lies in its former resident range at physical 0x10000..0x3CB08,
 * below the XBE allocation pool; the whole reserved identity image fits there.
 * LED breadcrumbs (frozen colour on a hang): RED = died copying code/data,
 * ORANGE = died in the PTE rewrite, GREEN = died in the atomic switch. */
static void xbox_relocate_identity(void) {
    uintptr_t img_start = (uintptr_t)__loaded_image_start__;   /* 0x11000 */
    uintptr_t stk_bot   = (uintptr_t)__stack_bottom__;
    uintptr_t stk_top   = (uintptr_t)__stack_top__;
    uintptr_t img_end   = ((uintptr_t)__loaded_image_end__ + 0xFFFu) & ~0xFFFu;
    uintptr_t va;
    uint32_t  cr0;

    /*
     * A firmware-update trampoline enters the replacement loader with the
     * first loader's identity mappings still active.  Do not repeat the cold
     * relocation in that state: source and destination then alias the same
     * physical pages, including the live stack, and rewriting those live PTEs
     * creates an unnecessary MMU/cache hazard unique to Xbox soft re-entry.
     *
     * The kernel-launched XBE is scattered across allocated physical pages, so
     * at least one PTE differs and takes the relocation path below.
     */
    bool already_identity = true;
    for(va = img_start; va < img_end; va += 0x1000u) {
        volatile uint32_t *pte =
            (volatile uint32_t *)(uintptr_t)(XBOX_SELFMAP_PT + ((va >> 12) << 2));
        uint32_t entry = *pte;
        if((entry & 0xFFFFF001u) !=
            (uint32_t)((va & 0xFFFFF000u) | 1u)) {
            already_identity = false;
            break;
        }
    }
    if(already_identity)
        return;

    xbox_smc_led(XBOX_LED_RED);

    /* Permit writes to the (possibly read-only) page tables and alias pages. */
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0 & ~0x00010000u) : "memory");

    /* Copy code + data + non-stack BSS to identity physical via the cached alias.
     * The non-stack BSS carries live pre-relocation state (fb_phys/stride/height,
     * monotonic_epoch_ticks) that must survive the switch; it does not change
     * before the atomic block below, so a plain copy here is safe. */
    memcpy((void *)(uintptr_t)(XBOX_RAM_ALIAS + img_start),
           (const void *)img_start, stk_bot - img_start);

    xbox_smc_led(XBOX_LED_ORANGE);

    /* Rewrite the loader's PTEs to identity.  The TLB still holds the OLD
     * mappings, so execution continues from the old physical pages until the
     * single CR3 reload in the atomic block. */
    for(va = img_start; va < img_end; va += 0x1000u) {
        volatile uint32_t *pte =
            (volatile uint32_t *)(uintptr_t)(XBOX_SELFMAP_PT + ((va >> 12) << 2));
        *pte = (uint32_t)((va & 0xFFFFF000u) | (*pte & 0x00000FFFu));
    }

    xbox_smc_led(XBOX_LED_GREEN);

    /* Atomic switch: snapshot the LIVE stack to identity physical, flush the
     * caches (all copies went through the cached alias), then reload CR3 to flush
     * the TLB.  VA == PA afterward, with our frames intact at the new physical.
     * There must be NO stack writes between the rep-movsl and the CR3 reload, so
     * it is one asm block. */
    {
        uintptr_t d_esi, d_edi, d_ecx; /* rep movsl consumes esi/edi/ecx */
        __asm__ volatile(
            "cli\n\t"
            "cld\n\t"
            "rep movsl\n\t"              /* copy stack [esi]->[edi], ecx dwords */
            "wbinvd\n\t"                 /* push code+data+stack copies to RAM  */
            "movl %%cr3, %%eax\n\t"
            "movl %%eax, %%cr3\n\t"      /* TLB flush -> identity mapping live   */
            : "=S"(d_esi), "=D"(d_edi), "=c"(d_ecx)
            : "0"(stk_bot),
              "1"((uintptr_t)(XBOX_RAM_ALIAS + stk_bot)),
              "2"((stk_top - stk_bot) >> 2)
            : "eax", "memory", "cc");
    }

    /* Identity-mapped now.  Restore CR0.WP. */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}

static int xbox_init(void) {
    /* Establish a loader-owned monotonic epoch before DHCP starts.  RDTSC
     * continues running while uploaded programs execute, so lease expiry can
     * be rebased accurately when control returns. */
    monotonic_epoch_ticks = read_tsc();
    xbox_clean_rtc_init();

    /* LED boot status: RED while video initializes, GREEN once the framebuffer
     * and encoder are ready.  Allocation or mode-set failure remains RED. */
    xbox_smc_led(XBOX_LED_RED);
    xbox_video_init();               /* reuse the kernel's live display mode */

    /* Video is up; now silence the MS kernel and fully own the machine. */
    xbox_kernel_takeover();

    /* Relocate to run identity-mapped at physical 0x11000 (VA == PA), so the NIC
     * DMA path needs no page-walk.  Must follow the takeover, after the former
     * kernel destination pages are no longer executing. */
    xbox_relocate_identity();
    return 0;
}

static void xbox_draw_string(int x, int y, const char *str, uint32_t color) {
    xbox_video_draw_string(x, y, str, color);
}

static void xbox_clear_screen(uint32_t color) {
    xbox_video_clear(color);
}

static void xbox_setup_video(uint32_t mode, uint32_t bg_color) {
    (void)mode;
    (void)bg_color;
    /* Framebuffer already acquired in xbox_init(). */
}

static void xbox_execute(uint32_t address) {
    /* Make sure freshly uploaded code/data is visible, then run it.  go()
     * returns when the program returns (or the exception handler halts). */
    cache_flush_range((const void *)address, 0);
    go(address);
}

static void xbox_disable_cache(void) {
    cache_disable();
}

/* Global wrapper for shared code that references disable_cache() by name. */
void disable_cache(void) {
    cache_disable();
}

static void xbox_reboot(void) {
    /* Chipset reset (port 0xCF9) — the kernel's own reboot mechanism, reliable
     * even after the loader has installed its own IDT/GDT (a CPU triple-fault
     * hangs in that state).  Re-runs flash -> reloads kernel -> dashboard. */
    xbox_reset();
}

static void xbox_set_console_enabled(bool enabled) {
    uint32_t cr0;

    console_enabled = enabled;

    /* Guests read this word and skip their syscalls unless it says 0xdeadbeef.
     * It lives in read-only .text, so drop CR0.WP across the store the way
     * xbox_relocate_identity() does. */
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0 & ~0x00010000) : "memory");
    *(volatile uint32_t *)(uintptr_t)XBOX_KOSLOAD_BASE = enabled ? 0xdeadbeef : 0xfeedface;
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}

static void xbox_set_rtc(uint32_t unix_timestamp) {
    xbox_clean_set_rtc(unix_timestamp);
}

static uint32_t xbox_get_rtc(void) {
    uint32_t now;

    /* Prefer the CMOS RTC so this is a real Unix timestamp per the
     * target_ops contract. */
    if(xbox_clean_get_rtc(&now) == 0)
        return now;

    /* Unreadable: fall back to seconds since loader start.  Every consumer
     * differences get_rtc (DHCP lease expiry), so a monotonic counter still
     * works — it just isn't wall-clock.  RDTSC runs across guest execution. */
    return (uint32_t)((read_tsc() - monotonic_epoch_ticks) / XBOX_CPU_HZ);
}

static uint64_t xbox_get_ticks(void) {
    return read_tsc();
}

static void xbox_restart_timer(void) {
    /* TSC is free-running; nothing to restart. */
}

static void xbox_fill_rect(int x, int y, int w, int h, uint32_t color) {
    xbox_video_fill_rect(x, y, w, h, color);
}

static void xbox_draw_bitmap(int x, int y, int w, int h, const uint32_t *bits, uint32_t color) {
    xbox_video_draw_bitmap(x, y, w, h, bits, color);
}

static uint32_t xbox_detect_ram_size(void) {
    /* Size of the mapped guest arena declared by kosload-ip.ld.in. */
    return 32u * 1024u * 1024u;
}

const target_ops_t xbox_target_ops = {
    .name = "Xbox",
    .default_load = XBOX_DEFAULT_LOAD_ADDR,
    .init = xbox_init,
    .draw_string = xbox_draw_string,
    .clear_screen = xbox_clear_screen,
    .setup_video = xbox_setup_video,
    .execute = xbox_execute,
    .disable_cache = xbox_disable_cache,
    .reboot = xbox_reboot,
    .cdfs_redir_save = NULL,
    .cdfs_redir_enable = NULL,
    .cdfs_redir_disable = NULL,
    .set_console_enabled = xbox_set_console_enabled,
    .set_rtc = xbox_set_rtc,
    .get_rtc = xbox_get_rtc,
    .get_ticks = xbox_get_ticks,
    .ticks_per_second = XBOX_CPU_HZ,
    .fill_rect = xbox_fill_rect,
    .draw_bitmap = xbox_draw_bitmap,
    .restart_timer = xbox_restart_timer,
    .detect_ram_size = xbox_detect_ram_size,
    .fw_update_prepare = NULL,
};

const target_ops_t *target_get_ops(void) {
    return &xbox_target_ops;
}
