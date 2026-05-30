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

#include <stdint.h>

#define DC_HW_TYPE_RETAIL      0x0
#define DC_SYSMODE_REG         0xa05f74b0
#define DC_SYSINFO_VECTOR      0x8c0000b0
#define DC_SYSINFO_FUNC_INIT   0
#define DC_SYSINFO_FUNC_ID     3
#define DC_FALLBACK_ID_ADDR    0xa021a056

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
static const char *last_error = "NO ETHERNET ADAPTER DETECTED!";

static int dc_hardware_type(void) {
    uint32_t sysmode = *(volatile uint32_t *)DC_SYSMODE_REG;

    return (int)((sysmode >> 4) & 0x0f);
}

static int dc_sysinfo_id(uint64_t *id) {
    uintptr_t sysinfo = *(volatile uintptr_t *)DC_SYSINFO_VECTOR;
    uint64_t *id_ptr;

    if(sysinfo == 0 || sysinfo == (uintptr_t)-1)
        return -1;

    ((void (*)(int, int, int, int))sysinfo)(0, 0, 0, DC_SYSINFO_FUNC_INIT);
    id_ptr = ((uint64_t *(*)(int, int, int, int))sysinfo)(0, 0, 0, DC_SYSINFO_FUNC_ID);

    if(id_ptr == 0)
        return -1;

    *id = *id_ptr;

    if(*id == 0 || *id == (uint64_t)-1)
        return -1;

    return 0;
}

static int w5500_dc_generate_sysinfo_mac(unsigned char *mac) {
    uint64_t id;
    unsigned char *id_bytes = (unsigned char *)&id;

    if(dc_hardware_type() != DC_HW_TYPE_RETAIL)
        return -1;

    if(dc_sysinfo_id(&id) < 0)
        return -1;

    /* Locally administered, unicast. */
    mac[0] = 0x02;
    mac[1] = id_bytes[3];
    mac[2] = id_bytes[4];
    mac[3] = id_bytes[5];
    mac[4] = id_bytes[6];
    mac[5] = id_bytes[7];

    return 0;
}

static void w5500_dc_generate_fallback_mac(unsigned char *mac) {
    volatile unsigned char *sysid = (volatile unsigned char *)DC_FALLBACK_ID_ADDR;

    /* Locally administered, unicast. */
    mac[0] = 0x02;
    mac[1] = 0x57; /* 'W' */
    mac[2] = 0x55; /* 'U' */
    mac[3] = sysid[0];
    mac[4] = sysid[1];
    mac[5] = sysid[2];
}

/*
 * Generate a locally-administered MAC address for the W5500.
 *
 * Retail Dreamcasts expose a unique 64-bit ID through the BIOS sysinfo
 * syscall. The raw 0xa021a056 seed is retained only as a fallback for
 * non-retail/unknown systems or failed sysinfo reads.
 */
static void w5500_dc_generate_mac(unsigned char *mac) {
    if(w5500_dc_generate_sysinfo_mac(mac) < 0)
        w5500_dc_generate_fallback_mac(mac);
}

int adapter_detect(void) {
    /* Try BBA first (most common, doesn't conflict with serial) */
    if(adapter_bba.detect() >= 0) {
        bb = &adapter_bba;
    } else if(adapter_la.detect() >= 0) {
        bb = &adapter_la;
    } else {
        /* Try W5500 on SCIF-SPI.
         * This takes over the serial port, so it's tried last. */
        w5500_set_spi_ops(&dc_w5500_spi_ops);
        w5500_dc_generate_mac(adapter_w5500.mac);

        if(adapter_w5500.detect() >= 0) {
            bb = &adapter_w5500;
        } else {
            last_error = "NO ETHERNET ADAPTER DETECTED!";
            return -1;
        }
    }

    /* Initialize the chosen adapter */
    if(bb->init() < 0) {
        last_error = "NETWORK ADAPTER INIT FAILED!";
        return -1;
    }

    last_error = "NO ETHERNET ADAPTER DETECTED!";

    escape_loop = 0;

    return 0;
}

const char *adapter_get_last_error(void) {
    return last_error;
}

const char *adapter_get_phase_status(void) {
    return 0;
}

void adapter_start_static_ip(void) {
}
