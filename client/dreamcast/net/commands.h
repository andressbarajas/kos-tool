/* client/dreamcast/net/commands.h */
/*
 * Command definitions for network protocol.
 * Based on dcload-ip: dcload-ip/target-src/dcload/commands.h
 * Types come from <kosload/protocol.h> via packet.h.
 */

#ifndef __COMMANDS_H__
#define __COMMANDS_H__

#include "packet.h"

/* command_t is the DC net stack name for net_command_t */
typedef net_command_t command_t;

#define CMD_EXECUTE  "EXEC"
#define CMD_LOADBIN  "LBIN"
#define CMD_PARTBIN  "PBIN"
#define CMD_DONEBIN  "DBIN"
#define CMD_SENDBIN  "SBIN"
#define CMD_SENDBINQ "SBIQ"
#define CMD_VERSION  "VERS"
#define CMD_CAPABILITIES "CAPS"
#define CMD_RETVAL   "RETV"
#define CMD_REBOOT   "RBOT"
#define CMD_MAPLE    "MAPL"
#define CMD_PMCR     "PMCR"
#define CMD_SETRTC   "SRTC"

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

#endif /* __COMMANDS_H__ */
