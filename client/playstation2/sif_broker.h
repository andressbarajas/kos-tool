/* client/playstation2/sif_broker.h
 *
 * Loader half of the kos-load SIF broker ABI. KOS-side authoritative
 * source: kernel/arch/playstation2/include/ps2/kosload_sif_abi.h.
 *
 * Types and constants below MUST mirror that header byte-for-byte
 * (struct layout, enum values, magic, version). Identifier names are
 * loader-local — the contract is layout + values, not symbols.
 */

#ifndef __PS2_SIF_BROKER_H
#define __PS2_SIF_BROKER_H

#include <stdint.h>

/* === Loader detection (PS2 inner-loader header, fixed link base) ====== */

#define KOSLOAD_SIF_IFACE_ADDR     0x80000294  /* entry+0x14, see crt0.S */

/* === Interface discovery ============================================= */

#define KOSLOAD_SIF_MAGIC          0x4B534946  /* 'KSIF' */
#define KOSLOAD_SIF_VERSION        7           /* bump on ANY iface change */

/* === Size / limit contract ============================================ */

#define KOSLOAD_SIF_MAX_CMD_PKT    112    /* SIFCMD psize (8-bit field) */
#define KOSLOAD_SIF_MAX_RPC_XFER   65536
#define KOSLOAD_SIF_MAX_IOP_XFER   65536

/* === Error codes ====================================================== */

typedef enum {
    KOSLOAD_OK          =  0,
    KOSLOAD_EINFLIGHT   =  1,
    KOSLOAD_EBUSY       = -1,
    KOSLOAD_EALIGN      = -2,
    KOSLOAD_EBADARG     = -3,
    KOSLOAD_ENOMEM      = -4,
    KOSLOAD_ENOSESSION  = -5,
    KOSLOAD_EVERSION    = -6,
    KOSLOAD_ENOSRV      = -7,
    KOSLOAD_ETIMEOUT    = -8,
} kosload_sif_err_t;

/* === Client =========================================================== */

typedef struct kosload_sif_client {
    void (*done)(void *arg);
    void *done_arg;
    uint32_t _priv[8];
} kosload_sif_client_t;

/* === Function table =================================================== */

