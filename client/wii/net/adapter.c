/* client/wii/net/adapter.c
 *
 * Socket-backed adapter facade for Wii IOS networking.
 *
 * The existing kosload network path consumes Ethernet/IP/UDP frames.  IOS
 * sockets expose UDP payloads.  This adapter bridges the two by synthesizing
 * inbound Ethernet/IP/UDP headers and unwrapping outbound packets back to UDP
 * sends.  The actual IOS IPC calls live in ../ios_net.c.
 */

#include <string.h>
#include <stdint.h>
#include <kosload/protocol.h>
#include <kosload/info.h>
#include <kosload/net_stack.h>
#include <kosload/screensaver.h>
#include <kosload/target.h>
#include "adapter.h"
#include "kosload.h"
#include "packet.h"
#include "../ios_net.h"

volatile unsigned char escape_loop = 0;
static uint64_t wii_next_link_poll;
static int wii_last_link = -1;
static int wii_interface_type = WII_NET_INTERFACE_UNKNOWN;

/* Tick when it's safe to resume IOS IPCs after a link-up event.  IOS spends
 * several seconds internally re-running DHCP DISCOVER/OFFER/REQUEST/ACK once
 * the cable is replugged. */
static uint64_t wii_resume_ipcs_at;
#define WII_LINK_UP_SETTLE_MS 5000

adapter_t *bb;

__attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE];
__attribute__((aligned(2))) unsigned char *current_pkt = &(raw_current_pkt[2]);

static const unsigned char synthetic_host_mac[6] = { 0x02, 0x57, 0x49, 0x49, 0x10, 0x01 };
static const char *last_error = "Wii IOS socket service unavailable";
static char adapter_name[32] = "Internal Wi-Fi";

extern volatile unsigned int our_ip;
extern volatile unsigned int installed_adapter;
extern void process_pkt(unsigned char *pkt);
extern kosload_info_t kosload_info;

static int wii_adapter_init(void);
static void wii_adapter_start(void);
static void wii_adapter_stop(void);
static void wii_adapter_loop(bool is_main_loop);
static int wii_adapter_tx(unsigned char *pkt, int len);

static void set_adapter_name_for_interface(int interface_type)
{
    const char *name = "LAN Adapter (RVL-015)";
    unsigned int i;

    if (interface_type == WII_NET_INTERFACE_WIRED)
        name = "LAN Adapter (RVL-015)";
    else if (interface_type == WII_NET_INTERFACE_WIFI)
        name = "Internal Wi-Fi";

    for (i = 0; i + 1 < sizeof(adapter_name) && name[i] != '\0'; i++)
        adapter_name[i] = name[i];
    adapter_name[i] = '\0';
}

adapter_t adapter_wii_ios = {
    .name = adapter_name,
    .mac = { 0x02, 0x57, 0x49, 0x49, 0x00, 0x01 },
    .pad = { 0, 0 },
    .detect = wii_adapter_init,
    .init = wii_adapter_init,
    .start = wii_adapter_start,
    .stop = wii_adapter_stop,
    .loop = wii_adapter_loop,
    .tx = wii_adapter_tx,
};

static void synthesize_udp_frame(const uint8_t *payload, uint32_t payload_len,
                                 uint32_t src_ip, uint16_t src_port)
{
    ether_header_t *ether = (ether_header_t *)current_pkt;
    ip_header_t *ip = (ip_header_t *)(current_pkt + ETHER_H_LEN);
    udp_header_t *udp = (udp_header_t *)(current_pkt + ETHER_H_LEN + IP_H_LEN);

    if (payload_len > WII_NET_MAX_UDP_PAYLOAD)
        return;

    make_ether((unsigned char *)bb->mac, (unsigned char *)synthetic_host_mac, ether);
    make_ip(our_ip, src_ip, UDP_H_LEN + (int)payload_len, IP_UDP_PROTOCOL, ip, 0);
    memcpy(udp->data, payload, payload_len);
    make_udp(NET_DEFAULT_PORT, src_port, (int)payload_len, ip, udp);

    process_pkt(current_pkt);
}

