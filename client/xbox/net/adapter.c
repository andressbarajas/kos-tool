/* client/xbox/net/adapter.c
 *
 * Network adapter detection for the original Xbox.  The Xbox has a single
 * onboard NIC: the nForce (NVnet) Ethernet MAC, so detection is a one-entry
 * probe, mirroring the PS2 SMAP adapter.c.
 */

#include "adapter.h"
#include "nvnet.h"

#include <kosload/target.h>

volatile unsigned char escape_loop = 0;
int timeout_loop = 0;
int loop_secs_elapsed = 0;

adapter_t *bb;

__attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE];
__attribute__((aligned(2))) unsigned char *current_pkt = &(raw_current_pkt[2]);
static const char *last_error = "NO ETHERNET ADAPTER DETECTED!";

int adapter_detect(void) {
    if(adapter_nvnet.detect() >= 0) {
        bb = &adapter_nvnet;
        if(bb->init() < 0) {
            last_error = "NVNET INIT FAILED!";
            return -1;
        }
        last_error = "NO ETHERNET ADAPTER DETECTED!";
        escape_loop = 0;
        return 0;
    }

    last_error = nvnet_get_last_error();
    return -1;
}

const char *adapter_get_last_error(void) {
    return last_error;
}

const char *adapter_get_phase_status(void) {
    return 0;
}

void adapter_start_static_ip(void) {
    if(bb == &adapter_nvnet)
        bb->start();
}
