/* client/dreamcast/net/packet.h */
#ifndef __PACKET_H__
#define __PACKET_H__

/* Based on dcload-ip: dcload-ip/target-src/dcload/commands.h */

#include "bswap.h"

#define ntohl bswap32
#define htonl bswap32
#define ntohs bswap16
#define htons bswap16

#include <kosload/packet.h>

#endif
