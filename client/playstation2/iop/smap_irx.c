/* client/playstation2/iop/smap_irx.c
 *
 * IOP-side Ethernet driver.
 *
 * The IOP owns the PS2 Ethernet hardware, so this IRX does the low-level
 * SMAP work and exposes a small RPC interface to the EE loader.
 *
 * Basic design:
 *   - Allocate one shared IOP-RAM block for RX/TX slots and status.
 *   - Return that block's layout to the EE with GET_LAYOUT.
 *   - Put received packets into RX slots for the EE to read.
 *   - Transmit packets after the EE DMAs bytes into a TX slot and calls
 *     SUBMIT_TX.
 *
 * Even if hardware init fails, the IRX tries to register RPC so the EE can
 * ask GET_STATUS and show a useful fault code.
 *
 */

#include "kosload_iop.h"
#include "smap_protocol.h"

KOSLOAD_IRX_ID("smap", 1, 0);

/* ================================================================== *
 * Imports
 * ================================================================== */

/* thbase — thread spawn + delay.  Versions and export #s validated
 * against extracted/11_Multi_Thread_Manager.bin (see
 * AGENT/docs/re/cleanroom-iop-loader/10-thbase-imports.md). */
KOSLOAD_IMPORT_TABLE(thbase, 1, 1);
KOSLOAD_IMPORT(thbase,  4, CreateThread, int, (kosload_iop_thread_t *t));
KOSLOAD_IMPORT(thbase,  6, StartThread,  int, (int thid, void *arg));
KOSLOAD_IMPORT(thbase, 20, ThreadId,     int, (void));
KOSLOAD_IMPORT(thbase, 33, DelayThread,  int, (int microseconds));
KOSLOAD_IMPORT_TABLE_END(thbase);

/* loadcore — full IOP D-cache flush + library-table query.  FlushDcache
 * is used before SIF DMA when the source might have been written
 * through a cached IOP alias.  QueryLibraryEntryTable is used by the
 * inline dual-sifcmd workaround helper at the bottom of this file. */
KOSLOAD_IMPORT_TABLE(loadcore, 1, 1);
KOSLOAD_IMPORT(loadcore,  5, FlushDcache, void, (void));
KOSLOAD_IMPORT(loadcore, 11, QueryLibraryEntryTable, void **,
               (const kosload_iop_link_table_header_t *library));
KOSLOAD_IMPORT_TABLE_END(loadcore);

/* sifman — raw SIF DMA from IOP to EE.  Currently unused on the new
 * shared-region path (RX bytes live in shared memory; EE reads via
 * SBUS-bridge), but kept linked because the IRX may grow back to
 * IOP→EE DMA for higher-throughput RX delivery later. */
KOSLOAD_IMPORT_TABLE(sifman, 1, 1);
KOSLOAD_IMPORT(sifman, 7, sceSifSetDma, int, (void *dmat, int count));
KOSLOAD_IMPORT(sifman, 8, sceSifDmaStat, int, (int trid));
KOSLOAD_IMPORT_TABLE_END(sifman);

/* sifcmd — RPC server primitives. Private struct definitions below keep
 * this file self-contained and restrict the import table to the symbols
 * SMAP actually uses. */
typedef void *(*smap_rpc_handler_t)(int fno, void *buffer, int length);

typedef struct smap_iop_rpc_client_data {
    void *pkt_addr;
    unsigned int rpc_id;
    int sema_id;
    unsigned int mode;
    unsigned int command;
    void *buf;
    void *cbuf;
    void (*end_function)(void *end_param);
    void *end_param;
    struct smap_iop_rpc_server *server;
} smap_iop_rpc_client_data_t;

typedef struct smap_iop_rpc_server {
    int sid;
    smap_rpc_handler_t func;
    void *buf;
    int size;
    smap_rpc_handler_t cfunc;
    void *cbuf;
    int size2;
    smap_iop_rpc_client_data_t *client;
    void *pkt_addr;
    int rpc_number;
    void *recvbuf;
    int rsize;
    int rmode;
    int rid;
    struct smap_iop_rpc_server *link;
    struct smap_iop_rpc_server *next;
    struct smap_iop_rpc_queue *base;
} smap_iop_rpc_server_t;

typedef struct smap_iop_rpc_queue {
    int thread_id;
    int active;
    smap_iop_rpc_server_t *link;
    smap_iop_rpc_server_t *start;
    smap_iop_rpc_server_t *end;
    struct smap_iop_rpc_queue *next;
} smap_iop_rpc_queue_t;

KOSLOAD_IMPORT_TABLE(sifcmd, 1, 1);
KOSLOAD_IMPORT(sifcmd, 17, RpcRegister, void,
               (smap_iop_rpc_server_t *sd, int sid, smap_rpc_handler_t func,
                void *buf, smap_rpc_handler_t cfunc, void *cbuf,
                smap_iop_rpc_queue_t *qd));
KOSLOAD_IMPORT(sifcmd, 19, RpcSetQueue, smap_iop_rpc_queue_t *,
               (smap_iop_rpc_queue_t *qd, int thread_id));
KOSLOAD_IMPORT(sifcmd, 21, RpcExecRequest, void,
               (smap_iop_rpc_server_t *sd));
KOSLOAD_IMPORT_TABLE_END(sifcmd);

/* sysmem — shared-region allocation. */
KOSLOAD_IMPORT_TABLE(sysmem, 1, 1);
KOSLOAD_IMPORT(sysmem, 4, AllocSysMemory, void *,
               (int mode, int size, void *ptr));
KOSLOAD_IMPORT_TABLE_END(sysmem);

#define SMAP_SYSMEM_ALLOC_FIRST 0

/* cdvdman — CDVD/Mechacon clock access.  Export numbers match the
 * cleanroom BIOS resident-module table. */
typedef struct smap_cdvd_clock {
    unsigned char stat;
    unsigned char second;
    unsigned char minute;
    unsigned char hour;
    unsigned char pad;
    unsigned char day;
    unsigned char month;
    unsigned char year;
} smap_cdvd_clock_t;

KOSLOAD_IMPORT_TABLE(cdvdman, 1, 1);
KOSLOAD_IMPORT(cdvdman, 24, iop_cd_read_clock, int,
               (smap_cdvd_clock_t *clock));
KOSLOAD_IMPORT(cdvdman, 25, iop_cd_write_clock, int,
               (const smap_cdvd_clock_t *clock));
KOSLOAD_IMPORT_TABLE_END(cdvdman);

/* JST bias and the BCD/calendar math live in a shared header so host-side
 * tests can exercise the exact same code.
 * SMAP_CDVD_RTC_JST_BIAS_SECS, smap_bcd_to_int, smap_int_to_bcd,
 * smap_datetime_to_unix and smap_unix_to_datetime come from there. */
#include "smap_rtc_calendar.h"

/* dev9 — version 1.9 matches Sony's INET_SMAP_driver and ps2link's
 * smap_irx import block.  kosdev9.irx registers a resident "dev9" v1.9
 * library exporting #9 = ReadEEPROM via PIO bit-bang against the SMAP
 * EEPROM.  Bootstrap loads kosdev9.irx before smap.irx so the library
 * is in LOADCORE's registered list by the time LinkImports walks our
 * import block. */
KOSLOAD_IMPORT_TABLE(dev9, 1, 9);
KOSLOAD_IMPORT(dev9, 9, dev9_read_eeprom, int, (unsigned short *out));
KOSLOAD_IMPORT_TABLE_END(dev9);

/* ================================================================== *
 * Register window addresses (IOP KSEG1 direct-mapped)
 * ================================================================== */

#define SMAP_DEV9_BASE          0xb0000000u
#define SMAP_DEV9_ID            (SMAP_DEV9_BASE + 0x0002u)  /* u16 */
#define SMAP_DEV9_STATUS        (SMAP_DEV9_BASE + 0x0004u)  /* u16 */
#define SMAP_DEV9_PIO_OUT       (SMAP_DEV9_BASE + 0x0102u)  /* u8 */
#define SMAP_DEV9_INTR_VEC      (SMAP_DEV9_BASE + 0x0128u)  /* u16 */

#define SMAP_INTR_STAT          (SMAP_DEV9_BASE + 0x0028u)  /* u16, R/W */
#define SMAP_INTR_ENABLE        (SMAP_DEV9_BASE + 0x002au)  /* u16 */
#define SMAP_BD_MODE            (SMAP_DEV9_BASE + 0x0102u)  /* u8 */
#define SMAP_INTR_CLR           (SMAP_DEV9_BASE + 0x0128u)  /* u16, W1C */
#define SMAP_INTR_BITMSK        0x007Cu
#define SMAP_INTR_ENABLE_MASK   0x0078u
#define SMAP_INTR_RXEND         0x0020u
#define SMAP_INTR_TXEND         0x0010u
#define SMAP_INTR_RXDNV         0x0008u
#define SMAP_INTR_TXDNV         0x0004u

#define SMAP_EMAC3_RESET_REG    (SMAP_DEV9_BASE + 0x1000u)  /* u8 */
#define SMAP_EMAC3_FIFO_RST_REG (SMAP_DEV9_BASE + 0x1030u)  /* u8 */

#define SMAP_STACR              (SMAP_DEV9_BASE + 0x2000u)  /* u32 */

#define SMAP_EMAC3_MR1          (SMAP_DEV9_BASE + 0x2004u)  /* u32 */
#define SMAP_EMAC3_TXMODE1      (SMAP_DEV9_BASE + 0x200cu)  /* u32 */
#define SMAP_EMAC3_RXMODE       (SMAP_DEV9_BASE + 0x2010u)  /* u32 */
#define SMAP_EMAC3_INTR_STAT    (SMAP_DEV9_BASE + 0x2014u)  /* u32, W1C */
#define SMAP_EMAC3_INTR_ENABLE  (SMAP_DEV9_BASE + 0x2018u)  /* u32 */
#define SMAP_EMAC3_ADDR_HI      (SMAP_DEV9_BASE + 0x201cu)  /* u32 */
#define SMAP_EMAC3_ADDR_LO      (SMAP_DEV9_BASE + 0x2020u)  /* u32 */
#define SMAP_EMAC3_PAUSE_TIMER  (SMAP_DEV9_BASE + 0x202cu)  /* u32 */
#define SMAP_EMAC3_GROUP_HASH1  (SMAP_DEV9_BASE + 0x2040u)  /* u32 */
#define SMAP_EMAC3_GROUP_HASH2  (SMAP_DEV9_BASE + 0x2044u)  /* u32 */
#define SMAP_EMAC3_GROUP_HASH3  (SMAP_DEV9_BASE + 0x2048u)  /* u32 */
#define SMAP_EMAC3_GROUP_HASH4  (SMAP_DEV9_BASE + 0x204cu)  /* u32 */
#define SMAP_EMAC3_IFG          (SMAP_DEV9_BASE + 0x2058u)  /* u32 */
#define SMAP_EMAC3_TX_THRESHOLD (SMAP_DEV9_BASE + 0x2060u)  /* u32 */
#define SMAP_EMAC3_RX_WATERMARK (SMAP_DEV9_BASE + 0x2064u)  /* u32 */

