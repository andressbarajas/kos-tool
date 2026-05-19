/* client/playstation2/net/smap.c
 *
 * PS2 SMAP adapter glue.
 *
 * The common network stack expects an adapter with detect/init/loop/tx
 * callbacks.  On PS2, those callbacks mostly forward to smap.irx, the IOP
 * driver that owns the Ethernet hardware.
 *
 * Short flow:
 *   detect -> load DEV9/SMAP IRXs and bind RPC
 *   init   -> copy the MAC address into the common adapter struct
 *   loop   -> poll received frames, process packets, run DHCP/status timers
 *   tx     -> send one Ethernet frame through smap.irx
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <kosload/dhcp.h>
#include <kosload/target.h>
#include <kosload/protocol.h>
#include <kosload/screensaver.h>

#include "smap.h"
#include "adapter.h"
#include "net.h"
#include "dcload.h"
#include "../iop_dev9.h"
#include "../iop_smap.h"
#include "../iop/smap_protocol.h"
#include "../bootstrap/iop_bootstrap.h"

/* clear_lines lives in video.h but that header declares setup_video
 * with a uint32_t-typed prototype that conflicts with dcload.h's
 * unsigned-int-typed one under the MIPS ABI we use here.  Forward-
 * declare the one symbol we actually need so we don't have to drag
 * the entire video.h in. */
extern void clear_lines(int y, int height, unsigned int color);

#ifdef SMAP_DIAG
/* Diagnostic-dump helpers (also forward-declared to avoid the video.h
 * include conflict).  Constants mirror video.h literals. */
extern void ps2_video_draw_string(int x, int y, const char *str,
                                  uint32_t color);
extern void ps2_video_fill_rect(int x, int y, int w, int h,
                                uint32_t color);
#define PS2_DIAG_SCREEN_WIDTH 640
#define PS2_DIAG_CHAR_HEIGHT  24
#endif

/*--------------------------------------------------------------------------*/
/* adapter_t entry-point declarations                                       */
/*--------------------------------------------------------------------------*/

static int  smap_detect_adapter(void);
static int  smap_init_adapter(void);
static void smap_start_adapter(void);
static void smap_stop_adapter(void);
static void smap_loop_adapter(bool is_main_loop);
static int  smap_tx_adapter(unsigned char *pkt, int len);

/*--------------------------------------------------------------------------*/
/* Adapter table                                                            */
/*--------------------------------------------------------------------------*/

adapter_t adapter_smap = {
    "Broadband Adapter",
    { 0 },      /* MAC address — filled by smap_init_adapter */
    { 0 },      /* 2-byte alignment pad */
    smap_detect_adapter,
    smap_init_adapter,
    smap_start_adapter,
    smap_stop_adapter,
    smap_loop_adapter,
    smap_tx_adapter,
};

/*--------------------------------------------------------------------------*/
/* Last-error string for adapter.c                                           */
/*--------------------------------------------------------------------------*/

static const char *g_last_error = "NO ETHERNET ADAPTER DETECTED!";

const char *smap_get_last_error(void) {
    return g_last_error;
}

/*--------------------------------------------------------------------------*/
/* Detect: run DEV9 + SMAP bringup through the IOP                           */
/*--------------------------------------------------------------------------*/

static int smap_detect_adapter(void) {
    int rc;

    /* Bootstrap the DEV9 expansion bay first.  iop_dev9_init() loads
     * dev9_init.irx + kosdev9.irx via the legacy bootstrap chain (and
     * inline-loads smap.irx so it is resident by the time we bind
     * RPC). */
    rc = iop_dev9_init();
    if(rc < 0) {
        g_last_error = "DEV9 INIT FAILED";
        return -1;
    }

    /* Bind SIFRPC against PS2_SMAP_RPC_ID and fetch the shared-region
     * layout via GET_LAYOUT.  Cached on first call. */
    rc = ps2_bootstrap_iop_smap_init();
    if(rc < 0) {
        g_last_error = "SMAP INIT FAILED";
        return -1;
    }

    /* Publish the BBA adapter ID so the host's prepare_comms gets a
     * PS2-specific value back from cmd_version. */
    global_bg_color = SMAP_BG_COLOR;
    installed_adapter = ADAPTER_PS2_BBA;

    g_last_error = "NO ETHERNET ADAPTER DETECTED!";
    return 0;
}

