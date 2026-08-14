/* client/include/kosload/commands.h
 *
 * Canonical network-command declarations, shared by all platforms.
 *
 * Command-ID strings have a single source of truth in
 * <kosload/protocol.h> (the NET_CMD_* macros) — there are deliberately
 * no CMD_* aliases here; call sites use NET_CMD_* directly.
 *
 * Network struct types (net_command_t, ether/ip/udp_header_t) come from
 * <kosload/protocol.h>, so this header is self-contained.  Consumers that
 * also need the platform packet macros (ETHER_H_LEN, checksum helpers, ...)
 * include the per-console "packet.h" themselves.
 */
#ifndef KOSLOAD_CLIENT_COMMANDS_H
#define KOSLOAD_CLIENT_COMMANDS_H

#include <kosload/protocol.h>

/* command_t is the net stack name for net_command_t */
typedef net_command_t command_t;

extern unsigned int tool_ip;
extern unsigned char tool_mac[6];
extern unsigned short tool_port;
extern unsigned int tool_version;

#define DCTOOL_MAJOR ((tool_version & 0x00ff0000) >> 16)
#define DCTOOL_MINOR ((tool_version & 0x0000ff00) >> 8)
#define DCTOOL_PATCH (tool_version & 0x000000ff)

void cmd_reboot(void);
void cmd_execute(ether_header_t *ether, ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_loadbin(ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_partbin(command_t *command);
void cmd_donebin(ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_sendbinq(ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_sendbin(ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_capabilities(ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_version(ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_retval(ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_maple(ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_pmcr(ip_header_t *ip, udp_header_t *udp, command_t *command);
void cmd_setrtc(ip_header_t *ip, udp_header_t *udp, command_t *command);

#endif /* KOSLOAD_CLIENT_COMMANDS_H */
