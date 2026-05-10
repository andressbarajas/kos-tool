/* client/playstation2/iop/smap_protocol.h
 *
 * Shared protocol between the EE loader and the IOP SMAP IRX.
 *
 * Mental model:
 *   - The IOP driver allocates one shared IOP-RAM block.
 *   - GET_LAYOUT tells the EE where each part of that block lives.
 *   - The IOP writes RX descriptors, packet bytes, link state, and counters.
 *   - The EE reads those fields through the 0xBC bridge address.
 *   - For TX, the EE DMAs packet bytes into a TX slot, then sends an RPC
 *     saying "slot N is ready."
 *
 * Control messages intentionally use SIFRPC instead of shared-memory command
 * fields.  That avoids the slim-only problem where EE writes to IOP RAM could
 * be delayed or missed by the IOP.
 *
 * This IRX does not install a SIFCMD user command buffer.  RPC registration
 * is enough for the EE to bind and call the driver.
 */

#ifndef PS2_SMAP_PROTOCOL_H
#define PS2_SMAP_PROTOCOL_H

/* ============================================================
 * RPC identity
 * ============================================================ */

#define PS2_SMAP_RPC_ID         0x534D4150u  /* 'PAMS' */

/* RPC function numbers.  Whitelisted on the IOP side; the handler
 * rejects anything outside this set. */
enum ps2_smap_fno {
    PS2_SMAP_FNO_GET_LAYOUT       = 1,
    PS2_SMAP_FNO_GET_STATUS       = 2,
    PS2_SMAP_FNO_GET_DIAG         = 3,
    PS2_SMAP_FNO_RELEASE_RX       = 4,
    PS2_SMAP_FNO_SUBMIT_TX        = 5,
    PS2_SMAP_FNO_RESET_LINK       = 6,  /* optional */
    PS2_SMAP_FNO_WAKE_TX          = 7,  /* optional */
    PS2_SMAP_FNO_POLL_RX          = 8,  /* optional */
    PS2_SMAP_FNO_RELEASE_RX_BATCH = 9,  /* batched RELEASE_RX (ABI v4) */
    PS2_SMAP_FNO_SUBMIT_TX_BATCH  = 10, /* batched SUBMIT_TX  (ABI v5) */
    PS2_SMAP_FNO_GET_RTC          = 11, /* read CDVD/Mechacon RTC (ABI v6) */
    PS2_SMAP_FNO_SET_RTC          = 12  /* write CDVD/Mechacon RTC (ABI v6) */
};

/* RPC return codes.  0 = ok; negatives are categorized failures. */
enum ps2_smap_rc {
    PS2_SMAP_RC_OK            =  0,
    PS2_SMAP_RC_BAD_OP        = -1,
    PS2_SMAP_RC_BAD_SLOT      = -2,
    PS2_SMAP_RC_BAD_LEN       = -3,
    PS2_SMAP_RC_BAD_SEQ       = -4,
    PS2_SMAP_RC_BAD_STATE     = -5,
    PS2_SMAP_RC_NOT_READY     = -6,
    PS2_SMAP_RC_INTERNAL      = -7
};

/* ============================================================
 * Wire constants
 * ============================================================ */

/* Ethernet max frame: 6+6+2+1500+4 (DST/SRC/TYPE/PAYLOAD/FCS). */
#define PS2_SMAP_FRAME_MAX     1518u

/* Slot counts.  Both sides compile with these sizes, and GET_LAYOUT repeats
 * them so the EE can verify it is talking to the matching IRX.
 *
 * We use 16 RX/TX slots so short bursts do not stall while the other side is
 * catching up.  Earlier 4-slot rings worked, but had less room for batched
 * PARTBIN and console traffic. */
#define PS2_SMAP_RX_SLOTS      16u
#define PS2_SMAP_TX_SLOTS      16u

/* Per-slot byte size in the shared region.  Must accommodate
 * PS2_SMAP_FRAME_MAX with 16-byte alignment for SIF DMA QWORD writes. */
#define PS2_SMAP_SLOT_SIZE     1536u   /* (1518 + 15) & ~15 = 1520, rounded to 1536 */

/* TX slot state codes (u32 in shared memory). */
enum ps2_smap_tx_state {
    PS2_SMAP_TX_FREE  = 0,
    PS2_SMAP_TX_BUSY  = 1,
    PS2_SMAP_TX_DONE  = 2
};

/* Layout magic: cross-checked by EE after GET_LAYOUT to detect garbled
 * replies and stale layout caches. */
#define PS2_SMAP_LAYOUT_MAGIC  0x534D504Cu  /* 'LPMS' */

/* ABI version.  Bump this when the structs or function numbers change.
 *
 *   3 — initial SIFRPC + shared-region transport.
 *   4 — RX_SLOTS bumped to 16, hot_diag adds last_release_batch_size,
 *       new fno RELEASE_RX_BATCH.
 *   5 — TX_SLOTS bumped to 16, hot_diag adds last_submit_batch_size,
 *       new fno SUBMIT_TX_BATCH, RELEASE_BATCH_MAX bumped to 16.
 *   6 — adds GET_RTC / SET_RTC RPCs backed by CDVDMAN clock calls. */
