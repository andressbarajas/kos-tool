/* client/xbox/net/packet.c
 *
 * Packet construction and checksum routines for Xbox (x86, little-endian).
 * Byte-order-sensitive helpers stay target-local; make_ip/make_udp are the
 * shared endian-clean copies in client/common/network/packet_helpers.c.
 * Same algorithm as the PS2 port (both little-endian, standard memcpy).
 */

#include <string.h>
#include "packet.h"

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
        sum += (unsigned short)(*((unsigned char *)buf));
        if(sum & 0xffff0000) {
            sum &= 0xffff;
            sum++;
        }
    }

    return ~(sum & 0xffff);
}

unsigned short checksum_udp(unsigned short *buf_pseudo, unsigned short *buf_data, int datacount,
                            bool is_odd) {
    unsigned long sum = 0;
    int pseudocount = PSEUDO_H_LEN / 2;

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
        sum += (unsigned short)(*((unsigned char *)buf_data));
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

__attribute__((aligned(4))) unsigned char pseudo_array[PSEUDO_H_LEN];
