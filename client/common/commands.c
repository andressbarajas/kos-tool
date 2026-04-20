/* client/common/commands.c */
/*
 * Network command handlers for kosload client.
 * Ported from dcload-ip commands.c.
 *
 * These functions are called by the network stack (net.c) when
 * commands arrive over UDP. Only linked into kosload-ip via
 * static library selective linking.
 *
 * Based on dcload-ip: dcload-ip/target-src/dcload/commands.c
 */

#include <stdbool.h>
#include <string.h>
#include <kosload/target.h>
#include <kosload/info.h>
#include <kosload/screensaver.h>
#include <kosload/net_adapter.h>
#include <kosload/net_stack.h>
#include "commands.h"
#include "dcload.h"
#include "maple.h"
#include "perfctr.h"
#include <kosload/memfuncs.h>
#include <kosload/divutil.h>
#include "scif.h"

extern kosload_info_t kosload_info;

/* Debug instrumentation in upload command path can stall PBIN handling; keep off. */
#define CMD_DEBUG 0
#if CMD_DEBUG
extern void draw_string(int x, int y, const char *str, int color);
static unsigned int dbg_partbin_count = 0;
static unsigned char dbg_hex2[12];
#endif

/* Version string for cmd_version response */
#ifndef KOSLOAD_VERSION_STRING
#define KOSLOAD_VERSION_STRING "0.1.0"
#endif

/* Syscall state (defined in network syscalls) */
extern unsigned int syscall_retval;
extern unsigned char *syscall_data;
extern unsigned short dcload_syscall_port;


/* Forward declarations for syscalls used in error paths */
extern int write(int fd, const void *buf, unsigned int count);
extern void progexit(int ret_code);

/* Direct hardware access (from ASM files) */

/* ===== Network globals ===== */

__attribute__((aligned(4))) volatile unsigned int our_ip = 0;
unsigned int tool_ip = 0;
unsigned char tool_mac[6] = {0};
unsigned short tool_port = 0;
unsigned int tool_version = 0;

static bool cached_dest = false;
static bool payload1024 = false;

#define min(a, b) ((a) < (b) ? (a) : (b))

/*
 * Binary load tracking map (bitfield).
 * Each bit tracks whether a packet-sized chunk has been received.
 * MAX_RAM_BYTES is defined per platform via CMake (e.g. 32MB for DC, 24MB for GC).
 * Map entries = ceil(MAX_RAM_BYTES / 1440), stored as 1 bit per entry.
 */
#ifndef MAX_RAM_BYTES
#error "MAX_RAM_BYTES must be defined per platform in CMakeLists.txt"
#endif
#define BIN_INFO_MAP_ENTRIES ((MAX_RAM_BYTES + 1439) / 1440)
#define BIN_INFO_MAP_BYTES  (((BIN_INFO_MAP_ENTRIES + 63) / 64) * 8)

typedef struct {
	unsigned int load_address;
	unsigned int load_size;
	unsigned char map[BIN_INFO_MAP_BYTES];
} bin_info_t;

__attribute__((aligned(8))) static bin_info_t bin_info;

static inline void map_clear_all(void)
{
	fast_memset_zero_64(bin_info.map, BIN_INFO_MAP_BYTES / 8);
}

static inline void map_set(int index)
{
	bin_info.map[index >> 3] |= (1 << (index & 7));
}

static inline int map_test(int index)
{
	return bin_info.map[index >> 3] & (1 << (index & 7));
}

/* ===== Command Handlers ===== */

void cmd_reboot(void)
{
	booted = false;
	running = false;

	const target_ops_t *t = target_get_ops();
	t->reboot();
}

