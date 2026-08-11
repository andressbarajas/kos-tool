/* include/kosload/types.h */
#ifndef KOSLOAD_TYPES_H
#define KOSLOAD_TYPES_H

#include <stdint.h>

/*
 * Shared types used by both host and client.
 * These match the dcload on-wire format expected by KallistiOS.
 */

/* dcload stat structure (matches KOS expectations) */
typedef struct {
    uint16_t st_dev;
    uint16_t st_ino;
    int32_t  st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint16_t st_rdev;
    int32_t  st_size;
    int32_t  st_atime_val;
    int32_t  st_spare1;
    int32_t  st_mtime_val;
    int32_t  st_spare2;
    int32_t  st_ctime_val;
    int32_t  st_spare3;
    int32_t  st_blksize;
    int32_t  st_blocks;
    int32_t  st_spare4[2];
} kosload_stat_t;

/* dcload dirent structure (matches KOS expectations) */
typedef struct {
    uint32_t d_ino;
    int32_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} kosload_dirent_t;

/* SH4 exception frame (for exception reporting) */
typedef struct {
    uint8_t  id[4]; /* "EXPT" */
    uint32_t expt_code;
    uint32_t pc;
    uint32_t pr;
    uint32_t sr;
    uint32_t gbr;
    uint32_t vbr;
    uint32_t dbr;
    uint32_t mach;
    uint32_t macl;
    uint32_t r0b0, r1b0, r2b0, r3b0, r4b0, r5b0, r6b0, r7b0;
    uint32_t r0b1, r1b1, r2b1, r3b1, r4b1, r5b1, r6b1, r7b1;
    uint32_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint32_t fpscr;
    uint32_t fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7;
    uint32_t fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15;
    uint32_t fpul;
    uint32_t xf0, xf1, xf2, xf3, xf4, xf5, xf6, xf7;
    uint32_t xf8, xf9, xf10, xf11, xf12, xf13, xf14, xf15;
} __attribute__((packed)) sh4_exception_frame_t;

/* PPC 750 (Gekko) exception frame (for exception reporting).
 * Layout: 4-byte "EXPT" header + 428-byte exc_save_area from exception.S. */
typedef struct {
    uint8_t  id[4];     /* "EXPT" */
    uint32_t expt_code; /* Exception vector offset (e.g. 0x0300 = DSI) */
    uint32_t srr0;      /* PC at exception */
    uint32_t srr1;      /* Saved MSR */
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
    uint32_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint32_t r16, r17, r18, r19, r20, r21, r22, r23;
    uint32_t r24, r25, r26, r27, r28, r29, r30, r31;
    uint32_t lr;
    uint32_t ctr;
    uint32_t xer;
    uint32_t cr;
    uint32_t dsisr;
    uint32_t dar;
    /* FPU state (double-precision, 8 bytes each via stfd) */
    uint32_t fpscr_hi, fpscr_lo; /* mffs result (hi reserved, lo = FPSCR) */
    uint32_t f0_hi, f0_lo, f1_hi, f1_lo, f2_hi, f2_lo, f3_hi, f3_lo;
    uint32_t f4_hi, f4_lo, f5_hi, f5_lo, f6_hi, f6_lo, f7_hi, f7_lo;
    uint32_t f8_hi, f8_lo, f9_hi, f9_lo, f10_hi, f10_lo, f11_hi, f11_lo;
    uint32_t f12_hi, f12_lo, f13_hi, f13_lo, f14_hi, f14_lo, f15_hi, f15_lo;
    uint32_t f16_hi, f16_lo, f17_hi, f17_lo, f18_hi, f18_lo, f19_hi, f19_lo;
    uint32_t f20_hi, f20_lo, f21_hi, f21_lo, f22_hi, f22_lo, f23_hi, f23_lo;
    uint32_t f24_hi, f24_lo, f25_hi, f25_lo, f26_hi, f26_lo, f27_hi, f27_lo;
    uint32_t f28_hi, f28_lo, f29_hi, f29_lo, f30_hi, f30_lo, f31_hi, f31_lo;
} __attribute__((packed)) gc_exception_frame_t;

/* PS2 R5900 (EE) exception frame (for exception reporting).
 * Layout: 4-byte "EXPT" header + 416-byte exception_save_area from
 * client/playstation2/exception.S.  Unlike DC/GC there is no separate
 * expt_code field — the exception class is derived from COP0 Cause.ExcCode.
 * GPRs are saved 64-bit (sd); the host decodes the low 32 bits.  On the
 * little-endian EE the low half is the first word of each 8-byte slot. */
