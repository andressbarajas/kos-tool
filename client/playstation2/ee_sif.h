/* client/playstation2/ee_sif.h
 *
 * EE-side SIFCMD/SIFRPC helper library.
 */

/** \file    ee_sif.h
    \brief   EE-side SIFCMD/SIFRPC transport helpers.
    \ingroup ee_sif

    This header exposes a small SIF transport layer for the PS2.  All public
    entry points execute on the Emotion Engine (EE) and communicate with
    resident IOP services through SIFCMD, SIFRPC, and SIF DMA.

    Hardware choreography:

      - EE to IOP transfers use SIF1 source-chain DMA.
      - IOP to EE replies use the SIF0 receive buffer and are polled.
      - Shared transfer buffers are explicitly cache-flushed before DMA.
      - IOP RAM reads/writes use IOP physical/KSEG1 addresses as appropriate.

    The raw SIF DMA layer is the bottom of the stack. SIFCMD sends command
    packets through that DMA layer and dispatches them on interrupt/completion.
    SIFRPC then builds the familiar client/server call model on top of SIFCMD.

    \warning
    This is a small polling-based SIF helper: it owns one receive buffer, one
    SIF1 stream scratch buffer, and a small RPC packet pool inside the
    implementation. It is intended for code paths that need a self-contained
    SIF stack without external scheduler/IRQ dependencies.
*/

/** \defgroup ee_sif EE SIF Transport
    \brief          EE-side helpers for PS2 SIFCMD/SIFRPC.

    These helpers cover the EE-side primitives needed to reset the IOP, bind
    BIOS RPC services, copy IRX images into IOP memory, and load IOP modules.

    The public API is intentionally prefixed with \c ee_sif_ because the caller
    is always EE-side code. IOP-side IRXs declare only the resident imports
    they need.
*/

#ifndef __PS2_EE_SIF_H
#define __PS2_EE_SIF_H

#include <stdint.h>
#include "ps2_memory_map.h"

/** \defgroup ee_sif_iop_addr IOP Address Constants
    \brief                     Constants for addressing IOP RAM.
    \ingroup                   ee_sif
    @{
*/
#define EE_SIF_IOP_RAM_SIZE       PS2_IOP_MAIN_RAM_SIZE /**< \brief Usable 2 MB IOP RAM size. */
#define EE_SIF_IOP_KSEG1_BASE     PS2_IOP_KSEG1_BASE /**< \brief IOP uncached KSEG1 base. */

/** @} */

/** \defgroup ee_sif_sbus_regs SIF SBUS Registers
    \brief                     Memory-mapped SIF SBUS register addresses.
    \ingroup                   ee_sif

    The SIF "sub-bus" register set used for EE/IOP handshake and command/
    flag exchange. The EE sees these registers at the KSEG1 (uncached)
    addresses below; on the IOP side the same registers appear at IOP
    physical 0x1D0000xx (KSEG1 alias 0xBD0000xx).

    Register | EE addr    | IOP addr   | Notes
    ---------|------------|------------|------------------------------------
    MSCOM    | 0xB000F200 | 0xBD000000 | Only writable by EE.
    SMCOM    | 0xB000F210 | 0xBD000010 | Only writable by IOP.
    MSFLG    | 0xB000F220 | 0xBD000020 | EE writes set, IOP writes clear-mask.
    SMFLG    | 0xB000F230 | 0xBD000030 | IOP writes set, EE writes clear-mask.
    CTRL     | 0xB000F240 | 0xBD000040 | SBUS control. Init sequence writes 0x100.
    BD6      | 0xB000F260 | 0xBD000060 | Accessed during DMA bring-up; layout undocumented.

    Each address resolves to a 32-bit MMIO register. Cast to a
    \c volatile \c uint32_t pointer to read or write.

    @{
*/
#define EE_SIF_REG_MSCOM      PS2_EE_REG_SIF_MSCOM /**< \brief Main->sub command word; only writable by EE. */
#define EE_SIF_REG_SMCOM      PS2_EE_REG_SIF_SMCOM /**< \brief Sub->main command word; only writable by IOP. */
#define EE_SIF_REG_MSFLG      PS2_EE_REG_SIF_MSFLAG /**< \brief Main->sub flag word. EE write = set bits, IOP write = clear-mask. */
#define EE_SIF_REG_SMFLG      PS2_EE_REG_SIF_SMFLAG /**< \brief Sub->main flag word. IOP write = set bits, EE write = clear-mask. */
#define EE_SIF_REG_CTRL       PS2_EE_REG_SIF_CTRL /**< \brief SBUS control register; touched during init bring-up. */
#define EE_SIF_REG_BD6        PS2_EE_REG_SIF_BD6 /**< \brief SBUS BD6 register; touched during DMA bring-up. */

/** @} */

/** \defgroup ee_sif_stat_bits MSFLG / SMFLG Bring-Up Bits
    \brief                     Coarse bring-up status bits crossed via the FLG registers.
    \ingroup                   ee_sif

    These bits are set/cleared on \ref EE_SIF_REG_MSFLG and
    \ref EE_SIF_REG_SMFLG to signal stage transitions between the two
    CPUs. A side sets a bit on its outgoing FLG register (EE: MSFLG,
    IOP: SMFLG) and the peer clears the bit by writing the same value
    to the corresponding incoming FLG (write-mask = clear).

    @{
*/
#define EE_SIF_STAT_DMAINIT   0x00010000 /**< \brief SIF DMA hardware initialized. IOP sets on SMFLG; EE acks on MSFLG. */
#define EE_SIF_STAT_CMDINIT   0x00020000 /**< \brief SIFCMD layer initialized. Set by IOP-side SIFCMD on MSFLG. */
#define EE_SIF_STAT_BOOTEND   0x00040000 /**< \brief IOP boot finished. BIOS EESYNC sets this via SIFMAN SetSMFlag. */