/*--------------------------------------------------------------------------*/
/* Init: publish MAC                                                         */
/*--------------------------------------------------------------------------*/

static int smap_init_adapter(void) {
    const unsigned char *mac = ps2_smap_mac();

    memcpy(adapter_smap.mac, mac, 6);
    return 0;
}

/*--------------------------------------------------------------------------*/
/* Start/stop: no-ops                                                        */
/*                                                                           */
/* The IOP polling thread is always running once smap.irx is loaded — it     */
/* never needs to be started or stopped from the EE side.                    */
/*--------------------------------------------------------------------------*/

static void smap_start_adapter(void) {
}

static void smap_stop_adapter(void) {
}

/*--------------------------------------------------------------------------*/
/* TX: hand the frame to the IRX                                             */
/*--------------------------------------------------------------------------*/

static int smap_tx_adapter(unsigned char *pkt, int len) {
    if(len <= 0)
        return -1;

    return ps2_smap_send(pkt, (uint32_t)len);
}

#define LINK_SETTLE_SECS 2u

/* Link-status tracking, file-scope so loader-entry reset (smap_loop_adapter)
 * can clear them.  `g_link_seen_up` is per-loader-entry: it controls whether
 * a DOWN transition reads as "link change..." (haven't seen UP since the
 * loader regained control) or "link lost..." (link was up earlier in this
 * session).  Reset on every cold boot and every return from a uploaded
 * program. */
static uint32_t g_last_link_state = 0xffffffffu;
static int      g_link_seen_up    = 0;

/* Sticky across program returns; zero only on a fresh loader instance
 * (cold boot / FW update — BSS-init).  Gates the one-shot
 * "link change..." → 2s settle → "idle..." sequence at entry. */
static int      g_loader_settled  = 0;

static void smap_update_link_status(void) {
    ps2_smap_hot_snapshot_t hot;
    uint32_t link_state;

    if (ps2_smap_get_hot_snapshot(&hot) < 0)
        return;

    link_state = hot.link_state;
    if (link_state == g_last_link_state)
        return;

    if (link_state == PS2_SMAP_LINK_DOWN) {
        screensaver_wake();
        if (g_link_seen_up)
            disp_status("link lost...");
        else
            disp_status("link change...");
    } else {
        g_link_seen_up = 1;
        screensaver_wake();
        if (booted && !running)
            disp_status("idle...");
    }

    g_last_link_state = link_state;
}

/*--------------------------------------------------------------------------*/
/* Main loop: drain RX + DHCP + timeout countdown                            */
/*                                                                           */
/* Lifted from the previous driver's smap_loop_adapter — the network-stack   */
/* contract is that this function blocks until escape_loop is set.           */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* Diagnostic dump — renders the IRX hot-diag block + EE-side state at      */
/* row 174.  Cheap (one shared-memory snapshot, no RPC).                    */
/*                                                                          */
/* Compile with -DSMAP_DIAG to enable.  Off by default in production.       */
/*--------------------------------------------------------------------------*/

#ifdef SMAP_DIAG
static char *append_str_d(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

static char *append_hex8(char *p, uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 28; i >= 0; i -= 4) *p++ = hex[(v >> i) & 0xfu];
    return p;
}

static char *append_hex4(char *p, uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 12; i >= 0; i -= 4) *p++ = hex[(v >> i) & 0xfu];
    return p;
}

