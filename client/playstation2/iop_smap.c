/* client/playstation2/iop_smap.c
 *
 * EE-side client for the IOP SMAP driver.
 *
 * Plain version:
 *   - The IOP owns the Ethernet hardware.
 *   - The EE asks the IOP to do control actions with SIFRPC.
 *   - The IOP publishes received packets in a shared IOP-RAM area.
 *   - The EE copies transmit packet bytes into that same area with SIF DMA,
 *     then tells the IOP which TX slot is ready.
 *
 * One important rule keeps this working on PS2 slim: the EE does not write
 * normal shared-memory "command fields" for the IOP to poll.  Those writes
 * were the source of the slim-only mailbox bug.  Control changes go through
 * RPC; bulk packet bytes go through DMA.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "ee_sif.h"
#include "iop_smap.h"
#include "iop/smap_protocol.h"
#include "ps2_memory_map.h"
#include "video.h"   /* DEBUG: on-screen dump-and-halt */

/* Convert an IOP RAM address into the EE address that reads the same bytes.
 * IOP RAM is only 2 MB; from the EE it appears under the 0xBC000000 bridge. */
#define EE_IOP_UNCACHED(iop_phys) \
    ((volatile void *)(0xBC000000u | ((uint32_t)(iop_phys) & 0x1FFFFFu)))

/* Maximum number of bind retries.  ps2-load-ip's other clients use 64. */
#define SMAP_BIND_ATTEMPTS  64u

/* Bounded busy-poll budget for the SMAP RPC-ready sentinel.  Same order
 * of magnitude as the dev9 mailbox poll in iop_bootstrap.c (which the
 * comment there documents as covering a few seconds on real hardware).
 * smap_rpc_thread publishes READY within milliseconds of _start
 * returning once the IOP scheduler runs it, so in practice this passes
 * on the first read; the cap only exists so a never-ready IRX fails
 * cleanly instead of hanging. */
#define SMAP_READY_POLL_BUDGET  200000000

/* Static state — populated by ps2_smap_init() and used by the
 * subsequent send/poll calls. */
static volatile ee_sif_rpc_client_t smap_client
    __attribute__((aligned(64)));

/* RPC scratch buffers — must be 16-byte aligned and stay alive across
 * the call (the SIFRPC stack DMAs reply bytes into them). */
static ps2_smap_layout_rsp_t           smap_layout_rsp        __attribute__((aligned(64)));
static ps2_smap_status_rsp_t           smap_status_rsp        __attribute__((aligned(64)));
static ps2_smap_cold_diag_rsp_t        smap_cold_diag_rsp     __attribute__((aligned(64)));
static ps2_smap_rtc_req_t              smap_rtc_req           __attribute__((aligned(64)));
static ps2_smap_rtc_rsp_t              smap_rtc_rsp           __attribute__((aligned(64)));
static ps2_smap_submit_tx_req_t        smap_submit_tx_req     __attribute__((aligned(64)));
static ps2_smap_release_rx_batch_req_t smap_release_batch_req __attribute__((aligned(64)));
static ps2_smap_submit_tx_batch_req_t  smap_submit_batch_req  __attribute__((aligned(64)));
static ps2_smap_rc_rsp_t               smap_rc_rsp            __attribute__((aligned(64)));

/* RX frames are released in batches.  ps2_smap_poll() records which slots
 * it consumed, and ps2_smap_release_pending() sends one RPC with the whole
 * list.  Callers must flush this before leaving the network loop so the IOP
 * knows those RX slots can be reused. */
static struct {
    uint32_t slot;
    uint32_t seq;
} g_pending_releases[PS2_SMAP_RELEASE_BATCH_MAX];
static uint32_t g_pending_release_count;

/* TX frames are submitted in batches too.  ps2_smap_send() copies packet
 * bytes into a free TX slot, then records {slot, len, seq} here.  The slot
 * is marked as "claimed" locally until the batched SUBMIT RPC reaches the
 * IOP, which prevents the EE from choosing the same slot twice. */
static struct {
    uint32_t slot;
    uint32_t len;
    uint32_t seq;
} g_pending_submits[PS2_SMAP_SUBMIT_BATCH_MAX];
static uint32_t g_pending_submit_count;
/* Bitmask of slots staged but not yet submitted.  Up to 16 slots. */
static uint32_t g_tx_claimed_mask;

