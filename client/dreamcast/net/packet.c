/* client/dreamcast/net/packet.c */
/*
 * Based on dcload-ip: dcload-ip/target-src/dcload/commands.c
 *
 * Only the Ethernet header copy stays target-local: it uses the SH4
 * 16-bit-at-a-time copy rather than memcpy.  The checksums live in the
 * shared client/common/network/checksum.c, and make_ip/make_udp in
 * client/common/network/packet_helpers.c.
 */

#include "packet.h"
#include "memfuncs.h"

void make_ether(unsigned char *dest, unsigned char *src, ether_header_t *ether) {
    memcpy_16bit(ether->dest, dest, 6 / 2);
    memcpy_16bit(ether->src, src, 6 / 2);
    ether->type[0] = 8;
    ether->type[1] = 0;
}

__attribute__((aligned(4))) unsigned char pseudo_array[PSEUDO_H_LEN];  // Here's a global array (not really
                                                                       // global, but... search terms)