void cmd_execute(ether_header_t *ether, ip_header_t *ip, udp_header_t *udp, command_t *command)
{
	if (!running)
	{
		bb->stop();

		tool_ip = ntohl(ip->src);
		tool_port = ntohs(udp->src);
		memcpy(tool_mac, ether->src, 6);
		our_ip = ntohl(ip->dest);

		unsigned int cmd_size = ntohl(command->size);

		unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
		command_t *response = (command_t *)buffer;
		memcpy(response, command, COMMAND_LEN);

		make_ip(tool_ip, our_ip, UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
		make_udp(tool_port, ntohs(udp->dest), COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
		bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);

		/* Extract command-line arguments from EXEC data payload if present.
		 * Payload format: [argc (4 bytes BE)] [command_line string with NUL]
		 * Data starts at command->data, length derived from UDP packet size. */
		{
			unsigned int udp_payload = ntohs(udp->length) - UDP_H_LEN;
			unsigned int data_len = (udp_payload > COMMAND_LEN) ? udp_payload - COMMAND_LEN : 0;

			kosload_info.argc = 0;
			kosload_info.command_line[0] = '\0';

			if (data_len >= 5) { /* at least 4-byte argc + 1-byte NUL */
				unsigned int argc_be;
				memcpy(&argc_be, command->data, 4);
				unsigned int prog_argc = ntohl(argc_be);
				unsigned int cmdline_len = data_len - 4;

				if (prog_argc > 0 && cmdline_len > 0) {
					if (cmdline_len > KOSLOAD_MAX_CMDLINE)
						cmdline_len = KOSLOAD_MAX_CMDLINE;
					kosload_info.argc = prog_argc;
					memcpy(kosload_info.command_line, command->data + 4, cmdline_len);
					kosload_info.command_line[cmdline_len - 1] = '\0';
				}
			}
		}

		if (!booted)
			disp_info();
		else
			disp_status("executing...");

		{
			const target_ops_t *t = target_get_ops();
			t->set_console_enabled(cmd_size & 1);

			if ((cmd_size >> 1) && t->cdfs_redir_enable)
				t->cdfs_redir_enable();
		}

		running = true;

		{
			const target_ops_t *t = target_get_ops();
			t->execute(ntohl(command->address));
			t->restart_timer();
		}

		/* Adapter hardware state may have been modified by the executed
		 * program (e.g. KOS networking).  Do a full re-init so receive
		 * buffers and registers are back to our known-good configuration.
		 * bb->start() re-enables RX after init (init alone doesn't). */
		bb->stop();
		bb->init();
		bb->start();

		running = false;
		disp_info();
		disp_status("idle...");
		screensaver_reset();
	}
}

void cmd_loadbin(ip_header_t *ip, udp_header_t *udp, command_t *command)
{
#if CMD_DEBUG
	scif_puts((unsigned char *)"LBIN: enter\n");
	clear_lines(222, 24, 0x0000);
	draw_string(0, 222, "LBIN ENTER", 0x07e0);
#endif

	bin_info.load_address = ntohl(command->address);
	bin_info.load_size = ntohl(command->size);

	/* Legacy check for versions < 2.0.0 */
	if (DCTOOL_MAJOR < 2)
	{
		if (bin_info.load_size > (BIN_INFO_MAP_ENTRIES * 1024))
		{
			write(1, "ERROR: Size >11656KB (legacy mode)\r\n", 37);
			progexit(-1);
			bb->start();
			return;
		}
	}
	else
	{
		const target_ops_t *t = target_get_ops();
		uint32_t max_size = t->detect_ram_size();
		if (bin_info.load_size > max_size)
		{
			write(1, "ERROR: Size exceeds RAM\r\n", 25);
			progexit(-1);
			bb->start();
			return;
		}
	}

	/* Zero out the received packet map */
	map_clear_all();

	our_ip = ntohl(ip->dest);

	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t *response = (command_t *)buffer;
	memcpy(response, command, COMMAND_LEN);

	/* Check for cacheable destination address */
	unsigned int cacheable_check = bin_info.load_address >> 29;
	if ((cacheable_check != 0x5) && (cacheable_check != 0x7))
	{
		cached_dest = true;
	}
	else
	{
		cached_dest = false;
	}

	/* Set up partbin payload size based on tool version */
	if (DCTOOL_MAJOR < 2)
		payload1024 = true;
	else
		payload1024 = false;

	make_ip(ntohl(ip->src), our_ip, UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);

#if CMD_DEBUG
	scif_puts((unsigned char *)"LBIN: tx done\n");
	uint_to_string(bin_info.load_address, dbg_hex2);
	scif_puts((unsigned char *)"  addr=");
	scif_puts(dbg_hex2);
	uint_to_string(bin_info.load_size, dbg_hex2);
	scif_puts((unsigned char *)" size=");
	scif_puts(dbg_hex2);
	scif_puts((unsigned char *)"\n");
	clear_lines(222, 24, 0x0000);
	draw_string(0, 222, "LBIN TX OK", 0x07e0);
	dbg_partbin_count = 0;
#endif

	if (!running) {
		if (!booted)
			disp_info();
		disp_status("receiving data...");
	}
}

void cmd_partbin(command_t *command)
{
	int index = 0;
	unsigned int cmd_addr = ntohl(command->address);
	unsigned int cmd_size = ntohl(command->size);

#if CMD_DEBUG
	dbg_partbin_count++;
	/* Show every 10th PARTBIN to avoid flooding the display */
	if ((dbg_partbin_count % 10) == 1) {
		uint_to_string(dbg_partbin_count, dbg_hex2);
		clear_lines(246, 24, 0x0000);
		draw_string(0, 246, "PBIN:", 0xffff);
		draw_string(80, 246, (const char *)dbg_hex2, 0xffff);
		uint_to_string(cmd_addr, dbg_hex2);
		draw_string(200, 246, (const char *)dbg_hex2, 0xffff);
	}
#endif

	/* Use platform-optimized memcpy with cached source for speed */
	fast_aligned_memcpy((void *)cmd_addr, to_cached(command->data), cmd_size);
	if (cached_dest)
	{
		cache_block_purge((void *)cmd_addr, (cmd_size + 31) / 32 + 2);
	}

	if (__builtin_expect(payload1024, 0))
		index = (cmd_addr - bin_info.load_address) / 1024;
	else
		index = UDIV_CONST(cmd_addr - bin_info.load_address, 1440);

	if (index >= 0 && index < BIN_INFO_MAP_ENTRIES)
		map_set(index);
}

void cmd_donebin(ip_header_t *ip, udp_header_t *udp, command_t *command)
{
#if CMD_DEBUG
	scif_puts((unsigned char *)"DBIN: enter\n");
	clear_lines(222, 24, 0x0000);
	draw_string(0, 222, "DONEBIN", 0x07e0);
#endif

	unsigned int i;
	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t *response = (command_t *)buffer;
	memcpy(response, command, COMMAND_LEN);

	unsigned int map_index_verify, payload_size;

	if (DCTOOL_MAJOR < 2)
	{
		map_index_verify = (bin_info.load_size + 1023) / 1024;
		payload_size = 1024;
	}
	else
	{
		map_index_verify = UDIV_CONST(bin_info.load_size + 1439, 1440);
		payload_size = 1440;
	}

	for (i = 0; i < map_index_verify; i++)
		if (!map_test(i))
			break;

	if (i == map_index_verify)
	{
		response->address = 0;
		response->size = 0;
	}
	else
	{
		response->address = htonl(bin_info.load_address + i * payload_size);
		response->size = htonl(min(bin_info.load_size - i * payload_size, payload_size));
	}

	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);

	if (!running) {
		if (!booted)
			disp_info();
		if (i == map_index_verify)
			disp_status("idle...");
		else
			disp_status("receiving data...");
	}
}

