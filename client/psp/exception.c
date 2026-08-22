/* client/psp/exception.c — Allegrex exception ownership and crash screen.
 *
 * The Allegrex keeps exception handler addresses in COP0 registers rather than
 * at a fixed memory vector (RE'd from 6.61 exceptionman.prx):
 *
 *   COP0 $25         general exception handler   (mtc0)
 *   COP0 control $9  NMI / reset handler         (ctc0), reached via the
 *                    64-byte stub the firmware leaves at 0xBFC00000
 *
 * Taking these over makes the firmware's kernel partition safe to reuse: that
 * region holds handler *bodies*, never the vectors themselves.  Reachability
 * is a separate Tachyon permission-table policy.
 *
 * In a reclaim build stage 1 has already opened that policy -- it has to, since
 * it copies this image into the reclaimed range (client/psp/stub/stub.S).  What
 * remains here is an idempotent re-verification that fails closed, plus the
 * part of the cache sweep stage 1 could not safely leave to itself.
 *
 * ORDER MATTERS.  Hardware interrupts are delivered through the *general*
 * exception vector, so installing $25 while firmware interrupts are still
 * enabled would route the next timer IRQ into a handler that has no idea how to
 * dispatch it.  Interrupts are therefore masked first.  That is safe here only
 * because this loader is fully polled and never calls a firmware service; it
 * would not be safe for an ordinary PSP program.
 */

#include <stdint.h>
#include <kosload/target.h>
#include <kosload/protocol.h>
#include <kosload/types.h>

#include "video.h"
#include "cache.h"
#include "hw_trace.h"

/* Must match the frame layout in exception.S. */
typedef struct {
    uint32_t gpr[32];
    uint32_t cause;
    uint32_t epc;
    uint32_t status;
    uint32_t badvaddr;
    uint32_t is_nmi;
    uint32_t errorepc;
    uint32_t nmi_mask;
    uint32_t nmi_flags;
    uint32_t core_id;
    uint32_t cop0_24;
    uint32_t nmi_d14;
    uint32_t usb_80;
} exc_frame_t;

extern exc_frame_t psp_exc_frame;
extern void psp_exception_entry(void);
extern void psp_nmi_entry(void);

/* The wire frame is the tag followed by the save area verbatim, so the two
 * layouts have to agree word for word or the host decodes garbage. */
_Static_assert(sizeof(psp_exception_frame_t) == 4 + sizeof(exc_frame_t),
               "psp_exception_frame_t does not match exception.S's psp_exc_frame");

/* From go.S: restores the loader context go() saved and returns into
 * psp_execute(), exactly as a program's own `jr ra` would. */
extern void go_return(void);

/* go.S's saved-context block.  Word 10 (byte offset 40) is the loader $sp,
 * which is zero until go() has run at least once — resuming through a context
 * that was never saved would jump to address zero. */
extern uint32_t go_save[12];
#define GO_SAVE_SP 10

/* Set by psp_execute() around go(), so the crash path can tell a fault inside
 * a loaded program (resumable — go() has a saved context) from a fault in
 * loader code itself (not resumable). */
extern volatile uint32_t psp_guest_running;

/* Crash frame staged here and shipped by psp_execute() once the loader context
 * is back.  Nothing is transmitted from the handler itself: USB I/O needs the
 * poll loop and a valid stack, neither of which the fault context can promise. */
uint8_t           psp_exc_wire[sizeof(psp_exception_frame_t)];
volatile uint32_t psp_exc_pending;

/* This owns a complete cache line.  exception_init() publishes its cached BSS
 * zeros once, after which both producers and the crash reader use KSEG1 only. */
__attribute__((aligned(64))) struct psp_hw_trace psp_hw_trace;

static void breadcrumb_access(uint32_t step, uint32_t addr, uint32_t value) {
    volatile struct psp_hw_trace *trace = psp_hw_trace_uncached();
    trace->addr = addr;
    trace->value = value;
    __asm__ volatile("sync" ::: "memory");
    trace->step = step;
    __asm__ volatile("sync" ::: "memory");
}

__attribute__((unused)) static void breadcrumb(uint32_t value) {
    breadcrumb_access(value, 0, 0);
}

static void hex_word(char *dst, uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    for(int i = 0; i < 8; i++)
        dst[i] = digits[(v >> ((7 - i) * 4)) & 0xF];
    dst[8] = '\0';
}

/* Append src to dst at *pos, bounded by lim.  Keeps the crash path free of any
 * dependency on string helpers that may themselves be what crashed. */
static void append(char *dst, int *pos, const char *src, int lim) {
    while(*src && *pos < lim - 1)
        dst[(*pos)++] = *src++;
    dst[*pos] = '\0';
}

