/* client/playstation2/sif_broker.c
 *
 * EE-side SIF broker exposed at KOSLOAD_SIF_IFACE_ADDR (0x80000294) for
 * KOS guests. Thin adapter over ee_sif.c so the loader stays the single
 * permanent owner of the SIF bridge.
 *
 * v2 surface:
 *   - sync:  open / close / cmd_send / iop_write / iop_read /
 *            iop_alloc / iop_free / register_cmd_handler /
 *            unregister_cmd_handler / result
 *   - async: rpc_bind / rpc_call / load_module_buffer
 *
 * Async ops submit through the loader's polling SIF stack but complete
 * via a DMAC INT1 / SIF0 handler installed at ps2_init() time. The ISR
 * demuxes RPC_END to the right kosload_sif_client_t (by pkt_addr) and
 * inbound SIFCMDs to the registered cmd handlers; it never blocks.
 *
 * IE gating: the loader runs polling with Status.IE=0 (set in crt0), so
 * the ISR cannot fire in loader context. The ISR runs only when guest
 * code holds IE=1. Sync broker entry points called from guest context
 * bracket their internal ee_sif polling with ee_cop0_save_clear_ie /
 * ee_cop0_restore_ie so the ISR can't steal a reply mid-poll.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ee_sif.h"
#include "ee_cop0.h"
#include "cache.h"
#include "ps2_memory_map.h"
#include "sif_broker.h"

#define EE_UNCACHED(addr)  ((void *)PS2_EE_KSEG1_ADDR(addr))
#define EE_PHYS(addr)      PS2_EE_PHYS(addr)

/* === Guest (active) state ============================================= */

#define BROKER_MAX_IOP_ALLOCS     32
#define BROKER_MAX_CMD_HANDLERS    8

typedef void (*broker_cmd_handler_fn)(const void *pkt, uint32_t len,
                                       void *arg);

static struct {
    int                   active;     /* 0 = no guest / broker not open */
    uint32_t              iop_addrs[BROKER_MAX_IOP_ALLOCS]; /* 0 = free slot */
    struct {
        uint32_t              cid;    /* 0 = free slot */
        broker_cmd_handler_fn fn;
        void                 *arg;
    } handlers[BROKER_MAX_CMD_HANDLERS];
} g_session;

/* No session token: one guest at a time, tracked by g_session.active. */
static int session_valid(void) {
    return g_session.active;
}

static int alloc_record(uint32_t iop_addr) {
    unsigned i;
    for (i = 0; i < BROKER_MAX_IOP_ALLOCS; i++) {
        if (g_session.iop_addrs[i] == 0) {
            g_session.iop_addrs[i] = iop_addr;
            return 0;
        }
    }
    return -1;
}

static int alloc_find(uint32_t iop_addr) {
    unsigned i;
    for (i = 0; i < BROKER_MAX_IOP_ALLOCS; i++) {
        if (g_session.iop_addrs[i] == iop_addr)
            return (int)i;
    }
    return -1;
}

/* === Broker-managed packet pool + active-client tracking ============== */

#define BROKER_PKT_COUNT  8
#define BROKER_PKT_SIZE   64

static volatile uint8_t broker_pkts[BROKER_PKT_COUNT][BROKER_PKT_SIZE]
    __attribute__((aligned(64)));
static kosload_sif_client_t *broker_active[BROKER_PKT_COUNT];

static int pkt_alloc_slot(void) {
    unsigned i;
    for (i = 0; i < BROKER_PKT_COUNT; i++) {
        if (broker_active[i] == NULL)
            return (int)i;
    }
    return -1;
}

/* Match an RPC_END reply against our active slots by the cd field
 * (the EE-side client-data pointer we put in pkt->cd on submit).
 *
 * Empirically (observed in the event ring during v3 bring-up): the
 * BIOS-resident IOP SIFRPC server echoes pkt_addr in BIND_END but
 * not in CALL_END — CALL_END replies arrive with pkt_addr = 0
 * regardless of what we submitted. cd, by contrast, is round-tripped
 * uniformly in both reply types. That makes cd the load-bearing
 * match field; this matcher works for BIND, CALL, and LMB without
 * having to special-case reply type. */
static int slot_of_client(const void *cd) {
    unsigned i;
    for (i = 0; i < BROKER_PKT_COUNT; i++) {
        if ((const void *)broker_active[i] == cd)
            return (int)i;
    }
    return -1;
}

/* === Private state stored in kosload_sif_client_t._priv[8] ============ */

#define BROKER_STATUS_IDLE      0
#define BROKER_STATUS_INFLIGHT  1
#define BROKER_STATUS_DONE      2

#define BROKER_OP_BIND  1
#define BROKER_OP_CALL  2
#define BROKER_OP_LMB   3

/* Per-client broker state. Must fit in kosload_sif_client_t._priv
 * (32 bytes per ABI). pkt slot index lives here instead of the pkt
 * address — the ISR derives the slot from the reply's pkt_addr by
 * scanning broker_pkts[], so no per-client copy is needed.
 *
 * LMB staging address is broker-scope (single LMB in flight at a time),
 * not per-client. That keeps this struct at the 32-byte limit. */
