/* client/xbox/net/packet.h — endian shim for the Xbox network stack.
 *
 * x86 is little-endian like the Dreamcast/PS2, so htonl/htons are byteswaps.
 * All shared declarations (header-length macros, ip_udp_pseudo_header_t,
 * checksum/make_* prototypes, pseudo_array) come from <kosload/packet.h>.
 */
#ifndef __PACKET_H__
#define __PACKET_H__

#include "bswap.h"

#define ntohl bswap32
#define htonl bswap32
#define ntohs bswap16
#define htons bswap16

#include <kosload/packet.h>

#endif /* __PACKET_H__ */