#define PS2_SMAP_ABI_VERSION   6u

/* ============================================================
 * Enums kept for chip_init / status reporting
 * ============================================================ */

enum ps2_smap_boot_status {
    PS2_SMAP_BOOT_DEAD    = 0,
    PS2_SMAP_BOOT_BOOTING = 1,
    PS2_SMAP_BOOT_READY   = 2,
    PS2_SMAP_BOOT_FAULT   = 3
};

enum ps2_smap_link_state {
    PS2_SMAP_LINK_DOWN    = 0,
    PS2_SMAP_LINK_10HDX   = 1,
    PS2_SMAP_LINK_10FDX   = 2,
    PS2_SMAP_LINK_100HDX  = 3,
    PS2_SMAP_LINK_100FDX  = 4
};

/* IOP-defined chip-init fault codes returned by smap_chip_init().
 * Surfaced to the EE via ps2_smap_status_rsp_t::fault. */
enum ps2_smap_fault {
    PS2_SMAP_FAULT_NONE             = 0,
    PS2_SMAP_FAULT_MAILBOX_BAD      = 1,    /* legacy, retained for chip_init switch */
    PS2_SMAP_FAULT_THREAD_CREATE    = 2,
    PS2_SMAP_FAULT_THREAD_START     = 3,
    PS2_SMAP_FAULT_EMAC3_RESET      = 4,
    PS2_SMAP_FAULT_FIFO_RESET       = 5,
    PS2_SMAP_FAULT_PHY_RESET        = 6,
    PS2_SMAP_FAULT_PHY_AUTONEG      = 7,
    PS2_SMAP_FAULT_EEPROM_CKSUM     = 8,
    PS2_SMAP_FAULT_DEV9_NOT_READY   = 9,
    PS2_SMAP_FAULT_DEV9_ID_BAD      = 10,
    PS2_SMAP_FAULT_STACR_TIMEOUT    = 11,
    PS2_SMAP_FAULT_EEPROM_IO        = 12,
    PS2_SMAP_FAULT_SHARED_ALLOC     = 13,    /* sysmem alloc for shared region failed */
    PS2_SMAP_FAULT_RPC_REGISTER     = 14     /* iop_rpc_register failed */
};

/* ============================================================
 * Shared-memory data structures
 *
 * These live inside the IOP-allocated shared block.  The IOP writes most of
 * them; the EE reads them.  16-byte alignment keeps DMA transfers tidy.
 * ============================================================ */

/* RX descriptor — one per ring slot.  IOP fills it after copying
 * frame bytes into the slot's data region; EE reads it after observing
 * rx_head advance. */
typedef struct ps2_smap_rx_desc {
    volatile unsigned int len;     /* payload bytes in slot's data region */
    volatile unsigned int status;  /* 0 = OK; non-zero reserved for FCS/trunc */
    volatile unsigned int seq;     /* per-slot monotonic, written by IOP */
    volatile unsigned int _pad;    /* align to 16 bytes */
} ps2_smap_rx_desc_t;

/* Hot diagnostics — polled by EE every frame.  Located at
 * layout.diag_offset. */
typedef struct ps2_smap_hot_diag {
    volatile unsigned int link_state;          /* enum ps2_smap_link_state */
    volatile unsigned int rx_head;             /* IOP-write producer cursor */
    volatile unsigned int rx_tail_iop_view;    /* IOP's last-known EE consumer cursor */
    volatile unsigned int tx_slot_state[PS2_SMAP_TX_SLOTS]; /* enum ps2_smap_tx_state */
    volatile unsigned int rx_packets;
    volatile unsigned int tx_packets;
    volatile unsigned int rx_drops_no_slot;
    volatile unsigned int rx_drops_fcs;
    volatile unsigned int tx_underruns;
    volatile unsigned int heartbeat;           /* frame-pump bumps every iteration */
    volatile unsigned int last_rpc_op;         /* enum ps2_smap_fno */
    volatile int          last_rpc_result;     /* enum ps2_smap_rc */
    volatile unsigned int last_release_batch_size; /* size of last RELEASE_RX_BATCH */
    volatile unsigned int last_submit_batch_size;  /* size of last SUBMIT_TX_BATCH */
    unsigned int          _pad[3];             /* align to 16-byte boundary */
} ps2_smap_hot_diag_t;

/* ============================================================
 * RPC request/response structs
 *
 * Fixed-size, no variable-length payloads.  All 16-byte aligned.
 * ============================================================ */

