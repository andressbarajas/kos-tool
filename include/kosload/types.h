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

/* Dirent offset used by KOS (anything under 100 is treated as invalid) */
#define DIRENT_OFFSET   1337

/* Max open directories for fileserver */
#define MAX_OPEN_DIRS   512

#endif /* KOSLOAD_TYPES_H */