#define SMAP_MR1_INIT_LE          0x00008164u
#define SMAP_TXMODE1_INIT_LE      0x0000380fu
#define SMAP_RXMODE_INIT_LE       0x0000c058u
#define SMAP_INTR_TX_ERR_BITS_LE  0x01c00000u
#define SMAP_E3_INTR_ALL_LE       0x03fb03ffu
#define SMAP_E3_DEAD_ALL_LE       0x03200000u
#define SMAP_PAUSE_TIMER_INIT_LE  0xffff0000u
#define SMAP_IFG_INIT_LE          0x00040000u
#define SMAP_TX_THRESHOLD_INIT_LE 0x00006000u
#define SMAP_RX_WATERMARK_INIT_LE 0x40000800u

#define SMAP_EMAC3_MODE0          (SMAP_DEV9_BASE + 0x2000u)  /* u32 */
#define SMAP_EMAC3_STA_CTRL       (SMAP_DEV9_BASE + 0x205cu)  /* u32 */

#define SMAP_MODE0_ENABLE_LE      0x00001800u

#define SMAP_PHY_CMD_READ         0x1000u
#define SMAP_PHY_CMD_WRITE        0x2000u
#define SMAP_PHY_ADDR_DSPHYTER    (1u << 5)

#define SMAP_PHY_BMCR             0x00u
#define SMAP_PHY_BMSR             0x01u
#define SMAP_PHY_ANAR             0x04u
#define SMAP_PHY_ANLPAR           0x05u
#define SMAP_PHY_PHYSTS           0x10u

#define SMAP_BMCR_RST             (1u << 15)
#define SMAP_BMCR_100M            (1u << 13)
#define SMAP_BMCR_ANEN            (1u << 12)
#define SMAP_BMCR_DUPM            (1u << 8)

#define SMAP_BMSR_ANCP            (1u << 5)
#define SMAP_BMSR_LINK            (1u << 2)

#define SMAP_AN_10HDX             (1u << 5)
#define SMAP_AN_10FDX             (1u << 6)
#define SMAP_AN_100HDX            (1u << 7)
#define SMAP_AN_100FDX            (1u << 8)

#define SMAP_PHYSTS_ANCP          (1u << 4)
#define SMAP_PHYSTS_FDX           (1u << 2)
#define SMAP_PHYSTS_10M           (1u << 1)
#define SMAP_PHYSTS_LINK          (1u << 0)

#define SMAP_MODE1_FDX            (1u << 31)
#define SMAP_MODE1_FLOWCTRL       (1u << 28)
#define SMAP_MODE1_ALLOW_PF       (1u << 27)
#define SMAP_MODE1_IGNORE_SQE     (1u << 24)
#define SMAP_MODE1_MEDIA_MSK      (3u << 22)
#define SMAP_MODE1_MEDIA_10M      (0u << 22)
#define SMAP_MODE1_MEDIA_100M     (1u << 22)

#define SMAP_PHY_RST_RETRIES      100000
#define SMAP_PHY_OPCOMP_RETRIES   100000
#define SMAP_PHY_ANCP_RETRIES     1000000
#define SMAP_PHY_LINK_RETRIES     500000

/* TX/RX FIFO + BD ring constants. */
#define SMAP_TXFIFO_CTRL        (SMAP_DEV9_BASE + 0x1000u)
#define SMAP_TXFIFO_WR_PTR      (SMAP_DEV9_BASE + 0x1004u)
#define SMAP_TXFIFO_FRAME_CNT   (SMAP_DEV9_BASE + 0x100Cu)
#define SMAP_TXFIFO_FRAME_INC   (SMAP_DEV9_BASE + 0x1010u)
#define SMAP_TXFIFO_DATA        (SMAP_DEV9_BASE + 0x1100u)

#define SMAP_RXFIFO_CTRL        (SMAP_DEV9_BASE + 0x1030u)
#define SMAP_RXFIFO_RD_PTR      (SMAP_DEV9_BASE + 0x1034u)
#define SMAP_RXFIFO_FRAME_CNT   (SMAP_DEV9_BASE + 0x103Cu)
#define SMAP_RXFIFO_FRAME_DEC   (SMAP_DEV9_BASE + 0x1040u)
#define SMAP_RXFIFO_DATA        (SMAP_DEV9_BASE + 0x1200u)

#define SMAP_EMAC3_TXMODE0      (SMAP_DEV9_BASE + 0x2008u)
#define SMAP_TXMODE0_GNP_0_LE   0x00008000u

#define SMAP_TXBUFBASE          0x1000u
#define SMAP_TXBUFSIZE          0x1000u
#define SMAP_RXBUFBASE          0x4000u

#define SMAP_BD_MAX_ENTRY       64
#define SMAP_BD_TX_BASE         (SMAP_DEV9_BASE + 0x3000u)
#define SMAP_BD_RX_BASE         (SMAP_DEV9_BASE + 0x3200u)
#define SMAP_BD_STRIDE          8u

#define SMAP_BD_TX_READY        0x8000u
#define SMAP_BD_TX_GENFCS       0x0200u
#define SMAP_BD_TX_GENPAD       0x0100u
#define SMAP_BD_TX_ERRMASK      0x03FFu
#define SMAP_BD_RX_EMPTY_BIT    0x8000u
#define SMAP_BD_RX_ERRMASK      0x7FFFu

#define SMAP_TXMAXSIZE          1514
#define SMAP_RXMINSIZE          14
#define SMAP_RXMAXSIZE          1518

#define SMAP_BD_BASE            (SMAP_DEV9_BASE + 0x3000u)
#define SMAP_BD_TX_END          (SMAP_BD_BASE   + 0x0200u)
#define SMAP_BD_RX_END          (SMAP_BD_BASE   + 0x0400u)
#define SMAP_BD_RX_EMPTY        0x8000u

#define SMAP_IRQ_NUM            0x7cu

#define SMAP_RESET_RETRIES      1000
#define SMAP_STACR_RETRIES      1000

#define SMAP_STACR_INIT         0x00002000u
#define SMAP_STACR_READY        0x20000000u

/* ================================================================== *
 * Volatile register helpers
 * ================================================================== */

#define REG8(addr)  (*(volatile unsigned char  *)(addr))
#define REG16(addr) (*(volatile unsigned short *)(addr))
#define REG32(addr) (*(volatile unsigned int   *)(addr))

/* ================================================================== *
 * Polling thread params
 * ================================================================== */

/* Pump cadence — frame-pump thread sleeps SMAP_POLL_USEC between
 * iterations.  Bumped from 500us to 200us in ABI v5 to absorb bursts
 * faster: more wake-ups per second, but the deeper RX ring (16 slots
 * since ABI v4) easily tolerates the extra absorbed bursts.  At 200us
 * the pump runs ~5000 Hz; conditional FlushDcache (see pump_thread)
 * keeps idle iterations cheap so the higher cadence is sustainable. */
#define SMAP_POLL_USEC         200
/* Both worker threads MUST run at lower priority (higher number) than
 * LOADFILE's RPC service thread (priority 88).  Otherwise the workers
     * preempt LOADFILE during _start and starve it — LMB never returns
     * RPC_END, EE side prints "SMAP NO REPLY". */
#define SMAP_RPC_PRIORITY      96
#define SMAP_RPC_STACKSIZE     0x1000
#define SMAP_PUMP_PRIORITY     100
#define SMAP_PUMP_STACKSIZE    0x2000
#define SMAP_LINK_POLL_TICKS   500u

/* ================================================================== *
 * Module state
 * ================================================================== */

struct smap_chan_iop {
    unsigned char hwaddr[6];
    unsigned char _pad[2];
    unsigned int  flags;

    /* TX bookkeeping. */
    int           txbds;
    int           txbdi;
    int           txbdusedcnt;
    int           txfreebufsize;
    unsigned int  txbwp;

    /* RX bookkeeping. */
    int           rxbdi;
    unsigned int  rxbrp;
};

static struct smap_chan_iop g_smap_chan;

/* IRX-private status globals — replace the old mailbox header fields.
 * Volatile because the RPC handler thread reads while the frame-pump
 * thread (and during init, _start itself) writes.  These are the
 * single source of truth for boot/fault/link/MAC state. */
static volatile unsigned int  g_smap_boot_state;   /* enum ps2_smap_boot_status */
static volatile unsigned int  g_smap_fault;        /* enum ps2_smap_fault */
static volatile unsigned int  g_smap_link_state;   /* enum ps2_smap_link_state */
static volatile unsigned char g_smap_mac[6];
static volatile unsigned int  g_smap_init_done;

/* MAC override parsed from argv[1]; only used if string is parseable
 * and produces a valid MAC. */
static unsigned char g_mac_override[6];
static int           g_have_mac_override;

/* ================================================================== *
 * Shared region + RPC server state
 * ================================================================== */

static unsigned char *g_shared_base;          /* IOP-virtual base of region */
static unsigned int   g_shared_iop_phys;      /* IOP-physical (1F-ish) */
static unsigned int   g_shared_size;
static volatile ps2_smap_hot_diag_t  *g_hot_diag;
static volatile ps2_smap_rx_desc_t   *g_rx_ring;
static unsigned char                 *g_rx_data;   /* IOP-side write pointer */
static unsigned char                 *g_tx_data;   /* IOP-side read pointer */
static unsigned int                   g_rx_data_offset;
static unsigned int                   g_tx_data_offset;
static unsigned int                   g_rx_slot_size;
static unsigned int                   g_tx_slot_size;

/* Per-slot sequence counters.  IOP bumps each on free→busy transition
 * (or RX delivered).  RPC handler validates the EE-supplied seq before
 * accepting RELEASE_RX / SUBMIT_TX. */
static volatile unsigned int g_rx_seq[PS2_SMAP_RX_SLOTS];
static volatile unsigned int g_tx_seq[PS2_SMAP_TX_SLOTS];

/* Per-slot length staging — the SUBMIT_TX RPC writes the requested
 * length; the frame pump consumes it on the next iteration. */
static volatile unsigned int g_tx_slot_len[PS2_SMAP_TX_SLOTS];

/* Last validation reject snapshot (filled when RPC handler rejects
 * a request — surfaced via GET_DIAG cold path). */
static volatile unsigned int g_last_validation_op;
static volatile unsigned int g_last_validation_slot;
static volatile unsigned int g_last_validation_seq;
static volatile unsigned int g_last_validation_len;

/* RPC server scaffolding. */
static smap_iop_rpc_queue_t  g_queue   __attribute__((aligned(64)));
static smap_iop_rpc_server_t g_server  __attribute__((aligned(64)));

/* RPC reception buffer.  Sized to PS2_SMAP_RPC_BUF_SIZE so the largest
 * request payload fits.  RpcRegister is told this is the receive
 * buffer; the RPC handler interprets the bytes as a request struct. */
static unsigned char g_rpc_buf[PS2_SMAP_RPC_BUF_SIZE]
    __attribute__((aligned(64)));

/* Cached layout response.  Built once in _start; GET_LAYOUT returns a
 * pointer to this. */
static ps2_smap_layout_rsp_t g_layout __attribute__((aligned(64)));