/* Per-slot sequence numbers catch stale or duplicate TX submissions.  The
 * EE bumps its copy when staging a slot; the IOP bumps its copy when it
 * accepts that slot. */
static uint32_t g_ee_tx_seq[16];
static int      ps2_smap_flush_tx(void);

/* Cached layout fields, populated once at init time. */
static int      g_layout_valid;
static uint32_t g_shared_iop_phys;
static uint32_t g_shared_size;
static uint32_t g_rx_slot_count;
static uint32_t g_rx_slot_size;
static uint32_t g_tx_slot_count;
static uint32_t g_tx_slot_size;
static uint32_t g_rx_ring_offset;
static uint32_t g_rx_data_offset;
static uint32_t g_tx_state_offset;
static uint32_t g_tx_data_offset;
static uint32_t g_diag_offset;

/* Volatile EE-uncached views of the shared region's IOP-write fields. */
static volatile ps2_smap_hot_diag_t *g_hot_diag_view;
static volatile ps2_smap_rx_desc_t  *g_rx_ring_view;
static volatile uint8_t             *g_rx_data_view;
static volatile uint32_t            *g_tx_state_view;

/* EE-side RX consumer cursor.  IOP advances rx_head; we advance our
 * own local rx_tail as we drain frames (and ack via RELEASE_RX). */
static uint32_t g_rx_tail;

/* Round-robin TX slot pointer. */
static uint32_t g_tx_next_slot;

/* Cached MAC from GET_STATUS at init. */
static unsigned char g_mac[6];

static int rpc_call_sync(int fno, const void *send, uint32_t send_size,
                         void *recv, uint32_t recv_size)
{
    return ee_sif_rpc_call(&smap_client, fno, send, send_size,
                           recv, recv_size);
}

int ps2_smap_init(void)
{
    int rc;
    int i;

    memset((void *)&smap_client, 0, sizeof(smap_client));

    /* Gate the bind on smap.irx's RPC-ready sentinel.  smap.irx's _start
     * returns RESIDENT_END before its smap_rpc_thread has run
     * RpcRegister() + smap_register_queue_workaround(); LMB-return does
     * NOT mean the server is bindable.  On cold boot IOP-IPL timing hides
     * this, but on -F (IOP reset mid-session) the EE can bind a not-yet-
     * registered server and get a garbage server/buf that wild-DMAs (the
     * observed TLBS fault).  The sentinel is the load-bearing fix here: a
     * numeric range check on server/buf can't catch it because the bad
     * value lands inside IOP RAM.  See PS2_SMAP_READY_* in
     * smap_protocol.h.  The bootstrap pre-cleared this word before the
     * smap.irx LMB load, so a stale READY can't survive the IOP reset. */
    {
        volatile uint32_t *ready =
            (volatile uint32_t *)EE_IOP_UNCACHED(PS2_SMAP_READY_IOP_PHYS);
        uint32_t spins = 0;
        while (*ready != PS2_SMAP_READY_MAGIC) {
            if (++spins >= SMAP_READY_POLL_BUDGET)
                return -6;   /* server never came up — fail cleanly */
        }
    }

    if (ee_sif_rpc_bind_retry(&smap_client, PS2_SMAP_RPC_ID,
                              SMAP_BIND_ATTEMPTS) < 0)
        return -1;

    memset(&smap_layout_rsp, 0, sizeof(smap_layout_rsp));
    rc = rpc_call_sync(PS2_SMAP_FNO_GET_LAYOUT,
                       0, 0,
                       &smap_layout_rsp, sizeof(smap_layout_rsp));
    if (rc < 0)
        return -2;

    if (smap_layout_rsp.rc != PS2_SMAP_RC_OK)
        return -3;
    if (smap_layout_rsp.magic != PS2_SMAP_LAYOUT_MAGIC)
        return -4;
    if (smap_layout_rsp.abi_version != PS2_SMAP_ABI_VERSION)
        return -5;

    g_shared_iop_phys = smap_layout_rsp.shared_iop_phys;
    g_shared_size     = smap_layout_rsp.shared_size;
    g_rx_slot_count   = smap_layout_rsp.rx_slot_count;
    g_rx_slot_size    = smap_layout_rsp.rx_slot_size;
    g_tx_slot_count   = smap_layout_rsp.tx_slot_count;
    g_tx_slot_size    = smap_layout_rsp.tx_slot_size;
    g_rx_ring_offset  = smap_layout_rsp.rx_ring_offset;
    g_rx_data_offset  = smap_layout_rsp.rx_data_offset;
    g_tx_state_offset = smap_layout_rsp.tx_state_offset;
    g_tx_data_offset  = smap_layout_rsp.tx_data_offset;
    g_diag_offset     = smap_layout_rsp.diag_offset;

    g_hot_diag_view = (volatile ps2_smap_hot_diag_t *)
        EE_IOP_UNCACHED(g_shared_iop_phys + g_diag_offset);
    g_rx_ring_view  = (volatile ps2_smap_rx_desc_t *)
        EE_IOP_UNCACHED(g_shared_iop_phys + g_rx_ring_offset);
    g_rx_data_view  = (volatile uint8_t *)
        EE_IOP_UNCACHED(g_shared_iop_phys + g_rx_data_offset);
    g_tx_state_view = (volatile uint32_t *)
        EE_IOP_UNCACHED(g_shared_iop_phys + g_tx_state_offset);

    g_rx_tail      = 0u;
    g_tx_next_slot = 0u;
    g_pending_release_count = 0u;
    g_pending_submit_count  = 0u;
    g_tx_claimed_mask       = 0u;
    for (i = 0; i < 16; i++)
        g_ee_tx_seq[i] = 0u;

    /* Snapshot status (mainly for the cached MAC). */
    memset(&smap_status_rsp, 0, sizeof(smap_status_rsp));
    rc = rpc_call_sync(PS2_SMAP_FNO_GET_STATUS,
                       0, 0,
                       &smap_status_rsp, sizeof(smap_status_rsp));
    if (rc >= 0 && smap_status_rsp.rc == PS2_SMAP_RC_OK) {
        int i;
        for (i = 0; i < 6; i++)
            g_mac[i] = smap_status_rsp.mac[i];
    }

    g_layout_valid = 1;
    return 0;
}