typedef struct {
    uint8_t  status;
    uint8_t  op;
    uint16_t pad;
    int32_t  result;
    void    *server;      /* cached BIND-reply server ptr (RPC_CALL uses)    */
    void    *buf;         /* cached BIND-reply buf                            */
    void    *cbuf;        /* cached BIND-reply cbuf                           */
    void    *recv_buf;    /* RPC_CALL recv buf (for cache invalidate)         */
    uint32_t recv_size;
} broker_priv_t;

/* Compile-time check that the private layout fits the ABI-fixed _priv. */
typedef char assert_priv_fits
    [(sizeof(broker_priv_t) <= sizeof(((kosload_sif_client_t *)0)->_priv))
        ? 1 : -1];

static broker_priv_t *priv_of(kosload_sif_client_t *c) {
    return (broker_priv_t *)c->_priv;
}

/* === Deferred IOP-heap free queue ====================================
 * LMB completion fires from ISR context, where we can't run another
 * IOPHEAP RPC (would re-enter the broker's polling stack). Push the
 * staging addr onto this queue; the next sync op or close()
 * drains it. */

#define BROKER_PENDING_FREE_MAX 8
static volatile uint32_t pending_free[BROKER_PENDING_FREE_MAX];
static volatile uint32_t pending_free_count;

static void pending_free_push(uint32_t iop_addr) {
    uint32_t n = pending_free_count;
    if (n < BROKER_PENDING_FREE_MAX)
        pending_free[n] = iop_addr;
    pending_free_count = n + 1;   /* count overflow harmless: drains skip past */
}

static void pending_free_drain(void) {
    uint32_t n = pending_free_count;
    uint32_t i;
    if (n > BROKER_PENDING_FREE_MAX) n = BROKER_PENDING_FREE_MAX;
    for (i = 0; i < n; i++) {
        if (pending_free[i] != 0) {
            (void)ee_sif_free_iop_heap((void *)pending_free[i]);
            pending_free[i] = 0;
        }
    }
    pending_free_count = 0;
}

/* === LMB scaffolding ================================================== */

/* RPC dispatcher fnos for LOADFILE. Server ID and SIFCMD ids
 * (RPC_BIND/RPC_CALL/RPC_END) come from ee_sif.h.
 *   fn 0 = LoadModule (path-based, resident LOADFILE opens the file)
 *   fn 6 = LoadModuleBuffer (EE-staged image) */
#define LOADFILE_LOAD_FILE_FN   0
#define LOADFILE_LOAD_BUF_FN    6

/* LMB LOAD_BUF arg block — must match the resident IOP LOADFILE layout
 * (same as ee_sif.c::ee_sif_load_module_args_t). */
typedef struct {
    uint32_t module_ptr;
    uint32_t arg_len;
    uint8_t  unused[252];
    char     args[252];
} lmb_args_t;

static volatile lmb_args_t lmb_args __attribute__((aligned(64)));

/* LOADFILE fn 0 (LoadModule) arg block. RE'd from the resident IOP
 * LOADFILE path handler (NOT a copied PS2SDK header): path string at
 * offset 0, arg_len at 252, args at 256. aligned(16) pads sizeof to a
 * 512-byte SIF DMA quantum. The reply word(s) share lmb_result with the
 * buffer path. */
typedef struct {
    char     path[252];
    uint32_t arg_len;
    char     args[252];
} __attribute__((aligned(16))) lmp_args_t;

static volatile lmp_args_t lmp_args __attribute__((aligned(64)));
static volatile uint32_t   lmb_result[2] __attribute__((aligned(64)));  /* {result, modres} */
static int                 lmb_in_flight;     /* one LMB at a time */
static uint32_t            lmb_in_flight_staging;  /* IOP heap addr to free at completion */

/* LOADFILE bind — lazy, cached across the guest. The broker uses its
 * own client struct (not the loader's loadfile_client) to keep state
 * scoped to the broker. */
static volatile ee_sif_rpc_client_t broker_loadfile_client
    __attribute__((aligned(64)));
static int broker_loadfile_bound;

/* === Tiny IE-mask scope helper ======================================= *
 *
 * Brackets a call into ee_sif.c's polling internals. Called from broker
 * entry points that run in guest context; suppresses the SIF0 ISR for
 * the duration of the polling so the ISR can't consume the reply we're
 * about to poll for. */

#define BROKER_POLL_SCOPE_BEGIN  uint32_t _br_ie = ee_cop0_save_clear_ie()
#define BROKER_POLL_SCOPE_END    ee_cop0_restore_ie(_br_ie)

/* === Op packet builders ============================================== */

/* SIFCMD packet header (matches ee_sif_cmd_header_t in ee_sif.h). */
typedef struct {
    uint8_t  psize;
    uint8_t  dsize_lo, dsize_mid, dsize_hi;
    void    *dest;
    uint32_t cid;
    uint32_t opt;
} sif_cmd_hdr_t;

/* SIFRPC BIND request (matches ee_sif_rpc_bind_pkt_t, 36 bytes). */
typedef struct {
    ee_sif_cmd_header_t sifcmd;
    int32_t             rec_id;
    void               *pkt_addr;
    int32_t             rpc_id;
    void               *cd;
    int32_t             sid;
} sif_bind_pkt_t;