/* Response staging structs — must outlive the RPC handler call so the
 * SIFRPC stack can DMA them to EE. */
static ps2_smap_status_rsp_t    g_status_rsp     __attribute__((aligned(64)));
static ps2_smap_cold_diag_rsp_t g_cold_diag_rsp  __attribute__((aligned(64)));
static ps2_smap_rtc_rsp_t       g_rtc_rsp        __attribute__((aligned(64)));
static ps2_smap_rc_rsp_t        g_rc_rsp         __attribute__((aligned(64)));

/* ================================================================== *
 * Helpers: argv MAC parse, MAC validation
 * ================================================================== */

static int parse_mac_arg(const char *text, unsigned char *mac)
{
    int i;

    if (text == 0 || mac == 0)
        return -1;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;

    for (i = 0; i < 6; i++) {
        unsigned int value = 0;
        int n;
        if (*text == ':' || *text == '-')
            text++;
        for (n = 0; n < 2; n++) {
            unsigned int digit;
            char c = *text++;
            if (c >= '0' && c <= '9')
                digit = (unsigned int)(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = (unsigned int)(c - 'a') + 10u;
            else if (c >= 'A' && c <= 'F')
                digit = (unsigned int)(c - 'A') + 10u;
            else
                return -1;
            value = (value << 4) | digit;
        }
        mac[i] = (unsigned char)value;
    }

    return (*text == '\0') ? 0 : -1;
}

static int smap_mac_is_valid(const unsigned char *mac)
{
    if (mac == 0)
        return 0;
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0u)
        return 0;
    if ((mac[0] & mac[1] & mac[2] & mac[3] & mac[4] & mac[5]) == 0xffu)
        return 0;
    if (mac[0] & 1u)
        return 0;
    return 1;
}

/* IOP IRX byte-publication helpers — see
 * memory/feedback_iop_irx_byte_publication.md.  noinline+noipa keeps
 * the compiler from re-introducing absolute HI16/LO16 addressing for
 * `g_smap_mac` writes. */
static __attribute__((noinline, noipa))
void smap_iop_publish_word_barrier(volatile unsigned int *probe)
{
    __asm__ volatile ("" ::: "memory");
    (void)*probe;
    __asm__ volatile ("" ::: "memory");
}

static __attribute__((noinline, noipa))
void smap_publish_mac_bytes(volatile unsigned char *dst,
                            const unsigned char *src)
{
    unsigned int hi = (unsigned int)src[0]
                    | ((unsigned int)src[1] << 8)
                    | ((unsigned int)src[2] << 16)
                    | ((unsigned int)src[3] << 24);
    unsigned short lo = (unsigned short)src[4]
                      | (unsigned short)((unsigned int)src[5] << 8);
    *(volatile unsigned int   *)(void *)dst        = hi;
    *(volatile unsigned short *)(void *)(dst + 4)  = lo;
    smap_iop_publish_word_barrier((volatile unsigned int *)(void *)dst);
}

/* ================================================================== *
 * Phase 2 — DEV9 hardware presence check
 * ================================================================== */

static int smap_dev9_validate(void)
{
    unsigned int status;
    unsigned int dev_id;

    status = REG16(SMAP_DEV9_STATUS);
    if ((status & 1u) == 0)
        return -(int)PS2_SMAP_FAULT_DEV9_NOT_READY;

    dev_id = REG16(SMAP_DEV9_ID);
    if (dev_id < 17u)
        return -(int)PS2_SMAP_FAULT_DEV9_ID_BAD;

    return 0;
}

/* ================================================================== *
 * Phase 4 — EMAC3 soft reset
 * ================================================================== */

static int smap_emac3_soft_reset(void)
{
    int i;

    REG8(SMAP_EMAC3_RESET_REG) = 1;

    for (i = 0; i < SMAP_RESET_RETRIES; i++) {
        if ((REG8(SMAP_EMAC3_RESET_REG) & 1u) == 0)
            return 0;
    }

    return -1;
}

/* ================================================================== *
 * Phase 5 — TX/RX FIFO reset
 * ================================================================== */

static int smap_fifo_reset(void)
{
    int i;

    REG8(SMAP_EMAC3_FIFO_RST_REG) = 1;

    for (i = 0; i < SMAP_RESET_RETRIES; i++) {
        if ((REG8(SMAP_EMAC3_FIFO_RST_REG) & 1u) == 0)
            return 0;
    }

    return -1;
}

/* ================================================================== *
 * Phase 6 — EMAC3 MODE0 soft reset (via STACR alias)
 * ================================================================== */

static int smap_stacr_init(void)
{
    unsigned int v;
    int i;

    REG32(SMAP_STACR) = SMAP_STACR_INIT;
    (void)REG32(SMAP_STACR);
    (void)REG32(SMAP_STACR);

    for (i = 0; i < SMAP_STACR_RETRIES; i++) {
        v = REG32(SMAP_STACR);
        v = (v >> 16) | (v << 16);
        if ((v & SMAP_STACR_READY) == 0)
            return 0;
    }

    return -1;
}

/* ================================================================== *
 * Phase 7 — clear PIO out
 * ================================================================== */

static void smap_clear_pio(void)
{
    REG8(SMAP_DEV9_PIO_OUT) = 0;
}

static void smap_interrupts_disable_clear(void)
{
    REG16(SMAP_INTR_ENABLE) &= (unsigned short)~SMAP_INTR_ENABLE_MASK;
    REG32(SMAP_EMAC3_INTR_ENABLE) = 0u;
    REG16(SMAP_INTR_CLR) = SMAP_INTR_BITMSK;
    REG32(SMAP_EMAC3_INTR_STAT) = SMAP_E3_INTR_ALL_LE;
}

static void smap_interrupts_enable(void)
{
    REG16(SMAP_INTR_CLR) = SMAP_INTR_BITMSK;
    REG32(SMAP_EMAC3_INTR_STAT) = SMAP_E3_INTR_ALL_LE;
    REG16(SMAP_INTR_ENABLE) |= SMAP_INTR_ENABLE_MASK;
    REG32(SMAP_EMAC3_INTR_ENABLE) = SMAP_E3_INTR_ALL_LE;
}

static void smap_bd_mode_init(void)
{
    REG8(SMAP_BD_MODE) = 0u;
}

/* ================================================================== *
 * Phase 8/9 — TX + RX BD zero-fill
 * ================================================================== */

static void smap_descriptors_zero(void)
{
    unsigned int addr;

    for (addr = SMAP_BD_BASE; addr < SMAP_BD_TX_END; addr += 8u) {
        REG16(addr + 0) = 0;
        REG16(addr + 2) = 0;
        REG16(addr + 4) = 0;
        REG16(addr + 6) = 0;
    }

    for (addr = SMAP_BD_TX_END; addr < SMAP_BD_RX_END; addr += 8u) {
        REG16(addr + 0) = SMAP_BD_RX_EMPTY;
        REG16(addr + 2) = 0;
        REG16(addr + 4) = 0;
        REG16(addr + 6) = 0;
    }
}

/* ================================================================== *
 * Phase 10 — register SMAP IRQ vector in DEV9
 * ================================================================== */

static void smap_install_irq_vector(void)
{
    REG16(SMAP_DEV9_INTR_VEC) = SMAP_IRQ_NUM;
}

/* ================================================================== *
 * Phase 11 — read MAC from EEPROM
 * ================================================================== */

static int smap_get_node_addr(struct smap_chan_iop *smap)
{
    unsigned short buf[4];
    int rc;

    if (g_have_mac_override) {
        int i;
        for (i = 0; i < 6; i++)
            smap->hwaddr[i] = g_mac_override[i];
        return 0;
    }

    buf[0] = 0;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 0;

    rc = dev9_read_eeprom(buf);
    if (rc < 0)
        return -(int)PS2_SMAP_FAULT_EEPROM_IO;

    smap->hwaddr[0] = (unsigned char)(buf[0] & 0xffu);
    smap->hwaddr[1] = (unsigned char)((buf[0] >> 8) & 0xffu);
    smap->hwaddr[2] = (unsigned char)(buf[1] & 0xffu);
    smap->hwaddr[3] = (unsigned char)((buf[1] >> 8) & 0xffu);
    smap->hwaddr[4] = (unsigned char)(buf[2] & 0xffu);
    smap->hwaddr[5] = (unsigned char)((buf[2] >> 8) & 0xffu);

    return 0;
}

/* ================================================================== *
 * Phase 12 — EMAC3 mode-register block
 * ================================================================== */

static unsigned int smap_byteswap_halfword(unsigned int v)
{
    return ((v & 0xff00u) >> 8) | ((v & 0x00ffu) << 8);
}

static void smap_emac3_program_mode(const struct smap_chan_iop *smap)
{
    unsigned int mac01, mac23, mac45;
    unsigned int addr_hi, addr_lo;

    REG32(SMAP_EMAC3_MR1) = SMAP_MR1_INIT_LE;
    (void)REG32(SMAP_EMAC3_MR1);

    REG32(SMAP_EMAC3_TXMODE1) = SMAP_TXMODE1_INIT_LE;
    (void)REG32(SMAP_EMAC3_TXMODE1);

    REG32(SMAP_EMAC3_RXMODE) = SMAP_RXMODE_INIT_LE;
    (void)REG32(SMAP_EMAC3_RXMODE);

    REG32(SMAP_EMAC3_INTR_STAT) = SMAP_INTR_TX_ERR_BITS_LE;
    (void)REG32(SMAP_EMAC3_INTR_STAT);

    REG32(SMAP_EMAC3_INTR_ENABLE) = SMAP_INTR_TX_ERR_BITS_LE;
    (void)REG32(SMAP_EMAC3_INTR_ENABLE);

    mac01 = smap_byteswap_halfword(((unsigned int)smap->hwaddr[1] << 8)
                                 | (unsigned int)smap->hwaddr[0]);
    mac23 = smap_byteswap_halfword(((unsigned int)smap->hwaddr[3] << 8)
                                 | (unsigned int)smap->hwaddr[2]);
    mac45 = smap_byteswap_halfword(((unsigned int)smap->hwaddr[5] << 8)
                                 | (unsigned int)smap->hwaddr[4]);

    addr_hi = mac01 << 16;
    REG32(SMAP_EMAC3_ADDR_HI) = addr_hi;
    (void)REG32(SMAP_EMAC3_ADDR_HI);

    addr_lo = (mac45 << 16) | (mac23 & 0xffffu);
    REG32(SMAP_EMAC3_ADDR_LO) = addr_lo;
    (void)REG32(SMAP_EMAC3_ADDR_LO);

    REG32(SMAP_EMAC3_PAUSE_TIMER) = SMAP_PAUSE_TIMER_INIT_LE;
    (void)REG32(SMAP_EMAC3_PAUSE_TIMER);

    REG32(SMAP_EMAC3_GROUP_HASH1) = 0u;
    (void)REG32(SMAP_EMAC3_GROUP_HASH1);
    REG32(SMAP_EMAC3_GROUP_HASH2) = 0u;
    (void)REG32(SMAP_EMAC3_GROUP_HASH2);
    REG32(SMAP_EMAC3_GROUP_HASH3) = 0u;
    (void)REG32(SMAP_EMAC3_GROUP_HASH3);
    REG32(SMAP_EMAC3_GROUP_HASH4) = 0u;
    (void)REG32(SMAP_EMAC3_GROUP_HASH4);

    REG32(SMAP_EMAC3_IFG) = SMAP_IFG_INIT_LE;
    (void)REG32(SMAP_EMAC3_IFG);

    REG32(SMAP_EMAC3_TX_THRESHOLD) = SMAP_TX_THRESHOLD_INIT_LE;
    (void)REG32(SMAP_EMAC3_TX_THRESHOLD);

    REG32(SMAP_EMAC3_RX_WATERMARK) = SMAP_RX_WATERMARK_INIT_LE;
    (void)REG32(SMAP_EMAC3_RX_WATERMARK);
}