static void line_pair(int row, const char *key1, uint32_t value1,
                      const char *key2, uint32_t value2) {
    char line[48];
    char hex[9];
    int p = 0;

    append(line, &p, key1, (int)sizeof(line));
    hex_word(hex, value1);
    append(line, &p, hex, (int)sizeof(line));
    append(line, &p, " ", (int)sizeof(line));
    append(line, &p, key2, (int)sizeof(line));
    hex_word(hex, value2);
    append(line, &p, hex, (int)sizeof(line));
    psp_video_draw_string(0, 4 + row * 24, line, 0x00FFFFFF);
}

#define PSP_MEMPROT_BASE       0xBC000000u
#define PSP_MEMPROT_WORDS      4u
#define PSP_MEMPROT_ALL_RW     0xFFFFFFFFu
#define PSP_SYSREG_RESET       0xBC10004Cu
#define PSP_SYSREG_RESET_ME    0x00000004u
#define PSP_LOW_RAM_BASE       0x08000000u
#define PSP_LOW_RAM_SIZE       0x00800000u

/* A reclaim build promises the caller a 32-MiB arena.  Continuing after a
 * failed reset or permission write would turn a clear startup error into a
 * later, address-dependent data bus error.  Video is already initialized. */
__attribute__((noreturn)) static void reclaim_failed(const char *reason,
                                                     uint32_t step,
                                                     uint32_t addr,
                                                     uint32_t expected,
                                                     uint32_t actual) {
    breadcrumb_access(step, addr, actual);
    psp_video_attach_existing();
    psp_video_clear(0x00000060u);
    psp_video_draw_string(0, 4, "LOW RAM RECLAIM FAILED", 0x00FFFFFFu);
    psp_video_draw_string(0, 28, reason, 0x00FFFFFFu);
    line_pair(2, "ADDR ", addr, "STEP ", step);
    line_pair(3, "WANT ", expected, "READ ", actual);
    for(;;)
        __asm__ volatile("");
}

/* Write back + invalidate one low-RAM interval through both of its cached
 * views.  step_id must be even: the K0 pass reports step_id | 1. */
static void purge_low_range(uint32_t start, uint32_t end, uint32_t step_id) {
    if(end <= start)
        return;

    breadcrumb_access(step_id, start, end - start);
    cache_flush_range((const void *)start, end - start);
    breadcrumb_access(step_id | 1u, start | 0x80000000u, end - start);
    cache_flush_range((const void *)(start | 0x80000000u), end - start);
}

/* Sweep the reclaimed low 8 MiB EXCEPT the window this loader occupies.
 *
 * Stage 1 already swept the complete 8 MiB, through both aliases, after its
 * protection readback and before it copied us down here -- the one moment the
 * whole range is provably ours and empty.  Repeating that over our own window
 * now would not be idempotent: this sweep runs after crt0 has set $sp and
 * cleared BSS, so our stack, exception frame, USB descriptors and USB buffers
 * are live dirty KU0 lines.  The KU0 pass writes them back correctly, but a
 * following K0 pass over the same physical addresses could write back a stale
 * firmware line on top of what we just published.  Skipping our own window
 * removes that ordering hazard entirely, and costs nothing: stage 1 already
 * invalidated it and nothing but us has touched it since.
 *
 * The clamps keep this correct for any loader window, including one outside
 * the low 8 MiB, rather than only for the configuration in mk/memory.mk. */
static void purge_low_ram_outside_loader(void) {
    const uint32_t low_start = PSP_LOW_RAM_BASE;
    const uint32_t low_end = PSP_LOW_RAM_BASE + PSP_LOW_RAM_SIZE;
    uint32_t loader_start = PSP_LOADER_BASE;
    uint32_t loader_end = (uint32_t)PSP_LOADER_BASE + PSP_LOADER_SIZE;

    if(loader_start < low_start)
        loader_start = low_start;
    if(loader_start > low_end)
        loader_start = low_end;
    if(loader_end < loader_start)
        loader_end = loader_start;
    if(loader_end > low_end)
        loader_end = low_end;

    purge_low_range(low_start, loader_start, 0xE100003Cu);
    purge_low_range(loader_end, low_end, 0xE100003Eu);
}