typedef struct kosload_sif_ops {
    /* open: KOSLOAD_OK = broker ready for this guest; <0 = refuse. Marks
     * the loader's internal active flag; a call while already active is
     * treated as a fresh start (crash-without-close recovery). */
    int (*open)(void);

    /* close: end this guest's use of the broker — clears the active flag,
     * frees every block iop_alloc'd, drops all cmd handlers, and abandons
     * any in-flight async op (done() never fires afterward). No-op if no
     * guest is active. The loader also does this implicitly at guest exit. */
    void (*close)(void);

    /* Send one raw SIFCMD packet (cid + 1..KOSLOAD_SIF_MAX_CMD_PKT bytes) to
     * the IOP. Synchronous, fire-and-forget — no reply is tracked; for a
     * reply use rpc_call, or register a handler for the IOP's response cid. */
    int (*cmd_send)(uint32_t cid, const void *pkt, uint32_t pkt_len);

    /* Register h() for inbound SIFCMD packets the IOP sends with this cid
     * (IOP->EE notifications). h runs in IRQ context from on_sif0_irq: keep
     * it short, don't re-enter the API, and copy out anything you need — the
     * pkt is valid only for that call. cid must be non-zero; a duplicate cid
     * is rejected; the handler table is small/bounded. */
    int (*register_cmd_handler)(uint32_t cid, void (*h)(const void *pkt, uint32_t len, void *arg), void *arg);

    /* Drop the handler previously registered for cid. */
    int (*unregister_cmd_handler)(uint32_t cid);

    /* Bind c to the IOP SIFRPC server with id rpc_id (must precede rpc_call
     * on c). Async: returns KOSLOAD_EINFLIGHT, completes via c->done(); on
     * success the bound server is cached in c. result(c) is KOSLOAD_OK, or
     * KOSLOAD_ENOSRV if the server isn't present. */
    int (*rpc_bind)(kosload_sif_client_t *c, uint32_t rpc_id);

    /* Invoke server function fno on a bound client c: sends send_len bytes,
     * receives up to recv_len into recv. Async (EINFLIGHT -> c->done()).
     * result(c) is KOSLOAD_OK/negative status — the reply payload lands in
     * recv (coherent once done() fires). send/recv must be 16-byte aligned
     * with 16-byte-multiple lengths (else KOSLOAD_EALIGN), each <=
     * KOSLOAD_SIF_MAX_RPC_XFER; the broker owns recv until done(). */
    int (*rpc_call)(kosload_sif_client_t *c, uint32_t fno, const void *send, uint32_t send_len, void *recv,
                    uint32_t recv_len);

    /* Copy len bytes EE<->IOP RAM (iop_write: src EE -> iop_addr IOP;
     * iop_read: iop_addr IOP -> dst EE). Synchronous; on iop_read return dst
     * is cache-coherent. Addr, buffer, and len must be 16-byte aligned/
     * multiples (KOSLOAD_EALIGN) and len <= KOSLOAD_SIF_MAX_IOP_XFER. */
    int (*iop_write)(uint32_t iop_addr, const void *src, uint32_t len);
    int (*iop_read)(void *dst, uint32_t iop_addr, uint32_t len);

    /* Allocate / free IOP-heap RAM. iop_alloc is synchronous and writes the
     * IOP address to *iop_addr_out (KOSLOAD_ENOMEM if the heap is full);
     * blocks are owned by the guest and auto-reclaimed at close(). iop_free
     * releases one previously returned by iop_alloc. */
    int (*iop_alloc)(uint32_t len, uint32_t *iop_addr_out);
    int (*iop_free)(uint32_t iop_addr);

    /* Load an IOP module from an EE-supplied IRX image: the broker stages it
     * into IOP RAM (iop_alloc + iop_write) and loads it — for modules KOS
     * builds itself (rom0/Sony modules use load_module_path). Async: blocks
     * on the IRX _start, fires c->done; result(c) is the IOP module id
     * (>= 0) on success, or a negative KOSLOAD_* error. irx must be 16-byte
     * aligned; one module-load (buffer OR path) in flight broker-wide. */
    int (*load_module_buffer)(kosload_sif_client_t *c, const void *irx, uint32_t irx_len, const char *args,
                              uint32_t args_len);

    /* Load an IOP module the resident LOADFILE opens by path (vs
     * load_module_buffer's EE-staged image) — how a guest brings up Sony
     * rom0 modules (SIO2MAN/PADMAN/MCMAN/MCSERV/LIBSD/SDRDRV) it can't ship.
     * rom0: only for now; other prefixes return KOSLOAD_EBADARG pending the
     * clean-room prefix-check/fileio patches. Async like load_module_buffer
     * (blocks on _start, fires c->done; result(c) = module id >= 0, or a
     * negative error); one module-load in flight at a time. */
    int (*load_module_path)(kosload_sif_client_t *c, const char *path, const char *args, uint32_t args_len);

    /* Unload a module previously loaded by load_module_buffer/path, by id.
     * Async (same completion contract as the load ops: returns
     * KOSLOAD_EINFLIGHT, real result via c->done() + result(c) == OK/neg)
     * because a clean unload runs the module's _stop, which can block.
     * Shares the single broker-wide module load/unload in-flight slot.
     *
     * STUB for now: returns KOSLOAD_ENOSRV. Stock BIOS MODLOAD has no safe
     * stop+unload of a resident module; real teardown needs our own
     * cooperative-_stop helper. Grouped with the module ops for now. */
    int (*unload_module)(kosload_sif_client_t *c, uint32_t module_id);

    /* Read the result of the most-recently-completed async op on c.
     * Caller must wait until c->done has fired; behavior undefined before. */
    int32_t (*result)(const kosload_sif_client_t *c);

    /* SIF0 (DMAC ch5, IOP->EE) ISR body — KOS calls this from its own SIF0
     * IRQ handler. Runs in IRQ context: must not block or re-enter the API.
     * Matches the inbound RPC_END to an active client by cd, fires that
     * client's done() (signal-only by contract), then acks D_STAT.SIF0 and
     * re-arms the rx chain. Idempotent; safe to call spuriously (just
     * ack+rearm, no done() fires). */
    void (*on_sif0_irq)(void);

    /* SIF software system-register (SREG) access. set_sreg publishes a
     * value to the IOP via a SET_SREG SIFCMD and mirrors it locally;
     * get_sreg reads the local shadow, which also captures values an IOP
     * module pushed to us via inbound SET_SREG (dispatched from the SIF0
     * ISR). This is the SifSetReg/SifGetReg pair some resident services
     * (e.g. CDVD) use for readiness/handshake.
     *
     * Indices 0..4 are reserved by the loader's bring-up handshake
     * (SUBADDR/MAINADDR/RPCINIT/flags); guests must use index >= 5. */
    int (*get_sreg)(uint32_t index, uint32_t *value_out);
    int (*set_sreg)(uint32_t index, uint32_t value);

} kosload_sif_ops_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    const kosload_sif_ops_t *ops;
} kosload_sif_iface_t;

/* === Loader-side entry points ========================================= */

/* Publish &broker_iface at KOSLOAD_SIF_IFACE_ADDR + arm the SIF0 DMAC
 * INT1 mask. Idempotent. Called once from ps2_init() before any guest
 * can run. */
void ps2_sif_broker_publish(void);

/* SIF0 DMAC INT1 dispatch. Demuxes the most-recent rx packet to either
 * the matching async-op kosload_sif_client_t (RPC_END) or the registered
 * cmd handler table (other cids), then acks D_STAT and rearms the rx
 * channel. Safe to call from interrupt context; runs to completion
 * without blocking. */
void ps2_sif_broker_isr(void);

#endif /* __PS2_SIF_BROKER_H */