static int wii_adapter_init(void)
{
    wii_net_config_t config;

    if (wii_ios_net_init(&config) < 0) {
        last_error = wii_ios_net_last_error();
        return -1;
    }

    set_adapter_name_for_interface(config.interface_type);
    wii_interface_type = config.interface_type;
    memcpy(adapter_wii_ios.mac, config.mac, sizeof(adapter_wii_ios.mac));
    our_ip = config.ip;

    bb = &adapter_wii_ios;
    /* Self-report the adapter ID like every other platform's NIC driver.
     * Without this it stays 0, which the host's prepare_comms() treats as
     * its "not yet initialized" sentinel — defeating the one-shot VERS
     * handshake cache, so it re-handshakes (and reprints the version
     * banner) on every round-trip. */
    installed_adapter = ADAPTER_WII_LAN;
    if (config.ip_source_dhcp)
        kosload_info.capabilities |= KOSLOAD_CAP_DHCP;
    escape_loop = 0;
    last_error = "Wii IOS socket service unavailable";
    return 0;
}

static void wii_adapter_start(void)
{
}

static void wii_adapter_stop(void)
{
}

static uint64_t wii_ms_to_ticks(const target_ops_t *t, unsigned int ms)
{
    return ((uint64_t)t->ticks_per_second * ms) / 1000;
}

static void wii_poll_link_change(void)
{
    const target_ops_t *t = target_get_ops();
    uint64_t now;
    int link_up;

    /* WiFi has no meaningful "cable plugged" signal */
    if (wii_interface_type != WII_NET_INTERFACE_WIRED)
        return;

    now = t->get_ticks();

    if (wii_next_link_poll && now < wii_next_link_poll)
        return;

    wii_next_link_poll = now + wii_ms_to_ticks(t, 100);
    link_up = wii_ios_net_poll_link_state();

    if (wii_last_link < 0 || link_up != wii_last_link) {
        int was_down = (wii_last_link == 0);
        wii_last_link = link_up;

        screensaver_wake();

        if (link_up) {
            disp_status("idle...");
            wii_ios_net_shutdown();
            wii_adapter_init();
            /* Arm the post-replug settle only on a real down→up edge.  The
             * initial wii_last_link = -1 → 1 path is the cold-boot init, not
             * a recovery, so no settle needed there. */
            if (was_down)
                wii_resume_ipcs_at = now + wii_ms_to_ticks(t, WII_LINK_UP_SETTLE_MS);
        } else {
            disp_status("link lost...");
        }
    }
}

/* Mirror the DC/GC/BBA adapter .loop contract (cf. bba_loop): a syscall
 * does build_send_packet(...); bb->loop(0); and relies on that single
 * call servicing inbound packets until the host's NET_CMD_RETVAL sets
 * escape_loop (cmd_retval, commands.c).  The host pulls every
 * write()/read() buffer back with a re-requestable, multi-packet
 * download — a single recvfrom cannot service that, so the old
 * single-shot version collapsed any exchange needing more than one
 * inbound packet: the host's re-requests went unanswered and it gave
 * up after 10 passes.  Spin until escape_loop, then clear it for the
 * next call, exactly as bba_loop does.  At the top (idle) level
 * escape_loop is never set, so this is the main loop and never
 * returns; nested syscall waits exit when cmd_retval fires.
 *
 * Two distinct shapes, by design:
 *   - is_main_loop=true  : non-blocking SOPoll check + ~1 Hz lease/link
 *                          refresh + screensaver_poll.  The 1 Hz tick
 *                          owns the DHCP-lease display directly — IOS
 *                          handles renewal, we just paint the current
 *                          remaining seconds via SOGetInterfaceOpt
 *                          0xC001 each second, no seed-and-decrement
 *                          state machine like the other consoles.
 *   - is_main_loop=false : pure blocking recvfrom in a tight loop, no
 *                          IPC overhead beyond what each inbound packet
 *                          costs.  This is the known-good cb176b5
 *                          baseline.  SOPoll's struct shape (and the
 *                          ip/top ioctl 0x0B cmd id) was never verified
 *                          against an allowed binary; do not guess this
 *                          in the upload path. */