/** @} */

/** \defgroup ee_sif_cmd_ids SIFCMD IDs
    \brief                    Resident system command IDs used by SIFCMD/SIFRPC.
    \ingroup                  ee_sif

    These values are SIFCMD command IDs, not RPC server IDs.

    @{
*/
#define EE_SIF_CMD_CHANGE_SADDR      0x80000000 /**< \brief Change EE receive-buffer address. */
#define EE_SIF_CMD_SET_SREG          0x80000001 /**< \brief Set an EE-side SIF system register.  */
#define EE_SIF_CMD_INIT_CMD          0x80000002 /**< \brief Initialize SIFCMD command channel. */
#define EE_SIF_CMD_IOP_RESET         0x80000003 /**< \brief Reset/reboot the IOP.  */
#define EE_SIF_CMD_RESERVED_04       0x80000004 /**< \brief No resident handler found in checked BIOS SIFCMD modules. */
#define EE_SIF_CMD_RESERVED_05       0x80000005 /**< \brief No resident handler found in checked BIOS SIFCMD modules. */
#define EE_SIF_CMD_RESERVED_06       0x80000006 /**< \brief No resident handler found in checked BIOS SIFCMD modules. */
#define EE_SIF_CMD_RESERVED_07       0x80000007 /**< \brief No resident handler found in checked BIOS SIFCMD modules. */
#define EE_SIF_CMD_RPC_END           0x80000008 /**< \brief SIFRPC reply/completion packet.  */
#define EE_SIF_CMD_RPC_BIND          0x80000009 /**< \brief SIFRPC bind request. */
#define EE_SIF_CMD_RPC_CALL          0x8000000A /**< \brief SIFRPC call request. */
#define EE_SIF_CMD_RESERVED_0B       0x8000000B /**< \brief No resident handler found in checked BIOS SIFCMD modules. */
#define EE_SIF_CMD_GET_OTHER_DATA    0x8000000C /**< \brief Request remote data copy. */
#define EE_SIF_CMD_RPC_RDATA         EE_SIF_CMD_GET_OTHER_DATA /**< \brief Common alternate name for GET_OTHER_DATA. */

/** @} */

/** \defgroup ee_sif_dma_attr SIF DMA Attribute Bits
    \brief                       Attribute bits accepted by ee_sif_set_dma().
    \ingroup                     ee_sif

    The checked BIOS SIFMAN code tests these low bits while building its
    internal DMA tags. This EE helper uses the same public shape for
    compatibility with resident SIFCMD/SIFRPC packet transfers.

    @{
*/
#define EE_SIF_DMA_ATTR_INT_O    0x04 /**< \brief Request interrupt-on-completion in the DMA tag. */
#define EE_SIF_DMA_ATTR_ERT      0x40 /**< \brief Mark the transfer as an end/terminal record. */

/** @} */

/** \defgroup ee_sif_rpc_ids BIOS RPC Server IDs
    \brief                    Resident IOP RPC server IDs.
    \ingroup                  ee_sif

    Distinct from the SIFCMD command IDs above.

    Bit 31 distinguishes "system" server IDs (set) from user-registered
    ones (clear).  Games can register any user ID without conflicting
    with these.

    @{
*/
#define EE_SIF_FILEIO_RPC_ID            0x80000001 /**< \brief Legacy file I/O service. */
#define EE_SIF_IOPHEAP_RPC_ID           0x80000003 /**< \brief IOP heap allocator RPC server. */
#define EE_SIF_LOADFILE_RPC_ID          0x80000006 /**< \brief LOADFILE RPC server. */

/* PADMAN */
#define EE_SIF_PADMAN_RPC_ID            0x80000100 /**< \brief Pad service.  */
#define EE_SIF_PADMAN_AUX_RPC_ID        0x80000101 /**< \brief Pad service auxiliary/extension entry-point. */
#define EE_SIF_PADMAN_LEGACY_RPC_ID     0x8000010F /**< \brief Legacy pad service used only by the original rom0 PADMAN.   */
#define EE_SIF_PADMAN_LEGACY_AUX_RPC_ID 0x8000011F /**< \brief Legacy pad-service aux entry, original rom0 58_PADMAN.bin only. */

/* MCSERV */
#define EE_SIF_MCSERV_RPC_ID            0x80000400 /**< \brief Memory-card service. */

/* CDVDFSV */
#define EE_SIF_CDVD_INIT_RPC_ID         0x80000592 /**< \brief CDVD init service. */
#define EE_SIF_CDVD_S_CMD_RPC_ID        0x80000593 /**< \brief CDVD "S" command service. */
#define EE_SIF_CDVD_N_CMD_RPC_ID        0x80000595 /**< \brief CDVD "N" command service. */
#define EE_SIF_CDVD_SEARCHFILE_RPC_ID   0x80000597 /**< \brief CDVD SearchFile service. */
#define EE_SIF_CDVD_DISKREADY_RPC_ID    0x8000059A /**< \brief CDVD Disk-Ready service. */

