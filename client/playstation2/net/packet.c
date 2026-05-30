/* client/playstation2/net/packet.c */
/*
 * Packet construction and checksum routines for PS2.
 * Based on dcload-ip: dcload-ip/target-src/dcload/commands.c
 * Uses standard memcpy (not SH4-specific memcpy_16bit).
 */

#include <string.h>
#include "packet.h"

// The two checksums here are different because the UDP one needs a
// "pseudo-header," while the IP one doesn't
unsigned short checksum(unsigned short *buf, int count, bool is_odd) {
    unsigned long sum = 0;

    while(count--) {
        sum += *buf++;
        if(sum & 0xffff0000) {
            sum &= 0xffff;
            sum++;
        }
    }

    if(is_odd) {
        sum += (unsigned short)(*((unsigned char *)buf));  // The sum is a little-endian sum, so an
                                                           // odd byte will be an 8-bit int

        if(sum & 0xffff0000) {
            sum &= 0xffff;
            sum++;
        }
    }

    return ~(sum & 0xffff);
}

// Pass odd as length%2 where datacount is length/2.
unsigned short checksum_udp(unsigned short *buf_pseudo, unsigned short *buf_data, int datacount,
                            bool is_odd) {
    unsigned long sum = 0;
    int pseudocount = PSEUDO_H_LEN/2;

    while(pseudocount--) {
        sum += *buf_pseudo++;
        if(sum & 0xffff0000) {
            sum &= 0xffff;
            sum++;
        }
    }

    while(datacount--) {
        sum += *buf_data++;
        if(sum & 0xffff0000) {
            sum &= 0xffff;
            sum++;
        }
    }

    if(is_odd) {
        sum += (unsigned short)(*((unsigned char *)buf_data));  // The sum is a little-endian sum, so
                                                                // an odd byte will be an 8-bit int

        if(sum & 0xffff0000) {
            sum &= 0xffff;
            sum++;
        }
    }

    return ~(sum & 0xffff);
}

void make_ether(unsigned char *dest, unsigned char *src, ether_header_t *ether) {
    memcpy(ether->dest, dest, 6);
    memcpy(ether->src, src, 6);
    ether->type[0] = 8;
    ether->type[1] = 0;
}

/* make_ip() / make_udp() are endian-clean (only htons/htonl, resolved
 * via this platform's packet.h shim) and live in the shared
 * client/common/network/packet_helpers.c, same as Dreamcast/GameCube.  Only
 * checksum/checksum_udp/make_ether stay target-local here (byte order
 * and hardware copy behavior differ per console). */

__attribute__((aligned(4))) unsigned char pseudo_array[PSEUDO_H_LEN];