static void wii_adapter_loop(bool is_main_loop)
{
    static uint64_t last_sec_tick = 0;
    const target_ops_t *t = target_get_ops();
    uint8_t payload[WII_NET_MAX_UDP_PAYLOAD];
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    int received;

    if (!is_main_loop) {
        /* Nested syscall wait: pure blocking recvfrom in a tight loop. */
        while (!escape_loop) {
            received = wii_ios_net_recvfrom(payload, sizeof(payload),
                                            &src_ip, &src_port);
            if (received > 0)
                synthesize_udp_frame(payload, (uint32_t)received,
                                     src_ip, src_port);
        }
        escape_loop = 0;
        return;
    }

    while (!escape_loop) {
        uint64_t now = (t != 0) ? t->get_ticks() : 0;
        /* After a cable-replug, IOS is internally busy re-running DHCP for
         * several seconds. Pause every hot-path IOS call during the settle window. */
        int ipcs_paused = (t != 0 && wii_resume_ipcs_at && now < wii_resume_ipcs_at);

        /* Non-blocking SOPoll (ioctl 0x0B with timeout=0): asks IOS
         * "is data ready?". */
        if (!ipcs_paused && wii_ios_net_poll_recv(0) > 0) {
            received = wii_ios_net_recvfrom(payload, sizeof(payload),
                                            &src_ip, &src_port);
            if (received > 0)
                synthesize_udp_frame(payload, (uint32_t)received,
                                     src_ip, src_port);
        }

        if (!ipcs_paused)
            wii_poll_link_change();

        /* ~1 Hz: repaint the DHCP-lease line from IOS's current
         * remaining-seconds (also gated by the post-replug settle). */
        if (!ipcs_paused && t != 0) {
            if (now - last_sec_tick >= t->ticks_per_second) {
                last_sec_tick = now;

                if ((kosload_info.capabilities & KOSLOAD_CAP_DHCP) &&
                    !screensaver_is_active()) {
                    int lease = wii_ios_net_lease_secs();
                    if (lease > 0)
                        update_lease_time_display((unsigned int)lease);
                }
            }
        }

        screensaver_poll();
    }
    escape_loop = 0;
}

static int wii_adapter_tx(unsigned char *pkt, int len)
{
    ip_header_t *ip;
    udp_header_t *udp;
    uint32_t udp_len;
    uint32_t payload_len;

    if (len < ETHER_H_LEN + IP_H_LEN + UDP_H_LEN)
        return -1;

    ip = (ip_header_t *)(pkt + ETHER_H_LEN);
    if (ip->protocol != IP_UDP_PROTOCOL)
        return -1;

    udp = (udp_header_t *)(pkt + ETHER_H_LEN + IP_H_LEN);
    udp_len = ntohs(udp->length);
    if (udp_len < UDP_H_LEN)
        return -1;

    payload_len = udp_len - UDP_H_LEN;
    return wii_ios_net_sendto(udp->data, payload_len, ntohl(ip->dest),
                              ntohs(udp->dest));
}

int adapter_detect(void)
{
    bb = &adapter_wii_ios;
    return adapter_wii_ios.detect();
}

const char *adapter_get_last_error(void)
{
    return last_error;
}

/* IOS handles network bring-up opaquely on Wii — there's no incremental
 * phase (DHCP DISCOVER, REQUEST, ...) to surface like the other consoles.
 * Return an empty string so the shared entry.c phase-display logic stays
 * uniform without platform ifdefs. */
const char *adapter_get_phase_status(void)
{
    return "";
}

void adapter_start_static_ip(void)
{
    if (bb)
        bb->start();
}