/* ================================================================== *
 * Phase 13 — PHY MII helpers + reset + autoneg
 * ================================================================== */

static int smap_phy_wait_op_comp(void)
{
    int i;
    unsigned int v;

    for (i = 0; i < SMAP_PHY_OPCOMP_RETRIES; i++) {
        v = REG32(SMAP_EMAC3_STA_CTRL);
        if (v & 0x80000000u)
            return 0;
    }
    return -1;
}

static int smap_phy_write(unsigned int reg, unsigned int data)
{
    unsigned int cmd_le, cmd_swapped;

    cmd_le = (data << 16)
           | SMAP_PHY_CMD_WRITE
           | SMAP_PHY_ADDR_DSPHYTER
           | (reg & 0x1fu);

    cmd_swapped = (cmd_le << 16) | (cmd_le >> 16);
    REG32(SMAP_EMAC3_STA_CTRL) = cmd_swapped;
    (void)REG32(SMAP_EMAC3_STA_CTRL);

    return smap_phy_wait_op_comp();
}

static int smap_phy_read(unsigned int reg, unsigned int *data_out)
{
    unsigned int cmd_le, cmd_swapped, v;

    cmd_le = SMAP_PHY_CMD_READ
           | SMAP_PHY_ADDR_DSPHYTER
           | (reg & 0x1fu);

    cmd_swapped = (cmd_le << 16) | (cmd_le >> 16);
    REG32(SMAP_EMAC3_STA_CTRL) = cmd_swapped;
    (void)REG32(SMAP_EMAC3_STA_CTRL);

    if (smap_phy_wait_op_comp() < 0)
        return -1;

    v = REG32(SMAP_EMAC3_STA_CTRL);
    v = (v << 16) | (v >> 16);
    *data_out = (v >> 16) & 0xffffu;
    return 0;
}

static int smap_phy_reset(void)
{
    unsigned int phyval;
    int i;

    if (smap_phy_write(SMAP_PHY_BMCR,
                       SMAP_BMCR_RST | SMAP_BMCR_100M
                       | SMAP_BMCR_ANEN | SMAP_BMCR_DUPM) < 0)
        return -1;

    for (i = 0; i < SMAP_PHY_RST_RETRIES; i++) {
        if (smap_phy_read(SMAP_PHY_BMCR, &phyval) < 0)
            return -1;
        if (!(phyval & SMAP_BMCR_RST))
            return 0;
    }

    return -1;
}

static unsigned int smap_decode_link(unsigned int phyval)
{
    unsigned int fdx = phyval & SMAP_PHYSTS_FDX;
    unsigned int speed_10 = phyval & SMAP_PHYSTS_10M;

    if (speed_10)
        return fdx ? PS2_SMAP_LINK_10FDX : PS2_SMAP_LINK_10HDX;
    return fdx ? PS2_SMAP_LINK_100FDX : PS2_SMAP_LINK_100HDX;
}

static unsigned int smap_decode_an_link(unsigned int anar, unsigned int anlpar)
{
    unsigned int common = anar & anlpar;

    if (common & SMAP_AN_100FDX)
        return PS2_SMAP_LINK_100FDX;
    if (common & SMAP_AN_100HDX)
        return PS2_SMAP_LINK_100HDX;
    if (common & SMAP_AN_10FDX)
        return PS2_SMAP_LINK_10FDX;
    if (common & SMAP_AN_10HDX)
        return PS2_SMAP_LINK_10HDX;
    return PS2_SMAP_LINK_DOWN;
}

static void smap_apply_link_mode(unsigned int link_state)
{
    unsigned int e3v, e3v_le;

    if (link_state == PS2_SMAP_LINK_DOWN)
        return;

    e3v_le = REG32(SMAP_EMAC3_MR1);
    e3v    = (e3v_le << 16) | (e3v_le >> 16);

    if (link_state == PS2_SMAP_LINK_100FDX ||
        link_state == PS2_SMAP_LINK_10FDX) {
        e3v |= (SMAP_MODE1_FDX | SMAP_MODE1_FLOWCTRL | SMAP_MODE1_ALLOW_PF);
    } else {
        e3v &= ~(SMAP_MODE1_FDX | SMAP_MODE1_FLOWCTRL | SMAP_MODE1_ALLOW_PF);
        if (link_state == PS2_SMAP_LINK_10HDX)
            e3v &= ~SMAP_MODE1_IGNORE_SQE;
    }

    e3v &= ~SMAP_MODE1_MEDIA_MSK;
    if (link_state == PS2_SMAP_LINK_10HDX ||
        link_state == PS2_SMAP_LINK_10FDX)
        e3v |= SMAP_MODE1_MEDIA_10M;
    else
        e3v |= SMAP_MODE1_MEDIA_100M;

    e3v_le = (e3v << 16) | (e3v >> 16);
    REG32(SMAP_EMAC3_MR1) = e3v_le;
    (void)REG32(SMAP_EMAC3_MR1);
}

static unsigned int smap_phy_poll_link(void)
{
    unsigned int bmsr;
    unsigned int physts;
    unsigned int anar;
    unsigned int anlpar;
    unsigned int link_state;

    if (smap_phy_read(SMAP_PHY_BMSR, &bmsr) < 0)
        return PS2_SMAP_LINK_DOWN;
    if (smap_phy_read(SMAP_PHY_BMSR, &bmsr) < 0)
        return PS2_SMAP_LINK_DOWN;

    if ((bmsr & SMAP_BMSR_LINK) == 0)
        return PS2_SMAP_LINK_DOWN;

    if (smap_phy_read(SMAP_PHY_PHYSTS, &physts) == 0 &&
        (physts & SMAP_PHYSTS_LINK) != 0) {
        link_state = smap_decode_link(physts);
        smap_apply_link_mode(link_state);
        return link_state;
    }

    if ((bmsr & SMAP_BMSR_ANCP) == 0)
        return PS2_SMAP_LINK_DOWN;
    if (smap_phy_read(SMAP_PHY_ANAR, &anar) < 0)
        return PS2_SMAP_LINK_DOWN;
    if (smap_phy_read(SMAP_PHY_ANLPAR, &anlpar) < 0)
        return PS2_SMAP_LINK_DOWN;

    link_state = smap_decode_an_link(anar, anlpar);
    smap_apply_link_mode(link_state);
    return link_state;
}

static int smap_phy_init(void)
{
    int rc;

    rc = smap_phy_reset();
    if (rc < 0)
        return -(int)PS2_SMAP_FAULT_PHY_RESET;

    g_smap_link_state = PS2_SMAP_LINK_DOWN;
    return 0;
}

/* ================================================================== *
 * Phase 14 — Enable TX/RX MAC
 * ================================================================== */

static void smap_emac3_enable_mac(void)
{
    REG32(SMAP_EMAC3_MODE0) = SMAP_MODE0_ENABLE_LE;
    (void)REG32(SMAP_EMAC3_MODE0);
}

/* ================================================================== *
 * Orchestrator: smap_chip_init
 *
 * On fault returns a negative ps2_smap_fault code; on success returns
 * 0 and leaves smap->hwaddr filled with the EEPROM MAC.
 * ================================================================== */

static int smap_chip_init(struct smap_chan_iop *smap)
{
    int rc;

    rc = smap_dev9_validate();
    if (rc < 0)
        return rc;

    if (smap_emac3_soft_reset() < 0)
        return -(int)PS2_SMAP_FAULT_EMAC3_RESET;

    if (smap_fifo_reset() < 0)
        return -(int)PS2_SMAP_FAULT_FIFO_RESET;

    if (smap_stacr_init() < 0)
        return -(int)PS2_SMAP_FAULT_STACR_TIMEOUT;

    smap_clear_pio();
    smap_interrupts_disable_clear();
    smap_bd_mode_init();
    smap_descriptors_zero();
    smap_install_irq_vector();

    rc = smap_get_node_addr(smap);
    if (rc < 0)
        return rc;

    smap_emac3_program_mode(smap);

    rc = smap_phy_init();
    if (rc < 0)
        return rc;

    smap_interrupts_enable();
    smap_emac3_enable_mac();

    return 0;
}

/* ================================================================== *
 * BD ring helpers
 * ================================================================== */

#define SMAP_TX_BD_ADDR(i) (SMAP_BD_TX_BASE + ((unsigned int)(i)) * SMAP_BD_STRIDE)
#define SMAP_RX_BD_ADDR(i) (SMAP_BD_RX_BASE + ((unsigned int)(i)) * SMAP_BD_STRIDE)

static unsigned int smap_bd_ctrl_stat(unsigned int bd_addr)
{
    return REG16(bd_addr + 0u);
}

static unsigned int smap_bd_length(unsigned int bd_addr)
{
    return REG16(bd_addr + 4u);
}

static void smap_bd_set_ctrl_stat(unsigned int bd_addr, unsigned int v)
{
    REG16(bd_addr + 0u) = (unsigned short)v;
}

static void smap_bd_set_length(unsigned int bd_addr, unsigned int v)
{
    REG16(bd_addr + 4u) = (unsigned short)v;
}

static void smap_bd_set_pointer(unsigned int bd_addr, unsigned int v)
{
    REG16(bd_addr + 6u) = (unsigned short)v;
}

static unsigned int smap_bd_pointer(unsigned int bd_addr)
{
    return REG16(bd_addr + 6u);
}

static int smap_bd_next(int idx)
{
    if (idx == SMAP_BD_MAX_ENTRY - 1)
        return 0;
    return idx + 1;
}

/* ================================================================== *
 * Phase 15 — software-side BD ring init
 * ================================================================== */

static void smap_iop_chan_init(struct smap_chan_iop *smap)
{
    smap->txbds         = 0;
    smap->txbdi         = 0;
    smap->txbdusedcnt   = 0;
    smap->txfreebufsize = SMAP_TXBUFSIZE;
    smap->txbwp         = SMAP_TXBUFBASE;

    smap->rxbdi         = 0;
    smap->rxbrp         = SMAP_RXBUFBASE;
}

/* ================================================================== *
 * Phase 16a — TX completion drain
 * ================================================================== */