/* SIFRPC CALL request (matches ee_sif_rpc_call_pkt_t, 56 bytes). */
typedef struct {
    ee_sif_cmd_header_t sifcmd;
    int32_t             rec_id;
    void               *pkt_addr;
    int32_t             rpc_id;
    void               *cd;
    int32_t             rpc_number;
    int32_t             send_size;
    void               *recvbuf;
    int32_t             recv_size;
    int32_t             rmode;
    void               *sd;
} sif_call_pkt_t;

/* SIFRPC reply (RPC_END) (matches ee_sif_rpc_rend_pkt_t, 48 bytes). */
typedef struct {
    ee_sif_cmd_header_t sifcmd;
    int32_t             rec_id;
    void               *pkt_addr;
    int32_t             rpc_id;
    void               *cd;
    uint32_t            cid;
    void               *sd;
    void               *buf;
    void               *cbuf;
} sif_rend_pkt_t;

static uint32_t broker_next_rid = 1;

/* === ISR ============================================================= */

static void broker_complete(kosload_sif_client_t *c,
                             volatile const sif_rend_pkt_t *r) {
    broker_priv_t *p = priv_of(c);

    switch (p->op) {
    case BROKER_OP_BIND:
        p->server = r->sd;
        p->buf    = r->buf;
        p->cbuf   = r->cbuf;
        p->result = (r->sd != NULL) ? KOSLOAD_OK : KOSLOAD_ENOSRV;
        break;
    case BROKER_OP_CALL:
        /* Invalidate-only over the KOS recv buffer. cache_flush_dc
         * here would do writeback+invalidate, which corrupts the IOP's
         * just-DMA'd reply: any stale cache lines for recv that KOS
         * code re-populated between broker_rpc_call's pre-flush and
         * this ISR (stack pressure near recv, etc.) get written back,
         * clobbering RAM. ABI §7.2 specifies invalidate, not flush. */
        if (p->recv_buf != NULL && p->recv_size != 0)
            arch_dcache_inval_range((uintptr_t)p->recv_buf, p->recv_size);
        p->result = KOSLOAD_OK;
        break;
    case BROKER_OP_LMB: {
        volatile const uint32_t *res = (volatile const uint32_t *)
            EE_UNCACHED(lmb_result);
        /* res[0] is LOADFILE's result = the IOP module id (>= 0) on
         * success; res[1] is StartModule's. Surface the id so KOS can
         * correlate the load (and act on the module later); fold any
         * negative LOADFILE error into KOSLOAD_EBADARG. result(c) thus
         * follows the contract "module id >= 0, or negative error". */
        p->result = ((int32_t)res[0] >= 0) ? (int32_t)res[0] : KOSLOAD_EBADARG;
        if (lmb_in_flight_staging != 0) {
            pending_free_push(lmb_in_flight_staging);
            lmb_in_flight_staging = 0;
        }
        lmb_in_flight = 0;
        break;
    }
    default:
        p->result = KOSLOAD_EBADARG;
        break;
    }
    p->status = BROKER_STATUS_DONE;

    if (c->done != NULL)
        c->done(c->done_arg);
}

static void broker_dispatch_cmd_handler(volatile const ee_sif_cmd_header_t *hdr) {
    unsigned i;
    for (i = 0; i < BROKER_MAX_CMD_HANDLERS; i++) {
        if (g_session.handlers[i].cid != 0 &&
            g_session.handlers[i].cid == hdr->cid) {
            g_session.handlers[i].fn((const void *)hdr, hdr->psize,
                                      g_session.handlers[i].arg);
            return;
        }
    }
}

void ps2_sif_broker_isr(void) {
    volatile ee_sif_cmd_header_t *hdr = ee_sif_recv_header();

    if (hdr->psize == 0) {
        /* Spurious / nothing valid in rx buf — just rearm. */
        ee_sif_recv_ack();
        ee_sif_rearm_receive();
        return;
    }

    if (hdr->cid == EE_SIF_CMD_RPC_END) {
        volatile sif_rend_pkt_t *r = (volatile sif_rend_pkt_t *)hdr;
        int slot = slot_of_client(r->cd);
        if (slot >= 0) {
            kosload_sif_client_t *c = broker_active[slot];
            broker_active[slot] = NULL;
            if (c != NULL)
                broker_complete(c, r);
        }
    } else if (hdr->cid == EE_SIF_CMD_SET_SREG) {
        /* An IOP module published a SREG value to us. Record it into the
         * shadow so a later get_sreg() observes it. (The polling path's
         * ee_sif_dispatch_cmd does the same; the broker ISR is the live
         * path during a guest run.) */
        volatile ee_sif_set_sreg_t *sp = (volatile ee_sif_set_sreg_t *)hdr;
        ee_sif_sreg_write_local(sp->index, sp->value);
    } else if (g_session.active) {
        broker_dispatch_cmd_handler(hdr);
    }

    ee_sif_recv_ack();
    ee_sif_rearm_receive();
}

/* === Sync op shims (IE-mask brackets around polling) ================= */