static void smap_diag_dump(void) {
    static unsigned int dump_skip = 0;
    static char buf[128];
    ps2_smap_hot_snapshot_t hot;
    char *p;

    /* Throttle to ~once every 128 outer-loop iterations so we don't
     * burn frame budget redrawing every poll. */
    if (++dump_skip < 128u) return;
    dump_skip = 0;

    if (ps2_smap_get_hot_snapshot(&hot) < 0) {
        ps2_video_fill_rect(16, 174, PS2_DIAG_SCREEN_WIDTH - 32, PS2_DIAG_CHAR_HEIGHT,
                            0x000000u);
        ps2_video_draw_string(16, 174,
            "smap diag: not initialized", 0xff4040u);
        return;
    }

    /* Row 1: rx counters + tx counters. */
    p = buf;
    p = append_str_d(p, "rx:");      p = append_hex4(p, hot.rx_packets);
    p = append_str_d(p, " tx:");     p = append_hex4(p, hot.tx_packets);
    p = append_str_d(p, " hd:");     p = append_hex4(p, hot.rx_head);
    p = append_str_d(p, " tl:");     p = append_hex4(p, hot.ee_rx_tail);
    p = append_str_d(p, " iv:");     p = append_hex4(p, hot.rx_tail_iop_view);
    p = append_str_d(p, " hb:");     p = append_hex4(p, hot.heartbeat);
    *p = '\0';
    ps2_video_fill_rect(16, 174, PS2_DIAG_SCREEN_WIDTH - 32, PS2_DIAG_CHAR_HEIGHT,
                        0x000000u);
    ps2_video_draw_string(16, 174, buf, 0xffffffu);

    /* Row 2: link, drops, rpc state, batch sizes.  TX slot states for
     * all 16 slots get summarized here as a hex string of 16 nibbles
     * (one per slot, low nibble of state). */
    p = buf;
    p = append_str_d(p, "lk:");      p = append_hex4(p, hot.link_state);
    p = append_str_d(p, " dr:");     p = append_hex4(p, hot.rx_drops_no_slot);
    p = append_str_d(p, " fc:");     p = append_hex4(p, hot.rx_drops_fcs);
    p = append_str_d(p, " un:");     p = append_hex4(p, hot.tx_underruns);
    p = append_str_d(p, " op:");     p = append_hex4(p, hot.last_rpc_op);
    p = append_str_d(p, " rc:");     p = append_hex4(p, (uint32_t)hot.last_rpc_result);
    p = append_str_d(p, " rb:");     p = append_hex4(p, hot.last_release_batch_size);
    p = append_str_d(p, " sb:");     p = append_hex4(p, hot.last_submit_batch_size);
    *p = '\0';
    ps2_video_fill_rect(16, 198, PS2_DIAG_SCREEN_WIDTH - 32, PS2_DIAG_CHAR_HEIGHT,
                        0x000000u);
    ps2_video_draw_string(16, 198, buf, 0xffffffu);

    /* Row 3: per-slot TX states (16 nibbles), pending TX queue depth. */
    p = buf;
    p = append_str_d(p, "ts:");
    {
        int i;
        for (i = 0; i < 16; i++) {
            static const char hex_d[] = "0123456789abcdef";
            *p++ = hex_d[hot.tx_slot_state[i] & 0xfu];
        }
    }
    p = append_str_d(p, " ps:");     p = append_hex4(p, hot.ee_pending_submits);
    p = append_str_d(p, " pr:");     p = append_hex4(p, hot.ee_pending_releases);
    *p = '\0';
    ps2_video_fill_rect(16, 222, PS2_DIAG_SCREEN_WIDTH - 32, PS2_DIAG_CHAR_HEIGHT,
                        0x000000u);
    ps2_video_draw_string(16, 222, buf, 0xffffffu);
}
#endif /* SMAP_DIAG */

