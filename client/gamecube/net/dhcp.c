// client/gamecube/net/dhcp.c - DHCP client for GameCube.
// Adapted from DC version: uses PPC TBR for timing instead of SH4 PMCR.
//
// Original credits:
// Consider the code in dhcp.h & dhcp.c that is not obviously marked
// 'KallistiOS stuff', which is specifically KOS-licensed, to be in the public
// domain. --Moopthehedgehog

#include <string.h>
#include "packet.h"
#include "net.h"
#include "adapter.h"
#include "dhcp.h"
#include <kosload/target.h>
#include "dcload.h"

#define DHCP_RENEW_TYPE 0

#define DHCP_DEST_PORT 67
#define DHCP_SOURCE_PORT 68

#define DHCP_PKT_LEN 300

// Maximum number of DHCP discovery attempts before giving up
#define DHCP_MAX_ATTEMPTS 30

// Maximum number of consecutive NAK responses before giving up
#define DHCP_MAX_NAKS 5
#define DHCP_MIN_OPTIONS_SIZE 64

static void build_send_dhcp_packet(unsigned char kind);
static int kos_net_dhcp_fill_options(unsigned char *bbmac, dhcp_pkt_t *req, uint8 msgtype);
static int kos_net_dhcp_get_message_type(dhcp_pkt_t *pkt, int len);
static uint32 kos_net_dhcp_get_32bit(dhcp_pkt_t *pkt, uint8 opt, int len);

static unsigned int dhcpoffer_server_ip_from_pkt = 0;
static unsigned int dhcpoffer_ip_from_pkt = 0;
static unsigned int dhcpoffer_xid = 0;

volatile unsigned int dhcp_lease_time = 0;
static unsigned int renewal_increment = 0;

static unsigned char dhcp_acked = 0;
static unsigned char dhcp_renewal = 0;
static unsigned char dhcp_renewal_nak = 0;
static unsigned char dhcp_nak_received = 0;
unsigned char dhcp_nest_counter_maxed = 0;
unsigned int dhcp_attempts = 0;

static unsigned char router_mac[6] = {0};

#define DHCP_TX_PKT_BUF_SIZE TX_PKT_BUF_SIZE
#define dhcp_pkt_buf pkt_buf

#define DHCP_TX_PKT_BUF_ZEROING_SIZE 344

static void build_send_dhcp_packet(unsigned char kind)
{
	ether_header_t *dhcp_ether_header = (ether_header_t *)dhcp_pkt_buf;
	ip_header_t *dhcp_ip_header = (ip_header_t *)(dhcp_pkt_buf + ETHER_H_LEN);
	udp_header_t *dhcp_udp_header = (udp_header_t *)(dhcp_pkt_buf + ETHER_H_LEN + IP_H_LEN);

	unsigned char *dhcp_out_pkt = dhcp_pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	unsigned char *dhcp_out_mac = (unsigned char *)broadcast;
	unsigned int dhcp_out_ip = 0xffffffff;

	kos_net_dhcp_fill_options(bb->mac, (dhcp_pkt_t *)dhcp_out_pkt, kind);

	if (kind == DHCP_MSG_DHCPDISCOVER)
	{
		bb->start();
	}
	else if (kind == DHCP_MSG_DHCPREQUEST)
	{
		/* Don't need to do anything */
	}
	else /* Assume renewal */
	{
		bb->start();
		dhcp_out_mac = router_mac;
		dhcp_out_ip = dhcpoffer_server_ip_from_pkt;
	}

	make_ether(dhcp_out_mac, bb->mac, dhcp_ether_header);
	make_ip(dhcp_out_ip, our_ip, UDP_H_LEN + DHCP_PKT_LEN, IP_UDP_PROTOCOL, dhcp_ip_header, 0);
	make_udp(DHCP_DEST_PORT, DHCP_SOURCE_PORT, DHCP_PKT_LEN, dhcp_ip_header, dhcp_udp_header);
	bb->tx(dhcp_pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + DHCP_PKT_LEN);
}

int handle_dhcp_reply(unsigned char *routersrcmac, dhcp_pkt_t *pkt_data, unsigned short len)
{
	int msg_type = kos_net_dhcp_get_message_type(pkt_data, len);

	if (msg_type == DHCP_MSG_DHCPOFFER)
	{
		dhcpoffer_server_ip_from_pkt = kos_net_dhcp_get_32bit(pkt_data, DHCP_OPTION_SERVER_ID, len);
		dhcpoffer_ip_from_pkt = ntohl(pkt_data->yiaddr);
		dhcpoffer_xid = pkt_data->xid;

		memcpy(router_mac, routersrcmac, 6);

		return 0;
	}
	else if (msg_type == DHCP_MSG_DHCPACK)
	{
		if ((pkt_data->xid == dhcpoffer_xid) &&
		    ((dhcp_renewal == 1) || (ntohl(pkt_data->yiaddr) == dhcpoffer_ip_from_pkt)))
		{
			if (dhcp_renewal)
			{
				dhcpoffer_ip_from_pkt = ntohl(pkt_data->yiaddr);
			}

			dhcp_lease_time = kos_net_dhcp_get_32bit(pkt_data, DHCP_OPTION_IP_LEASE_TIME, len);
			/* Lease countdown is initialized by entry.c after dhcp_go/dhcp_renew returns */
			dhcp_acked = 1;

			return 0;
		}
	}
	else if (msg_type == DHCP_MSG_DHCPNAK)
	{
		if (dhcp_renewal == 1)
		{
			// IP no longer valid; dhcp_renew() will return -2 to trigger re-discovery
			dhcp_renewal_nak = 1;
			return 0;
		}

		// Signal dhcp_go() to retry discovery (iterative, no recursion)
		dhcp_nak_received = 1;
		return 0;
	}

	return -1;
}

