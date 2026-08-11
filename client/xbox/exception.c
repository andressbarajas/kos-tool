/* client/xbox/exception.c — x86 IDT setup + crash handler for the Xbox loader.
 *
 * IDT covering CPU vectors 0..19, with the asm stubs in exception.S.  On a
 * fault: draw the register dump to the framebuffer first (so a crash that also
 * kills the network still shows something), then send an x86_exception_frame_t
 * to the host via write(1, ...) as DC/GC/PS2 do.
 *
 * Then it spins re-asserting the display mode to keep the dump visible, so the
 * console does NOT return to the loader and needs a power cycle (HW-confirmed:
 * no ICMP afterwards).  GC/PS2 instead jump back to the loader entry; doing the
 * same here means re-entering from ISR context, which re-runs the kernel
 * takeover and identity relocation. */

#include <stdint.h>
#include <string.h>
#include <kosload/protocol.h>
#include <kosload/types.h>
#include "video.h"

/* Frame pushed by exception.S (isr_common): pusha regs, then the vector +
 * error code the stub pushed, then the CPU's fault frame. */
typedef struct {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; /* pusha order */
    uint32_t vector, errcode;
    uint32_t eip, cs, eflags;
} __attribute__((packed)) x86_frame_t;

/* 32-bit interrupt gate. */
typedef struct {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_hi;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

#define IDT_ENTRIES 20

static idt_entry_t idt[IDT_ENTRIES];

extern volatile uint32_t nvnet_dbg_cur_rx;
extern volatile uint32_t nvnet_dbg_cur_tx;
extern volatile uint32_t nvnet_dbg_bad_desc;
extern volatile uint32_t nvnet_dbg_tx_timeout;

/* ISR entry points from exception.S. */
extern void isr_0(void), isr_1(void), isr_2(void), isr_3(void), isr_4(void);
extern void isr_5(void), isr_6(void), isr_7(void), isr_8(void), isr_9(void);
extern void isr_10(void), isr_11(void), isr_12(void), isr_13(void), isr_14(void);
extern void isr_15(void), isr_16(void), isr_17(void), isr_18(void), isr_19(void);

static void (*const isr_table[IDT_ENTRIES])(void) = {
    isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7,  isr_8,  isr_9,
    isr_10, isr_11, isr_12, isr_13, isr_14, isr_15, isr_16, isr_17, isr_18, isr_19,
};

/* Loader syscall path back to the host (client/common/network). */
extern int  write(int fd, const void *buf, unsigned int count);
extern void progexit(int ret_code);

/* ESP must land in mapped memory before the handler dereferences it: the loader
 * window plus the guest arena, not physical RAM (which varies by console). */
#define XBOX_MAPPED_BOTTOM 0x00001000u
#define XBOX_MAPPED_TOP    ((uint32_t)XBOX_GUEST_ARENA_TOP)

/* A fault inside the reporting path must not re-enter write() — that turns a
 * debuggable crash into a hang.  Second entry only draws. */
static volatile int in_exception;

/* Read a stack word if `addr` is a dword-aligned address inside RAM. */
static uint32_t safe_read32(uint32_t addr) {
    if(addr < XBOX_MAPPED_BOTTOM || addr > XBOX_MAPPED_TOP - 4u || (addr & 3u))
        return 0;
    return *(const volatile uint32_t *)(uintptr_t)addr;
}

/* Pack the fault state into the wire frame and hand it to the host. */
static void report_exception(const x86_frame_t *f, uint32_t esp, uint32_t ds, uint32_t es,
                             uint32_t ss, uint32_t fs, uint32_t gs, uint32_t cr0, uint32_t cr2,
                             uint32_t cr3, uint32_t cr4) {
    x86_exception_frame_t frame;
    int i;

    memset(&frame, 0, sizeof(frame));
    frame.id[0] = 'E';
    frame.id[1] = 'X';
    frame.id[2] = 'P';
    frame.id[3] = 'T';
    frame.expt_code = f->vector;
    frame.errcode   = f->errcode;
    frame.eip       = f->eip;
    frame.eflags    = f->eflags;
    frame.eax       = f->eax;
    frame.ebx       = f->ebx;
    frame.ecx       = f->ecx;
    frame.edx       = f->edx;
    frame.esi       = f->esi;
    frame.edi       = f->edi;
    frame.ebp       = f->ebp;
    frame.esp       = esp;
    frame.cs        = f->cs;
    frame.ds        = ds;
    frame.es        = es;
    frame.ss        = ss;
    frame.fs        = fs;
    frame.gs        = gs;
    frame.cr0       = cr0;
    frame.cr2       = cr2;
    frame.cr3       = cr3;
    frame.cr4       = cr4;

    /* Stack sample for the host-side backtrace scan; unreadable slots stay 0. */
    for(i = 0; i < XBOX_EXC_STACK_WORDS; i++)
        frame.stack[i] = safe_read32(esp + (uint32_t)i * 4u);

    write(1, &frame, sizeof(frame));

    /* As GC/PS2: KOS installs its own vectors, so this only fires for
     * bare-metal guests and loader faults. */
    progexit(0);
}

static void hex_word(char *dst, uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    for(int i = 0; i < 8; i++)
        dst[i] = h[(v >> ((7 - i) * 4)) & 0xf];
}

void exception_handler_c(x86_frame_t *f) {
    char line[64];
    uint32_t cr0, cr2, cr3, cr4;
    uint32_t ds, es, ss, fs, gs;
    uint32_t stack0 = 0xBAD0BAD0;
    uint32_t stack1 = 0xBAD1BAD1;
    /* pusha's saved ESP is taken after the CPU frame and vector/errcode were
     * pushed; the interrupted code's ESP is 5 dwords above it (no privilege
     * change, so the CPU pushed no SS:ESP). */
    uint32_t esp = f->esp_dummy + 20;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    __asm__ volatile("mov %%es, %0" : "=r"(es));
    __asm__ volatile("mov %%ss, %0" : "=r"(ss));
    __asm__ volatile("mov %%fs, %0" : "=r"(fs));
    __asm__ volatile("mov %%gs, %0" : "=r"(gs));
    if(esp >= 0x00010000 &&
       esp <= 0x03FFFFF8) {
        const uint32_t *fault_stack = (const uint32_t *)esp;
        stack0 = fault_stack[0];
        stack1 = fault_stack[1];
    }

    setup_video(0, 0);
    xbox_video_clear(0x00200000); /* dark red crash background */

    /* "EXCEPTION: <name>" */
    {
        const char *name = exception_code_to_string(f->vector);
        int i = 0;
        const char *pre = "EXCEPTION: ";
        while(pre[i]) { line[i] = pre[i]; i++; }
        int j = 0;
        while(name[j] && i < 60) line[i++] = name[j++];
        line[i] = '\0';
        draw_string(30, 40, line, 0x00FFFFFF);
    }

    /* "VEC=xx ERR=xxxxxxxx EIP=xxxxxxxx EFL=xxxxxxxx" */
    {
        const char *tmpl = "VEC=........ ERR=........ EIP=........ EFL=........";
        int n = 0;
        while(tmpl[n]) { line[n] = tmpl[n]; n++; }
        line[n] = '\0';
        hex_word(line + 4,  f->vector);
        hex_word(line + 17, f->errcode);
        hex_word(line + 30, f->eip);
        hex_word(line + 43, f->eflags);
        draw_string(30, 70, line, 0x00FFFFFF);
    }

    {
        const char *tmpl = "EAX=........ EBX=........ ECX=........ EDX=........";
        int n = 0;
        while(tmpl[n]) { line[n] = tmpl[n]; n++; }
        line[n] = '\0';
        hex_word(line + 4,  f->eax);
        hex_word(line + 17, f->ebx);
        hex_word(line + 30, f->ecx);
        hex_word(line + 43, f->edx);
        draw_string(30, 85, line, 0x00FFFFFF);
    }

    {
        const char *tmpl = "ESI=........ EDI=........ EBP=........ ESP=........";
        int n = 0;
        while(tmpl[n]) { line[n] = tmpl[n]; n++; }
        line[n] = '\0';
        hex_word(line + 4,  f->esi);
        hex_word(line + 17, f->edi);
        hex_word(line + 30, f->ebp);
        hex_word(line + 43, esp);
        draw_string(30, 100, line, 0x00FFFFFF);
    }

    {
        const char *tmpl = "CS=........ DS=........ ES=........ SS=........";
        int n = 0;
        while(tmpl[n]) { line[n] = tmpl[n]; n++; }
        line[n] = '\0';
        hex_word(line + 3,  f->cs);
        hex_word(line + 15, ds);
        hex_word(line + 27, es);
        hex_word(line + 39, ss);
        draw_string(30, 115, line, 0x00FFFFFF);
    }

    {
        const char *tmpl = "FS=........ GS=........ CR0=........ CR4=........";
        int n = 0;
        while(tmpl[n]) { line[n] = tmpl[n]; n++; }
        line[n] = '\0';
        hex_word(line + 3,  fs);
        hex_word(line + 15, gs);
        hex_word(line + 28, cr0);
        hex_word(line + 41, cr4);
        draw_string(30, 130, line, 0x00FFFFFF);
    }

    /* Include the first two saved stack words: an EIP in non-code memory is
     * usually a corrupted return, and these distinguish that from DMA damage. */
    {
        const char *tmpl = "CR2=........ CR3=........ S0=........ S1=........";
        int n = 0;
        while(tmpl[n]) { line[n] = tmpl[n]; n++; }
        line[n] = '\0';
        hex_word(line + 4,  cr2);
        hex_word(line + 17, cr3);
        hex_word(line + 29, stack0);
        hex_word(line + 41, stack1);
        draw_string(30, 145, line, 0x00FFFFFF);
    }

    {
        const char *tmpl = "RX=........ TX=........ BAD=........ TO=........";
        int n = 0;
        while(tmpl[n]) { line[n] = tmpl[n]; n++; }
        line[n] = '\0';
        hex_word(line + 3,  nvnet_dbg_cur_rx);
        hex_word(line + 15, nvnet_dbg_cur_tx);
        hex_word(line + 28, nvnet_dbg_bad_desc);
        hex_word(line + 40, nvnet_dbg_tx_timeout);
        draw_string(30, 160, line, 0x00FFFFFF);
    }

    /* Screen dump is up; now hand the host a symbolizable frame. */
    if(!in_exception) {
        in_exception = 1;
        report_exception(f, esp, ds, es, ss, fs, gs, cr0, cr2, cr3, cr4);
    }

    /* Keep the crash screen up: the kernel tears the display mode down after
     * handoff, so re-assert the mode each iteration instead of halting.  The
     * register dump was drawn above and persists (the mode-set does not touch
     * the framebuffer). */
    for(;;)
        xbox_video_kick();
}

void exception_init(void) {
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));

    for(int i = 0; i < IDT_ENTRIES; i++) {
        uint32_t handler = (uint32_t)isr_table[i];
        idt[i].offset_lo = handler & 0xFFFF;
        idt[i].selector = cs;
        idt[i].zero = 0;
        idt[i].type_attr = 0x8E; /* present, ring 0, 32-bit interrupt gate */
        idt[i].offset_hi = (handler >> 16) & 0xFFFF;
    }

    idt_ptr_t idtr = {
        .limit = sizeof(idt) - 1,
        .base = (uint32_t)idt,
    };
    __asm__ volatile("lidt %0" ::"m"(idtr));
}