void cmd_sendbinq(ip_header_t *ip, udp_header_t *udp, command_t *command)
{
	our_ip = ntohl(ip->dest);

	unsigned int payload_size, numpackets, i;
	unsigned int bytes_thistime;

	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t *response = (command_t *)buffer;

	unsigned int cmd_addr = ntohl(command->address);
	unsigned int bytes_left = ntohl(command->size);

	if (DCTOOL_MAJOR < 2)
	{
		payload_size = 1024;
		numpackets = (bytes_left + 1023) / 1024;
	}
	else
	{
		payload_size = 1440;
		numpackets = UDIV_CONST(bytes_left + 1439, 1440);
	}

	unsigned int ip_src = ntohl(ip->src);
	unsigned short udp_src = ntohs(udp->src);
	unsigned short udp_dest = ntohs(udp->dest);

	memcpy(response->id, CMD_SENDBIN, 4);

	for (i = 0; i < numpackets; i++)
	{
		if (bytes_left >= payload_size)
			bytes_thistime = payload_size;
		else
			bytes_thistime = bytes_left;
		bytes_left -= bytes_thistime;

		fast_aligned_memcpy(to_cached(response->data), (void *)cmd_addr, bytes_thistime);

		response->address = htonl(cmd_addr);
		response->size = htonl(bytes_thistime);
		make_ip(ip_src, our_ip, UDP_H_LEN + COMMAND_LEN + bytes_thistime, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
		make_udp(udp_src, udp_dest, COMMAND_LEN + bytes_thistime, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
		bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + bytes_thistime);
		cmd_addr += bytes_thistime;
	}

	memcpy(response->id, CMD_DONEBIN, 4);
	response->address = 0;
	response->size = 0;
	make_ip(ip_src, our_ip, UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(udp_src, udp_dest, COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);
}

void cmd_sendbin(ip_header_t *ip, udp_header_t *udp, command_t *command)
{
	if (!running) {
		if (!booted)
			disp_info();
		disp_status("sending data...");
	}

	cmd_sendbinq(ip, udp, command);

	if (!running) {
		disp_status("idle...");
	}
}

void cmd_version(ip_header_t *ip, udp_header_t *udp, command_t *command)
{
#if CMD_DEBUG
	scif_puts((unsigned char *)"VERS: enter\n");
	clear_lines(222, 24, 0x0000);
	draw_string(0, 222, "VERS", 0x07e0);
#endif

	int datalength, j;
	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t *response = (command_t *)buffer;

	/* dc-tool stores its version in the address field (added in 2.0.0) */
	tool_version = ntohl(command->address);

	/* New versions get the syscall port from the UDP dest port */
	if (DCTOOL_MAJOR >= 2)
		dcload_syscall_port = ntohs(udp->dest);
	else
		dcload_syscall_port = 31313;

	datalength = strlen(LOADER_NAME " " KOSLOAD_VERSION_STRING " using ");
	memcpy(response, command, COMMAND_LEN);
	memcpy(response->data, LOADER_NAME " " KOSLOAD_VERSION_STRING " using ", datalength);

	/* Append adapter name */
	j = strlen(bb->name) + 1;
	memcpy(response->data + datalength, bb->name, j);
	datalength += j;

	response->size = htonl(datalength);
	/* Stuff adapter type (lower 16 bits) and capability flags (upper 16 bits)
	 * in the otherwise unused address field */
	response->address = htonl((kosload_info.capabilities << 16) | installed_adapter);

	make_ip(ntohl(ip->src), our_ip, UDP_H_LEN + COMMAND_LEN + datalength, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), dcload_syscall_port, COMMAND_LEN + datalength, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + datalength);

#if CMD_DEBUG
	scif_puts((unsigned char *)"VERS: tx done, tool_ver=");
	uint_to_string(tool_version, dbg_hex2);
	scif_puts(dbg_hex2);
	scif_puts((unsigned char *)" major=");
	uint_to_string(DCTOOL_MAJOR, dbg_hex2);
	scif_puts(dbg_hex2);
	scif_puts((unsigned char *)"\n");
	clear_lines(222, 24, 0x0000);
	draw_string(0, 222, "VERS TX OK", 0x07e0);

	/* Dump NIC registers after VERS TX to diagnose RX failure */
	{
		volatile unsigned short *d_nic16 = (volatile unsigned short *)0xa1001700;
		volatile unsigned char *d_nic8 = (volatile unsigned char *)0xa1001700;
		volatile unsigned int *d_nic32 = (volatile unsigned int *)0xa1001700;

		/* Line 270: ISR, CHIPCMD, RXCONFIG */
		clear_lines(270, 24, 0x0000);
		draw_string(0, 270, "ISR:", 0xffff);
		uint_to_string(d_nic16[0x3E/2], dbg_hex2);
		draw_string(50, 270, (const char *)dbg_hex2, 0xffff);
		draw_string(170, 270, "CMD:", 0xffff);
		uint_to_string(d_nic8[0x37], dbg_hex2);
		draw_string(220, 270, (const char *)dbg_hex2, 0xffff);
		draw_string(340, 270, "RXC:", 0xffff);
		uint_to_string(d_nic32[0x44/4], dbg_hex2);
		draw_string(390, 270, (const char *)dbg_hex2, 0xffff);

		/* Line 294: RXBUFTAIL, RXBUFHEAD, RXMISSED */
		clear_lines(294, 24, 0x0000);
		draw_string(0, 294, "TAIL:", 0xffff);
		uint_to_string(d_nic16[0x38/2], dbg_hex2);
		draw_string(70, 294, (const char *)dbg_hex2, 0xffff);
		draw_string(190, 294, "HEAD:", 0xffff);
		uint_to_string(d_nic16[0x3A/2], dbg_hex2);
		draw_string(260, 294, (const char *)dbg_hex2, 0xffff);
		draw_string(380, 294, "MISS:", 0xffff);
		uint_to_string(d_nic32[0x4C/4], dbg_hex2);
		draw_string(440, 294, (const char *)dbg_hex2, 0xffff);

		/* Line 318: TXSTATUS of all 4 descriptors */
		clear_lines(318, 24, 0x0000);
		draw_string(0, 318, "TX0:", 0xffff);
		uint_to_string(d_nic32[0x10/4], dbg_hex2);
		draw_string(50, 318, (const char *)dbg_hex2, 0xffff);
		draw_string(170, 318, "TX1:", 0xffff);
		uint_to_string(d_nic32[0x14/4], dbg_hex2);
		draw_string(220, 318, (const char *)dbg_hex2, 0xffff);
	}
#endif
}

void cmd_retval(ip_header_t *ip, udp_header_t *udp, command_t *command)
{
	if (running)
	{
		bb->stop();

		unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
		command_t *response = (command_t *)buffer;
		memcpy(response, command, COMMAND_LEN);

		make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
		make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
		bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);

		syscall_retval = ntohl(command->address);
		/* Points into the RX buffer — safe because bb->stop() above
		 * disables hardware RX, and gdbpacket() copies immediately
		 * after the loop exits (single-threaded, no DMA/interrupts). */
		syscall_data = command->data;
		escape_loop = 1;
	}
}