static void smap_loop_adapter(bool is_main_loop) {
    const target_ops_t *t = target_get_ops();
    uint64_t last_sec_tick = 0;
    unsigned int local_secs_elapsed = 0;

    if(is_main_loop) {
        if(!(booted || running))
            disp_info();

        /* Loader-entry reset.  Each time we (re-)enter the main loop —
         * cold boot or after a uploaded program returns — the status row
         * should read "link change..." if the link is currently DOWN,
         * regardless of whether it was UP earlier in this session. */
        g_last_link_state = 0xffffffffu;
        g_link_seen_up = 0;

        /* One-shot link-change → settle → idle sequence for fresh loader
         * instances (cold boot / FW update).  Forces "link change..." to be
         * visible even when the IRX already reports link-up (FW update
         * with cable plugged); skipped on program-return entries so the
         * user doesn't see a 2-second pause after every upload. */
        if(!g_loader_settled) {
            ps2_smap_hot_snapshot_t hot;

            if(booted && !running)
                disp_status("link change...");

            for(;;) {
                if(ps2_smap_get_hot_snapshot(&hot) >= 0 &&
                   hot.link_state != PS2_SMAP_LINK_DOWN)
                    break;
            }

            uint64_t deadline = t->get_ticks() +
                (uint64_t)t->ticks_per_second * LINK_SETTLE_SECS;
            while(t->get_ticks() < deadline)
                ;

            g_loader_settled = 1;
            g_link_seen_up = 1;
            g_last_link_state = hot.link_state;
            if(booted && !running)
                disp_status("idle...");
        }
    }

    if(timeout_loop > 0)
        last_sec_tick = t->get_ticks();

    while(!escape_loop) {
        smap_update_link_status();

        /* Drain RX directly into current_pkt at the +2 alignment
         * offset (matches DC/GC convention) — single copy from the
         * IOP-uncached SBUS view to the EE buffer.  Releases are
         * staged as a batch and flushed below. */
        for (;;) {
            uint32_t rx_len = 0;
            int rc = ps2_smap_poll(current_pkt, RX_PKT_BUF_SIZE, &rx_len);
            if (rc != 0)
                break;
            if (rx_len == 0u)
                break;
            process_pkt(current_pkt);
        }

        /* Flush any pending RELEASE_RX_BATCH RPCs so the IOP can reuse
         * the slots we just consumed.  Without this, rx_tail_iop_view
         * never advances and the ring fills up on bursty PARTBINs. */
        (void)ps2_smap_release_pending();

        if(is_main_loop) {
#ifdef SMAP_DIAG
            smap_diag_dump();
#endif
            smap_update_link_status();
            dhcp_tick();   /* lease countdown — always, no network ops */
            /* Only a *known* link-DOWN suppresses DHCP.  An unread or
             * unknown snapshot (0xffffffff sentinel) must NOT gate it
             * off: dhcp_go/dhcp_renew tolerate no-link (they retry), and
             * blocking on "unknown" stalls acquisition forever when the
             * IRX hot-snapshot is slow/failing after a loader re-entry
             * (symptom: intermittent blank IP + DHCP lease 0). */
            if(g_last_link_state != PS2_SMAP_LINK_DOWN)
                dhcp_poll();
            screensaver_poll();
        }

        if(timeout_loop > 0) {
            uint64_t now = t->get_ticks();
            if((now - last_sec_tick) >= t->ticks_per_second) {
                last_sec_tick = now;
                local_secs_elapsed++;
                if(dhcp_attempts > 1) {
                    disp_dhcp_attempts_count();
                    disp_dhcp_next_attempt(timeout_loop - local_secs_elapsed + 1);
                }
                if(local_secs_elapsed > (unsigned int)timeout_loop) {
                    timeout_loop = -1;
                    escape_loop = 1;
                }
            }
        }
    }
    /* Drain the batch one more time before yielding to the loader's
     * idle-wait or the executing handoff — otherwise the slot reuse
     * stalls until the next poll loop entry. */
    (void)ps2_smap_release_pending();
    escape_loop = 0;
}