int dhcp_go(unsigned int *dhcp_ip_address_buffer)
{
	dhcp_acked = 0;
	dhcp_nak_received = 0;
	dhcp_attempts = 0;
	timeout_loop = 1;
	unsigned char nak_count = 0;

	while (!dhcp_acked)
	{
		dhcp_attempts++;
		if (dhcp_attempts > DHCP_MAX_ATTEMPTS)
		{
			dhcp_nest_counter_maxed = 1;
			break;
		}
		if (timeout_loop < 0)
		{
			timeout_loop = (dhcp_attempts > 10 ? 30 : 3 * (dhcp_attempts));
		}
		build_send_dhcp_packet(DHCP_MSG_DHCPDISCOVER);
		bb->loop(0);
		if (timeout_loop < 0) continue;
		build_send_dhcp_packet(DHCP_MSG_DHCPREQUEST);
		bb->loop(0);
		if (timeout_loop < 0) continue;

		if (dhcp_nak_received)
		{
			dhcp_nak_received = 0;
			nak_count++;
			if (nak_count >= DHCP_MAX_NAKS)
			{
				dhcp_nest_counter_maxed = 1;
				break;
			}
			continue; /* Retry discovery from the top */
		}
	}

	dhcp_attempts = 0;
	timeout_loop = 0;

	if (dhcp_acked)
	{
		*dhcp_ip_address_buffer = dhcpoffer_ip_from_pkt;
		return 0;
	}
	else if (dhcp_nest_counter_maxed)
	{
		return 0;
	}
	else
	{
		return -1;
	}
}

int dhcp_renew(unsigned int *dhcp_ip_address_buffer)
{
	dhcp_acked = 0;
	dhcp_renewal = 1;
	dhcp_renewal_nak = 0;

	build_send_dhcp_packet(DHCP_RENEW_TYPE);
	bb->loop(0);

	dhcp_renewal = 0;

	if (dhcp_renewal_nak)
	{
		return -2;
	}
	else if (dhcp_acked)
	{
		*dhcp_ip_address_buffer = dhcpoffer_ip_from_pkt;
		return 0;
	}
	else
	{
		return -1;
	}
}

//==============================================================================
// START KOS STUFF
//==============================================================================

/* KallistiOS 2.1.0

   kernel/net/net_dhcp.c
   Copyright (C) 2008, 2009, 2013 Lawrence Sebald

	 Redistribution and use in source and binary forms, with or without
	 modification, are permitted provided that the following conditions
	 are met:
	 1. Redistributions of source code must retain the above copyright
	    notice, this list of conditions and the following disclaimer.
	 2. Redistributions in binary form must reproduce the above copyright
	    notice, this list of conditions and the following disclaimer in the
	    documentation and/or other materials provided with the distribution.
	 3. Neither the name of Cryptic Allusion nor the names of its contributors
	    may be used to endorse or promote products derived from this software
	    without specific prior written permission.

	 THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
	 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	 ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
	 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
	 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
	 OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
	 HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
	 LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
	 OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
	 SUCH DAMAGE.
*/