static int broker_cmd_send(uint32_t cid,
                            const void *pkt, uint32_t pkt_len) {
    int rc;
    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (pkt == NULL || pkt_len == 0 || pkt_len > KOSLOAD_SIF_MAX_CMD_PKT)
        return KOSLOAD_EBADARG;

    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_send_cmd(cid, (void *)pkt, pkt_len, NULL, NULL, 0);
        BROKER_POLL_SCOPE_END;
    }
    return (rc < 0) ? KOSLOAD_EBADARG : KOSLOAD_OK;
}

static int broker_iop_write(uint32_t iop_addr,
                             const void *src, uint32_t len) {
    int rc;
    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (src == NULL || len == 0 || len > KOSLOAD_SIF_MAX_IOP_XFER)
        return KOSLOAD_EBADARG;
    if (((uint32_t)src & 0xF) != 0 || (len & 0xF) != 0 ||
         (iop_addr & 0xF) != 0)
        return KOSLOAD_EALIGN;

    pending_free_drain();
    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_iop_write((void *)iop_addr, src, len);
        BROKER_POLL_SCOPE_END;
    }
    return (rc < 0) ? KOSLOAD_EBADARG : KOSLOAD_OK;
}

static int broker_iop_read(void *dst,
                            uint32_t iop_addr, uint32_t len) {
    int rc;
    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (dst == NULL || len == 0 || len > KOSLOAD_SIF_MAX_IOP_XFER)
        return KOSLOAD_EBADARG;
    if (((uint32_t)dst & 0xF) != 0 || (len & 0xF) != 0 ||
         (iop_addr & 0xF) != 0)
        return KOSLOAD_EALIGN;

    pending_free_drain();
    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_iop_read(iop_addr, dst, len);
        BROKER_POLL_SCOPE_END;
    }
    return (rc < 0) ? KOSLOAD_EBADARG : KOSLOAD_OK;
}

static int broker_iop_alloc(uint32_t len,
                             uint32_t *iop_addr_out) {
    void *iop;

    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (iop_addr_out == NULL || len == 0)
        return KOSLOAD_EBADARG;

    pending_free_drain();
    {
        BROKER_POLL_SCOPE_BEGIN;
        iop = ee_sif_alloc_iop_heap(len);
        BROKER_POLL_SCOPE_END;
    }
    if (iop == NULL)
        return KOSLOAD_ENOMEM;

    if (alloc_record((uint32_t)iop) < 0) {
        BROKER_POLL_SCOPE_BEGIN;
        (void)ee_sif_free_iop_heap(iop);
        BROKER_POLL_SCOPE_END;
        return KOSLOAD_ENOMEM;
    }

    *iop_addr_out = (uint32_t)iop;
    return KOSLOAD_OK;
}

static int broker_iop_free(uint32_t iop_addr) {
    int slot, rc;

    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (iop_addr == 0)
        return KOSLOAD_EBADARG;
    slot = alloc_find(iop_addr);
    if (slot < 0)
        return KOSLOAD_EBADARG;

    pending_free_drain();
    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_free_iop_heap((void *)iop_addr);
        BROKER_POLL_SCOPE_END;
    }
    if (rc < 0)
        return KOSLOAD_EBADARG;

    g_session.iop_addrs[slot] = 0;
    return KOSLOAD_OK;
}

/* === Sync ops: open/close + handler table ============================ */

static void session_reset_state(void) {
    unsigned i;
    for (i = 0; i < BROKER_MAX_IOP_ALLOCS; i++)
        g_session.iop_addrs[i] = 0;
    for (i = 0; i < BROKER_MAX_CMD_HANDLERS; i++) {
        g_session.handlers[i].cid = 0;
        g_session.handlers[i].fn  = NULL;
        g_session.handlers[i].arg = NULL;
    }
    for (i = 0; i < BROKER_PKT_COUNT; i++)
        broker_active[i] = NULL;
    broker_loadfile_bound = 0;
    lmb_in_flight = 0;
    lmb_in_flight_staging = 0;
}

static int broker_open(void) {
    /* Version is validated at discovery (iface->version), not here. */
    /* Any open() is a fresh start. If a previous guest died without
     * close() (active still set), this force-cleans the stale per-guest
     * state instead of rejecting — the single active flag is all the
     * crash detection we need (no session token to compare). */
    session_reset_state();
    g_session.active = 1;
    return KOSLOAD_OK;
}

static void broker_close(void) {
    unsigned i;
    if (!session_valid())
        return;

    /* Reclaim any IOP-heap left allocated. */
    pending_free_drain();
    for (i = 0; i < BROKER_MAX_IOP_ALLOCS; i++) {
        uint32_t addr = g_session.iop_addrs[i];
        if (addr != 0) {
            BROKER_POLL_SCOPE_BEGIN;
            (void)ee_sif_free_iop_heap((void *)addr);
            BROKER_POLL_SCOPE_END;
            g_session.iop_addrs[i] = 0;
        }
    }

    /* Drop registered handlers and any active client tracking. */
    for (i = 0; i < BROKER_MAX_CMD_HANDLERS; i++) {
        g_session.handlers[i].cid = 0;
        g_session.handlers[i].fn  = NULL;
        g_session.handlers[i].arg = NULL;
    }
    for (i = 0; i < BROKER_PKT_COUNT; i++)
        broker_active[i] = NULL;
    broker_loadfile_bound = 0;
    lmb_in_flight = 0;
    lmb_in_flight_staging = 0;

    g_session.active = 0;
}

