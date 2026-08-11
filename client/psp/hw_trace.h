/* Cache-line-isolated hardware-init trace shared by USB and the NMI screen. */
#ifndef KOSLOAD_PSP_HW_TRACE_H
#define KOSLOAD_PSP_HW_TRACE_H

#include <stdint.h>

struct psp_hw_trace {
    uint32_t step;       /* bit31: operation begun; clear: trailing sync done */
    uint32_t addr;       /* exact MMIO/object address */
    uint32_t value;      /* intended store or completed load value */
    uint32_t nmi_pre;    /* BC100000 before USB init */
    uint32_t post_gate_4c; /* canonical reset readback after bring-up */
    uint32_t post_gate_50; /* canonical bus-clock readback after bring-up */
    uint32_t post_gate_54; /* canonical functional-clock readback */
    uint32_t post_gate_78; /* canonical USB I/O-enable readback */
    uint32_t post_aux_74;  /* retail USB PHY/internal-I/O enable readback */
    uint32_t usb_80_preclear;   /* sysreg USB causes inherited at takeover */
    uint32_t usb_80_after_clear;/* causes after reset-held startup acquire */
    uint32_t usb_80_after_ack;  /* causes after reset-ready acquire */
};

extern __attribute__((aligned(64))) struct psp_hw_trace psp_hw_trace;

static inline volatile struct psp_hw_trace *psp_hw_trace_uncached(void) {
    uintptr_t p = (uintptr_t)&psp_hw_trace;
    return (volatile struct psp_hw_trace *)((p & 0x1FFFFFFFu) | 0x40000000u);
}

#endif /* KOSLOAD_PSP_HW_TRACE_H */