int ps2_smap_get_status(ps2_smap_status_rsp_t *out)
{
    int rc;

    if (!g_layout_valid || out == 0)
        return -1;

    memset(&smap_status_rsp, 0, sizeof(smap_status_rsp));
    rc = rpc_call_sync(PS2_SMAP_FNO_GET_STATUS,
                       0, 0,
                       &smap_status_rsp, sizeof(smap_status_rsp));
    if (rc < 0)
        return -2;
    memcpy(out, &smap_status_rsp, sizeof(*out));
    return 0;
}

int ps2_smap_get_diag(ps2_smap_cold_diag_rsp_t *out)
{
    int rc;

    if (!g_layout_valid || out == 0)
        return -1;

    memset(&smap_cold_diag_rsp, 0, sizeof(smap_cold_diag_rsp));
    rc = rpc_call_sync(PS2_SMAP_FNO_GET_DIAG,
                       0, 0,
                       &smap_cold_diag_rsp, sizeof(smap_cold_diag_rsp));
    if (rc < 0)
        return -2;
    memcpy(out, &smap_cold_diag_rsp, sizeof(*out));
    return 0;
}

int ps2_smap_get_rtc(uint32_t *unix_timestamp)
{
    int rc;

    if (!g_layout_valid || unix_timestamp == 0)
        return -1;

    memset(&smap_rtc_rsp, 0, sizeof(smap_rtc_rsp));
    rc = rpc_call_sync(PS2_SMAP_FNO_GET_RTC,
                       0, 0,
                       &smap_rtc_rsp, sizeof(smap_rtc_rsp));
    if (rc < 0)
        return -2;
    if (smap_rtc_rsp.rc != PS2_SMAP_RC_OK)
        return -3;

    *unix_timestamp = smap_rtc_rsp.unix_timestamp;
    return 0;
}

int ps2_smap_set_rtc(uint32_t unix_timestamp)
{
    int rc;

    if (!g_layout_valid)
        return -1;

    memset(&smap_rtc_req, 0, sizeof(smap_rtc_req));
    memset(&smap_rc_rsp, 0, sizeof(smap_rc_rsp));
    smap_rtc_req.unix_timestamp = unix_timestamp;

    rc = rpc_call_sync(PS2_SMAP_FNO_SET_RTC,
                       &smap_rtc_req, sizeof(smap_rtc_req),
                       &smap_rc_rsp, sizeof(smap_rc_rsp));
    if (rc < 0)
        return -2;
    if (smap_rc_rsp.rc != PS2_SMAP_RC_OK)
        return -3;

    return 0;
}

