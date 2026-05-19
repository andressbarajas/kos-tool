/* client/common/network/net.c
 *
 * Shared network stack (ARP / ICMP / UDP processing + command dispatch)
 * for GameCube and PlayStation 2 — their previous per-platform
 * net/net.c files were byte-identical, so this is the single copy.
 *
 * Dreamcast keeps its own client/dreamcast/net/net.c: it is genuinely
 * divergent (SH4 size-dependent memcmp_16bit/32bit ops, a #if NET_DEBUG
 * scif block, hardcoded byteswapped ARP constants, and different
 * screensaver-wake behavior), so it stays target-local rather than
 * being force-merged.
 *
 * GC and PS2 both use the standard library memcpy/memcmp, so no
 * per-platform memory-op shim is required here.
 */

#include <string.h>
#include "commands.h"
#include "packet.h"
#include "adapter.h"
#include "net.h"
#include <kosload/dhcp.h>
#include <kosload/screensaver.h>

static void process_broadcast(unsigned char *pkt);
static void process_icmp(ether_header_t *ether, ip_header_t *ip, icmp_header_t *icmp);
static void process_udp(ether_header_t *ether, ip_header_t *ip, udp_header_t *udp);
static void process_mine(unsigned char *pkt);

const unsigned char broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* Packet transmit buffer */
__attribute__((aligned(32))) unsigned char raw_pkt_buf[RAW_TX_PKT_BUF_SIZE];
__attribute__((aligned(2))) unsigned char *pkt_buf = &(raw_pkt_buf[2]);

static void process_broadcast(unsigned char *pkt)
{
	ether_header_t *ether_header = (ether_header_t *)pkt;
	arp_header_t *arp_header = (arp_header_t *)(pkt + ETHER_H_LEN);

	if (ether_header->type[1] == 0x00)
		process_mine(pkt);

	if (ether_header->type[1] != 0x06) /* ARP */
		return;

	/* hardware address space = ethernet */
	if (arp_header->hw_addr_space != htons(0x0001))
		return;

	/* protocol address space = IP */
	if (arp_header->proto_addr_space != htons(0x0800))
		return;

	if (arp_header->opcode == htons(0x0001)) /* arp request */
	{
		if (our_ip == 0)
			return;

		unsigned int ip = htonl(our_ip);
		if (memcmp(arp_header->proto_target, &ip, 4) == 0) /* for us */
		{
			memcpy(ether_header->dest, ether_header->src, 6);
			memcpy(ether_header->src, bb->mac, 6);

			/* arp reply */
			arp_header->opcode = htons(0x0002);
			memcpy(arp_header->hw_target, arp_header->hw_sender, 10);
			memcpy(arp_header->hw_sender, bb->mac, 6);
			memcpy(arp_header->proto_sender, &ip, 4);

			bb->tx(pkt, ETHER_H_LEN + ARP_H_LEN);
		}
	}
}

static void process_icmp(ether_header_t *ether, ip_header_t *ip, icmp_header_t *icmp)
{
	if (icmp->type == 8) /* echo request */
	{
		unsigned short ip_length = ntohs(ip->length);
		unsigned char ip_ihl = ip->version_ihl & 0x0f;
		unsigned short i = icmp->checksum;
		icmp->checksum = 0;
		icmp->checksum = checksum((unsigned short *)icmp,
		                          ip_length / 2 - 2 * ip_ihl,
		                          ip_length % 2);
		if (i != icmp->checksum)
			return;

		icmp->type = 0;
		memcpy(ether->dest, ether->src, 6);
		memcpy(ether->src, bb->mac, 6);
		ip->dest = ip->src;
		ip->src = htonl(our_ip);

		ip->checksum = 0;
		ip->checksum = checksum((unsigned short *)ip, 2 * ip_ihl, 0);
		icmp->checksum = 0;
		icmp->checksum = checksum((unsigned short *)icmp,
		                          ip_length / 2 - 2 * ip_ihl,
		                          ip_length % 2);

		bb->tx((unsigned char *)ether, ETHER_H_LEN + ip_length);
	}
}

