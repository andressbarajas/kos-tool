/* client/include/kosload/net_stack.h */
#ifndef KOSLOAD_NET_STACK_H
#define KOSLOAD_NET_STACK_H

/*
 * Shared network stack declarations.
 *
 * TX_PKT_BUF_SIZE is the usable Ethernet frame size without CRC. The raw
 * transmit buffer is larger so packet data, over-read behavior, and cache
 * maintenance stay safe on every supported target.
 *
 * The Ethernet frame size is 1514 bytes, which is not a multiple of 8.
 * Ethernet header (14) + IP header (20) + UDP header (8) + command struct (12)
 * leaves command->data 54 bytes from the frame start. Offsetting pkt_buf by 2
 * bytes makes command->data 56 bytes from raw_pkt_buf, keeping it 8-byte
 * aligned. The raw allocation also leaves room for legacy 4-byte packet copy
 * over-reads and 32-byte cache-line operations.
 */
#define RAW_TX_PKT_BUF_SIZE 1536
#define TX_PKT_BUF_SIZE 1514

/* IP protocol identifiers */
#define IP_UDP_PROTOCOL 17
#define IP_ICMP_PROTOCOL 1

void process_pkt(unsigned char *pkt);

extern const unsigned char broadcast[6];

extern __attribute__((aligned(32))) unsigned char raw_pkt_buf[RAW_TX_PKT_BUF_SIZE];
extern __attribute__((aligned(2))) unsigned char *pkt_buf;

/* Defined in client/common/commands.c. */
extern __attribute__((aligned(4))) volatile unsigned int our_ip;

#endif /* KOSLOAD_NET_STACK_H */
