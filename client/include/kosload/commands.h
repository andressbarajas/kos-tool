/* client/include/kosload/commands.h */
#ifndef KOSLOAD_CLIENT_COMMANDS_H
#define KOSLOAD_CLIENT_COMMANDS_H

#include <stdint.h>
#include <kosload/protocol.h>

/*
 * Network command handler declarations.
 *
 * These are called by the network stack (net.c) when UDP commands arrive.
 * Only linked into kosload-ip; kosload-serial handles commands inline
 * in its transport layer.
 */

/* Network command handlers */
void cmd_execute(ether_header_t *ether, ip_header_t *ip,
                 udp_header_t *udp, net_command_t *command);
void cmd_loadbin(ip_header_t *ip, udp_header_t *udp, net_command_t *command);
void cmd_partbin(net_command_t *command);
void cmd_donebin(ip_header_t *ip, udp_header_t *udp, net_command_t *command);
void cmd_sendbin(ip_header_t *ip, udp_header_t *udp, net_command_t *command);
void cmd_sendbinq(ip_header_t *ip, udp_header_t *udp, net_command_t *command);
void cmd_version(ip_header_t *ip, udp_header_t *udp, net_command_t *command);
void cmd_retval(ip_header_t *ip, udp_header_t *udp, net_command_t *command);
void cmd_reboot(void);
void cmd_maple(ip_header_t *ip, udp_header_t *udp, net_command_t *command);
void cmd_pmcr(ip_header_t *ip, udp_header_t *udp, net_command_t *command);

/* Shared state (defined in commands.c) */
extern volatile unsigned int our_ip;
extern unsigned int tool_ip;
extern unsigned char tool_mac[6];
extern unsigned short tool_port;
extern unsigned int tool_version;

/* Shared display helpers (defined in network entry.c) */
extern volatile unsigned char booted;
extern volatile unsigned char running;
void disp_info(void);
void disp_status(const char *status);

/* CDFS redirection management (defined in cdfs.c) */
void cdfs_init(void);
void cdfs_enable(void);
void cdfs_disable(void);

/* Syscall helpers (gethostinfo defined in transport-specific syscall files) */
int gethostinfo(unsigned int *ip, unsigned int *port);

#endif /* KOSLOAD_CLIENT_COMMANDS_H */