/** @} */

/** \brief   Optional status callback type.
    \ingroup ee_sif

    Used to surface short phase strings while SIF handshakes are in progress.
    The callback must be fast and must not call back into this library.

    \param  status          Static or temporary NUL-terminated status string.
*/
typedef void (*ee_sif_status_fn_t)(const char *status);

/** \brief   SIFCMD command-handler callback signature.
    \ingroup ee_sif

    This is the resident IOP-side handler ABI used by the SIFCMD dispatcher:
    \p data points at the received SIFCMD packet and \p harg is the handler
    argument registered with the command ID. EE code in this library polls
    incoming packets directly, but documenting this signature keeps the wire
    packet descriptions tied to the real dispatcher ABI.
*/
typedef void (*ee_sif_cmd_handler_t)(void *data, void *harg);

/** \brief   SIFRPC server callback signature.
    \ingroup ee_sif

    IOP RPC servers are invoked with a function number, a request buffer, and
    the request length. The return value is the buffer that should be copied
    back to the EE when the call completes.
*/
typedef void *(*ee_sif_rpc_func_t)(int fno, void *buff, int length);

/** \brief   SIFRPC asynchronous completion callback signature.
    \ingroup ee_sif

    The polling EE implementation leaves completion callbacks unused, but the
    field is part of the canonical SIFRPC client-data layout.
*/
typedef void (*ee_sif_rpc_end_fn_t)(void *end_param);

typedef struct ee_sif_rpc_client ee_sif_rpc_client_t;
typedef struct ee_sif_rpc_server ee_sif_rpc_server_t;
typedef struct ee_sif_rpc_queue ee_sif_rpc_queue_t;
typedef struct ee_sif_rpc_receive_data ee_sif_rpc_receive_data_t;

/** \brief   Common SIFCMD packet header.
    \ingroup ee_sif

    This is the first 16 bytes of every SIFCMD packet sent through
    ee_sif_send_cmd() or received through ee_sif_poll_packet().
*/
typedef struct {
    uint32_t psize : 8;    /**< \brief Packet size in bytes (1..112). */
    uint32_t dsize : 24;   /**< \brief Extra-DMA size in bytes (transferred to \c dest). */
    void *dest;            /**< \brief Optional destination for the extra DMA payload. */
    uint32_t cid;          /**< \brief SIFCMD command ID. */
    uint32_t opt;          /**< \brief Command-specific option word. */
} ee_sif_cmd_header_t;

/** \brief   SIFCMD packet for EE_SIF_CMD_CHANGE_SADDR.
    \ingroup ee_sif

    Sent by the EE to tell the resident IOP SIFCMD module where future SIF0
    command packets should be DMAed in EE memory.
*/
typedef struct {
    ee_sif_cmd_header_t header; /**< \brief Common SIFCMD header (cid = EE_SIF_CMD_CHANGE_SADDR). */
    void *buff;                 /**< \brief EE physical receive-buffer address. */
} ee_sif_saddr_pkt_t;

/** \brief   SIFCMD packet for EE_SIF_CMD_INIT_CMD.
    \ingroup ee_sif

    During SIFCMD/SIFRPC bring-up the EE sends this twice: first with
    \c header.opt == 0 and \c buff holding the EE receive address, then later
    with \c header.opt == 1 to finish SIFRPC initialization. The resident
    handler ignores \c buff in the \c opt == 1 form, so that form may be sent
    as a header-only packet.
*/
typedef struct {
    ee_sif_cmd_header_t header; /**< \brief Common SIFCMD header (cid = EE_SIF_CMD_INIT_CMD). */
    void *buff;                 /**< \brief EE physical receive-buffer address when \c opt == 0. */
} ee_sif_init_pkt_t;

/** \brief   SIFCMD packet for EE_SIF_CMD_SET_SREG.
    \ingroup ee_sif

    Wire-format packet used to publish a 32-bit value to the peer's SIF system
    register shadow. The receiving side updates its shadow at \c index with
    \c value. Indices 0..2 are reserved for the SUBADDR/MAINADDR/RPCINIT
    handshake; user code typically uses indices >= 3.
*/
typedef struct {
    ee_sif_cmd_header_t header; /**< \brief Common SIFCMD header (cid = EE_SIF_CMD_SET_SREG). */
    uint32_t index;             /**< \brief Target SIF system-register index. */
    uint32_t value;             /**< \brief 32-bit value to store at \c index. */
} ee_sif_set_sreg_t;

#define EE_SIF_IOP_RESET_ARG_MAX 80 /**< \brief Bytes reserved for MODLOAD reset arguments. */

/** \brief   SIFCMD packet for EE_SIF_CMD_IOP_RESET.
    \ingroup ee_sif

    Resident REBOOT modules read \c arg_len at offset 0x10, copy that many
    bytes from \c arg at offset 0x18 into their reboot request block, and copy
    \c mode from offset 0x14. The packet size sent on the wire is 0x68 bytes.

    \note
    The exact \c mode bit meanings are not fully documented here; BIOS code
    copies the word into the reboot request block before handing control to
    the reboot helper.
*/
typedef struct {
    ee_sif_cmd_header_t header;             /**< \brief Common SIFCMD header (cid = EE_SIF_CMD_IOP_RESET). */
    int32_t arg_len;                        /**< \brief Argument byte count copied from \c arg. */
    int32_t mode;                           /**< \brief Reboot mode word consumed by REBOOT. */
    char arg[EE_SIF_IOP_RESET_ARG_MAX];     /**< \brief Optional MODLOAD argument string bytes. */
} ee_sif_iop_reset_pkt_t;

