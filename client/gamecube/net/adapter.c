/* client/gamecube/net/adapter.c */
/*
 * Network adapter detection for GameCube.
 * Supports BBA (Broadband Adapter) and ENC28J60 (GCNet/ETH2GC).
 */

#include "adapter.h"
#include "bba.h"
#include "enc28j60.h"

/* Loop escape flag, used by all drivers */
volatile unsigned char escape_loop = 0;
int timeout_loop = 0;
int loop_secs_elapsed = 0;

/* The currently configured driver */
adapter_t *bb;

/* Packet receive buffer */
__attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE];
/* Offset by 2 for command->data alignment (same trick as DC) */
__attribute__((aligned(2))) unsigned char *current_pkt = &(raw_current_pkt[2]);

int adapter_detect(void)
{
    if (adapter_enc28j60.detect() >= 0) {
        bb = &adapter_enc28j60;
    } else if (adapter_bba.detect() >= 0) {
        bb = &adapter_bba;
    } else {
        return -1;
    }

    if (bb->init() < 0)
        return -1;

    escape_loop = 0;

    return 0;
}