const unsigned char *ps2_smap_mac(void)
{
    return g_mac;
}

int ps2_smap_get_hot_snapshot(ps2_smap_hot_snapshot_t *out)
{
    int i;

    if (out == 0)
        return -1;
    if (!g_layout_valid || g_hot_diag_view == 0)
        return -1;

    out->link_state        = g_hot_diag_view->link_state;
    out->rx_head           = g_hot_diag_view->rx_head;
    out->rx_tail_iop_view  = g_hot_diag_view->rx_tail_iop_view;
    /* tx_slot_state[] is a 16-element snapshot in ABI v5. */
    for (i = 0; i < 16; i++)
        out->tx_slot_state[i] = g_hot_diag_view->tx_slot_state[i];
    out->rx_packets             = g_hot_diag_view->rx_packets;
    out->tx_packets             = g_hot_diag_view->tx_packets;
    out->rx_drops_no_slot       = g_hot_diag_view->rx_drops_no_slot;
    out->rx_drops_fcs           = g_hot_diag_view->rx_drops_fcs;
    out->tx_underruns           = g_hot_diag_view->tx_underruns;
    out->heartbeat              = g_hot_diag_view->heartbeat;
    out->last_rpc_op            = g_hot_diag_view->last_rpc_op;
    out->last_rpc_result        = g_hot_diag_view->last_rpc_result;
    out->last_release_batch_size = g_hot_diag_view->last_release_batch_size;
    out->last_submit_batch_size  = g_hot_diag_view->last_submit_batch_size;
    out->ee_rx_tail             = g_rx_tail;
    out->ee_pending_releases    = g_pending_release_count;
    out->ee_pending_submits     = g_pending_submit_count;
    return 0;
}

/* Submit a single TX entry via the legacy fno-5 path.  Used as a
 * fallback when the batch RPC fails — re-issues per-entry so we can
 * isolate which one (if any) the IRX rejects.  Returns 0 on success or
 * a negative ps2_smap_rc on failure. */
static int ps2_smap_submit_tx_single(uint32_t slot, uint32_t len, uint32_t seq)
{
    int rc;

    memset(&smap_submit_tx_req, 0, sizeof(smap_submit_tx_req));
    smap_submit_tx_req.slot = slot;
    smap_submit_tx_req.len  = len;
    smap_submit_tx_req.seq  = seq;

    memset(&smap_rc_rsp, 0, sizeof(smap_rc_rsp));
    rc = rpc_call_sync(PS2_SMAP_FNO_SUBMIT_TX,
                       &smap_submit_tx_req,
                       sizeof(smap_submit_tx_req),
                       &smap_rc_rsp,
                       sizeof(smap_rc_rsp));
    if (rc < 0)
        return -5;
    if (smap_rc_rsp.rc != PS2_SMAP_RC_OK)
        return smap_rc_rsp.rc;
    return 0;
}

/* Issue a batched SUBMIT_TX RPC and clear the pending queue.  Returns
 * 0 on success, negative on RPC failure.  On batch-RPC failure, falls
 * back to single SUBMIT_TX per entry so we don't lose frames silently;
 * any single failure is reported but the queue is still cleared (the
 * IRX has rejected those slots and won't re-evaluate them). */
static int ps2_smap_flush_tx(void)
{
    uint32_t i;
    int rc;
    int rc_fallback = 0;

    if (!g_layout_valid)
        return -1;
    if (g_pending_submit_count == 0u)
        return 0;

    memset(&smap_submit_batch_req, 0, sizeof(smap_submit_batch_req));
    smap_submit_batch_req.count = g_pending_submit_count;
    for (i = 0; i < g_pending_submit_count; i++) {
        smap_submit_batch_req.entries[i].slot = g_pending_submits[i].slot;
        smap_submit_batch_req.entries[i].len  = g_pending_submits[i].len;
        smap_submit_batch_req.entries[i].seq  = g_pending_submits[i].seq;
    }

    memset(&smap_rc_rsp, 0, sizeof(smap_rc_rsp));
    rc = rpc_call_sync(PS2_SMAP_FNO_SUBMIT_TX_BATCH,
                       &smap_submit_batch_req,
                       sizeof(smap_submit_batch_req),
                       &smap_rc_rsp, sizeof(smap_rc_rsp));

    if (rc >= 0 && smap_rc_rsp.rc == PS2_SMAP_RC_OK) {
        /* Batch accepted — release all claimed slots. */
        g_tx_claimed_mask = 0u;
        g_pending_submit_count = 0u;
        return 0;
    }

    /* Batch failed.  Fallback: re-submit each entry via the single
     * fno-5 path so we can identify which one(s) the IRX rejects.  The
     * batch handler's all-or-nothing semantics mean tx_seq has not
     * advanced on the IRX — our staged seq values are still the
     * expected ones. */
    for (i = 0; i < g_pending_submit_count; i++) {
        int rc_one = ps2_smap_submit_tx_single(
            g_pending_submits[i].slot,
            g_pending_submits[i].len,
            g_pending_submits[i].seq);
        if (rc_one < 0 && rc_fallback == 0)
            rc_fallback = rc_one;
    }
    g_tx_claimed_mask = 0u;
    g_pending_submit_count = 0u;
    return rc_fallback;
}

