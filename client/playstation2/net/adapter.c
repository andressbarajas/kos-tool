/* client/playstation2/net/adapter.c */
/*
 * Network adapter detection for PlayStation 2.
 * PS2 only has the SMAP adapter (via DEV9 expansion bay).
 */

#include "adapter.h"
#include "smap.h"
#include "../iop_dev9.h"

volatile unsigned char escape_loop = 0;
int timeout_loop = 0;
int loop_secs_elapsed = 0;

adapter_t *bb;

__attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE];
__attribute__((aligned(2))) unsigned char *current_pkt = &(raw_current_pkt[2]);
static const char *last_error = "NO ETHERNET ADAPTER DETECTED!";

int adapter_detect(void) {
	if (adapter_smap.detect() >= 0) {
		bb = &adapter_smap;
		if (bb->init() < 0) {
			last_error = "SMAP INIT FAILED!";
			return -1;
		}
		last_error = "NO ETHERNET ADAPTER DETECTED!";
		escape_loop = 0;
		return 0;
	}

	last_error = smap_get_last_error();
	return -1;
}

const char *adapter_get_last_error(void)
{
	return last_error;
}

const char *adapter_get_phase_status(void)
{
	/* When DEV9 / SMAP bringup fails, the network-init error screen
	 * prefixes our last phase-status string onto the user-facing
	 * "DEV9 INIT FAILED" message so it's clear which sub-step died
	 * (e.g. "PHASE1: IOP RESET FAIL | DEV9 INIT FAILED").  On a
	 * successful boot this string is just "PHASE1: SMAP LOADED" and
	 * is never displayed (we only render network_init_error_msg on
	 * the red error path).  See network_transport.c:
	 * set_network_init_error_with_phase. */
	return 0;//iop_dev9_phase1_status();
}

void adapter_start_static_ip(void)
{
	if (bb == &adapter_smap)
		bb->start();
}
