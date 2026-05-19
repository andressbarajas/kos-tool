/* client/playstation2/net/packet.h */
#ifndef __PACKET_H__
#define __PACKET_H__

/* Based on dcload-ip: dcload-ip/target-src/dcload/commands.h.
 * PS2 (R5900) is little-endian like the Dreamcast, so the byte-order
 * macros are byteswaps.  All shared declarations (header-length
 * macros, ip_udp_pseudo_header_t, checksum/make_* prototypes,
 * pseudo_array) come from <kosload/packet.h>; only this endian shim is
 * target-local. */

#include "bswap.h"

#define ntohl bswap32
#define htonl bswap32
#define ntohs bswap16
#define htons bswap16

#include <kosload/packet.h>

#endif /* __PACKET_H__ */