/** \brief   Single SIF DMA transfer descriptor.
    \ingroup ee_sif

    Describes one source/destination pair for ee_sif_set_dma(). On the EE,
    descriptors submitted by this helper feed the SIF1 FIFO, the EE to IOP
    direction. The destination is an IOP address; the source is an EE buffer.

    The local implementation accepts a bounded descriptor array and queues the
    resulting SIF1 chain without using the EE BIOS syscall.
*/
typedef struct {
    void *src;     /**< \brief EE source address. */
    void *dest;    /**< \brief IOP destination address. */
    int32_t size;  /**< \brief Number of bytes to transfer. */
    int32_t attr;  /**< \brief SIF DMA attribute bits. */
} ee_sif_dma_transfer_t;

/** \brief   Common SIFRPC packet header (first 28 bytes).
    \ingroup ee_sif

    All SIFRPC bind/call requests and RPC_END / BIND_END replies start with
    this layout. The SIFCMD header carries \c cid = EE_SIF_CMD_RPC_BIND,
    EE_SIF_CMD_RPC_CALL, or EE_SIF_CMD_RPC_END; \c rec_id distinguishes
    bind-end vs call-end replies; \c pkt_addr / \c rpc_id let the EE match
    a reply against the request that generated it.
*/
typedef struct {
    ee_sif_cmd_header_t sifcmd; /**< \brief Common SIFCMD header. */
    int32_t rec_id;             /**< \brief Reply record-type id. */
    void *pkt_addr;             /**< \brief Original request packet address (for matching). */
    int32_t rpc_id;             /**< \brief Per-call reply id chosen by the requester. */
} ee_sif_rpc_header_t;

/** \brief   Alias used by older notes for the common packet header. */
typedef ee_sif_rpc_header_t ee_sif_rpc_pkt_header_t;

/** \brief   SIFRPC bind request packet.
    \ingroup ee_sif

    Wire format of an EE_SIF_CMD_RPC_BIND request. The IOP side replies with
    an ee_sif_rpc_rend_pkt_t whose \c rec_id identifies it as a bind-end and
    whose \c cd field carries the bound server pointer.
*/
typedef struct {
    ee_sif_cmd_header_t sifcmd; /**< \brief Common SIFCMD header (cid = EE_SIF_CMD_RPC_BIND). */
    int32_t rec_id;             /**< \brief Reply record-type id. */
    void *pkt_addr;             /**< \brief Address of this packet (for the matching reply). */
    int32_t rpc_id;             /**< \brief Per-bind reply id. */
    void *cd;                   /**< \brief EE-side client-data pointer; populated on reply. */
    int32_t sid;                /**< \brief Target IOP RPC server ID. */
} ee_sif_rpc_bind_pkt_t;

/** \brief   SIFRPC call request packet.
    \ingroup ee_sif

    Wire format of an EE_SIF_CMD_RPC_CALL request. The IOP server's reply
    arrives as an ee_sif_rpc_rend_pkt_t with \c rec_id identifying it as a
    call-end. If \c recvbuf is non-null the IOP DMAs \c recv_size bytes of
    reply payload there before sending the RPC_END SIFCMD.
*/
typedef struct {
    ee_sif_cmd_header_t sifcmd; /**< \brief Common SIFCMD header (cid = EE_SIF_CMD_RPC_CALL). */
    int32_t rec_id;             /**< \brief Reply record-type id. */
    void *pkt_addr;             /**< \brief Address of this packet (for the matching reply). */
    int32_t rpc_id;             /**< \brief Per-call reply id. */
    void *cd;                   /**< \brief Bound IOP server pointer (from prior bind). */
    int32_t rpc_number;         /**< \brief Server function number to invoke. */
    int32_t send_size;          /**< \brief Request payload size in bytes. */
    void *recvbuf;              /**< \brief EE address that will receive the reply payload. */
    int32_t recv_size;          /**< \brief Reply payload size in bytes. */
    int32_t rmode;              /**< \brief Reply mode flags. */
    void *sd;                   /**< \brief EE-side server-data pointer. */
} ee_sif_rpc_call_pkt_t;

/** \brief   SIFRPC reply packet (RPC_END / BIND_END).
    \ingroup ee_sif

    Wire format of an EE_SIF_CMD_RPC_END packet sent by the IOP for both
    bind-end and call-end replies. \c cd carries the bound server pointer
    (in a bind-end) or the original client-data pointer (in a call-end);
    \c buf and \c cbuf carry the IOP-side server receive and client buffers
    that subsequent EE_SIF_CMD_RPC_CALL packets must target.
*/
typedef struct {
    ee_sif_cmd_header_t sifcmd; /**< \brief Common SIFCMD header (cid = EE_SIF_CMD_RPC_END). */
    int32_t rec_id;             /**< \brief Bind-end vs call-end record-type id. */
    void *pkt_addr;             /**< \brief Original request packet address. */
    int32_t rpc_id;             /**< \brief Reply id matching the request. */
    void *cd;                   /**< \brief Server-pointer (bind-end) or client-data (call-end). */
    uint32_t cid;               /**< \brief Echoed SIFCMD command ID. */
    void *sd;                   /**< \brief Server-data pointer. */
    void *buf;                  /**< \brief IOP-side server receive buffer. */
    void *cbuf;                 /**< \brief IOP-side server client buffer. */
} ee_sif_rpc_rend_pkt_t;

