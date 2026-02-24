/* client/dreamcast/maple.c */
/*
 * Simple Maple Bus implementation for Dreamcast.
 *
 * Copied from dcload-ip: dcload-ip/target-src/dcload/maple.c
 *
 * Designed for simplicity, not efficiency. For good performance,
 * requests should be parallelized and interrupts used for DMA completion.
 */

#include "maple.h"
#include "memfuncs.h"

#define MAPLE(x) (*((volatile unsigned long *)(0xa05f6c00+(x))))

void maple_init(void) {
    MAPLE(0x8c) = 0x6155404f;
    MAPLE(0x10) = 0;
    /* Select 2Mbps bitrate, timeout of 50000 */
    MAPLE(0x80) = (50000 << 16) | 0;
    /* Enable bus */
    MAPLE(0x14) = 1;
}

void maple_wait_dma(void) {
    while (MAPLE(0x18) & 1)
        ;
}

__attribute__((aligned(32))) volatile unsigned char dmabuffer[MAPLE_DMA_SIZE];

void *maple_docmd(int port, int unit, int cmd, int datalen, void *data) {
    unsigned int *sendbuf, *recvbuf;
    int to, from;

    port &= 3;

    from = port << 6;
    to = (port << 6) | (unit > 0 ? ((1 << (unit - 1)) & 0x1f) : 0x20);

    if (datalen > 255)
        datalen = 255;
    else if (datalen < 0)
        datalen = 0;

    /* Allocate receive buffer at start of dmabuffer, uncached */
    recvbuf = (unsigned int *)((unsigned int)dmabuffer | 0xa0000000);
    /* Send buffer right after receive buffer */
    sendbuf = (unsigned int *)((unsigned int)recvbuf + 1024);

    maple_wait_dma();

    MAPLE(0x04) = (unsigned int)sendbuf & 0x0fffffff;

    /* Control word: port, length, last-message flag */
    *sendbuf++ = datalen | (port << 16) | 0x80000000;
    /* Response address */
    *sendbuf++ = ((unsigned int)recvbuf & 0x0fffffff);
    /* Frame header (big-endian Maple format) */
    *sendbuf++ = (cmd & 0xff) | (to << 8) | (from << 16) | (datalen << 24);

    if (datalen > 0) {
        memcpy_32bit(sendbuf, data, 4 / 4);
        SH4_aligned_memcpy(to_p1((void *)sendbuf + 4),
                           to_p1((void *)data + 4), (datalen - 1) * 4);
        CacheBlockWriteBack(to_p1((void *)((unsigned int)sendbuf & ~0x1f)),
                            ((datalen * 4) + 31) / 32);
    }

    MAPLE(0x18) = 1;
    maple_wait_dma();

    return recvbuf;
}
