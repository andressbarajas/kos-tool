/* client/gamecube/net/packet.h */
/*
 * Packet header definitions and construction helpers for GameCube.
 * Adapted from DC version: ntohl/htonl are identity on big-endian PPC.
 */

#ifndef __PACKET_H__
#define __PACKET_H__

#include "bswap.h"

/* Big-endian: network byte order = host byte order, so these are identity */
#define ntohl(x) (x)
#define htonl(x) (x)
#define ntohs(x) (x)
#define htons(x) (x)

#include <kosload/packet.h>

#endif /* __PACKET_H__ */