/* Re-assert and re-verify what stage 1 established, then finish the cache
 * cleanup outside our own window.
 *
 * Stage 1 (client/psp/stub/stub.S) must already have done all of this: it
 * copies this image into the reclaimed range, so it cannot wait for us.  Every
 * step here is idempotent and exists to fail closed if that never happened --
 * for instance in a build whose stage 1 was replaced, or if a guest returned
 * with the table rewritten.
 *
 * Clean-room provenance:
 * - 6.61 me_wrapper.prx text+0x1c30 uses the lowio reset calls whose NIDs map
 *   to reset mask 4, identifying BC10004C bit 2 as the Media Engine reset.
 * - 6.61 sysmem.prx text+0xa668 updates the four-bit permission nibbles at
 *   BC000000..BC00000C; power_01g.prx uses nibble F for user-visible volatile
 *   RAM.  The same four-F sequence and readback passed on PSP-1000 hardware,
 *   followed by a byte-perfect write/read/restore of the complete 8 MiB.
 */
static void reclaim_low_ram(void) {
    volatile uint32_t *const reset = (volatile uint32_t *)PSP_SYSREG_RESET;
    volatile uint32_t *const protection =
        (volatile uint32_t *)PSP_MEMPROT_BASE;
    uint32_t reset_value;

    breadcrumb_access(0xE1000010u, PSP_SYSREG_RESET, PSP_SYSREG_RESET_ME);
    reset_value = *reset | PSP_SYSREG_RESET_ME;
    *reset = reset_value;
    __asm__ volatile("sync" ::: "memory");
    reset_value = *reset;
    if(!(reset_value & PSP_SYSREG_RESET_ME))
        reclaim_failed("MEDIA ENGINE RESET", 0xE1FF0010u,
                       PSP_SYSREG_RESET, PSP_SYSREG_RESET_ME, reset_value);

    for(uint32_t i = 0; i < PSP_MEMPROT_WORDS; ++i) {
        uint32_t addr = PSP_MEMPROT_BASE + i * sizeof(uint32_t);
        breadcrumb_access(0xE1000020u + i, addr, PSP_MEMPROT_ALL_RW);
        protection[i] = PSP_MEMPROT_ALL_RW;
        __asm__ volatile("sync" ::: "memory");
    }

    for(uint32_t i = 0; i < PSP_MEMPROT_WORDS; ++i) {
        uint32_t addr = PSP_MEMPROT_BASE + i * sizeof(uint32_t);
        uint32_t value;
        breadcrumb_access(0xE1000030u + i, addr, PSP_MEMPROT_ALL_RW);
        value = protection[i];
        if(value != PSP_MEMPROT_ALL_RW)
            reclaim_failed("PROTECTION READBACK", 0xE1FF0030u + i,
                           addr, PSP_MEMPROT_ALL_RW, value);
    }

    /* Discard inherited KU0/K0 lines only after F readback.  A dirty firmware
     * line must be published now, not much later over guest data written
     * through KU1/K1 or DMA.  Our own window is deliberately excluded; see
     * purge_low_ram_outside_loader(). */
    purge_low_ram_outside_loader();
    __asm__ volatile("sync" ::: "memory");
    breadcrumb(0xE1000040u); /* ME held; low 8 MiB open, guest extents clean */
}