static void smap_iop_tx_intr(struct smap_chan_iop *smap)
{
    unsigned int bd_addr;
    unsigned int ctrl_stat;
    unsigned int length;
    unsigned int txlen;

    while (smap->txbdusedcnt > 0) {
        bd_addr = SMAP_TX_BD_ADDR(smap->txbdi);
        ctrl_stat = smap_bd_ctrl_stat(bd_addr);

        if (ctrl_stat & SMAP_BD_TX_READY)
            break;

        if (g_hot_diag) {
            if ((ctrl_stat & SMAP_BD_TX_ERRMASK) != 0)
                g_hot_diag->tx_underruns =
                    g_hot_diag->tx_underruns + 1u;
            else
                g_hot_diag->tx_packets =
                    g_hot_diag->tx_packets + 1u;
        }

        length = smap_bd_length(bd_addr);
        txlen = (length + 3u) & ~3u;
        smap->txfreebufsize += (int)txlen;
        smap->txbdusedcnt--;

        smap_bd_set_length(bd_addr, 0);
        smap_bd_set_pointer(bd_addr, 0);
        smap_bd_set_ctrl_stat(bd_addr, 0);

        smap->txbdi = smap_bd_next(smap->txbdi);
    }
}

/* ================================================================== *
 * Phase 16b — TX one frame
 * ================================================================== */

static int smap_iop_send(struct smap_chan_iop *smap,
                         const unsigned char *frame,
                         unsigned int total_len)
{
    unsigned int txlen;
    unsigned int bd_addr;
    unsigned int i;

    if (total_len > SMAP_TXMAXSIZE || total_len == 0)
        return -1;

    txlen = (total_len + 3u) & ~3u;

    if ((int)txlen > smap->txfreebufsize)
        return -2;
    if (smap->txbdusedcnt >= SMAP_BD_MAX_ENTRY - 1)
        return -3;

    bd_addr = SMAP_TX_BD_ADDR(smap->txbds);
    if (smap_bd_ctrl_stat(bd_addr) & SMAP_BD_TX_READY)
        return -4;

    {
        int spin;
        for (spin = SMAP_PHY_OPCOMP_RETRIES; spin > 0; spin--) {
            if (!(REG32(SMAP_EMAC3_TXMODE0) & SMAP_TXMODE0_GNP_0_LE))
                break;
        }
        if (spin == 0)
            return -5;
    }

    {
        const volatile unsigned int *src_w =
            (const volatile unsigned int *)(const volatile void *)frame;
        unsigned int full_words = total_len & ~3u;
        unsigned int tail_bytes = total_len & 3u;

        REG16(SMAP_TXFIFO_WR_PTR) = (unsigned short)(smap->txbwp & 0x0FFCu);
        for (i = 0; i < full_words; i += 4u)
            REG32(SMAP_TXFIFO_DATA) = *src_w++;
        if (tail_bytes != 0) {
            const volatile unsigned char *tail_b =
                (const volatile unsigned char *)src_w;
            unsigned int last = 0u;
            if (tail_bytes >= 1u) last |= (unsigned int)tail_b[0];
            if (tail_bytes >= 2u) last |= (unsigned int)tail_b[1] << 8;
            if (tail_bytes >= 3u) last |= (unsigned int)tail_b[2] << 16;
            REG32(SMAP_TXFIFO_DATA) = last;
        }
    }

    smap->txfreebufsize -= (int)txlen;

    smap_bd_set_length(bd_addr, total_len);
    smap_bd_set_pointer(bd_addr, smap->txbwp);
    REG8(SMAP_TXFIFO_FRAME_INC) = 1u;
    smap_bd_set_ctrl_stat(bd_addr,
                          SMAP_BD_TX_READY | SMAP_BD_TX_GENFCS
                          | SMAP_BD_TX_GENPAD);
    (void)smap_bd_ctrl_stat(bd_addr);
    (void)REG8(SMAP_TXFIFO_FRAME_CNT);
    smap->txbdusedcnt++;

    REG32(SMAP_EMAC3_TXMODE0) = SMAP_TXMODE0_GNP_0_LE;
    (void)REG32(SMAP_EMAC3_TXMODE0);

    smap->txbwp = SMAP_TXBUFBASE
                + ((smap->txbwp + txlen) % SMAP_TXBUFSIZE);
    smap->txbds = smap_bd_next(smap->txbds);

    return 0;
}

/* ================================================================== *
 * Phase 17 — RX drain
 *
 * Walks RX BDs from rxbdi while non-empty.  For each non-error frame,
 * picks a free RX slot in the shared region (rx_head producer cursor)
 * and copies FIFO bytes directly into the slot's data area.  No free
 * slot → drop and bump rx_drops_no_slot.
 *
 * Returns frames delivered (for diagnostic counters).
 * ================================================================== */

static int smap_rx_slot_is_free(unsigned int slot)
{
    /* RX slot is free if seq matches the descriptor seq the EE last
     * acknowledged (via RELEASE_RX).  We rely on the producer cursor
     * (rx_head) wrapping over rx_tail_iop_view to determine the next
     * write position; "free" really means "not currently occupied". */
    (void)slot;
    return 1;
}

static unsigned int smap_iop_drain_rx(struct smap_chan_iop *smap)
{
    unsigned int delivered = 0;
    unsigned int bd_addr;
    unsigned int rxstat;
    unsigned int pkt_len;
    unsigned int rxlen;
    unsigned int i;
    int empty_skips = 0;
    unsigned int next_head;
    unsigned int slot;

    if (g_hot_diag == 0 || g_rx_data == 0 || g_rx_ring == 0)
        return 0;

    while (1) {
        bd_addr = SMAP_RX_BD_ADDR(smap->rxbdi);
        rxstat = smap_bd_ctrl_stat(bd_addr);

        if (rxstat & SMAP_BD_RX_EMPTY_BIT) {
            if (REG8(SMAP_RXFIFO_FRAME_CNT) == 0)
                break;
            if (++empty_skips >= SMAP_BD_MAX_ENTRY)
                break;
            smap->rxbdi = smap_bd_next(smap->rxbdi);
            continue;
        }

        empty_skips = 0;

        if ((rxstat & SMAP_BD_RX_ERRMASK) != 0) {
            g_hot_diag->rx_drops_fcs = g_hot_diag->rx_drops_fcs + 1u;
            goto advance;
        }
        pkt_len = smap_bd_length(bd_addr);
        if (pkt_len < SMAP_RXMINSIZE || pkt_len > SMAP_RXMAXSIZE) {
            g_hot_diag->rx_drops_fcs = g_hot_diag->rx_drops_fcs + 1u;
            goto advance;
        }

        rxlen = (pkt_len + 3u) & ~3u;
        smap->rxbrp = smap_bd_pointer(bd_addr);

        /* Reservation check on the shared ring: if rx_head + 1 wraps to
         * the EE-acknowledged tail (rx_tail_iop_view), we're full. */
        next_head = (g_hot_diag->rx_head + 1u) % PS2_SMAP_RX_SLOTS;
        if (next_head == (g_hot_diag->rx_tail_iop_view % PS2_SMAP_RX_SLOTS)
            && g_hot_diag->rx_head != g_hot_diag->rx_tail_iop_view) {
            /* Drain into nowhere. */
            REG16(SMAP_RXFIFO_RD_PTR) = (unsigned short)(smap->rxbrp & 0x3FFCu);
            for (i = 0; i < rxlen; i += 4u)
                (void)REG32(SMAP_RXFIFO_DATA);
            g_hot_diag->rx_drops_no_slot =
                g_hot_diag->rx_drops_no_slot + 1u;
            goto advance;
        }

        slot = g_hot_diag->rx_head % PS2_SMAP_RX_SLOTS;
        (void)smap_rx_slot_is_free(slot);

        {
            volatile unsigned int *frame_dst_w =
                (volatile unsigned int *)(volatile void *)
                (g_rx_data + slot * g_rx_slot_size);

            REG16(SMAP_RXFIFO_RD_PTR) = (unsigned short)(smap->rxbrp & 0x3FFCu);
            for (i = 0; i < rxlen; i += 4u)
                *frame_dst_w++ = REG32(SMAP_RXFIFO_DATA);

            smap_iop_publish_word_barrier(
                (volatile unsigned int *)(g_rx_data + slot * g_rx_slot_size));
            g_rx_ring[slot].len    = pkt_len;
            g_rx_ring[slot].status = 0u;
            g_rx_seq[slot]         = g_rx_seq[slot] + 1u;
            g_rx_ring[slot].seq    = g_rx_seq[slot];
            smap_iop_publish_word_barrier(&g_rx_ring[slot].seq);

            g_hot_diag->rx_head = g_hot_diag->rx_head + 1u;
            smap_iop_publish_word_barrier(&g_hot_diag->rx_head);
            g_hot_diag->rx_packets = g_hot_diag->rx_packets + 1u;
        }
        delivered++;

advance:
        REG8(SMAP_RXFIFO_FRAME_DEC) = 1u;
        smap_bd_set_length(bd_addr, 0);
        smap_bd_set_pointer(bd_addr, 0);
        smap_bd_set_ctrl_stat(bd_addr, SMAP_BD_RX_EMPTY_BIT);
        smap->rxbdi = smap_bd_next(smap->rxbdi);
    }

    return delivered;
}

/* ================================================================== *
 * RPC handler — whitelisted fno dispatch
 * ================================================================== */

static void zero_bytes(void *ptr, unsigned int len)
{
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    unsigned int i;

    for (i = 0; i < len; i++)
        p[i] = 0;
}

/* Read mechacon RTC and return Unix UTC seconds.
 *
 * Mechacon stores a JST calendar (year 00..99 = 2000..2099, BCD).  We
 * convert that calendar to a Unix-style seconds count, then subtract
 * the JST bias to get true Unix UTC.  Doing the bias in the seconds
 * domain (not as a bare hour mutation) means day/month/year roll
 * back correctly when the JST hour is < 9. */
static int smap_cdvd_read_rtc(unsigned int *timestamp)
{
    smap_cdvd_clock_t clock;
    int sec;
    int min;
    int hour;
    int day;
    int month;
    int year;
    unsigned int jst_unix;

    if (timestamp == 0)
        return (int)PS2_SMAP_RC_BAD_LEN;

    zero_bytes(&clock, sizeof(clock));
    if (iop_cd_read_clock(&clock) <= 0)
        return (int)PS2_SMAP_RC_INTERNAL;

    sec   = smap_bcd_to_int(clock.second);
    min   = smap_bcd_to_int(clock.minute);
    hour  = smap_bcd_to_int(clock.hour);
    day   = smap_bcd_to_int(clock.day);
    month = smap_bcd_to_int(clock.month);
    year  = smap_bcd_to_int(clock.year);

    if (sec < 0 || sec > 59 || min < 0 || min > 59
        || hour < 0 || hour > 23 || day <= 0 || day > 31
        || month <= 0 || month > 12 || year < 0)
        return (int)PS2_SMAP_RC_INTERNAL;

    /* JST calendar -> Unix-style seconds (still JST-shifted). */
    jst_unix = smap_datetime_to_unix(2000u + (unsigned int)year,
                                     (unsigned int)month,
                                     (unsigned int)day,
                                     (unsigned int)hour,
                                     (unsigned int)min,
                                     (unsigned int)sec);

    /* JST -> UTC.  Mechacon dates are 2000+, so jst_unix is far above
     * the bias and underflow isn't reachable in practice. */
    *timestamp = jst_unix - SMAP_CDVD_RTC_JST_BIAS_SECS;
    return (int)PS2_SMAP_RC_OK;
}

