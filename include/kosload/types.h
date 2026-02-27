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
} dcload_stat_t;

/* dcload dirent structure (matches KOS expectations) */
typedef struct {
    uint32_t d_ino;
    int32_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} dcload_dirent_t;

/* SH4 exception frame (for exception reporting) */
typedef struct {
    uint8_t  id[4];         /* "EXPT" */
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
    uint8_t  id[4];         /* "EXPT" */
    uint32_t expt_code;     /* Exception vector offset (e.g. 0x0300 = DSI) */
    uint32_t srr0;          /* PC at exception */
    uint32_t srr1;          /* Saved MSR */
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
    uint32_t fpscr_hi, fpscr_lo;  /* mffs result (hi reserved, lo = FPSCR) */
    uint32_t f0_hi,  f0_lo,  f1_hi,  f1_lo,  f2_hi,  f2_lo,  f3_hi,  f3_lo;
    uint32_t f4_hi,  f4_lo,  f5_hi,  f5_lo,  f6_hi,  f6_lo,  f7_hi,  f7_lo;
    uint32_t f8_hi,  f8_lo,  f9_hi,  f9_lo,  f10_hi, f10_lo, f11_hi, f11_lo;
    uint32_t f12_hi, f12_lo, f13_hi, f13_lo, f14_hi, f14_lo, f15_hi, f15_lo;
    uint32_t f16_hi, f16_lo, f17_hi, f17_lo, f18_hi, f18_lo, f19_hi, f19_lo;
    uint32_t f20_hi, f20_lo, f21_hi, f21_lo, f22_hi, f22_lo, f23_hi, f23_lo;
    uint32_t f24_hi, f24_lo, f25_hi, f25_lo, f26_hi, f26_lo, f27_hi, f27_lo;
    uint32_t f28_hi, f28_lo, f29_hi, f29_lo, f30_hi, f30_lo, f31_hi, f31_lo;
} __attribute__((packed)) gc_exception_frame_t;

/* Dirent offset used by KOS (anything under 100 is treated as invalid) */
#define DIRENT_OFFSET   1337

/* Max open directories for fileserver */
#define MAX_OPEN_DIRS   512

#endif /* KOSLOAD_TYPES_H */