void psp_exception_handler_c(void) {
    const exc_frame_t *f = &psp_exc_frame;
    const volatile struct psp_hw_trace *trace = psp_hw_trace_uncached();
    uint32_t code = (f->cause >> 2) & 0x1F;
    uint32_t step = trace->step;
    uint32_t addr = trace->addr;
    uint32_t value = trace->value;
    uint32_t nmi_pre = trace->nmi_pre;
    uint32_t post_4c = trace->post_gate_4c;
    uint32_t post_50 = trace->post_gate_50;
    uint32_t post_54 = trace->post_gate_54;
    uint32_t post_78 = trace->post_gate_78;
    char line[64];
    int p = 0;

    /* Normal target init already established scanout before vector takeover.
     * Do not touch FIFO/LCDC/Syscon again while handling an NMI. */
    psp_video_attach_existing();
    psp_video_clear(0x00000060); /* dark red (ABGR) crash background */

    if(f->is_nmi) {
        psp_video_draw_string(0, 4, "LEVEL2/NMI - USB INIT HALT",
                              0x00FFFFFF);
        line_pair(1, "ERRPC ", f->errorepc, "RA ", f->gpr[31]);
        line_pair(2, "STEP ", step, "ADDR ", addr);
        line_pair(3, "VALUE ", value, "PRE ", nmi_pre);
        line_pair(4, "SRC0 ", f->nmi_mask, "RAW4 ", f->nmi_flags);
        line_pair(5, "R14 ", f->nmi_d14, "R80 ", f->usb_80);
        line_pair(6, "C0R24 ", f->cop0_24, "P4C ", post_4c);
        line_pair(7, "P50 ", post_50, "P54 ", post_54);
        line_pair(8, "P78 ", post_78, "CAUSE ", f->cause);
        line_pair(9, "STATUS ", f->status, "EPC ", f->epc);
        line_pair(10, "SP ", f->gpr[29], "GP ", f->gpr[28]);
    } else {
        append(line, &p, "EXCEPTION: ", (int)sizeof(line));
        append(line, &p, exception_code_to_string(code), (int)sizeof(line));
        psp_video_draw_string(0, 4, line, 0x00FFFFFF);
        line_pair(1, "EPC ", f->epc, "RA ", f->gpr[31]);
        line_pair(2, "CAUSE ", f->cause, "STATUS ", f->status);
        line_pair(3, "BAD ", f->badvaddr, "SP ", f->gpr[29]);
        line_pair(4, "STEP ", step, "ADDR ", addr);
        line_pair(5, "VALUE ", value, "CORE ", f->core_id);
    }

    /* Hand the crash to the host as an "EXPT" frame, the way DC/GC/Wii/PS2 do,
     * so kos-tool can print a symbolized register dump instead of the user
     * transcribing hex off the LCD.  The on-screen dump above stays as the
     * fallback for a cold boot with no host attached.
     *
     * Only a fault inside a loaded program is resumed.  An NMI is the USB-init
     * halt diagnostic and its evidence has to stay on screen; a fault in loader
     * code has no saved go() context to return through, and resuming into a
     * stale one would replace a legible crash with an illegible one.  Both keep
     * the historical spin.  The go_save check is the same belt-and-braces the
     * PS2 handler uses: psp_guest_running is set just before the call, so it is
     * briefly true while go() has not yet written its save block. */
    if(!f->is_nmi && psp_guest_running && go_save[GO_SAVE_SP] != 0) {
        const uint8_t *src = (const uint8_t *)f;
        uint32_t       i;

        psp_exc_wire[0] = 'E';
        psp_exc_wire[1] = 'X';
        psp_exc_wire[2] = 'P';
        psp_exc_wire[3] = 'T';
        for(i = 0; i < sizeof(exc_frame_t); i++)
            psp_exc_wire[4 + i] = src[i];
        psp_exc_pending = 1;

        /* Leave exception level before returning to ordinary code.  With EXL
         * still set the next fault would not record EPC/Cause, so a second
         * crash after this one would report the first one's PC. */
        {
            uint32_t status;
            __asm__ volatile("mfc0 %0, $12" : "=r"(status));
            status &= ~0x2u; /* Status.EXL */
            __asm__ volatile("mtc0 %0, $12\n nop\n nop" ::"r"(status) : "memory");
        }

        go_return();
    }

    for(;;)
        __asm__ volatile("");
}

/* Mask interrupts, take both exception vectors, and reclaim the low 8 MiB.
 *
 * After this returns no interrupt or exception can reach firmware handlers,
 * the ME is held in reset, and the protection-table readback has succeeded. */
void exception_init(void) {
    /* crt0 dirtied this BSS line through KSEG0.  Publish/invalidate it before
     * switching the trace permanently to its uncached alias. */
    cache_flush_range(&psp_hw_trace, sizeof(psp_hw_trace));
    uint32_t status;

    breadcrumb(0xE1000001u); /* entering exception takeover */

    /* 1. Mask interrupts before redirecting the general vector (see header). */
    __asm__ volatile("mfc0 %0, $12" : "=r"(status));
    status &= ~1u; /* Status.IE = 0 */
    __asm__ volatile("mtc0 %0, $12" ::"r"(status));
    __asm__ volatile("nop; nop");
    breadcrumb(0xE1000002u); /* IE masked */

    /* 2. General exceptions -> COP0 $25. */
    breadcrumb(0xE1000003u); /* before general-vector write */
    __asm__ volatile("mtc0 %0, $25" ::"r"(&psp_exception_entry));

    /* 3. NMI / reset -> COP0 control $9.  The 0xBFC00000 stub the firmware
     *    installed stays where it is (it lives at physical 0x1FC00000, outside
     *    main RAM, so reclaiming the kernel partition cannot disturb it); we
     *    only repoint the handler address it dispatches through.  Leaving $9
     *    alone would leave it aimed at exceptionman inside the reclaimed
     *    region. */
    breadcrumb(0xE1000004u); /* before NMI-vector write */
    __asm__ volatile("ctc0 %0, $9" ::"r"(&psp_nmi_entry));
    __asm__ volatile("nop; nop");
    breadcrumb(0xE1000005u); /* both exception vectors owned */

    /* The old vector bodies and volatile allocation are now unreachable by
     * firmware.  Re-assert the ME reset and the protection table stage 1
     * already established, and only then let startup continue with
     * PSP_RAM_BYTES=32 MiB.  We are ourselves executing out of the reclaimed
     * range by this point, so a failure here is reported, not survived. */
    reclaim_low_ram();
}