typedef struct ps2_smap_layout_rsp {
    int          rc;                     /* 0 = ok */
    unsigned int magic;                  /* PS2_SMAP_LAYOUT_MAGIC */
    unsigned int abi_version;            /* PS2_SMAP_ABI_VERSION */
    unsigned int shared_iop_phys;        /* IOP-physical base of shared region */
    unsigned int shared_size;            /* total bytes of shared region */

    unsigned int rx_slot_count;
    unsigned int rx_slot_size;
    unsigned int rx_ring_offset;         /* offset to ps2_smap_rx_desc_t[] */
    unsigned int rx_data_offset;         /* offset to RX bulk-data array */

    unsigned int tx_slot_count;
    unsigned int tx_slot_size;
    unsigned int tx_state_offset;        /* offset to tx_slot_state[] (in hot diag) */
    unsigned int tx_data_offset;         /* offset to TX bulk-data array */

    unsigned int diag_offset;            /* offset to ps2_smap_hot_diag_t */
    unsigned int _pad[3];                /* align to 16 bytes */
} ps2_smap_layout_rsp_t;

typedef struct ps2_smap_status_rsp {
    int          rc;
    unsigned int boot_state;             /* enum ps2_smap_boot_status */
    unsigned int fault;                  /* enum ps2_smap_fault */
    unsigned int link_state;             /* enum ps2_smap_link_state */
    unsigned char mac[6];
    unsigned char _pad0[2];
    unsigned int init_done;              /* 1 once chip_init returned */
    unsigned int _pad[2];                /* align to 16 bytes */
} ps2_smap_status_rsp_t;

typedef struct ps2_smap_cold_diag_rsp {
    int          rc;
    unsigned int last_validation_op;
    unsigned int last_validation_slot;
    unsigned int last_validation_seq;
    unsigned int last_validation_len;
    unsigned int mac_phy_status;
    unsigned int ring_invariant_flags;
    unsigned char mac[6];
    unsigned char _pad[10];              /* align to 16 bytes */
} ps2_smap_cold_diag_rsp_t;

typedef struct ps2_smap_rtc_req {
    unsigned int unix_timestamp;
    unsigned int _pad[3];                /* align to 16 bytes */
} ps2_smap_rtc_req_t;

typedef struct ps2_smap_rtc_rsp {
    int          rc;
    unsigned int unix_timestamp;
    unsigned int _pad[2];                /* align to 16 bytes */
} ps2_smap_rtc_rsp_t;

typedef struct ps2_smap_release_rx_req {
    unsigned int slot;
    unsigned int seq;
    unsigned int _pad[2];                /* align to 16 bytes */
} ps2_smap_release_rx_req_t;

/* Maximum RX releases that fit in a single batched RPC.  Sized so the
 * total request payload (header + entries) stays well under
 * PS2_SMAP_RPC_BUF_SIZE.  Sixteen entries × 8 bytes = 128 bytes, plus
 * a 16-byte header gives a 144-byte request.  Bumped from 8 in ABI v5
 * to halve RPC count for steady RX bursts at full RX-ring depth. */
#define PS2_SMAP_RELEASE_BATCH_MAX 16

typedef struct ps2_smap_release_rx_batch_req {
    unsigned int count;
    unsigned int _pad[3];                /* align entries[] to 16 bytes */
    struct {
        unsigned int slot;
        unsigned int seq;
    } entries[PS2_SMAP_RELEASE_BATCH_MAX];
} ps2_smap_release_rx_batch_req_t;

typedef struct ps2_smap_submit_tx_req {
    unsigned int slot;
    unsigned int len;
    unsigned int seq;
    unsigned int _pad;                   /* align to 16 bytes */
} ps2_smap_submit_tx_req_t;

/* Maximum TX submissions that fit in a single batched RPC (ABI v5).
 * Eight entries × 16 bytes = 128 bytes, plus a 16-byte header gives a
 * 144-byte request.  Caps batch fan-out so a worst-case TX-flood doesn't
 * starve the RPC handler thread for too long; the EE side flushes more
 * frequently than the cap when fewer slots are pending. */
#define PS2_SMAP_SUBMIT_BATCH_MAX 8

typedef struct ps2_smap_submit_tx_batch_req {
    unsigned int count;
    unsigned int _pad[3];                /* align entries[] to 16 bytes */
    struct {
        unsigned int slot;
        unsigned int len;
        unsigned int seq;
        unsigned int _pad;
    } entries[PS2_SMAP_SUBMIT_BATCH_MAX];
} ps2_smap_submit_tx_batch_req_t;

typedef struct ps2_smap_rc_rsp {
    int          rc;
    unsigned int _pad[3];                /* align to 16 bytes */
} ps2_smap_rc_rsp_t;

/* RPC reception buffer size.  Must accommodate the largest request OR
 * response payload.  Currently: cold_diag_rsp = 48 bytes; layout_rsp =
 * 64 bytes; release_rx_batch_req = 16 + 16*8 = 144 bytes;
 * submit_tx_batch_req = 16 + 8*16 = 144 bytes.  Round up generously. */
#define PS2_SMAP_RPC_BUF_SIZE  256u

#endif /* PS2_SMAP_PROTOCOL_H */