/** \brief   Remote-data descriptor for EE_SIF_CMD_GET_OTHER_DATA.
    \ingroup ee_sif

    This three-word layout is what the resident SIFCMD handler consumes when
    copying memory from one CPU side to the other. The EE request packet below
    inlines these fields at offsets 0x20, 0x24, and 0x28.
*/
struct ee_sif_rpc_receive_data {
    void *src;      /**< \brief Source address on the peer side. */
    void *dest;     /**< \brief Destination address on the requesting side. */
    int32_t size;   /**< \brief Number of bytes to copy. */
};

/** \brief   SIFRPC remote-data request packet.
    \ingroup ee_sif

    Wire format of EE_SIF_CMD_GET_OTHER_DATA (also called RPC_RDATA). BIOS
    handlers copy \c pkt_addr and \c receive into the RPC_END response, then
    send an extra DMA block from \c src to \c dest with \c size bytes.
*/
typedef struct {
    ee_sif_cmd_header_t sifcmd;       /**< \brief Common SIFCMD header (cid = EE_SIF_CMD_GET_OTHER_DATA). */
    int32_t rec_id;                   /**< \brief Reply record-type id. */
    void *pkt_addr;                   /**< \brief Address of this packet (for the matching reply). */
    int32_t rpc_id;                   /**< \brief Per-request reply id. */
    ee_sif_rpc_receive_data_t *receive; /**< \brief Optional receiver state pointer echoed in RPC_END. */
    void *src;                        /**< \brief Source address for the extra DMA copy. */
    void *dest;                       /**< \brief Destination address for the extra DMA copy. */
    int32_t size;                     /**< \brief Copy size in bytes. */
} ee_sif_rpc_other_data_pkt_t;

/** \brief   Prefix of a SIFRPC client-data block.
    \ingroup ee_sif

    Older notes call this prefix \c SifRpcHeader. It is not the same as the
    on-wire ee_sif_rpc_header_t used by bind/call packets.
*/
typedef struct {
    void *pkt_addr;       /**< \brief Last packet address used by the client. */
    uint32_t rpc_id;      /**< \brief Active RPC reply ID, if any. */
    int32_t sema_id;      /**< \brief Semaphore ID field. */
    uint32_t mode;        /**< \brief Client mode field. */
} ee_sif_rpc_client_header_t;

/** \brief   SIFRPC client state block.
    \ingroup ee_sif

    The fields match the SIFRPC packet state layout expected by the resident
    IOP RPC services. Callers should treat this as opaque and clear it with
    ee_sif_rpc_client_clear() before first use or after an IOP reset.

    \warning
    The structure is shared with IOP-side RPC reply packets. Keep instances
    stable in memory while an RPC bind or call is active.
*/
struct ee_sif_rpc_client {
    void *pkt_addr;                  /**< \brief Client-header field: last packet address used. */
    uint32_t rpc_id;                 /**< \brief Client-header field: active RPC reply ID. */
    int32_t sema_id;                 /**< \brief Client-header field: semaphore ID; unused by this polling stack. */
    uint32_t mode;                   /**< \brief Client-header field: client mode. */
    uint32_t command;                /**< \brief Last RPC function number submitted. */
    void *buf;                       /**< \brief IOP-side server receive buffer. */
    void *cbuf;                      /**< \brief IOP-side server client buffer. */
    ee_sif_rpc_end_fn_t end_function; /**< \brief Completion callback field; unused here. */
    void *end_param;                 /**< \brief Completion callback parameter; unused here. */
    ee_sif_rpc_server_t *server;     /**< \brief Bound IOP server data pointer. */
};

/** \brief   SIFRPC server state block.
    \ingroup ee_sif

    This is the IOP-side server layout used by the resident SIFRPC dispatcher.
    EE code normally treats pointers to this structure as opaque values
    returned by BIND, but the layout matters for cleanroom IRX servers.
*/
struct ee_sif_rpc_server {
    int32_t sid;                 /**< \brief RPC server ID. */
    ee_sif_rpc_func_t func;      /**< \brief Main request handler. */
    void *buff;                  /**< \brief Main request buffer. */
    int32_t size;                /**< \brief Main request-buffer size. */
    ee_sif_rpc_func_t cfunc;     /**< \brief Client-buffer request handler. */
    void *cbuff;                 /**< \brief Client buffer. */
    int32_t size2;               /**< \brief Client-buffer size. */
    ee_sif_rpc_client_t *client; /**< \brief Client associated with the active request. */
    void *pkt_addr;              /**< \brief Active packet address. */
    int32_t rpc_number;          /**< \brief Active RPC function number. */
    void *receive;               /**< \brief EE receive buffer for replies. */
    int32_t rsize;               /**< \brief Reply size. */
    int32_t rmode;               /**< \brief Reply mode flags. */
    int32_t rid;                 /**< \brief Reply ID. */
    ee_sif_rpc_server_t *link;   /**< \brief Link inside the queue's pending list. */
    ee_sif_rpc_server_t *next;   /**< \brief Next registered server. */
    ee_sif_rpc_queue_t *base;    /**< \brief Owning data queue. */
};

