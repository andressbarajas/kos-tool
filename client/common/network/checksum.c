/* client/common/network/checksum.c
 *
 * The internet checksum (RFC 1071), shared by every IP port: the word loop is
 * byte-order independent (sec. 2B), and the odd trailing byte is folded in by
 * building its zero-padded word and reading that back natively, so one
 * implementation serves both byte orders.
 *
 * Buffers must be 16-bit aligned -- there is deliberately no unaligned-start
 * path, since peeling a leading byte leaves the sum byte-swapped.
 */

#include "packet.h"

/* Fold the carries out of a 32-bit accumulator into 16 bits. */
static inline unsigned short fold(unsigned int sum) {
    while(sum >> 16)
        sum = (sum >> 16) + (sum & 0xffff);

    return (unsigned short)~sum;
}

/*
 * Add the final odd byte of a payload.
 *
 * It is the first half of a zero-padded 16-bit word.  Build that word and read
 * it back the way the machine reads every other word: it lands in the high
 * half on a big-endian target and the low half on a little-endian one, which
 * is exactly what the checksum definition asks for in each case.
 */
static inline unsigned int odd_byte(const unsigned short *buf) {
    unsigned short pad = 0;

    *(unsigned char *)&pad = *(const unsigned char *)buf;

    return pad;
}

unsigned short checksum(unsigned short *buf, int count, bool is_odd) {
    unsigned int sum = 0;

    while(count--)
        sum += *buf++;

    if(is_odd)
        sum += odd_byte(buf);

    return fold(sum);
}

/*
 * The UDP checksum covers a pseudo-header as well as the payload, so it sums
 * two buffers.  Pass is_odd as length % 2, where datacount is length / 2.
 */
unsigned short checksum_udp(unsigned short *buf_pseudo, unsigned short *buf_data, int datacount,
                            bool is_odd) {
    unsigned int sum = 0;
    int pseudocount = PSEUDO_H_LEN / 2;

    while(pseudocount--)
        sum += *buf_pseudo++;

    while(datacount--)
        sum += *buf_data++;

    if(is_odd)
        sum += odd_byte(buf_data);

    return fold(sum);
}
