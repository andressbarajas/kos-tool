/* client/dreamcast/net/packet.c */
/*
 * Based on dcload-ip: dcload-ip/target-src/dcload/commands.c
 */

#include "packet.h"
#include "memfuncs.h"

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
    memcpy_16bit(ether->dest, dest, 6 / 2);
    memcpy_16bit(ether->src, src, 6 / 2);
    ether->type[0] = 8;
    ether->type[1] = 0;
}

__attribute__((aligned(4))) unsigned char pseudo_array[PSEUDO_H_LEN];  // Here's a global array (not really
                                                                       // global, but... search terms)