/** \brief   SIFRPC server data queue.
    \ingroup ee_sif

    IOP servers attach one or more ee_sif_rpc_server_t nodes to a queue and
    then sleep in the resident dispatcher loop waiting for requests.
*/
struct ee_sif_rpc_queue {
    int32_t thread_id;            /**< \brief Thread servicing this queue. */
    int32_t active;               /**< \brief Queue active flag. */
    ee_sif_rpc_server_t *link;    /**< \brief Registered server list. */
    ee_sif_rpc_server_t *start;   /**< \brief First pending request. */
    ee_sif_rpc_server_t *end;     /**< \brief Last pending request. */
    ee_sif_rpc_queue_t *next;     /**< \brief Next queue in resident dispatcher list. */
};

/** \brief   LOADFILE LoadModuleBuffer result.
    \ingroup ee_sif

    Result block returned by ee_sif_load_module_buffer(). \c result is the
    LOADFILE RPC result; \c modres is the loaded module's StartModule result.
*/
typedef struct {
    uint32_t result;  /**< \brief LOADFILE RPC result. */
    uint32_t modres;  /**< \brief StartModule return value from the loaded IRX. */
} ee_sif_load_module_result_t;

/** \brief   Install an optional status callback.
    \ingroup ee_sif

    Passing \c NULL disables status reporting. The callback is used only for
    human-readable progress/debug strings; it is not part of protocol state.

    \param  fn              Callback to install, or \c NULL.
*/
void ee_sif_set_status_callback(ee_sif_status_fn_t fn);

/** \brief   Reset the EE-side SIF helper's private state.
    \ingroup ee_sif

    Clears cached SIF register shadows, RPC packet allocation state, and the
    built-in IOPHEAP/LOADFILE clients. This should be called before starting a
    fresh SIF bring-up sequence and is also called internally after a successful
    ee_sif_iop_reset().

    \warning
    This does not reset the IOP. It only resets this library's EE-side state.
*/
void ee_sif_reset_state(void);

/** \brief   Wait for the IOP BOOTEND flag.
    \ingroup ee_sif

    Polls the SIF SMFLAG register until the resident IOP side reports BOOTEND.
    BIOS EESYNC raises this bit from a LOADCORE boot-phase callback by calling
    SIFMAN's SetSMFlag(0x40000).

    \return                 0 on success, -1 on timeout.
*/
int ee_sif_wait_iop_bootend(void);

/** \brief   Initialize the EE SIFCMD receive path.
    \ingroup ee_sif

    Arms the EE receive buffer, reads the IOP SIFCMD sub-address, publishes the
    EE main-address, and sends the CHANGE_SADDR / INIT_CMD handshake.

    \return                 0 on success, negative on failure.
*/
int ee_sif_cmd_init(void);

/** \brief   Initialize the SIFRPC layer.
    \ingroup ee_sif

    Clears the local RPC packet pool, sends the SIFRPC INIT command, and waits
    for the IOP to publish the RPCINIT system-register value.

    \return                 0 on success, negative on failure.

    \warning
    ee_sif_cmd_init() must have completed successfully before calling this.
*/
int ee_sif_rpc_init(void);

/** \brief   Reset the IOP through SIFCMD.
    \ingroup ee_sif

    Sends EE_SIF_CMD_IOP_RESET and waits for BOOTEND after the reset. On
    success, this function also clears the EE-side SIF helper state because all
    previous IOP-side RPC bindings are invalid.

    \param  arg             Optional reset argument string, or \c NULL.
    \return                 0 on success, negative on failure.
*/
int ee_sif_iop_reset(const char *arg);

/** \brief   Send one SIFCMD packet, optionally with an extra DMA payload.
    \ingroup ee_sif

    The command packet is always sent to the current IOP SIFCMD sub-address.
    If \p extra_size is non-zero, \p extra_src is sent as an additional DMA
    block to \p extra_dest before the command packet itself.

    \param  cid             SIFCMD command ID.
    \param  packet          Packet buffer beginning with ee_sif_cmd_header_t.
    \param  packet_size     Packet size in bytes; must be 1..112.
    \param  extra_src       Optional EE source buffer for extra payload.
    \param  extra_dest      Optional IOP destination for extra payload.
    \param  extra_size      Optional extra payload size in bytes.
    \return                 0 on success, negative on failure.

    \warning
    ee_sif_cmd_init() must have discovered a non-zero IOP sub-address before
    this can send anything.
*/
int ee_sif_send_cmd(uint32_t cid, void *packet, uint32_t packet_size,
                    const void *extra_src, void *extra_dest,
                    uint32_t extra_size);

/** \brief   Poll for an incoming SIFCMD packet.
    \ingroup ee_sif

    Waits for the SIF0 receive channel to complete. Packets with command IDs
    other than \p cid are dispatched if they are system packets known to this
    helper, then the receive buffer is rearmed.

    \param  cid             Command ID to wait for.
    \param  timeout         Busy-poll iteration budget.
    \return                 Pointer to the uncached receive-buffer header, or
                            \c NULL on timeout.

    \warning
    The returned pointer is invalidated by ee_sif_rearm_receive().
*/
volatile ee_sif_cmd_header_t *ee_sif_poll_packet(uint32_t cid,
                                                 unsigned int timeout);