typedef struct {
    uint8_t  id[4];      /* "EXPT" */
    uint32_t gpr[32][2]; /* r0-r31, [0] = low 32 bits, [1] = high 32 bits */
    uint32_t epc;        /* COP0 EPC ($14) */
    uint32_t status;     /* COP0 Status ($12) */
    uint32_t cause;      /* COP0 Cause ($13) */
    uint32_t badvaddr;   /* COP0 BadVAddr ($8) */
    uint32_t fpr[32];    /* FPU f0-f31 (32-bit via swc1) */
    uint32_t fcr31;      /* FPU control/status register */
    uint32_t pad[3];     /* padding to the 416-byte save area */
} __attribute__((packed)) ps2_exception_frame_t;

/* PSP Allegrex exception frame (for exception reporting).
 * Layout: 4-byte "EXPT" header + the 176-byte psp_exc_frame from
 * client/psp/exception.S.  Like the PS2 there is no separate expt_code — the
 * exception class comes from COP0 Cause.ExcCode — but unlike the PS2 the GPRs
 * are 32-bit (the Allegrex has no 64-bit register file) and the tail carries
 * the NMI-only evidence the Allegrex crash path collects, which is zero for an
 * ordinary fault. */
typedef struct {
    uint8_t  id[4];      /* "EXPT" */
    uint32_t gpr[32];    /* r0-r31; k0/k1 are the handler's, not the faulter's */
    uint32_t cause;      /* COP0 Cause ($13) */
    uint32_t epc;        /* COP0 EPC ($14) */
    uint32_t status;     /* COP0 Status ($12) */
    uint32_t badvaddr;   /* COP0 BadVAddr ($8) */
    uint32_t is_nmi;     /* 1 if this came in through the NMI vector */
    uint32_t errorepc;   /* COP0 ErrorEPC ($30) — authoritative NMI PC */
    uint32_t nmi_mask;   /* sysreg 0xBC100000 — NMI source bitmap */
    uint32_t nmi_flags;  /* sysreg 0xBC100004 */
    uint32_t core_id;    /* COP0 $22 */
    uint32_t cop0_24;    /* COP0 $24 */
    uint32_t nmi_d14;    /* sysreg 0xBC100014 — source detail for index 9 */
    uint32_t usb_80;     /* sysreg 0xBC100080 — USB connect + cause bits */
} __attribute__((packed)) psp_exception_frame_t;

/* x86 (Pentium III) exception frame for the Xbox port.
 *
 * Assembled in C by client/xbox/exception.c, not dumped raw by the asm stub:
 * the x86 fault state is split across the CPU frame, pusha, and the segment
 * and control registers.
 *
 * The stack sample replaces an EBP walk: the loader is -fomit-frame-pointer and
 * the examples -O2 (same effect), so most functions leave EBP as a plain
 * callee-saved register and a walk would derail at the first such frame.  The
 * host scans these words instead — no unwind info needed. */
#define XBOX_EXC_STACK_WORDS 32

typedef struct {
    uint8_t  id[4];     /* "EXPT" */
    uint32_t expt_code; /* CPU exception vector (0..19) */
    uint32_t errcode;   /* CPU error code, 0 for vectors that push none */
    /* 20 registers; the order must match x86_register_names[] in the host. */
    uint32_t eip, eflags;
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    uint32_t cs, ds, es, ss, fs, gs;
    uint32_t cr0, cr2, cr3, cr4;
    /* Words read from the faulting stack starting at ESP.  Unreadable slots
     * are zero-filled (the client range-checks ESP before dereferencing). */
    uint32_t stack[XBOX_EXC_STACK_WORDS];
} __attribute__((packed)) x86_exception_frame_t;

/* Number of uint32 registers in x86_exception_frame_t's named block. */
#define XBOX_EXC_NUM_REGS 20
/* Byte offset of that block from the start of the frame. */
#define XBOX_EXC_REGS_OFFSET 12

/* Dirent offset used by KOS (anything under 100 is treated as invalid) */
#define DIRENT_OFFSET   1337

/* Max open directories for fileserver */
#define MAX_OPEN_DIRS   512

#endif /* KOSLOAD_TYPES_H */
