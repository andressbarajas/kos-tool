/* client/playstation2/iop_smap.h
 *
 * EE-side front end for the IOP Ethernet driver.
 *
 * smap.irx runs on the IOP and owns the actual network hardware.  This
 * header exposes the EE calls used by the shared network stack:
 *   - init/bind to the IOP driver
 *   - send one Ethernet frame
 *   - poll one received Ethernet frame
 *   - read status/diagnostics/RTC
 *
 * Data movement is split on purpose: packet bytes use shared IOP RAM and
 * DMA, while slot ownership uses RPC calls.  That avoids the old slim-only
 * mailbox problem.
 */

#ifndef IOP_SMAP_H
#define IOP_SMAP_H

#include <stdint.h>

struct ps2_smap_status_rsp;
struct ps2_smap_cold_diag_rsp;

/*
 * Connect to smap.irx after the bootstrap has loaded it into the IOP.
 * This binds RPC, asks for the shared-memory layout, and verifies the
 * layout version matches this EE code.
 *
 * Returns 0 on success, negative on failure.
 */
int ps2_smap_init(void);

/* Snapshot the IRX status block via GET_STATUS RPC. */
int ps2_smap_get_status(struct ps2_smap_status_rsp *out);

/* Snapshot the IRX cold-diag block via GET_DIAG RPC. */
int ps2_smap_get_diag(struct ps2_smap_cold_diag_rsp *out);

/* Read/write the PS2 CDVD/Mechacon RTC through the IOP SMAP RPC server.
 *
 * Time convention (both sides):
 *   - The value passed and returned is **Unix UTC seconds since epoch**
 *     (1970-01-01 00:00:00 UTC), the same basis as host time(NULL).
 *   - The mechacon hardware itself stores a JST (UTC+9) BCD calendar.
 *     The IRX applies the JST bias once at the boundary, so callers
 *     never deal with timezones.
 *   - get == time(NULL) on a host whose clock is in sync. */
int ps2_smap_get_rtc(uint32_t *unix_timestamp);
int ps2_smap_set_rtc(uint32_t unix_timestamp);

/*
 * Send one Ethernet frame.  Copies the bytes into a free TX slot with SIF
 * DMA, then tells the IOP that slot is ready.
 */
int ps2_smap_send(const void *frame, uint32_t len);

/*
 * Poll at most one received frame.  Returns 0 when a frame was copied,
 * 1 when no frame is waiting, and negative on errors.
 */
int ps2_smap_poll(void *out_buf, uint32_t buf_size, uint32_t *out_len);

/* Cached MAC bytes from GET_STATUS at init time. */
const unsigned char *ps2_smap_mac(void);

/* Cheap status snapshot read directly from shared memory.  No RPC. */
typedef struct ps2_smap_hot_snapshot {
    uint32_t link_state;
    uint32_t rx_head;
    uint32_t rx_tail_iop_view;
    uint32_t tx_slot_state[16];       /* PS2_SMAP_TX_SLOTS (ABI v5) */
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_drops_no_slot;
    uint32_t rx_drops_fcs;
    uint32_t tx_underruns;
    uint32_t heartbeat;
    uint32_t last_rpc_op;
    int32_t  last_rpc_result;
    uint32_t last_release_batch_size; /* size of last RELEASE_RX_BATCH */
    uint32_t last_submit_batch_size;  /* size of last SUBMIT_TX_BATCH (ABI v5) */
    uint32_t ee_rx_tail;              /* EE-side local consumer cursor */
    uint32_t ee_pending_releases;     /* batch-RPC entries staged but not flushed */
    uint32_t ee_pending_submits;      /* TX-batch entries staged but not flushed (ABI v5) */
} ps2_smap_hot_snapshot_t;

int ps2_smap_get_hot_snapshot(ps2_smap_hot_snapshot_t *out);

/*
 * Send any queued RX-release and TX-submit notifications to the IOP.  Call
 * this before leaving the network loop; otherwise RX slots may stay full and
 * queued TX frames may never be transmitted.
 */
int ps2_smap_release_pending(void);

#endif /* IOP_SMAP_H */