void cmd_maple(ip_header_t *ip, udp_header_t *udp, command_t *command)
{
	unsigned char *res;
	int i;
	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t *response = (command_t *)buffer;

	memcpy(response, command, COMMAND_LEN);

	do {
		res = maple_docmd(command->data[0], command->data[1],
		                   command->data[2], command->data[3],
		                   command->data + 4);
	} while ((signed char)res[0] == MAPLE_RESPONSE_AGAIN);

	/* Send response back */
	i = (((signed char)res[0] < 0) ? 4 : ((res[3] + 1) << 2));
	response->size = htonl(i);
	fast_aligned_memcpy(to_cached(response->data), to_cached(res), i);

	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN + i, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN + i, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + i);
}

/* Performance counter control */
static char *ok_message = "OK";
static char *invalid_read_message = "PMCR: chan 1 or 2 only.";
static char *invalid_function_message = "PMCR: I, E, B, R, G, S, or D only.";
static char *invalid_option_message = "PMCR: chan 1, 2, or 3 (both) only.";
static char *invalid_mode_message = "PMCR modes: 0x1-0x29.";
static char *invalid_count_type_message = "PMCR count: 0 or 1 only.";
static char *invalid_reset_type_message = "PMCR reset: 0 or 1 only.";
static volatile unsigned int read_array[2] = {0};
static volatile unsigned char getconfig_array[2] = {0};