/* Write Unix UTC seconds to the mechacon RTC.
 *
 * Mechacon expects a JST calendar.  We add the JST bias in seconds
 * first, then split into calendar fields — this lets the calendar
 * helper roll day/month/year forward when the UTC hour is >= 15. */
static int smap_cdvd_write_rtc(unsigned int timestamp)
{
    smap_cdvd_clock_t clock;
    unsigned int jst_unix;
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;

    /* UTC -> JST in the seconds domain so date carry happens for free. */
    jst_unix = timestamp + SMAP_CDVD_RTC_JST_BIAS_SECS;

    smap_unix_to_datetime(jst_unix, &year, &month, &day,
                          &hour, &minute, &second);

    if (year < 2000u || year > 2099u)
        return (int)PS2_SMAP_RC_BAD_LEN;

    zero_bytes(&clock, sizeof(clock));
    clock.stat   = 0u;
    clock.second = smap_int_to_bcd(second);
    clock.minute = smap_int_to_bcd(minute);
    clock.hour   = smap_int_to_bcd(hour);
    clock.pad    = 0u;
    clock.day    = smap_int_to_bcd(day);
    clock.month  = smap_int_to_bcd(month);
    clock.year   = smap_int_to_bcd(year - 2000u);

    if (iop_cd_write_clock(&clock) <= 0)
        return (int)PS2_SMAP_RC_INTERNAL;

    return (int)PS2_SMAP_RC_OK;
}

static void *smap_rpc_handler(int fno, void *buffer, int length)
{
    int result_rc = (int)PS2_SMAP_RC_OK;
    void *out_ptr = (void *)&g_rc_rsp;

    (void)length;

    if (g_hot_diag != 0)
        g_hot_diag->last_rpc_op = (unsigned int)fno;

    switch (fno) {
    case PS2_SMAP_FNO_GET_LAYOUT:
        out_ptr = (void *)&g_layout;
        result_rc = (int)PS2_SMAP_RC_OK;
        break;

    case PS2_SMAP_FNO_GET_STATUS: {
        int i;
        zero_bytes(&g_status_rsp, sizeof(g_status_rsp));
        g_status_rsp.rc         = (int)PS2_SMAP_RC_OK;
        g_status_rsp.boot_state = g_smap_boot_state;
        g_status_rsp.fault      = g_smap_fault;
        g_status_rsp.link_state = g_smap_link_state;
        for (i = 0; i < 6; i++)
            g_status_rsp.mac[i] = g_smap_mac[i];
        g_status_rsp.init_done  = g_smap_init_done;
        out_ptr = (void *)&g_status_rsp;
        result_rc = g_status_rsp.rc;
        break;
    }

    case PS2_SMAP_FNO_GET_DIAG: {
        int i;
        unsigned int phyval = 0u;
        zero_bytes(&g_cold_diag_rsp, sizeof(g_cold_diag_rsp));
        g_cold_diag_rsp.rc                    = (int)PS2_SMAP_RC_OK;
        g_cold_diag_rsp.last_validation_op    = g_last_validation_op;
        g_cold_diag_rsp.last_validation_slot  = g_last_validation_slot;
        g_cold_diag_rsp.last_validation_seq   = g_last_validation_seq;
        g_cold_diag_rsp.last_validation_len   = g_last_validation_len;
        /* PHY status snapshot — best-effort, ignore read failure. */
        if (smap_phy_read(SMAP_PHY_PHYSTS, &phyval) < 0)
            phyval = 0xffffu;
        g_cold_diag_rsp.mac_phy_status        = phyval;
        g_cold_diag_rsp.ring_invariant_flags  = 0u;
        for (i = 0; i < 6; i++)
            g_cold_diag_rsp.mac[i] = g_smap_mac[i];
        out_ptr = (void *)&g_cold_diag_rsp;
        result_rc = g_cold_diag_rsp.rc;
        break;
    }

    case PS2_SMAP_FNO_RELEASE_RX: {
        ps2_smap_release_rx_req_t *req =
            (ps2_smap_release_rx_req_t *)buffer;
        zero_bytes(&g_rc_rsp, sizeof(g_rc_rsp));
        if (req->slot >= PS2_SMAP_RX_SLOTS) {
            g_last_validation_op   = (unsigned int)fno;
            g_last_validation_slot = req->slot;
            g_last_validation_seq  = req->seq;
            g_last_validation_len  = 0u;
            g_rc_rsp.rc = (int)PS2_SMAP_RC_BAD_SLOT;
        } else if (req->seq != g_rx_seq[req->slot]) {
            g_last_validation_op   = (unsigned int)fno;
            g_last_validation_slot = req->slot;
            g_last_validation_seq  = req->seq;
            g_last_validation_len  = 0u;
            g_rc_rsp.rc = (int)PS2_SMAP_RC_BAD_SEQ;
        } else {
            /* Mark slot reclaimed by advancing the IOP's view of the
             * EE consumer cursor.  The frame pump uses this to gate
             * overflow handling. */
            g_hot_diag->rx_tail_iop_view =
                g_hot_diag->rx_tail_iop_view + 1u;
            g_hot_diag->last_release_batch_size = 1u;
            g_rc_rsp.rc = (int)PS2_SMAP_RC_OK;
        }
        out_ptr = (void *)&g_rc_rsp;
        result_rc = g_rc_rsp.rc;
        break;
    }

    case PS2_SMAP_FNO_RELEASE_RX_BATCH: {
        ps2_smap_release_rx_batch_req_t *req =
            (ps2_smap_release_rx_batch_req_t *)buffer;
        unsigned int i;
        int rc_local = (int)PS2_SMAP_RC_OK;
        zero_bytes(&g_rc_rsp, sizeof(g_rc_rsp));

        /* All-or-nothing validation: walk the entire batch, reject the
         * whole thing if any single entry fails range or seq checks.
         * This keeps the rx_tail_iop_view cursor monotonic — partial
         * advance under failure would desync EE consumer/IOP producer. */
        if (req->count == 0u
            || req->count > PS2_SMAP_RELEASE_BATCH_MAX) {
            g_last_validation_op   = (unsigned int)fno;
            g_last_validation_slot = 0u;
            g_last_validation_seq  = req->count;
            g_last_validation_len  = 0u;
            rc_local = (int)PS2_SMAP_RC_BAD_LEN;
        } else {
            for (i = 0; i < req->count; i++) {
                unsigned int slot = req->entries[i].slot;
                unsigned int seq  = req->entries[i].seq;
                if (slot >= PS2_SMAP_RX_SLOTS) {
                    g_last_validation_op   = (unsigned int)fno;
                    g_last_validation_slot = slot;
                    g_last_validation_seq  = seq;
                    g_last_validation_len  = i;
                    rc_local = (int)PS2_SMAP_RC_BAD_SLOT;
                    break;
                }
                if (seq != g_rx_seq[slot]) {
                    g_last_validation_op   = (unsigned int)fno;
                    g_last_validation_slot = slot;
                    g_last_validation_seq  = seq;
                    g_last_validation_len  = i;
                    rc_local = (int)PS2_SMAP_RC_BAD_SEQ;
                    break;
                }
            }
            if (rc_local == (int)PS2_SMAP_RC_OK) {
                g_hot_diag->rx_tail_iop_view =
                    g_hot_diag->rx_tail_iop_view + req->count;
                g_hot_diag->last_release_batch_size = req->count;
            }
        }
        g_rc_rsp.rc = rc_local;
        out_ptr = (void *)&g_rc_rsp;
        result_rc = g_rc_rsp.rc;
        break;
    }

    case PS2_SMAP_FNO_SUBMIT_TX: {
        ps2_smap_submit_tx_req_t *req =
            (ps2_smap_submit_tx_req_t *)buffer;
        zero_bytes(&g_rc_rsp, sizeof(g_rc_rsp));
        if (req->slot >= PS2_SMAP_TX_SLOTS) {
            g_last_validation_op   = (unsigned int)fno;
            g_last_validation_slot = req->slot;
            g_last_validation_seq  = req->seq;
            g_last_validation_len  = req->len;
            g_rc_rsp.rc = (int)PS2_SMAP_RC_BAD_SLOT;
        } else if (req->len < (unsigned int)SMAP_RXMINSIZE
                || req->len > PS2_SMAP_FRAME_MAX) {
            g_last_validation_op   = (unsigned int)fno;
            g_last_validation_slot = req->slot;
            g_last_validation_seq  = req->seq;
            g_last_validation_len  = req->len;
            g_rc_rsp.rc = (int)PS2_SMAP_RC_BAD_LEN;
        } else if (req->seq != g_tx_seq[req->slot]) {
            g_last_validation_op   = (unsigned int)fno;
            g_last_validation_slot = req->slot;
            g_last_validation_seq  = req->seq;
            g_last_validation_len  = req->len;
            g_rc_rsp.rc = (int)PS2_SMAP_RC_BAD_SEQ;
        } else if (g_hot_diag->tx_slot_state[req->slot]
                   != PS2_SMAP_TX_FREE) {
            g_last_validation_op   = (unsigned int)fno;
            g_last_validation_slot = req->slot;
            g_last_validation_seq  = req->seq;
            g_last_validation_len  = req->len;
            g_rc_rsp.rc = (int)PS2_SMAP_RC_BAD_STATE;
        } else {
            g_tx_slot_len[req->slot] = req->len;
            g_tx_seq[req->slot] = g_tx_seq[req->slot] + 1u;
            g_hot_diag->tx_slot_state[req->slot] = PS2_SMAP_TX_BUSY;
            g_hot_diag->last_submit_batch_size = 1u;
            g_rc_rsp.rc = (int)PS2_SMAP_RC_OK;
        }
        out_ptr = (void *)&g_rc_rsp;
        result_rc = g_rc_rsp.rc;
        break;
    }

    case PS2_SMAP_FNO_SUBMIT_TX_BATCH: {
        ps2_smap_submit_tx_batch_req_t *req =
            (ps2_smap_submit_tx_batch_req_t *)buffer;
        unsigned int i;
        int rc_local = (int)PS2_SMAP_RC_OK;
        zero_bytes(&g_rc_rsp, sizeof(g_rc_rsp));

        /* All-or-nothing validation: pre-walk the entire batch,
         * checking range / seq / state.  Reject the whole thing on the
         * first bad entry — partial accept would leave g_tx_seq /
         * tx_slot_state out of sync with the EE's view of which
         * frames it has handed off. */
        if (req->count == 0u
            || req->count > PS2_SMAP_SUBMIT_BATCH_MAX) {
            g_last_validation_op   = (unsigned int)fno;
            g_last_validation_slot = 0u;
            g_last_validation_seq  = req->count;
            g_last_validation_len  = 0u;
            rc_local = (int)PS2_SMAP_RC_BAD_LEN;
        } else {
            for (i = 0; i < req->count; i++) {
                unsigned int slot = req->entries[i].slot;
                unsigned int len  = req->entries[i].len;
                unsigned int seq  = req->entries[i].seq;
                if (slot >= PS2_SMAP_TX_SLOTS) {
                    g_last_validation_op   = (unsigned int)fno;
                    g_last_validation_slot = slot;
                    g_last_validation_seq  = seq;
                    g_last_validation_len  = i;
                    rc_local = (int)PS2_SMAP_RC_BAD_SLOT;
                    break;
                }
                if (len < (unsigned int)SMAP_RXMINSIZE
                    || len > PS2_SMAP_FRAME_MAX) {
                    g_last_validation_op   = (unsigned int)fno;
                    g_last_validation_slot = slot;
                    g_last_validation_seq  = seq;
                    g_last_validation_len  = len;
                    rc_local = (int)PS2_SMAP_RC_BAD_LEN;
                    break;
                }
                if (seq != g_tx_seq[slot]) {
                    g_last_validation_op   = (unsigned int)fno;
                    g_last_validation_slot = slot;
                    g_last_validation_seq  = seq;
                    g_last_validation_len  = i;
                    rc_local = (int)PS2_SMAP_RC_BAD_SEQ;
                    break;
                }
                if (g_hot_diag->tx_slot_state[slot] != PS2_SMAP_TX_FREE) {
                    g_last_validation_op   = (unsigned int)fno;
                    g_last_validation_slot = slot;
                    g_last_validation_seq  = seq;
                    g_last_validation_len  = i;
                    rc_local = (int)PS2_SMAP_RC_BAD_STATE;
                    break;
                }
            }
            if (rc_local == (int)PS2_SMAP_RC_OK) {
                /* Second pass commits all slots atomically. */
                for (i = 0; i < req->count; i++) {
                    unsigned int slot = req->entries[i].slot;
                    unsigned int len  = req->entries[i].len;
                    g_tx_slot_len[slot] = len;
                    g_tx_seq[slot] = g_tx_seq[slot] + 1u;
                    g_hot_diag->tx_slot_state[slot] = PS2_SMAP_TX_BUSY;
                }
                g_hot_diag->last_submit_batch_size = req->count;
            }
        }
        g_rc_rsp.rc = rc_local;
        out_ptr = (void *)&g_rc_rsp;
        result_rc = g_rc_rsp.rc;
        break;
    }

    case PS2_SMAP_FNO_GET_RTC: {
        unsigned int timestamp = 0u;
        zero_bytes(&g_rtc_rsp, sizeof(g_rtc_rsp));
        g_rtc_rsp.rc = smap_cdvd_read_rtc(&timestamp);
        g_rtc_rsp.unix_timestamp = timestamp;
        out_ptr = (void *)&g_rtc_rsp;
        result_rc = g_rtc_rsp.rc;
        break;
    }

    case PS2_SMAP_FNO_SET_RTC: {
        ps2_smap_rtc_req_t *req = (ps2_smap_rtc_req_t *)buffer;
        zero_bytes(&g_rc_rsp, sizeof(g_rc_rsp));
        g_rc_rsp.rc = smap_cdvd_write_rtc(req->unix_timestamp);
        out_ptr = (void *)&g_rc_rsp;
        result_rc = g_rc_rsp.rc;
        break;
    }

    case PS2_SMAP_FNO_RESET_LINK:
    case PS2_SMAP_FNO_WAKE_TX:
    case PS2_SMAP_FNO_POLL_RX:
        /* TODO: implement when RPC clients need them.  For now
         * acknowledge as a no-op. */
        zero_bytes(&g_rc_rsp, sizeof(g_rc_rsp));
        g_rc_rsp.rc = (int)PS2_SMAP_RC_OK;
        out_ptr = (void *)&g_rc_rsp;
        result_rc = g_rc_rsp.rc;
        break;

    default:
        zero_bytes(&g_rc_rsp, sizeof(g_rc_rsp));
        g_rc_rsp.rc = (int)PS2_SMAP_RC_BAD_OP;
        out_ptr = (void *)&g_rc_rsp;
        result_rc = g_rc_rsp.rc;
        break;
    }

    if (g_hot_diag != 0)
        g_hot_diag->last_rpc_result = result_rc;

    FlushDcache();
    return out_ptr;
}