static int broker_register_cmd_handler(uint32_t cid,
                                        broker_cmd_handler_fn h, void *arg) {
    unsigned i;
    int free_slot = -1;

    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (cid == 0 || h == NULL)
        return KOSLOAD_EBADARG;

    for (i = 0; i < BROKER_MAX_CMD_HANDLERS; i++) {
        if (g_session.handlers[i].cid == cid)
            return KOSLOAD_EBADARG;     /* duplicate */
        if (free_slot < 0 && g_session.handlers[i].cid == 0)
            free_slot = (int)i;
    }
    if (free_slot < 0)
        return KOSLOAD_EBADARG;         /* table full */

    g_session.handlers[free_slot].cid = cid;
    g_session.handlers[free_slot].fn  = h;
    g_session.handlers[free_slot].arg = arg;
    return KOSLOAD_OK;
}

static int broker_unregister_cmd_handler(uint32_t cid) {
    unsigned i;
    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (cid == 0)
        return KOSLOAD_EBADARG;

    for (i = 0; i < BROKER_MAX_CMD_HANDLERS; i++) {
        if (g_session.handlers[i].cid == cid) {
            g_session.handlers[i].cid = 0;
            g_session.handlers[i].fn  = NULL;
            g_session.handlers[i].arg = NULL;
            return KOSLOAD_OK;
        }
    }
    return KOSLOAD_EBADARG;
}

/* === Async ops ======================================================= */

static int broker_rpc_bind(kosload_sif_client_t *c,
                            uint32_t rpc_id) {
    broker_priv_t *p;
    volatile sif_bind_pkt_t *pkt;
    int slot, rc;

    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (c == NULL || c->done == NULL)
        return KOSLOAD_EBADARG;

    p = priv_of(c);
    if (p->status == BROKER_STATUS_INFLIGHT)
        return KOSLOAD_EBUSY;

    pending_free_drain();

    slot = pkt_alloc_slot();
    if (slot < 0)
        return KOSLOAD_EBADARG;     /* no broker pkt buffer available */

    pkt = (volatile sif_bind_pkt_t *)broker_pkts[slot];
    memset((void *)pkt, 0, BROKER_PKT_SIZE);
    pkt->sifcmd.psize = BROKER_PKT_SIZE;
    pkt->sifcmd.dsize = 0;
    pkt->sifcmd.cid   = 0x80000009;  /* EE_SIF_CMD_RPC_BIND */
    pkt->rec_id       = (int32_t)((broker_next_rid++ << 16) | 0x05);
    pkt->pkt_addr     = EE_UNCACHED(broker_pkts[slot]);
    pkt->rpc_id       = 0;
    pkt->cd           = (void *)c;
    pkt->sid          = (int32_t)rpc_id;

    p->op       = BROKER_OP_BIND;
    p->status   = BROKER_STATUS_INFLIGHT;
    /* ETIMEOUT (-8) as the pre-submit sentinel so KOS can distinguish
     * "completion never ran" (-8) from a real IOP-reported EBADARG (-3). */
    p->result   = KOSLOAD_ETIMEOUT;
    p->recv_buf = NULL;
    p->recv_size = 0;
    broker_active[slot] = c;

    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_send_cmd(0x80000009, (void *)pkt, BROKER_PKT_SIZE,
                              NULL, NULL, 0);
        BROKER_POLL_SCOPE_END;
    }
    if (rc < 0) {
        broker_active[slot] = NULL;
        p->status = BROKER_STATUS_IDLE;
        return KOSLOAD_EBADARG;
    }
    return KOSLOAD_EINFLIGHT;
}

