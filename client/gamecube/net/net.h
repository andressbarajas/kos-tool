/* client/gamecube/net/net.h */
/*
 * Network stack definitions for GameCube.
 * Adapted from DC version.
 */

#ifndef __NET_H__
#define __NET_H__

/* Raw transmit buffer array size.
 * 1536 is the nearest multiple of 32 >= 1514, safe for cache operations. */
#define RAW_TX_PKT_BUF_SIZE 1536

/* Transmit buffer size (max Ethernet frame without CRC) */
#define TX_PKT_BUF_SIZE 1514

/* UDP Protocol Identifier */
#define IP_UDP_PROTOCOL 17

/* ICMP Protocol Identifier */
#define IP_ICMP_PROTOCOL 1

/* Main packet processing entry point */
void process_pkt(unsigned char *pkt);

extern const unsigned char broadcast[6]; /* Used in DHCP code */

extern __attribute__((aligned(32))) unsigned char raw_pkt_buf[RAW_TX_PKT_BUF_SIZE];
extern __attribute__((aligned(2))) unsigned char *pkt_buf;

/* Defined in commands.c, not net.c */
extern __attribute__((aligned(4))) volatile unsigned int our_ip;

#endif /* __NET_H__ */