/* ================================================================== *
 * Dual-sifcmd queue registration workaround.
 *
 * Query the resident sifcmd export table and append our RPC queue to
 * the active queue that the EE-side bind walk observes.
 * ================================================================== */

static void smap_register_queue_workaround(smap_iop_rpc_queue_t *qd)
{
    static const kosload_iop_link_table_header_t sifcmd_query
        __attribute__((aligned(4))) = {
        0, 0, 0x0101, 0, { 's','i','f','c','m','d',0,0 }
    };
    void **funcs = QueryLibraryEntryTable(&sifcmd_query);
    if (funcs == 0)
        return;

    unsigned int sifcmd_base = (unsigned int)funcs - 0x1574u;
    volatile unsigned int *aq_ptr =
        (volatile unsigned int *)(sifcmd_base + 0x2a60u);
    unsigned int aq = *aq_ptr;
    unsigned int cur = aq;
    unsigned int hops = 0u;
    unsigned int last_seen = 0u;
    while (cur != 0u && hops < 16u) {
        if (cur == (unsigned int)qd)
            return;
        last_seen = cur;
        cur = ((volatile unsigned int *)cur)[5];
        hops++;
    }
    if (last_seen != 0u)
        ((volatile unsigned int *)last_seen)[5] = (unsigned int)qd;
    else if (aq == 0u)
        *aq_ptr = (unsigned int)qd;
}

/* ================================================================== *
 * RPC handler thread
 * ================================================================== */

static void smap_rpc_thread(void *arg)
{
    int my_thid;
    (void)arg;

    my_thid = ThreadId();
    RpcSetQueue(&g_queue, my_thid);
    RpcRegister(&g_server, PS2_SMAP_RPC_ID, smap_rpc_handler,
                g_rpc_buf, 0, 0, &g_queue);
    smap_register_queue_workaround(&g_queue);

    /* Service is now stable: RpcRegister() has installed the server AND
     * smap_register_queue_workaround() has linked our queue onto the list
     * the EE-side bind walk traverses.  Publish the ready sentinel so the
     * EE's ps2_smap_init() can bind without racing this thread (the -F
     * bind-before-register fix — see PS2_SMAP_READY_* in smap_protocol.h).
     * Written through the KSEG1 uncached alias so it reaches IOP RAM
     * immediately for the EE's SBUS-bridge poll, with no FlushDcache —
     * exactly the kosdev9.irx mailbox-probe pattern. */
    *(volatile unsigned int *)(0xA0000000 | PS2_SMAP_READY_IOP_PHYS) =
        PS2_SMAP_READY_MAGIC;

    {
        volatile smap_iop_rpc_queue_t *vqd =
            (volatile smap_iop_rpc_queue_t *)&g_queue;
        for (;;) {
            if (vqd->start != 0) {
                smap_iop_rpc_server_t *pending =
                    (smap_iop_rpc_server_t *)vqd->start;
                vqd->start = pending->next;
                pending->next = 0;
                RpcExecRequest(pending);
            }
            /* Yield so the lower-priority pump thread (and anything
             * else IOP-side) gets CPU.  Without this the tight spin
             * starves the pump and chip RX never drains. */
            DelayThread(100);
        }
    }
}

/* ================================================================== *
 * Frame pump thread
 *
 * Per iteration:
 *   - Drain INTR_STAT latches (kept from old code; required to keep the
 *     chip pumping).
 *   - Drain TX completions back into smap->txfreebufsize / txbdusedcnt.
 *   - For each TX slot in BUSY state, pull bytes from shared.tx_data
 *     and feed smap_iop_send.  Mark slot FREE on success or failure.
 *   - Drain RX BDs into shared.rx_data.
 *   - Bump heartbeat (always — cheap).
 *   - Conditionally FlushDcache (see "dirty/flush strategy" below).
 *   - DelayThread(SMAP_POLL_USEC).
 *
 * Dirty/flush strategy (ABI v5):
 *   At 200us cadence the pump runs ~5000 Hz; an unconditional
 *   FlushDcache every iteration is the dominant idle cost.  We track a
 *   `dirty` flag set whenever we mutate shared memory the EE needs to
 *   observe (tx-scan completes a slot, drain_rx delivered any frames,
 *   RX-drop counters bumped).  At end-of-iteration we only call
 *   FlushDcache if dirty is set.
 *
 *   The heartbeat write IS shared memory, but its visibility lag only
 *   matters for the SMAP_DIAG debug overlay — bumping every iteration
 *   without flushing means EE sees the last-flushed heartbeat value
 *   until the next "real" event (tx complete, rx delivered).  In
 *   practice that's <1ms on any active link; under sustained idle the
 *   heartbeat appears stale, which is the documented price for
 *   skipping the flush.
 * ================================================================== */

static void smap_pump_thread(void *arg)
{
    struct smap_chan_iop *smap;
    unsigned int link_poll_ticks;
    (void)arg;

    smap = &g_smap_chan;
    link_poll_ticks = 0;

    for (;;) {
        unsigned int istat;
        unsigned int slot;
        unsigned int rx_delivered;
        int dirty;

        dirty = 0;

        if (g_hot_diag != 0) {
            g_hot_diag->heartbeat = g_hot_diag->heartbeat + 1u;
            g_hot_diag->link_state = g_smap_link_state;
        }

        if (link_poll_ticks == 0u) {
            unsigned int link_state = smap_phy_poll_link();
            if (link_state != g_smap_link_state) {
                g_smap_link_state = link_state;
                if (g_hot_diag != 0)
                    g_hot_diag->link_state = link_state;
                dirty = 1;
            }
            link_poll_ticks = SMAP_LINK_POLL_TICKS;
        } else {
            link_poll_ticks--;
        }

        istat = REG16(SMAP_INTR_STAT) & SMAP_INTR_BITMSK;
        if (istat != 0) {
            if (istat & SMAP_INTR_TXDNV) {
                REG32(SMAP_EMAC3_INTR_STAT) = SMAP_E3_DEAD_ALL_LE;
                (void)REG32(SMAP_EMAC3_INTR_STAT);
            }
            if (istat & 0x0040u) {
                unsigned int e3stat = REG32(SMAP_EMAC3_INTR_STAT);
                REG32(SMAP_EMAC3_INTR_STAT) = e3stat;
                (void)REG32(SMAP_EMAC3_INTR_STAT);
            }
            REG16(SMAP_INTR_CLR) = istat;
        }

        smap_iop_tx_intr(smap);

        /* TX submit: scan slots looking for BUSY transitions queued by
         * the RPC handler.  smap_iop_send may fail; either way drop the
         * BUSY state back to FREE so the EE can retry. */
        if (g_hot_diag != 0 && g_tx_data != 0) {
            for (slot = 0; slot < PS2_SMAP_TX_SLOTS; slot++) {
                if (g_hot_diag->tx_slot_state[slot] == PS2_SMAP_TX_BUSY) {
                    unsigned int len = g_tx_slot_len[slot];
                    int tx_rc = -1;
                    if (len >= (unsigned int)SMAP_RXMINSIZE
                        && len <= (unsigned int)SMAP_TXMAXSIZE) {
                        tx_rc = smap_iop_send(smap,
                            (const unsigned char *)
                                (g_tx_data + slot * g_tx_slot_size),
                            len);
                    }
                    if (tx_rc != 0)
                        g_hot_diag->tx_underruns =
                            g_hot_diag->tx_underruns + 1u;
                    /* Slot returns to FREE — EE polls tx_slot_state
                     * before reusing.  Sequence already bumped on
                     * SUBMIT. */
                    g_hot_diag->tx_slot_state[slot] = PS2_SMAP_TX_FREE;
                    dirty = 1;
                }
            }
        }

        rx_delivered = smap_iop_drain_rx(smap);
        if (rx_delivered != 0u)
            dirty = 1;

        /* Push every IOP-side write made this iteration (rx_head, RX
         * descriptor + bytes, tx_slot_state, hot diag counters) to
         * physical RAM so the EE-side SBUS-bridge polls observe them.
         * smap_iop_publish_word_barrier is a compiler barrier only —
         * it does not flush the IOP D-cache.
         *
         * Skip the flush when nothing observable changed — heartbeat
         * lag during idle is acceptable (SMAP_DIAG is debug-only and
         * the next active iteration will flush). */
        if (dirty)
            FlushDcache();

        DelayThread(SMAP_POLL_USEC);
    }
}