static int kos_net_dhcp_fill_options(unsigned char *bbmac, dhcp_pkt_t *req, uint8 msgtype)
{
	memset(raw_pkt_buf, 0, DHCP_TX_PKT_BUF_ZEROING_SIZE);

	uint32 serverid = 0, reqip = 0;
	int pos = 0;

	req->op = DHCP_OP_BOOTREQUEST;
	req->htype = DHCP_HTYPE_10MB_ETHERNET;
	req->hlen = DHCP_HLEN_ETHERNET;

	if (msgtype == DHCP_MSG_DHCPDISCOVER)
	{
		req->xid = htonl((uint32_t)target_get_ops()->get_ticks() ^ 0xDEADBEEF);
	}
	else if (msgtype == DHCP_MSG_DHCPREQUEST)
	{
		req->xid = dhcpoffer_xid;
		serverid = dhcpoffer_server_ip_from_pkt;
		reqip = dhcpoffer_ip_from_pkt;
	}
	else /* Assume renewal (msgtype == 0) */
	{
		dhcpoffer_xid = ((uint32_t)target_get_ops()->get_ticks() ^ 0xDEADBEEF) + renewal_increment;
		renewal_increment += 0x1000;
		req->xid = dhcpoffer_xid;
		req->ciaddr = htonl(our_ip);
		reqip = our_ip;
	}

	memcpy(req->chaddr, bbmac, DHCP_HLEN_ETHERNET);

	/* DHCP Magic Cookie */
	req->options[pos++] = 0x63;
	req->options[pos++] = 0x82;
	req->options[pos++] = 0x53;
	req->options[pos++] = 0x63;

	/* Message Type */
	req->options[pos++] = DHCP_OPTION_MESSAGE_TYPE;
	req->options[pos++] = 1;
	if (!msgtype)
		req->options[pos++] = DHCP_MSG_DHCPREQUEST;
	else
		req->options[pos++] = msgtype;

	/* Max Message Length: 1500 octets */
	req->options[pos++] = DHCP_OPTION_MAX_MESSAGE;
	req->options[pos++] = 2;
	req->options[pos++] = (1500 >> 8) & 0xFF;
	req->options[pos++] = (1500 >> 0) & 0xFF;

	/* Host Name */
	req->options[pos++] = DHCP_OPTION_HOST_NAME;
	req->options[pos++] = sizeof(LOADER_NAME) - 1;
	memcpy((char *)req->options + pos, LOADER_NAME, sizeof(LOADER_NAME) - 1);
	pos += sizeof(LOADER_NAME) - 1;

	/* Client Identifier */
	req->options[pos++] = DHCP_OPTION_CLIENT_ID;
	req->options[pos++] = 1 + DHCP_HLEN_ETHERNET;
	req->options[pos++] = DHCP_HTYPE_10MB_ETHERNET;
	memcpy(req->options + pos, bbmac, DHCP_HLEN_ETHERNET);
	pos += DHCP_HLEN_ETHERNET;

	/* Parameters requested */
	req->options[pos++] = DHCP_OPTION_PARAMETER_REQUEST;
	req->options[pos++] = 5;
	req->options[pos++] = DHCP_OPTION_SUBNET_MASK;
	req->options[pos++] = DHCP_OPTION_ROUTER;
	req->options[pos++] = DHCP_OPTION_DOMAIN_NAME_SERVER;
	req->options[pos++] = DHCP_OPTION_BROADCAST_ADDR;
	req->options[pos++] = DHCP_OPTION_INTERFACE_MTU;

	if (serverid) {
		req->options[pos++] = DHCP_OPTION_SERVER_ID;
		req->options[pos++] = 4;
		req->options[pos++] = (serverid >> 24) & 0xFF;
		req->options[pos++] = (serverid >> 16) & 0xFF;
		req->options[pos++] = (serverid >>  8) & 0xFF;
		req->options[pos++] = (serverid >>  0) & 0xFF;
	}

	if (reqip) {
		req->options[pos++] = DHCP_OPTION_REQ_IP_ADDR;
		req->options[pos++] = 4;
		req->options[pos++] = (reqip >> 24) & 0xFF;
		req->options[pos++] = (reqip >> 16) & 0xFF;
		req->options[pos++] = (reqip >>  8) & 0xFF;
		req->options[pos++] = (reqip >>  0) & 0xFF;
	}

	req->options[pos++] = DHCP_OPTION_END;

	return (pos < DHCP_MIN_OPTIONS_SIZE) ? DHCP_MIN_OPTIONS_SIZE : pos;
}

static int kos_net_dhcp_get_message_type(dhcp_pkt_t *pkt, int len)
{
	int i;

	len -= DHCP_H_LEN;

	for (i = 4; i < len;) {
		if (pkt->options[i] == DHCP_OPTION_MESSAGE_TYPE) {
			if (i + 2 < len)
				return pkt->options[i + 2];
			break;
		}
		else if (pkt->options[i] == DHCP_OPTION_PAD) {
			++i;
		}
		else if (pkt->options[i] == DHCP_OPTION_END) {
			break;
		}
		else {
			if (i + 1 >= len)
				break;
			i += pkt->options[i + 1] + 2;
		}
	}

	return -1;
}

static uint32 kos_net_dhcp_get_32bit(dhcp_pkt_t *pkt, uint8 opt, int len)
{
	int i;

	len -= DHCP_H_LEN;

	for (i = 4; i < len;) {
		if (pkt->options[i] == opt) {
			if (i + 5 >= len)
				return 0;
			if (pkt->options[i + 1] < 4)
				return 0;
			return (pkt->options[i + 2] << 24) | (pkt->options[i + 3] << 16) |
			       (pkt->options[i + 4] << 8) | (pkt->options[i + 5]);
		}
		else if (pkt->options[i] == DHCP_OPTION_PAD) {
			++i;
		}
		else if (pkt->options[i] == DHCP_OPTION_END) {
			break;
		}
		else {
			if (i + 1 >= len)
				break;
			i += pkt->options[i + 1] + 2;
		}
	}

	return 0;
}

//==============================================================================
// END KOS STUFF
//==============================================================================
