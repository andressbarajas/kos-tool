/* client/dreamcast/net/packet.h */
#ifndef __PACKET_H__
#define __PACKET_H__

/* Based on dcload-ip: dcload-ip/target-src/dcload/commands.h */

#include <stdbool.h>
#include "bswap.h"
#include <kosload/protocol.h>

/* Packet header types (ether_header_t, ip_header_t, udp_header_t,
 * icmp_header_t, arp_header_t) come from <kosload/protocol.h>. */

#define ETHER_H_LEN 14
#define IP_H_LEN 20
#define UDP_H_LEN 8
#define ICMP_H_LEN 8
#define ARP_H_LEN 28

/* IP/UDP pseudo header for checksum computation */
typedef struct __attribute__ ((packed, aligned(4))) {
	unsigned int src_ip;
	unsigned int dest_ip;
	unsigned char zero;
	unsigned char protocol;
	unsigned short udp_length;
	unsigned short src_port;
	unsigned short dest_port;
	unsigned short length;
	unsigned short checksum;
} ip_udp_pseudo_header_t;

#define PSEUDO_H_LEN 20

// For is_odd, pass length%2 where datacount is length/2 (remember: integer divides)
unsigned short checksum(unsigned short *buf, int count, bool is_odd);
unsigned short checksum_udp(unsigned short *buf_pseudo, unsigned short *buf_data, int datacount, bool is_odd);

void make_ether(unsigned char *dest, unsigned char *src, ether_header_t *ether);
void make_ip(int dest, int src, int length, char protocol, ip_header_t *ip, unsigned short pkt_id);
void make_udp(unsigned short dest, unsigned short src, int length, ip_header_t *ip, udp_header_t *udp);

#define ntohl bswap32
#define htonl bswap32
#define ntohs bswap16
#define htons bswap16

extern __attribute__((aligned(4))) unsigned char pseudo_array[PSEUDO_H_LEN];

#endif
