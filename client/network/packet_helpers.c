/* client/network/packet_helpers.c */
/*
 * Shared packet construction helpers for IP transports.
 *
 * Checksum and Ethernet copy helpers stay target-local because byte order and
 * hardware copy behavior differ between supported consoles.
 */

#include "packet.h"

void make_ip(int dest, int src, int length, char protocol, ip_header_t *ip,
             unsigned short pkt_id)
{
	ip->version_ihl = 0x45;
	ip->tos = 0;
	ip->length = htons(20 + length);
	ip->packet_id = pkt_id;
	ip->flags_frag_offset = htons(0x4000);
	ip->ttl = 64;
	ip->protocol = protocol;
	ip->checksum = 0;
	ip->src = htonl(src);
	ip->dest = htonl(dest);

	ip->checksum = checksum((unsigned short *)ip, IP_H_LEN / 2, 0);
}

void make_udp(unsigned short dest, unsigned short src, int length,
              ip_header_t *ip, udp_header_t *udp)
{
	ip_udp_pseudo_header_t *pseudo = (ip_udp_pseudo_header_t *)pseudo_array;

	udp->src = htons(src);
	udp->dest = htons(dest);
	udp->length = htons(length + UDP_H_LEN);
	udp->checksum = 0;

	pseudo->src_ip = ip->src;
	pseudo->dest_ip = ip->dest;
	pseudo->zero = 0;
	pseudo->protocol = ip->protocol;
	pseudo->udp_length = udp->length;
	pseudo->src_port = udp->src;
	pseudo->dest_port = udp->dest;
	pseudo->length = udp->length;
	pseudo->checksum = 0;

	udp->checksum = checksum_udp((unsigned short *)pseudo,
	                             (unsigned short *)udp->data,
	                             length / 2, length % 2);
	if (udp->checksum == 0)
		udp->checksum = 0xffff;
}