static int broker_rpc_call(kosload_sif_client_t *c,
                            uint32_t fno,
                            const void *send, uint32_t send_len,
                            void *recv, uint32_t recv_len) {
    broker_priv_t *p;
    volatile sif_call_pkt_t *pkt;
    int slot, rc;

    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (c == NULL || c->done == NULL)
        return KOSLOAD_EBADARG;
    p = priv_of(c);
    if (p->status == BROKER_STATUS_INFLIGHT)
        return KOSLOAD_EBUSY;
    if (p->server == NULL || p->buf == NULL)
        return KOSLOAD_EBADARG;     /* not bound */
    if (send_len > KOSLOAD_SIF_MAX_RPC_XFER ||
         recv_len > KOSLOAD_SIF_MAX_RPC_XFER)
        return KOSLOAD_EBADARG;
    if (send_len != 0 && send == NULL)
        return KOSLOAD_EBADARG;
    if (recv_len != 0 && recv == NULL)
        return KOSLOAD_EBADARG;
    /* SIF DMA quantum applies to send/recv buffers. */
    if (send_len != 0 && (((uint32_t)send & 0xF) || (send_len & 0xF)))
        return KOSLOAD_EALIGN;
    if (recv_len != 0 && (((uint32_t)recv & 0xF) || (recv_len & 0xF)))
        return KOSLOAD_EALIGN;

    pending_free_drain();

    slot = pkt_alloc_slot();
    if (slot < 0)
        return KOSLOAD_EBADARG;

    pkt = (volatile sif_call_pkt_t *)broker_pkts[slot];
    memset((void *)pkt, 0, BROKER_PKT_SIZE);
    pkt->sifcmd.psize = BROKER_PKT_SIZE;
    pkt->sifcmd.dsize = 0;
    pkt->sifcmd.cid   = EE_SIF_CMD_RPC_CALL;
    pkt->rec_id       = (int32_t)((broker_next_rid++ << 16) | 0x05);
    pkt->pkt_addr     = EE_UNCACHED(broker_pkts[slot]);
    pkt->rpc_id       = 0;
    pkt->cd           = (void *)c;
    pkt->rpc_number   = (int32_t)fno;
    pkt->send_size    = (int32_t)send_len;
    pkt->recvbuf      = recv;
    pkt->recv_size    = (int32_t)recv_len;
    pkt->rmode        = 1;
    pkt->sd           = p->server;

    p->op        = BROKER_OP_CALL;
    p->status    = BROKER_STATUS_INFLIGHT;
    /* ETIMEOUT pre-init so KOS can distinguish "broker_complete never
     * ran" (-8) from a real IOP-reported EBADARG (-3). */
    p->result    = KOSLOAD_ETIMEOUT;
    p->recv_buf  = recv;
    p->recv_size = recv_len;
    broker_active[slot] = c;

    if (recv_len != 0)
        cache_flush_dc(recv, recv_len);

    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_send_cmd(EE_SIF_CMD_RPC_CALL, (void *)pkt, BROKER_PKT_SIZE,
                              send, p->buf, send_len);
        BROKER_POLL_SCOPE_END;
    }
    if (rc < 0) {
        broker_active[slot] = NULL;
        p->status = BROKER_STATUS_IDLE;
        return KOSLOAD_EBADARG;
    }
    return KOSLOAD_EINFLIGHT;
}

static int broker_load_module_buffer(kosload_sif_client_t *c,
                                      const void *irx, uint32_t irx_len,
                                      const char *args, uint32_t args_len) {
    broker_priv_t *p;
    volatile sif_call_pkt_t *pkt;
    uint32_t qw_len;
    void *staging;
    int slot, rc;

    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (c == NULL || c->done == NULL)
        return KOSLOAD_EBADARG;
    if (irx == NULL || irx_len == 0)
        return KOSLOAD_EBADARG;
    if (args_len > 252)
        return KOSLOAD_EBADARG;
    if (args_len != 0 && args == NULL)
        return KOSLOAD_EBADARG;

    p = priv_of(c);
    if (p->status == BROKER_STATUS_INFLIGHT)
        return KOSLOAD_EBUSY;
    if (lmb_in_flight)
        return KOSLOAD_EBUSY;     /* only one LMB in flight broker-wide */

    pending_free_drain();

    /* Lazily bind LOADFILE, cached for the guest. */
    if (!broker_loadfile_bound) {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_rpc_bind_retry(&broker_loadfile_client,
                                    EE_SIF_LOADFILE_RPC_ID, 64);
        BROKER_POLL_SCOPE_END;
        if (rc < 0)
            return KOSLOAD_EBADARG;
        broker_loadfile_bound = 1;
    }

    qw_len = (irx_len + 15) & ~15;

    {
        BROKER_POLL_SCOPE_BEGIN;
        staging = ee_sif_alloc_iop_heap(qw_len);
        BROKER_POLL_SCOPE_END;
    }
    if (staging == NULL)
        return KOSLOAD_ENOMEM;

    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_iop_write(staging, irx, qw_len);
        BROKER_POLL_SCOPE_END;
    }
    if (rc < 0) {
        BROKER_POLL_SCOPE_BEGIN;
        (void)ee_sif_free_iop_heap(staging);
        BROKER_POLL_SCOPE_END;
        return KOSLOAD_EBADARG;
    }

    slot = pkt_alloc_slot();
    if (slot < 0) {
        BROKER_POLL_SCOPE_BEGIN;
        (void)ee_sif_free_iop_heap(staging);
        BROKER_POLL_SCOPE_END;
        return KOSLOAD_EBADARG;
    }

    /* Build the LOAD_BUF arg block (mirrors ee_sif_load_module_args_t). */
    memset((void *)&lmb_args, 0, sizeof(lmb_args));
    memset((void *)lmb_result, 0, sizeof(lmb_result));
    lmb_args.module_ptr =
        PS2_IOP_KSEG1_BASE | (((uint32_t)staging) & 0x1fffff);
    lmb_args.arg_len = args_len;
    if (args_len != 0)
        memcpy((void *)lmb_args.args, args, args_len);
    cache_flush_dc((const void *)&lmb_args, sizeof(lmb_args));
    cache_flush_dc((const void *)lmb_result, sizeof(lmb_result));

    pkt = (volatile sif_call_pkt_t *)broker_pkts[slot];
    memset((void *)pkt, 0, BROKER_PKT_SIZE);
    pkt->sifcmd.psize = BROKER_PKT_SIZE;
    pkt->sifcmd.dsize = 0;
    pkt->sifcmd.cid   = EE_SIF_CMD_RPC_CALL;
    pkt->rec_id       = (int32_t)((broker_next_rid++ << 16) | 0x05);
    pkt->pkt_addr     = EE_UNCACHED(broker_pkts[slot]);
    pkt->rpc_id       = 0;
    pkt->cd           = (void *)c;
    pkt->rpc_number   = (int32_t)LOADFILE_LOAD_BUF_FN;
    pkt->send_size    = (int32_t)sizeof(lmb_args);
    pkt->recvbuf      = (void *)lmb_result;
    pkt->recv_size    = (int32_t)sizeof(lmb_result);
    pkt->rmode        = 1;
    pkt->sd           = broker_loadfile_client.server;

    p->op          = BROKER_OP_LMB;
    p->status      = BROKER_STATUS_INFLIGHT;
    /* ETIMEOUT pre-init so KOS can distinguish "broker_complete never
     * ran" (-8) from a real IOP-reported EBADARG (-3). */
    p->result      = KOSLOAD_ETIMEOUT;
    p->recv_buf    = (void *)lmb_result;
    p->recv_size   = sizeof(lmb_result);
    broker_active[slot] = c;
    lmb_in_flight = 1;
    lmb_in_flight_staging = (uint32_t)staging;

    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_send_cmd(EE_SIF_CMD_RPC_CALL, (void *)pkt, BROKER_PKT_SIZE,
                              (const void *)&lmb_args,
                              broker_loadfile_client.buf,
                              sizeof(lmb_args));
        BROKER_POLL_SCOPE_END;
    }
    if (rc < 0) {
        broker_active[slot] = NULL;
        p->status = BROKER_STATUS_IDLE;
        lmb_in_flight = 0;
        pending_free_push((uint32_t)staging);
        return KOSLOAD_EBADARG;
    }
    return KOSLOAD_EINFLIGHT;
}

