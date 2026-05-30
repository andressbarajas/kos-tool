/* client/playstation2/bootstrap_main.c
 *
 * Small outer program that launches the real PS2 loader.
 *
 * Why this exists: PS2 launchers reliably load an ELF at 0x00100000, but
 * kosload wants the real loader to run lower in RAM at 0x80000280.  This
 * program is the bridge.  It embeds the real loader as raw bytes, copies
 * those bytes to the lower address, flushes CPU caches, then jumps there.
 *
 * It does not use syscalls.
 */

#include <stdint.h>
#include <stddef.h>

#include "cache.h"

extern void exception_init(void);

/* Required only so kosload_common.a links.  Interrupts stay disabled while
 * this bootstrap runs, so this function is never expected to execute. */
void interrupt_handler_c(void) {
}

#define STUB_INT(name)                                                                                       \
    int name(void);                                                                                          \
    int name(void) {                                                                                         \
        return -1;                                                                                           \
    }
STUB_INT(read)
STUB_INT(write)
STUB_INT(open)
STUB_INT(close)
STUB_INT(creat)
STUB_INT(link)
STUB_INT(unlink)
STUB_INT(chdir)
STUB_INT(chmod)
STUB_INT(lseek)
STUB_INT(fstat)
STUB_INT(time)
STUB_INT(stat)
STUB_INT(utime)
STUB_INT(assign_wrkmem)
STUB_INT(opendir)
STUB_INT(closedir)
STUB_INT(readdir)
STUB_INT(gethostinfo)
STUB_INT(gdbpacket)
STUB_INT(mkdir)
STUB_INT(rewinddir)

__attribute__((noreturn)) void progexit(int status);
__attribute__((noreturn)) void progexit(int status) {
    (void)status;
    for(;;) {
    }
}

/* Embedded inner-loader blob.  objcopy creates these symbols when
 * wrapping ps2-load-ip-image.bin into an .o. */
extern uint8_t _binary_ps2_load_ip_image_bin_start[];
extern uint8_t _binary_ps2_load_ip_image_bin_end[];

/* Two ways to address the same RAM:
 *   KSEG1 writes bypass the data cache, so the copy reaches RAM directly.
 *   KSEG0 executes through the normal cached instruction path. */
#define DEST_KSEG0 ((uint32_t)PS2_LOADER_BASE)
#define DEST_KSEG1 (((uint32_t)PS2_LOADER_BASE & 0x1FFFFFFF) | 0xA0000000)

void main(void) {
    uint32_t blob_size = (uint32_t)(_binary_ps2_load_ip_image_bin_end - _binary_ps2_load_ip_image_bin_start);
    uint32_t blob_words = blob_size / 4;
    volatile uint32_t *dst = (volatile uint32_t *)DEST_KSEG1;
    uint32_t *src = (uint32_t *)_binary_ps2_load_ip_image_bin_start;
    uint32_t addr;
    uint32_t i;

    /* Copy the embedded real loader to its runtime address. */
    for(i = 0; i < blob_words; i++)
        dst[i] = src[i];
    __asm__ volatile("sync.l" ::: "memory");

    /* Clear stale data-cache lines left by the launcher.  Otherwise an old
     * dirty line could write back later and overwrite part of the copy. */
    arch_dcache_purge_all();

    /* Clear stale instruction-cache lines before executing the copied code. */
    __asm__ volatile("sync.p" ::: "memory");
    for(addr = 0x80000000; addr < 0x80002000; addr += 64) {
        __asm__ volatile(".set push\n"
                         ".set noreorder\n"
                         "cache 0x07, 0(%0)\n" /* IXIN way 0 */
                         "cache 0x07, 1(%0)\n" /* IXIN way 1 */
                         ".set pop\n" ::"r"(addr)
                         : "memory");
    }
    __asm__ volatile("sync.p" ::: "memory");

    /* Jump with ERET rather than a plain JR.  The BIOS uses the same style
     * for ExecPS2 because it cleanly restarts instruction fetch at ErrorEPC. */
    __asm__ volatile(".set push\n"
                     ".set noreorder\n"
                     "mtc0   %0, $30\n" /* ErrorEPC <- target */
                     "sync.p\n"
                     "mfc0   $t0, $12\n"      /* read Status */
                     "ori    $t0, $t0, 0x4\n" /* set ERL (bit 2) */
                     "mtc0   $t0, $12\n"      /* Status.ERL = 1 */
                     "sync.p\n"
                     "eret\n"
                     "nop\n"
                     ".set pop\n" ::"r"((uint32_t)DEST_KSEG0)
                     : "$t0", "memory");

    /* Unreachable. */
    for(;;) {
    }
}