/** \brief   Clear and rearm the EE SIF0 receive buffer.
    \ingroup ee_sif

    The receive buffer is zeroed, flushed from dcache, and submitted as the
    destination-chain buffer for future IOP-to-EE packets.
*/
void ee_sif_rearm_receive(void);

/** \brief   Get the uncached alias of the current SIF0 receive-buffer header.
    \ingroup ee_sif

    Used by the broker SIF0 ISR to demux the most-recent incoming packet
    without going through the polling \ref ee_sif_poll_packet path.

    \return                 Uncached volatile pointer to the rx buffer header.
*/
volatile ee_sif_cmd_header_t *ee_sif_recv_header(void);

/** \brief   Ack the SIF0 DMAC done-bit in D_STAT (write 1 to clear).
    \ingroup ee_sif

    Companion to \ref ee_sif_recv_header for the broker SIF0 ISR. Equivalent
    to the bit-clear the polling loop in \ref ee_sif_poll_packet performs.
*/
void ee_sif_recv_ack(void);

/** \brief   Enable DMAC INT1 generation for SIF0 (channel 5).
    \ingroup ee_sif

    Sets the SIF0 mask bit in D_STAT[21]. After this is on, every SIF0
    DMA completion raises COP0 Cause.INT1_DMAC; the interrupt is gated
    by COP0 Status.IE so loader-context polling (IE=0) is unaffected.
*/
void ee_sif_enable_sif0_irq(void);

/** \brief   Initialize the EE SIF0 destination-chain receive channel.
    \ingroup ee_sif

    This is the local replacement for the resident EE BIOS SifSetDChain syscall.
    It does not call the syscall; when SIF0 is idle, it resets SIF0 QWC and
    starts EE DMAC channel 5 with CHCR 0x184 (busy, chain mode, tag-transfer
    enabled). The channel can be armed before the IOP has reply data ready.
*/
void ee_sif_set_dchain(void);

/** \brief   Queue one EE-to-IOP SIF1 DMA transfer.
    \ingroup ee_sif

    Builds a small EE-side source-chain stream from the provided descriptors
    and queues it for EE DMAC channel 6 (SIF1). This helper is normally used
    through ee_sif_send_cmd() and ee_sif_iop_write().

    This is the local replacement for the resident EE BIOS SifSetDma syscall.
    It does not call the syscall; it owns a small fixed queue, returns a
    positive transfer ID, and starts the next queued SIF1 chain as prior chains
    complete.

    \param  dmat            Array of DMA descriptors.
    \param  count           Descriptor count; valid range is implementation bounded.
    \return                 Positive transfer ID on success, 0 on failure or
                            if the local queue is full.

    \warning
    Source buffers are flushed for DMA visibility. Destination pointers are IOP
    addresses, not EE virtual addresses.
*/
int ee_sif_set_dma(ee_sif_dma_transfer_t *dmat, int count);

/** \brief   Poll SIF1 DMA completion.
    \ingroup ee_sif

    Mirrors the completion predicate used by the resident EE BIOS syscall:
    positive means queued, zero means running, and negative means complete or
    unknown.

    \param  trid            Transfer ID returned by ee_sif_set_dma().
    \return                 0 while still running, -1 once complete or idle.
*/
int ee_sif_dma_stat(int trid);

/** \brief   Copy bytes from EE memory to IOP memory.
    \ingroup ee_sif

    Splits large writes into chunks that fit the library's SIF1 scratch stream.
    The source data is flushed before each DMA chunk.

    \param  iop_dest        IOP destination address.
    \param  src             EE source buffer.
    \param  size            Number of bytes to copy.
    \return                 0 on success, negative on failure.
*/
int ee_sif_iop_write(void *iop_dest, const void *src, uint32_t size);

/** \brief   Copy bytes from IOP memory to EE memory.
    \ingroup ee_sif

    Uses the resident EE_SIF_CMD_RPC_RDATA / EE_SIF_CMD_GET_OTHER_DATA SIFCMD
    path and copies the uncached reply scratch into \p dst_ee.

    \param  src_iop         IOP source address.
    \param  dst_ee          EE destination buffer.
    \param  size            Number of bytes to copy.
    \return                 0 on success, negative on failure.
*/
int ee_sif_iop_read(uint32_t src_iop, void *dst_ee, uint32_t size);

/** \brief   Read one 32-bit word from IOP memory.
    \ingroup ee_sif

    \param  src_iop         IOP source address.
    \param  out             Destination for the word.
    \return                 0 on success, negative on failure.
*/
int ee_sif_iop_read_word(uint32_t src_iop, uint32_t *out);

/** \brief   Read a SIF software system register (SREG) shadow value.
    \ingroup ee_sif

    Returns the last value stored at \p index in the EE-side SIF system
    register shadow. Values are placed there either locally by
    ee_sif_sreg_publish() or by an inbound SET_SREG SIFCMD an IOP module
    sent to publish state to the EE. Indices are masked to 8 bits.

    \param  index           SIF system register index.
    \return                 The 32-bit shadow value at \p index.
*/
uint32_t ee_sif_sreg_read(uint32_t index);

