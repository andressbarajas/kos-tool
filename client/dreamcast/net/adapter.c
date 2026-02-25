/* client/dreamcast/net/adapter.c */
/*
 * Network adapter detection for Dreamcast.
 * Supports BBA (RTL8139), LAN Adapter (MB86967), and W5500 (SCIF-SPI).
 *
 * Detection priority: BBA > LAN Adapter > W5500
 * (BBA and LAN are tried first as they don't conflict with serial I/O)
 */

#include "adapter.h"
#include "rtl8139.h"
#include "lan_adapter.h"
#include "w5500_spi_dc.h"

/* Loop escape flag, used by all drivers */
volatile unsigned char escape_loop = 0;
int timeout_loop = 0;
int loop_secs_elapsed = 0;

/* The currently configured driver */
adapter_t *bb;

/* Packet receive buffer */
__attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE];
/* Offset by 2 for command->data alignment after Ethernet + IP + UDP headers */
__attribute__((aligned(2))) unsigned char *current_pkt = &(raw_current_pkt[2]);

/*
 * Generate a locally-administered MAC address for W5500 on Dreamcast.
 * Uses the system's hardware Plant and Machine IDs from the flash ROM
 * area at 0x0021A056 (machine code region).
 */
static void w5500_dc_generate_mac(unsigned char *mac)
{
    /* Read 3 bytes from the system flash as a hardware ID seed.
     * This region varies per console, providing uniqueness. */
    volatile unsigned char *sysid = (volatile unsigned char *)0xa021a056;

    /* Locally administered, unicast */
    mac[0] = 0x02;
    mac[1] = 0x57;  /* 'W' for W5500 */
    mac[2] = 0x55;  /* 'U' for W5500 */
    mac[3] = sysid[0];
    mac[4] = sysid[1];
    mac[5] = sysid[2];
}

int adapter_detect(void)
{
    /* Try BBA first (most common, doesn't conflict with serial) */
    if (adapter_bba.detect() >= 0) {
        bb = &adapter_bba;
    } else if (adapter_la.detect() >= 0) {
        bb = &adapter_la;
    } else {
        /* Try W5500 on SCIF-SPI.
         * This takes over the serial port, so it's tried last. */
        w5500_set_spi_ops(&dc_w5500_spi_ops);
        w5500_dc_generate_mac(adapter_w5500.mac);

        if (adapter_w5500.detect() >= 0) {
            bb = &adapter_w5500;
        } else {
            return -1;
        }
    }

    /* Initialize the chosen adapter */
    if (bb->init() < 0)
        return -1;

    escape_loop = 0;

    return 0;
}