/* v4: load a module the resident LOADFILE can open by path. rom0: only —
 * the prefix is already on stock LOADFILE's allow-list, so no sbv-style
 * patch is needed (cd:/dvd:/mc0:/mass: would need clean-room prefix-check
 * + fileio patches we don't have). Async, shares the single-in-flight LMB
 * machinery with load_module_buffer. */
static int path_has_rom0_prefix(const char *path) {
    return path[0] == 'r' && path[1] == 'o' && path[2] == 'm' &&
           path[3] == '0' && path[4] == ':';
}

static int broker_load_module_path(kosload_sif_client_t *c,
                                   const char *path,
                                   const char *args, uint32_t args_len) {
    broker_priv_t *p;
    volatile sif_call_pkt_t *pkt;
    size_t path_len;
    int slot, rc;

    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (c == NULL || c->done == NULL)
        return KOSLOAD_EBADARG;
    if (path == NULL)
        return KOSLOAD_EBADARG;
    if (args_len > 252)
        return KOSLOAD_EBADARG;
    if (args_len != 0 && args == NULL)
        return KOSLOAD_EBADARG;

    /* Path must fit (NUL included) and be rom0:-prefixed. */
    for (path_len = 0; path_len < sizeof(lmp_args.path); path_len++) {
        if (path[path_len] == '\0')
            break;
    }
    if (path_len == 0 || path_len >= sizeof(lmp_args.path))
        return KOSLOAD_EBADARG;
    if (path_len < 5 || !path_has_rom0_prefix(path))
        return KOSLOAD_EBADARG;   /* v4: rom0: only */

    p = priv_of(c);
    if (p->status == BROKER_STATUS_INFLIGHT)
        return KOSLOAD_EBUSY;
    if (lmb_in_flight)
        return KOSLOAD_EBUSY;     /* one module-load in flight broker-wide */

    pending_free_drain();

    /* Lazily bind LOADFILE, cached for the guest (shared with LMB). */
    if (!broker_loadfile_bound) {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_rpc_bind_retry(&broker_loadfile_client,
                                    EE_SIF_LOADFILE_RPC_ID, 64);
        BROKER_POLL_SCOPE_END;
        if (rc < 0)
            return KOSLOAD_EBADARG;
        broker_loadfile_bound = 1;
    }

    slot = pkt_alloc_slot();
    if (slot < 0)
        return KOSLOAD_EBADARG;

    /* Build the fn-0 LoadModule arg block. */
    memset((void *)&lmp_args, 0, sizeof(lmp_args));
    memset((void *)lmb_result, 0, sizeof(lmb_result));
    memcpy((void *)lmp_args.path, path, path_len);   /* NUL already zeroed */
    lmp_args.arg_len = args_len;
    if (args_len != 0)
        memcpy((void *)lmp_args.args, args, args_len);
    cache_flush_dc((const void *)&lmp_args, sizeof(lmp_args));
    cache_flush_dc((const void *)lmb_result, sizeof(lmb_result));

    pkt = (volatile sif_call_pkt_t *)broker_pkts[slot];
    memset((void *)pkt, 0, BROKER_PKT_SIZE);
    pkt->sifcmd.psize = BROKER_PKT_SIZE;
    pkt->sifcmd.dsize = 0;
    pkt->sifcmd.cid   = EE_SIF_CMD_RPC_CALL;
    pkt->rec_id       = (int32_t)((broker_next_rid++ << 16) | 0x05);
    pkt->pkt_addr     = EE_UNCACHED(broker_pkts[slot]);
    pkt->rpc_id       = 0;
    pkt->cd           = (void *)c;
    pkt->rpc_number   = (int32_t)LOADFILE_LOAD_FILE_FN;
    pkt->send_size    = (int32_t)sizeof(lmp_args);
    pkt->recvbuf      = (void *)lmb_result;
    pkt->recv_size    = (int32_t)sizeof(lmb_result);
    pkt->rmode        = 1;
    pkt->sd           = broker_loadfile_client.server;

    p->op          = BROKER_OP_LMB;
    p->status      = BROKER_STATUS_INFLIGHT;
    p->result      = KOSLOAD_ETIMEOUT;
    p->recv_buf    = (void *)lmb_result;
    p->recv_size   = sizeof(lmb_result);
    broker_active[slot] = c;
    lmb_in_flight = 1;
    lmb_in_flight_staging = 0;     /* path load owns no IOP staging heap */

    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_send_cmd(EE_SIF_CMD_RPC_CALL, (void *)pkt, BROKER_PKT_SIZE,
                              (const void *)&lmp_args,
                              broker_loadfile_client.buf,
                              sizeof(lmp_args));
        BROKER_POLL_SCOPE_END;
    }
    if (rc < 0) {
        broker_active[slot] = NULL;
        p->status = BROKER_STATUS_IDLE;
        lmb_in_flight = 0;
        return KOSLOAD_EBADARG;
    }
    return KOSLOAD_EINFLIGHT;
}