int ps2_smap_send(const void *frame, uint32_t len)
{
    uint32_t slot;
    uint32_t scanned;
    uint32_t tx_data_iop;
    uint32_t expected_seq;

    if (!g_layout_valid || frame == 0)
        return -1;
    if (len < 14u || len > PS2_SMAP_FRAME_MAX)
        return -2;
    if (len > g_tx_slot_size)
        return -2;

    /* Auto-flush if the pending TX queue is at capacity.  Without this
     * we'd have nowhere to stage the new entry. */
    if (g_pending_submit_count >= PS2_SMAP_SUBMIT_BATCH_MAX) {
        if (ps2_smap_flush_tx() < 0) {
            /* Flush failed — drop the queue and try to keep going.
             * Caller can retry. */
            return -6;
        }
    }

    /* Round-robin scan for a FREE slot.  Skip slots already claimed
     * (staged but not yet submitted) — those are FREE in the IOP's
     * view but reserved by us. */
    slot = g_tx_next_slot;
    for (scanned = 0; scanned < g_tx_slot_count; scanned++) {
        int claimed = (g_tx_claimed_mask & (1u << slot)) != 0u;
        if (!claimed && g_tx_state_view[slot] == PS2_SMAP_TX_FREE)
            break;
        slot++;
        if (slot >= g_tx_slot_count)
            slot = 0;
    }
    if (scanned >= g_tx_slot_count)
        return -3;
    g_tx_next_slot = slot + 1;
    if (g_tx_next_slot >= g_tx_slot_count)
        g_tx_next_slot = 0;

    /* SIF DMA the bytes into the shared TX-data slot.  The bytes need
     * to be in IOP RAM by the time the SUBMIT_TX_BATCH RPC dispatches,
     * which is guaranteed because SIF DMA completes synchronously
     * from the EE's perspective in ee_sif_iop_write. */
    tx_data_iop = g_shared_iop_phys + g_tx_data_offset
                + slot * g_tx_slot_size;
    if (ee_sif_iop_write((void *)(uintptr_t)tx_data_iop, frame, len) < 0)
        return -4;

    /* Stage the SUBMIT entry.  Bump the local seq so a subsequent
     * send to the same slot in a future batch passes the next-higher
     * seq.  g_tx_claimed_mask keeps us from re-picking the same slot
     * while it's queued. */
    expected_seq = (slot < 16u) ? g_ee_tx_seq[slot] : 0u;
    g_pending_submits[g_pending_submit_count].slot = slot;
    g_pending_submits[g_pending_submit_count].len  = len;
    g_pending_submits[g_pending_submit_count].seq  = expected_seq;
    g_pending_submit_count++;
    g_tx_claimed_mask |= (1u << slot);
    if (slot < 16u)
        g_ee_tx_seq[slot] = expected_seq + 1u;

    /* Auto-flush if we just hit the cap.  Keeps queue depth bounded
     * and frees a slot for the next caller. */
    if (g_pending_submit_count >= PS2_SMAP_SUBMIT_BATCH_MAX)
        (void)ps2_smap_flush_tx();

    return 0;
}

/* Stage one {slot, seq} into the pending-release batch.  If the batch
 * is already full, flush it via RELEASE_RX_BATCH first.  Returns 0 on
 * success or RPC failure (the caller has already consumed the slot;
 * a failed flush means the IOP didn't advance rx_tail_iop_view so the
 * ring may stall, but there's no point retrying inline). */