/** \brief   Store a SIF SREG shadow value locally without notifying the IOP.
    \ingroup ee_sif

    Updates only the EE-side shadow. Used by the SIF0 receive path to record
    an inbound SET_SREG without re-sending it.

    \param  index           SIF system register index.
    \param  value           Value to store.
*/
void ee_sif_sreg_write_local(uint32_t index, uint32_t value);

/** \brief   Publish a SIF SREG value to the IOP and the local shadow.
    \ingroup ee_sif

    Updates the EE shadow at \p index and sends a SET_SREG SIFCMD so the
    IOP-side SIF register at the same index is updated too. This is the
    EE-to-IOP half of the classic SifSetReg/SifGetReg pair.

    \param  index           SIF system register index.
    \param  value           Value to publish.
    \return                 0 on success, negative on transport failure.
*/
int ee_sif_sreg_publish(uint32_t index, uint32_t value);

/** \brief   Clear an RPC client state block.
    \ingroup ee_sif

    \param  client          Client block to clear. May be \c NULL.
*/
void ee_sif_rpc_client_clear(volatile ee_sif_rpc_client_t *client);

/** \brief   Bind to an IOP RPC server.
    \ingroup ee_sif

    On success, \p client is populated with the IOP server pointer and buffer
    addresses needed by ee_sif_rpc_call().

    \param  client          RPC client state block.
    \param  server_id       IOP RPC server ID.
    \return                 0 on success, negative on failure.
*/
int ee_sif_rpc_bind(volatile ee_sif_rpc_client_t *client,
                    uint32_t server_id);

/** \brief   Bind to an IOP RPC server, retrying NULL-server replies.
    \ingroup ee_sif

    Some resident services can reply before their server pointer is available.
    This helper retries only that specific condition and returns immediately for
    transport/protocol failures.

    \param  client          RPC client state block.
    \param  server_id       IOP RPC server ID.
    \param  attempts        Maximum bind attempts.
    \return                 0 on success, negative on failure.
*/
int ee_sif_rpc_bind_retry(volatile ee_sif_rpc_client_t *client,
                          uint32_t server_id,
                          unsigned int attempts);

/** \brief   Make a synchronous RPC call.
    \ingroup ee_sif

    Sends \p sendbuf to the server's IOP receive buffer, submits a SIFRPC call,
    and waits for RPC_END. If \p recv_size is non-zero, the receive buffer is
    flushed before the call so the IOP can DMA the reply into it.

    \param  client          Bound RPC client.
    \param  rpc_number      Server function number.
    \param  sendbuf         EE request buffer.
    \param  send_size       Request size in bytes.
    \param  recvbuf         EE reply buffer.
    \param  recv_size       Reply size in bytes.
    \return                 0 on success, negative on failure.
*/
int ee_sif_rpc_call(volatile ee_sif_rpc_client_t *client, int rpc_number,
                    const void *sendbuf, uint32_t send_size,
                    void *recvbuf, uint32_t recv_size);

/** \brief   Allocate memory from the IOP heap.
    \ingroup ee_sif

    Lazily binds to the resident IOPHEAP RPC server and requests an allocation.

    \param  size            Number of bytes to allocate.
    \return                 IOP heap pointer on success, \c NULL on failure.
*/
void *ee_sif_alloc_iop_heap(uint32_t size);

/** \brief   Release a block previously returned by ee_sif_alloc_iop_heap().
    \ingroup ee_sif

    Calls the resident IOPHEAP RPC server's HFREE function (RPC function 2)
    on \p iop_addr. Lazily binds the client if needed.

    \param  iop_addr        IOP heap address returned by ee_sif_alloc_iop_heap().
    \return                 0 on success, negative on failure.
*/
int ee_sif_free_iop_heap(void *iop_addr);

/** \brief   Load an IRX image from an EE memory buffer.
    \ingroup ee_sif

    Allocates IOP heap memory, copies the IRX image into IOP RAM, and calls the
    LOADFILE LoadModuleBuffer function. The result block is copied into
    \p out when supplied.

    \param  irx             EE pointer to the IRX image bytes.
    \param  irx_size        IRX image size in bytes.
    \param  args            Optional argument block passed to StartModule.
    \param  arg_len         Argument block length; maximum 252 bytes.
    \param  out             Optional LOADFILE/StartModule result block.
    \return                 IOP-side LoadModuleBuffer result on success,
                            negative on transport/setup failure.

    \warning
    ee_sif_apply_lmb_patch() must have succeeded first. Stock BIOS LOADFILE
    rejects the LoadModuleBuffer RPC function used by this helper.
*/
int ee_sif_load_module_buffer(const void *irx, uint32_t irx_size,
                              const char *args, uint32_t arg_len,
                              ee_sif_load_module_result_t *out);

/** \brief   Patch resident LOADFILE to allow LoadModuleBuffer.
    \ingroup ee_sif

    Finds the resident LOADFILE RPC dispatcher and modload exports in IOP RAM,
    installs a small IOP heap stub, and opens the dispatcher bounds check so
    LOADFILE function 6 can be used by ee_sif_load_module_buffer().

    The patch is idempotent: if the dispatcher already accepts function 6, this
    returns success without installing another stub.

    \return                 0 on success, negative on failure.

    \warning
    This intentionally modifies resident IOP memory. Call it only after
    SIFRPC and IOPHEAP are available.
*/
int ee_sif_apply_lmb_patch(void);

#endif /* __PS2_EE_SIF_H */
