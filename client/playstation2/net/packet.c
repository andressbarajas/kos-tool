/* client/playstation2/net/packet.c */
/*
 * Packet construction helpers for PS2.
 * Based on dcload-ip: dcload-ip/target-src/dcload/commands.c
 *
 * Only the Ethernet header copy stays target-local.  The checksums live in
 * the shared client/common/network/checksum.c, and make_ip/make_udp in
 * client/common/network/packet_helpers.c.
 */

#include <string.h>
#include "packet.h"

void make_ether(unsigned char *dest, unsigned char *src, ether_header_t *ether) {
    memcpy(ether->dest, dest, 6);
    memcpy(ether->src, src, 6);
    ether->type[0] = 8;
    ether->type[1] = 0;
}

__attribute__((aligned(4))) unsigned char pseudo_array[PSEUDO_H_LEN];