static void process_udp(ether_header_t *ether, ip_header_t *ip, udp_header_t *udp)
{
	ip_udp_pseudo_header_t *pseudo;
	unsigned short i;

	// UDP length field includes the 8-byte UDP header itself
	unsigned short udp_total = ntohs(udp->length);
	if (__builtin_expect(udp_total < UDP_H_LEN, 0))
		return;
	unsigned short udp_data_length = udp_total - UDP_H_LEN;

	pseudo = (ip_udp_pseudo_header_t *)pseudo_array;
	pseudo->src_ip = ip->src;
	pseudo->dest_ip = ip->dest;
	pseudo->zero = 0;
	pseudo->protocol = ip->protocol;
	pseudo->udp_length = udp->length;
	pseudo->src_port = udp->src;
	pseudo->dest_port = udp->dest;
	pseudo->length = udp->length;
	pseudo->checksum = 0;

	if (udp->checksum != 0)
		i = checksum_udp((unsigned short *)pseudo,
		                 (unsigned short *)udp->data,
		                 udp_data_length / 2,
		                 udp_data_length % 2);
	else
		i = 0;

	if (udp->checksum == 0xffff)
		udp->checksum = 0;

	if (__builtin_expect(i != udp->checksum, 0))
		return;

	/* Handle DHCP replies */
	dhcp_pkt_t *udp_pkt_data = (dhcp_pkt_t *)udp->data;
	if (__builtin_expect(udp_pkt_data->op == DHCP_OP_BOOTREPLY, 0))
	{
		if (!handle_dhcp_reply(ether->src, udp_pkt_data, udp_data_length))
		{
			escape_loop = 1;
		}
	}
	else
	{
		command_t *command = (command_t *)udp->data;

		/* Only host-directed command traffic should wake the screensaver.
		 * Broadcast discovery/DHCP and stray LAN chatter are still handled,
		 * but they should not dismiss the saver. */
		unsigned short dest_port = ntohs(udp->dest);
		if ((dest_port == NET_LEGACY_PORT || dest_port == NET_DEFAULT_PORT) &&
		    memcmp(ether->dest, bb->mac, 6) == 0)
			screensaver_wake();

		unsigned int pkt_match_id = *(unsigned int *)command->id;

		if (__builtin_expect((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_PARTBIN, 4) == 0), 1))
		{
			cmd_partbin(command);
			pkt_match_id = 0;
		}

		make_ether(ether->src, bb->mac, (ether_header_t *)pkt_buf);

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_MAPLE, 4) == 0))
		{
			cmd_maple(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_PMCR, 4) == 0))
		{
			cmd_pmcr(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_DONEBIN, 4) == 0))
		{
			cmd_donebin(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_RETVAL, 4) == 0))
		{
			cmd_retval(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_LOADBIN, 4) == 0))
		{
			cmd_loadbin(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_SENDBINQ, 4) == 0))
		{
			cmd_sendbinq(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_SENDBIN, 4) == 0))
		{
			cmd_sendbin(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_EXECUTE, 4) == 0))
		{
			cmd_execute(ether, ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_VERSION, 4) == 0))
		{
			cmd_version(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_CAPABILITIES, 4) == 0))
		{
			cmd_capabilities(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_SETRTC, 4) == 0))
		{
			cmd_setrtc(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (memcmp(&pkt_match_id, NET_CMD_REBOOT, 4) == 0))
		{
			cmd_reboot();
		}
	}
}

static void process_mine(unsigned char *pkt)
{
	ether_header_t *ether_header = (ether_header_t *)pkt;
	ip_header_t *ip_header = (ip_header_t *)(pkt + ETHER_H_LEN);
	icmp_header_t *icmp_header;
	udp_header_t *udp_header;

	if (__builtin_expect(ether_header->type[1] != 0x00, 0))
	{
		if (ether_header->type[1] == 0x06)
			process_broadcast(pkt);
		return;
	}

	/* ignore fragmented packets */
	if (__builtin_expect(ntohs(ip_header->flags_frag_offset) & 0x3fff, 0))
		return;

	unsigned char ip_ihl = ip_header->version_ihl & 0x0f;

	/* check ip header checksum */
	unsigned short i = ip_header->checksum;
	ip_header->checksum = 0;
	ip_header->checksum = checksum((unsigned short *)ip_header, 2 * ip_ihl, 0);
	if (i != ip_header->checksum)
		return;

	if (__builtin_expect(ip_header->protocol == IP_UDP_PROTOCOL, 1))
	{
		udp_header = (udp_header_t *)(pkt + ETHER_H_LEN + 4 * ip_ihl);
		process_udp(ether_header, ip_header, udp_header);
	}
	else if (__builtin_expect(ip_header->protocol == IP_ICMP_PROTOCOL, 0))
	{
		icmp_header = (icmp_header_t *)(pkt + ETHER_H_LEN + 4 * ip_ihl);
		process_icmp(ether_header, ip_header, icmp_header);
	}
}

void process_pkt(unsigned char *pkt)
{
	ether_header_t *ether_header = (ether_header_t *)pkt;

	if (ether_header->type[0] != 0x08)
		return;

	if (memcmp(ether_header->dest, bb->mac, 6) == 0)
	{
		process_mine(pkt);
		return;
	}

	if (memcmp(ether_header->dest, broadcast, 6) == 0)
	{
		process_broadcast(pkt);
		return;
	}
}