/* === v4: SREG access ================================================= */

static int broker_get_sreg(uint32_t index,
                           uint32_t *value_out) {
    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if (value_out == NULL)
        return KOSLOAD_EBADARG;
    if ((index & 0xff) < 5)
        return KOSLOAD_EBADARG;   /* 0..4 reserved by loader bring-up */

    *value_out = ee_sif_sreg_read(index);
    return KOSLOAD_OK;
}

static int broker_set_sreg(uint32_t index,
                           uint32_t value) {
    int rc;
    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    if ((index & 0xff) < 5)
        return KOSLOAD_EBADARG;   /* 0..4 reserved by loader bring-up */

    {
        BROKER_POLL_SCOPE_BEGIN;
        rc = ee_sif_sreg_publish(index, value);
        BROKER_POLL_SCOPE_END;
    }
    return (rc < 0) ? KOSLOAD_EBADARG : KOSLOAD_OK;
}

/* === Module unload (stub) ============================================ *
 *
 * Reserved async slot grouped with the module ops. Stock BIOS MODLOAD has
 * no safe stop+unload of a resident module, so this returns KOSLOAD_ENOSRV
 * for now (no completion fires). KOS must not depend on it. */
static int broker_unload_module(kosload_sif_client_t *c, uint32_t module_id) {
    (void)c;
    (void)module_id;
    if (!session_valid())
        return KOSLOAD_ENOSESSION;
    return KOSLOAD_ENOSRV;
}

/* === v2: result accessor ============================================= */

static int32_t broker_result(const kosload_sif_client_t *c) {
    const broker_priv_t *p = (const broker_priv_t *)c->_priv;
    return p->result;
}

/* === Ops table + iface =============================================== */

static const kosload_sif_ops_t broker_ops = {
    .open                   = broker_open,
    .close                  = broker_close,
    .cmd_send               = broker_cmd_send,
    .register_cmd_handler   = broker_register_cmd_handler,
    .unregister_cmd_handler = broker_unregister_cmd_handler,
    .rpc_bind               = broker_rpc_bind,
    .rpc_call               = broker_rpc_call,
    .iop_write              = broker_iop_write,
    .iop_read               = broker_iop_read,
    .iop_alloc              = broker_iop_alloc,
    .iop_free               = broker_iop_free,
    .load_module_buffer     = broker_load_module_buffer,
    .load_module_path       = broker_load_module_path,
    .unload_module          = broker_unload_module,
    .result                 = broker_result,
    /* v3: KOS owns the EE interrupt vector and calls this from its
     * own SIF0 DMAC IRQ trampoline. Body is the same routine the
     * loader's own interrupt_handler_c invokes for backward compat. */
    .on_sif0_irq            = ps2_sif_broker_isr,
    /* v4: SREG access + rom0: path module load. */
    .get_sreg               = broker_get_sreg,
    .set_sreg               = broker_set_sreg
};

static kosload_sif_iface_t broker_iface = {
    .magic   = KOSLOAD_SIF_MAGIC,
    .version = KOSLOAD_SIF_VERSION,
    .ops     = &broker_ops,
};

void ps2_sif_broker_publish(void) {
    *(volatile kosload_sif_iface_t **)KOSLOAD_SIF_IFACE_ADDR = &broker_iface;

    /* Arm the DMAC INT1 generation for SIF0 (channel 5). KOS owns the
     * EE interrupt vector and dispatches the channel to the broker via
     * the ops->on_sif0_irq trampoline. The mask bit stays live for the
     * guest's lifetime. */
    ee_sif_enable_sif0_irq();
}
