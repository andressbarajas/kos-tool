/* client/common/network/network_transport.c */
/*
 * Network transport implementation for kosload client.
 *
 * Based on dcload-ip: dcload-ip/target-src/dcload/dcload.c
 *
 * Handles adapter detection/init, delegates main loop to the
 * network adapter's loop function (which calls process_pkt for
 * incoming packets and dhcp_poll for DHCP management).
 *
 * Platform-independent: DC-specific features (maple) are
 * accessed via platform stubs in the net/ include path.
 */

#include <string.h>
#include <kosload/target.h>
#include <kosload/transport.h>
#include <kosload/info.h>
#include <kosload/net_adapter.h>
#include <kosload/net_stack.h>
#include "kosload.h"
#include "packet.h"
#include <kosload/screensaver.h>

/* IP configuration: default 0.0.0.0 means DHCP mode. */
#ifndef KOSLOAD_IP
#define KOSLOAD_IP "0.0.0.0"
#endif

/* Patchable IP config block.  The host tool can find "KOSLD_IP" inside the
 * loader ELF and replace ip[] without rebuilding:
 *   "0.0.0.0"      -> DHCP
 *   "172.16.0.10"  -> static IP
 */
typedef struct {
    char magic[8];   /* "KOSLD_IP" — host searches for this */
    char ip[16];     /* dotted-quad string, null-terminated */
} ip_config_block_t;

static ip_config_block_t ip_config __attribute__((used, section(".data"))) = {
    .magic = {'K','O','S','L','D','_','I','P'},
    .ip = KOSLOAD_IP
};

static char network_init_error_msg[96] = "NO ETHERNET ADAPTER DETECTED!";

/* From entry.c */
extern volatile bool booted;
extern volatile bool running;
extern unsigned int global_bg_color;

/* From commands.c */
extern unsigned int tool_ip;
extern unsigned short tool_port;

extern kosload_info_t kosload_info;

static void set_ip_from_string(void) {
    unsigned char *ip = (unsigned char *)&our_ip;
    const char *s = ip_config.ip;
    unsigned char i = 0, c = 0;

    /* If we already have an IP (e.g. returning from exception), keep it */
    if(ip[3] != 0)
        return;

    while(s[c] != '\0') {
        if(s[c] == '.') {
            i++;
            c++;
        } else {
            ip[i] *= 10;
            ip[i] += s[c] - '0';
            c++;
        }
    }

    /* IP was parsed in network byte order, convert to host byte order */
    our_ip = ntohl(our_ip);
}

static void network_restore_screen(void) {
    disp_info();
}

static void set_network_init_error(const char *msg) {
    size_t i;

    if(msg == 0)
        msg = "NO ETHERNET ADAPTER DETECTED!";

    for(i = 0; i + 1 < sizeof(network_init_error_msg) && msg[i] != '\0'; i++)
        network_init_error_msg[i] = msg[i];

    network_init_error_msg[i] = '\0';
}

static void set_network_init_error_with_phase(const char *msg) {
    const char *phase = adapter_get_phase_status();
    size_t i = 0;

    if(phase == 0 || phase[0] == '\0') {
        set_network_init_error(msg);
        return;
    }

    if(msg == 0)
        msg = "NO ETHERNET ADAPTER DETECTED!";

    while(i + 1 < sizeof(network_init_error_msg) && phase[i] != '\0') {
        network_init_error_msg[i] = phase[i];
        i++;
    }

    if(i + 3 < sizeof(network_init_error_msg)) {
        network_init_error_msg[i++] = ' ';
        network_init_error_msg[i++] = '|';
        network_init_error_msg[i++] = ' ';
    }

    {
        size_t j = 0;
        while(i + 1 < sizeof(network_init_error_msg) && msg[j] != '\0')
            network_init_error_msg[i++] = msg[j++];
    }

    network_init_error_msg[i] = '\0';
}

static int network_transport_init(void) {
    if(global_bg_color == 0)
        global_bg_color = 0x00100530;

    /* Reset boot state so display is redrawn (needed after program return) */
    booted = false;

    kosload_info.capabilities = KOSLOAD_CAP_NETWORK | KOSLOAD_CAP_GDB | KOSLOAD_CAP_ARGV;
    if(target_get_ops()->cdfs_redir_enable)
        kosload_info.capabilities |= KOSLOAD_CAP_CDFS_REDIR;
    kosload_info.transport = KOSLOAD_TRANSPORT_NETWORK;

    set_network_init_error("NO ETHERNET ADAPTER DETECTED!");

    if(adapter_detect() < 0) {
        set_network_init_error_with_phase(adapter_get_last_error());
        return -1;
    }

    /* Populate info block with adapter MAC */
    memcpy(kosload_info.mac, bb->mac, 6);

    /* Set initial IP address from patchable config block. */
    set_ip_from_string();

    /* Static IP skips DHCP, but the adapter may still need to enable RX so
     * it can answer the host's first VERS packet. */
    if(our_ip != 0)
        adapter_start_static_ip();

    /* Advertise DHCP only when the current config says DHCP.  This is runtime
     * state so a host-patched static ELF reports itself correctly. */
    if(our_ip == 0)
        kosload_info.capabilities |= KOSLOAD_CAP_DHCP;

    screensaver_init(network_restore_screen, global_bg_color);

    return 0;
}

static void network_transport_loop(bool is_main_loop) {
    /* Display adapter/IP info on first entry */
    if(!booted)
        disp_info();

    while(1) {
        if(booted && !screensaver_is_active())
            disp_status("idle...");

        bb->loop(is_main_loop);

        /* If not the main loop, return after one iteration */
        if(!is_main_loop)
            return;
    }
}

static int network_transport_syscall_send(const char cmd_id[4], const uint8_t *payload, size_t payload_len) {
    /* Network syscalls build their own packets directly.
     * This callback is not used by the network transport. */
    (void)cmd_id;
    (void)payload;
    (void)payload_len;
    return -1;
}

static void network_transport_exit_notify(void) {
    /* Handled by the progexit() syscall directly */
}

static void network_transport_stop(void) {
    bb->stop();
}

static void network_transport_start(void) {
    bb->start();
}

const client_transport_ops_t client_network_transport_ops = {
    .name = "network",
    .init_error_msg = network_init_error_msg,
    .init = network_transport_init,
    .loop = network_transport_loop,
    .syscall_send = network_transport_syscall_send,
    .exit_notify = network_transport_exit_notify,
    .stop = network_transport_stop,
    .start = network_transport_start,
};