/* ================================================================== *
 * Shared region allocation + layout build
 * ================================================================== */

static int smap_alloc_shared_region(void)
{
    unsigned int rx_ring_size  = sizeof(ps2_smap_rx_desc_t) * PS2_SMAP_RX_SLOTS;
    unsigned int rx_data_size  = PS2_SMAP_SLOT_SIZE * PS2_SMAP_RX_SLOTS;
    unsigned int tx_data_size  = PS2_SMAP_SLOT_SIZE * PS2_SMAP_TX_SLOTS;
    unsigned int diag_size     = (sizeof(ps2_smap_hot_diag_t) + 15u) & ~15u;
    unsigned int total_size;
    unsigned int rx_ring_offset;
    unsigned int rx_data_offset;
    unsigned int tx_data_offset;
    unsigned int diag_offset;
    void *base;

    /* Layout: 16-byte-aligned segments back-to-back. */
    rx_ring_offset = 0u;
    rx_data_offset = (rx_ring_offset + rx_ring_size + 15u) & ~15u;
    tx_data_offset = (rx_data_offset + rx_data_size + 15u) & ~15u;
    diag_offset    = (tx_data_offset + tx_data_size + 15u) & ~15u;
    total_size     = (diag_offset + diag_size + 15u) & ~15u;

    /* AllocSysMemory rounds size up to 256-byte boundary internally,
     * but we round up here too so total_size is a known multiple. */
    total_size = (total_size + 0xffu) & ~0xffu;

    base = AllocSysMemory(SMAP_SYSMEM_ALLOC_FIRST, (int)total_size, 0);
    if (base == 0)
        return -1;

    g_shared_base     = (unsigned char *)base;
    g_shared_iop_phys = (unsigned int)base & 0x1fffffu;
    g_shared_size     = total_size;

    g_rx_ring  = (volatile ps2_smap_rx_desc_t *)
                 (g_shared_base + rx_ring_offset);
    g_rx_data  = g_shared_base + rx_data_offset;
    g_tx_data  = g_shared_base + tx_data_offset;
    g_hot_diag = (volatile ps2_smap_hot_diag_t *)
                 (g_shared_base + diag_offset);

    g_rx_data_offset = rx_data_offset;
    g_tx_data_offset = tx_data_offset;
    g_rx_slot_size   = PS2_SMAP_SLOT_SIZE;
    g_tx_slot_size   = PS2_SMAP_SLOT_SIZE;

    /* Zero the entire region.  AllocSysMemory does not guarantee
     * zero-init.  We zero through the cached alias and rely on
     * subsequent FlushDcache calls in the pump thread to keep the
     * EE-visible view coherent. */
    {
        unsigned char *p = g_shared_base;
        unsigned int i;
        for (i = 0; i < total_size; i++)
            p[i] = 0;
    }

    /* Build cached layout response. */
    zero_bytes(&g_layout, sizeof(g_layout));
    g_layout.rc                = (int)PS2_SMAP_RC_OK;
    g_layout.magic             = PS2_SMAP_LAYOUT_MAGIC;
    g_layout.abi_version       = PS2_SMAP_ABI_VERSION;
    g_layout.shared_iop_phys   = g_shared_iop_phys;
    g_layout.shared_size       = g_shared_size;
    g_layout.rx_slot_count     = PS2_SMAP_RX_SLOTS;
    g_layout.rx_slot_size      = PS2_SMAP_SLOT_SIZE;
    g_layout.rx_ring_offset    = rx_ring_offset;
    g_layout.rx_data_offset    = rx_data_offset;
    g_layout.tx_slot_count     = PS2_SMAP_TX_SLOTS;
    g_layout.tx_slot_size      = PS2_SMAP_SLOT_SIZE;
    /* tx_state_offset is the offset within the shared region of the
     * tx_slot_state[] vector, which lives inside the hot-diag block. */
    g_layout.tx_state_offset   = diag_offset
        + (unsigned int)(((unsigned char *)
            &((ps2_smap_hot_diag_t *)0)->tx_slot_state[0]
          - (unsigned char *)0));
    g_layout.tx_data_offset    = tx_data_offset;
    g_layout.diag_offset       = diag_offset;

    FlushDcache();
    return 0;
}

/* ================================================================== *
 * _start
 *
 * Lifecycle:
 *   1.  Parse argv[1] (optional STATIC_MAC override).
 *   2.  Run smap_chip_init() through MAC/PHY reset.  Autoneg/link
 *       confirmation continues in the pump thread so the EE can show
 *       the main screen with "link change..." while the cable settles.
 *       On fault, record + still allocate the shared region + register
 *       the RPC server so the EE can read fault state via GET_STATUS.
 *   3.  smap_iop_chan_init.
 *   4.  Allocate shared region from sysmem.  If alloc fails, record
 *       FAULT_SHARED_ALLOC, return RESIDENT_END (no RPC server).
 *   5.  Build cached layout response.
 *   6.  Spawn RPC handler thread + frame-pump thread.
 *   7.  Set boot_state = READY, init_done = 1, return RESIDENT_END.
 * ================================================================== */

int _start(int argc, char **argv)
{
    kosload_iop_thread_t thread_desc;
    struct smap_chan_iop *smap;
    int rc;
    int rpc_thid;
    int pump_thid;
    int alloc_rc;
    int chip_rc;

    g_smap_boot_state = PS2_SMAP_BOOT_BOOTING;
    g_smap_fault      = PS2_SMAP_FAULT_NONE;
    g_smap_link_state = PS2_SMAP_LINK_DOWN;
    g_smap_init_done  = 0;

    /* Optional bootstrap-supplied MAC override.  Normal builds leave this
     * unset and read the adapter MAC from EEPROM during chip init. */
    g_have_mac_override = 0;
    if (argc >= 2 && argv != 0 && argv[1] != 0
        && parse_mac_arg(argv[1], g_mac_override) == 0
        && smap_mac_is_valid(g_mac_override)) {
        g_have_mac_override = 1;
    }

    smap    = &g_smap_chan;
    chip_rc = smap_chip_init(smap);

    if (chip_rc < 0) {
        g_smap_fault      = (unsigned int)(-chip_rc);
        g_smap_boot_state = PS2_SMAP_BOOT_FAULT;
    } else {
        smap_iop_chan_init(smap);
    }

    /* Publish MAC even on fault — the bytes are valid as long as
     * Phase 11 ran before failing; if we faulted earlier the array is
     * left zero. */
    {
        int i;
        for (i = 0; i < 6; i++)
            g_smap_mac[i] = smap->hwaddr[i];
        smap_publish_mac_bytes(g_smap_mac, smap->hwaddr);
    }

    /* Allocate shared region + build layout struct.  Even on chip
     * fault we try this so the EE can bind + GET_STATUS for diagnosis. */
    alloc_rc = smap_alloc_shared_region();
    if (alloc_rc < 0) {
        if (chip_rc >= 0) {
            g_smap_fault      = PS2_SMAP_FAULT_SHARED_ALLOC;
            g_smap_boot_state = PS2_SMAP_BOOT_FAULT;
        }
        /* Without the shared region we have nowhere to publish hot
         * diagnostics, so don't spawn either thread.  Return
         * RESIDENT_END so subsequent loaders can detect by binding
         * (which will fail -> they treat us as dead). */
        g_smap_init_done = 1;
        return KOSLOAD_MODULE_RESIDENT_END;
    }

    /* RPC handler thread — registers PS2_SMAP_RPC_ID. */
    thread_desc.attr      = KOSLOAD_TH_C;
    thread_desc.option    = 0;
    thread_desc.thread    = smap_rpc_thread;
    thread_desc.stacksize = SMAP_RPC_STACKSIZE;
    thread_desc.priority  = SMAP_RPC_PRIORITY;

    rpc_thid = CreateThread(&thread_desc);
    if (rpc_thid <= 0) {
        if (chip_rc >= 0) {
            g_smap_fault      = PS2_SMAP_FAULT_RPC_REGISTER;
            g_smap_boot_state = PS2_SMAP_BOOT_FAULT;
        }
        g_smap_init_done = 1;
        return KOSLOAD_MODULE_RESIDENT_END;
    }
    rc = StartThread(rpc_thid, 0);
    if (rc < 0) {
        if (chip_rc >= 0) {
            g_smap_fault      = PS2_SMAP_FAULT_THREAD_START;
            g_smap_boot_state = PS2_SMAP_BOOT_FAULT;
        }
        g_smap_init_done = 1;
        return KOSLOAD_MODULE_RESIDENT_END;
    }

    /* Frame-pump thread — only spawned when chip is alive. */
    if (chip_rc >= 0) {
        thread_desc.attr      = KOSLOAD_TH_C;
        thread_desc.option    = 0;
        thread_desc.thread    = smap_pump_thread;
        thread_desc.stacksize = SMAP_PUMP_STACKSIZE;
        thread_desc.priority  = SMAP_PUMP_PRIORITY;

        pump_thid = CreateThread(&thread_desc);
        if (pump_thid <= 0) {
            g_smap_fault      = PS2_SMAP_FAULT_THREAD_CREATE;
            g_smap_boot_state = PS2_SMAP_BOOT_FAULT;
            g_smap_init_done  = 1;
            return KOSLOAD_MODULE_RESIDENT_END;
        }
        if (StartThread(pump_thid, 0) < 0) {
            g_smap_fault      = PS2_SMAP_FAULT_THREAD_START;
            g_smap_boot_state = PS2_SMAP_BOOT_FAULT;
            g_smap_init_done  = 1;
            return KOSLOAD_MODULE_RESIDENT_END;
        }

        g_smap_boot_state = PS2_SMAP_BOOT_READY;
    }

    g_smap_init_done = 1;
    return KOSLOAD_MODULE_RESIDENT_END;
}
