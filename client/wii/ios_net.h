/* client/wii/ios_net.h - clean-room boundary for Wii IOS socket services */
#ifndef KOSLOAD_WII_IOS_NET_H
#define KOSLOAD_WII_IOS_NET_H

#include <stdint.h>

#define WII_NET_MAX_UDP_PAYLOAD 1472

#define WII_NET_INTERFACE_UNKNOWN 0
#define WII_NET_INTERFACE_WIFI    1
#define WII_NET_INTERFACE_WIRED   2

#define WII_NET_LINK_UNKNOWN     (-1)
#define WII_NET_LINK_DOWN        0
#define WII_NET_LINK_UP          1

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    int ip_source_dhcp;      /* 1 = DHCP, 0 = manual, -1 = unknown */
    int interface_type;      /* WII_NET_INTERFACE_* */
} wii_net_config_t;

const char *wii_ios_net_last_error(void);
int wii_ios_net_init(wii_net_config_t *config);
void wii_ios_net_shutdown(void);
int wii_ios_net_recvfrom(uint8_t *payload, uint32_t payload_capacity,
                         uint32_t *src_ip, uint16_t *src_port);
int wii_ios_net_sendto(const uint8_t *payload, uint32_t payload_len,
                       uint32_t dest_ip, uint16_t dest_port);

/* Opt 0xC001 = DHCP lease seconds (WiiBrew); <0 = IOS rc.  Does IOS
 * IPC — only call at init / re-init, never from the data-path loop. */
int wii_ios_net_lease_secs(void);

/* Re-query the active interface's link state via SOGetInterfaceOpt 0x1005
 * and update the cached value.  Cheap (single ioctlv); meant to be called
 * periodically from the idle loop to detect WiFi disassociation or wired
 * adapter unplug.  Returns the new WII_NET_LINK_* status or <0 on IOS error. */
int wii_ios_net_poll_link_state(void);

/* SOPoll on the bound UDP socket waiting for POLLIN.  Returns 1 if data is
 * available to read, 0 if the timeout elapsed with no data, <0 on IOS error.
 * Lets the idle loop pace itself without spinning: IOS does the wait, the
 * loop wakes either on data arrival or after timeout_ms.  Direct analog of
 * the MMIO link/status polls DC/GC/PS2 do at the top of their adapter loops. */
int wii_ios_net_poll_recv(int timeout_ms);

#endif /* KOSLOAD_WII_IOS_NET_H */