void cmd_pmcr(ip_header_t *ip, udp_header_t *udp, command_t *command)
{
	char *out_message = ok_message;
	unsigned int i = 3;
	unsigned char invalid_pmcr = 0, invalid_mode = 0, invalid_count = 0, invalid_reset = 0;
	unsigned char read = 0;

	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t *response = (command_t *)buffer;

	memcpy(response, command, COMMAND_LEN);

	if ((!command->data[1]) || (command->data[1] > 3))
	{
		invalid_pmcr = 1;
	}
	else if (command->data[0] == 'I') /* Init */
	{
		if ((!command->data[2]) || (command->data[2] > 0x29))
			invalid_mode = 1;
		else if (command->data[3] > 1)
			invalid_count = 1;
		else
			PMCR_Init(command->data[1], command->data[2], command->data[3]);
	}
	else if (command->data[0] == 'E') /* Enable */
	{
		if ((!command->data[2]) || (command->data[2] > 0x29))
			invalid_mode = 1;
		else if (command->data[3] > 1)
			invalid_count = 1;
		else if (command->data[4] > 1)
			invalid_reset = 1;
		else
			PMCR_Enable(command->data[1], command->data[2], command->data[3], command->data[4]);
	}
	else if (command->data[0] == 'B') /* Restart */
	{
		if ((!command->data[2]) || (command->data[2] > 0x29))
			invalid_mode = 1;
		else if (command->data[3] > 1)
			invalid_count = 1;
		else
			PMCR_Restart(command->data[1], command->data[2], command->data[3]);
	}
	else if (command->data[0] == 'R') /* Read */
	{
		if ((!command->data[1]) || (command->data[1] > 2))
		{
			out_message = invalid_read_message;
			i = 24;
		}
		else
		{
			PMCR_Read(command->data[1], (unsigned int *)read_array);
			read = 1;
		}
	}
	else if (command->data[0] == 'G') /* Get Config */
	{
		if ((!command->data[1]) || (command->data[1] > 2))
		{
			out_message = invalid_read_message;
			i = 24;
		}
		else
		{
			*(unsigned short *)getconfig_array = PMCR_Get_Config(command->data[1]);
			read = 2;
		}
	}
	else if (command->data[0] == 'S') /* Stop */
	{
		PMCR_Stop(command->data[1]);
	}
	else if (command->data[0] == 'D') /* Disable */
	{
		PMCR_Disable(command->data[1]);
	}
	else
	{
		out_message = invalid_function_message;
		i = 35;
	}

	/* Error flag checks */
	if (invalid_pmcr)
	{
		out_message = invalid_option_message;
		i = 35;
	}
	else if (invalid_mode)
	{
		out_message = invalid_mode_message;
		i = 22;
	}
	else if (invalid_count)
	{
		out_message = invalid_count_type_message;
		i = 25;
	}
	else if (invalid_reset)
	{
		out_message = invalid_reset_type_message;
		i = 25;
	}

	if (read == 1)
	{
		i = 8;
		out_message = (char *)read_array;
	}
	else if (read == 2)
	{
		i = 2;
		out_message = (char *)getconfig_array;
	}

	memcpy(response->data, out_message, i);
	response->size = htonl(i);

	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN + i, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN + i, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN + i);
}

void cmd_setrtc(ip_header_t *ip, udp_header_t *udp, command_t *command)
{
	unsigned int timestamp = ntohl(command->address);
	const target_ops_t *t = target_get_ops();
	t->set_rtc(timestamp);

	/* Send ACK */
	unsigned char *buffer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
	command_t *response = (command_t *)buffer;
	memcpy(response, command, COMMAND_LEN);

	make_ip(ntohl(ip->src), ntohl(ip->dest), UDP_H_LEN + COMMAND_LEN, IP_UDP_PROTOCOL, (ip_header_t *)(pkt_buf + ETHER_H_LEN), ip->packet_id);
	make_udp(ntohs(udp->src), ntohs(udp->dest), COMMAND_LEN, (ip_header_t *)(pkt_buf + ETHER_H_LEN), (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN));
	bb->tx(pkt_buf, ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + COMMAND_LEN);
}