static int ps2_smap_stage_release(uint32_t slot, uint32_t seq)
{
    if (g_pending_release_count >= PS2_SMAP_RELEASE_BATCH_MAX) {
        if (ps2_smap_release_pending() < 0) {
            /* Flush failed — drop the staged batch and try to keep going.
             * The next poll will see rx_head still ahead of
             * rx_tail_iop_view; if the ring fills up the IRX will start
             * incrementing rx_drops_no_slot which surfaces in diag. */
            g_pending_release_count = 0u;
        }
    }
    g_pending_releases[g_pending_release_count].slot = slot;
    g_pending_releases[g_pending_release_count].seq  = seq;
    g_pending_release_count++;
    return 0;
}

int ps2_smap_release_pending(void)
{
    uint32_t i;
    int rc;
    int rc_release = 0;
    int rc_submit;

    if (!g_layout_valid)
        return -1;

    /* Flush pending RELEASE_RX_BATCH first.  Skip the RPC entirely if
     * the queue is empty so the rendezvous-loop fast path stays cheap. */
    if (g_pending_release_count != 0u) {
        memset(&smap_release_batch_req, 0, sizeof(smap_release_batch_req));
        smap_release_batch_req.count = g_pending_release_count;
        for (i = 0; i < g_pending_release_count; i++) {
            smap_release_batch_req.entries[i].slot = g_pending_releases[i].slot;
            smap_release_batch_req.entries[i].seq  = g_pending_releases[i].seq;
        }

        memset(&smap_rc_rsp, 0, sizeof(smap_rc_rsp));
        rc = rpc_call_sync(PS2_SMAP_FNO_RELEASE_RX_BATCH,
                           &smap_release_batch_req,
                           sizeof(smap_release_batch_req),
                           &smap_rc_rsp, sizeof(smap_rc_rsp));
        g_pending_release_count = 0u;
        if (rc < 0)
            rc_release = -2;
        else if (smap_rc_rsp.rc != PS2_SMAP_RC_OK)
            rc_release = smap_rc_rsp.rc;
    }

    /* Then flush pending SUBMIT_TX_BATCH.  Same shape on the contract
     * side — main loop must drain TX before yielding so frames don't
     * sit unsubmitted when the caller transfers control to the loaded
     * program. */
    rc_submit = ps2_smap_flush_tx();

    if (rc_release < 0)
        return rc_release;
    return rc_submit;
}

int ps2_smap_poll(void *out_buf, uint32_t buf_size, uint32_t *out_len)
{
    uint32_t rx_head;
    uint32_t slot;
    uint32_t pkt_len;
    uint32_t pkt_seq;
    uint32_t i;
    volatile uint8_t *src;
    uint8_t *dst;

    if (out_len)
        *out_len = 0u;
    if (!g_layout_valid || out_buf == 0)
        return -1;

    rx_head = g_hot_diag_view->rx_head;
    if (rx_head == g_rx_tail)
        return 1;   /* empty */

    slot = g_rx_tail % g_rx_slot_count;
    pkt_len = g_rx_ring_view[slot].len;
    pkt_seq = g_rx_ring_view[slot].seq;

    if (pkt_len < 14u || pkt_len > PS2_SMAP_FRAME_MAX) {
        /* Bad descriptor — stage release anyway and skip. */
        (void)ps2_smap_stage_release(slot, pkt_seq);
        g_rx_tail = g_rx_tail + 1u;
        return -2;
    }
    if (pkt_len > buf_size) {
        /* Caller buffer too small — leave the slot occupied so the
         * caller can retry with a bigger buffer.  rx_tail unchanged. */
        return -3;
    }

    /* Read bytes from the IOP-uncached view into the caller buffer.
     * Because g_rx_data_view goes through the SBUS-bridge alias the
     * read hits physical RAM directly — no EE-side cache invalidate
     * needed.  The caller passes the destination as the +2 alignment
     * offset of current_pkt so this is the only copy on the path. */
    src = g_rx_data_view + slot * g_rx_slot_size;
    dst = (uint8_t *)out_buf;
    for (i = 0; i < pkt_len; i++)
        dst[i] = src[i];

    if (out_len)
        *out_len = pkt_len;

    /* Stage a release entry; the actual RPC fires when the batch fills
     * or the loader explicitly calls ps2_smap_release_pending(). */
    (void)ps2_smap_stage_release(slot, pkt_seq);
    g_rx_tail = g_rx_tail + 1u;

    return 0;
}
