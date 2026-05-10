/* client/playstation2/ee_sif.c
 *
 * EE-side SIF helper library.
 *
 * SIF is the message/DMA link between the PS2's EE and IOP CPUs.  This file
 * implements just the pieces kosload needs: bind/call RPC servers, load IRX
 * modules into IOP memory, and DMA bytes from EE RAM to IOP RAM.
 *
 * Cache rule of thumb here: build DMA packets in uncached memory, or flush
 * them before starting DMA, so the other CPU sees the bytes we just wrote.
 */

#include <stdint.h>
#include <string.h>

#include "cache.h"
#include "ee_sif.h"

#define SIF_REG_MAINADDR       1u
#define SIF_REG_SUBADDR        2u
#define SIF_REG_MSFLAG         3u
#define SIF_REG_SMFLAG         4u
#define SIF_SYSREG_SUBADDR     0x80000000
#define SIF_SYSREG_MAINADDR    0x80000001
#define SIF_SYSREG_RPCINIT     0x80000002

#define IOPHEAP_ALLOC_CID      1
#define LOADFILE_LOAD_BUF_CID  6

#define SIF_SEND_ATTR          (EE_SIF_DMA_ATTR_INT_O | EE_SIF_DMA_ATTR_ERT)

#define EE_UNCACHED(addr)      ((void *)PS2_EE_KSEG1_ADDR(addr))
#define EE_PHYS(addr)          PS2_EE_PHYS(addr)

#define POLL_TIMEOUT           100000000u
#define RPC_REPLY_TIMEOUT      (POLL_TIMEOUT * 5u)
#define SIF_PACKET_SIZE        64u
#define SIF_RECV_QWC           8u
#define SIF_RPC_PACKET_COUNT   8u
#define SIF_DMA_QUEUE_DEPTH    8u
#define SIF_DMA_MAX_DESCS      8u
#define SIF_DMA_JOB_STREAM_SIZE 1024u
#define SIF_DMA_MAX_CHUNK      (SIF_DMA_JOB_STREAM_SIZE - 32u)
#define SIF_CMD_RECV_BLOCK     128u

/* RPC arg block for LOADFILE function 6 (LoadModuleBuffer). The 252-byte
 * "unused" gap mirrors the layout the resident IOP LOADFILE expects. */
typedef struct {
    uint32_t module_ptr;
    uint32_t arg_len;
    uint8_t unused[252];
    char args[252];
} ee_sif_load_module_args_t;

#define SIF_ASSERT_SIZE(name, type, size) \
    typedef char name[(sizeof(type) == (size)) ? 1 : -1]

SIF_ASSERT_SIZE(assert_sif_cmd_header_size, ee_sif_cmd_header_t, 16);
SIF_ASSERT_SIZE(assert_sif_saddr_size, ee_sif_saddr_pkt_t, 20);
SIF_ASSERT_SIZE(assert_sif_init_size, ee_sif_init_pkt_t, 20);
SIF_ASSERT_SIZE(assert_sif_set_sreg_size, ee_sif_set_sreg_t, 24);
SIF_ASSERT_SIZE(assert_sif_iop_reset_size, ee_sif_iop_reset_pkt_t, 0x68);
SIF_ASSERT_SIZE(assert_sif_dma_transfer_size, ee_sif_dma_transfer_t, 16);
SIF_ASSERT_SIZE(assert_sif_rpc_header_size, ee_sif_rpc_header_t, 28);
SIF_ASSERT_SIZE(assert_sif_rpc_bind_size, ee_sif_rpc_bind_pkt_t, 36);
SIF_ASSERT_SIZE(assert_sif_rpc_call_size, ee_sif_rpc_call_pkt_t, 56);
SIF_ASSERT_SIZE(assert_sif_rpc_rend_size, ee_sif_rpc_rend_pkt_t, 48);
SIF_ASSERT_SIZE(assert_sif_rpc_other_data_size,
                ee_sif_rpc_other_data_pkt_t, 44);
SIF_ASSERT_SIZE(assert_sif_rpc_client_size, ee_sif_rpc_client_t, 40);
SIF_ASSERT_SIZE(assert_sif_rpc_server_size, ee_sif_rpc_server_t, 68);
SIF_ASSERT_SIZE(assert_sif_rpc_queue_size, ee_sif_rpc_queue_t, 24);

static ee_sif_status_fn_t g_status_fn;

static volatile uint8_t sif_recv_buf[SIF_RECV_QWC * 16u]
    __attribute__((aligned(64)));
static volatile uint8_t sif_rpc_packets[SIF_RPC_PACKET_COUNT][SIF_PACKET_SIZE]
    __attribute__((aligned(64)));
static volatile ee_sif_rpc_client_t iopheap_client
    __attribute__((aligned(64)));
static volatile ee_sif_rpc_client_t loadfile_client
    __attribute__((aligned(64)));
static volatile ee_sif_load_module_args_t load_args
    __attribute__((aligned(64)));
static volatile ee_sif_load_module_result_t load_result
    __attribute__((aligned(64)));
static volatile uint32_t iopheap_arg __attribute__((aligned(64)));
typedef struct {
    uint32_t id;
    uint32_t stream_size;
    int state;
    uint8_t stream[SIF_DMA_JOB_STREAM_SIZE] __attribute__((aligned(64)));
} ee_sif_dma_job_t;

static ee_sif_dma_job_t sif1_dma_jobs[SIF_DMA_QUEUE_DEPTH]
    __attribute__((aligned(64)));
static uint8_t sif1_dma_queue[SIF_DMA_QUEUE_DEPTH];

static uint32_t sif_subaddr;
static uint32_t rpc_next_packet;
static uint32_t sifrpc_next_rid;
static uint32_t sifrpc_call_seq;
static uint32_t sif1_dma_queue_head;
static uint32_t sif1_dma_queue_tail;
static uint32_t sif1_dma_queue_count;
static uint32_t sif1_dma_next_id = 1u;
static int sif1_dma_current = -1;
static uint32_t sif_sreg_shadow[256];

/* EE DMAC channel registers. */
#define D5_CHCR  (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF0_CHCR)
#define D5_QWC   (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF0_QWC)
#define D6_CHCR  (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF1_CHCR)
#define D6_QWC   (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF1_QWC)
#define D6_TADR  (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF1_TADR)
#define D_STAT   (*(volatile uint32_t *)PS2_EE_REG_DMAC_STAT)
#define D_ENABLER (*(volatile uint32_t *)PS2_EE_REG_DMAC_ENABLER)
#define D_ENABLEW (*(volatile uint32_t *)PS2_EE_REG_DMAC_ENABLEW)
#define DMAC_CPND   0x00010000u
#define D_STAT_SIF0 0x00000020u

#define CHCR_MOD_CHAIN     0x004u
#define CHCR_TIE           0x080u
#define CHCR_STR           0x100u
#define SIF0_DCHAIN_CHCR   (CHCR_STR | CHCR_TIE | CHCR_MOD_CHAIN)   /* Receive DMA chain from IOP. */
#define SIF1_SOURCE_CHCR   (CHCR_STR | CHCR_TIE | CHCR_MOD_CHAIN)   /* Send DMA chain to IOP. */

#define SBUS_MSCOM   (*(volatile uint32_t *)EE_SIF_REG_MSCOM)
#define SBUS_SMCOM   (*(volatile uint32_t *)EE_SIF_REG_SMCOM)
#define SBUS_MSFLAG  (*(volatile uint32_t *)EE_SIF_REG_MSFLG)
#define SBUS_SMFLAG  (*(volatile uint32_t *)EE_SIF_REG_SMFLG)

#define DMA_TAG_REFE  0u
#define DMA_TAG_REF   3u

#define SIF_DMA_JOB_FREE     0
#define SIF_DMA_JOB_QUEUED   1
#define SIF_DMA_JOB_RUNNING  2

#define IOP_RAM_BASE_EE      PS2_EE_IOP_RAM_UNCACHED_BASE
#define LF_DISPATCH_SIG_0    0x27bdffe8u
#define LF_DISPATCH_SIG_1A   0x2c820006u
#define LF_DISPATCH_SIG_1B   0x2c820007u
#define LF_DISPATCH_SIG_2    0x14400003u
#define LF_DISPATCH_SIG_3    0xafbf0010u
#define IOP_LOADCORE_LIBLIST_HEAD  0x00000800u
#define LIB_ENTRY_EXPORTS_OFF      0x18u

#define SIFRPC_REC_ID_FOR(rid) (((rid) << 16) | 0x05u)

static void ee_sif_status(const char *status) {
    if(g_status_fn != 0)
        g_status_fn(status);
}

void ee_sif_set_status_callback(ee_sif_status_fn_t fn) {
    g_status_fn = fn;
}

static void ee_delay(unsigned long loops) {
    volatile unsigned long i;
    for (i = 0; i < loops; i++)
        ;
}

void ee_sif_reset_state(void) {
    uint32_t i;

    memset((void *)sif_sreg_shadow, 0, sizeof(sif_sreg_shadow));
    memset((void *)&iopheap_client, 0, sizeof(iopheap_client));
    memset((void *)&loadfile_client, 0, sizeof(loadfile_client));
    for (i = 0; i < SIF_DMA_QUEUE_DEPTH; i++) {
        sif1_dma_jobs[i].id = 0u;
        sif1_dma_jobs[i].stream_size = 0u;
        sif1_dma_jobs[i].state = SIF_DMA_JOB_FREE;
        sif1_dma_queue[i] = 0u;
    }
    sif1_dma_queue_head = 0u;
    sif1_dma_queue_tail = 0u;
    sif1_dma_queue_count = 0u;
    sif1_dma_next_id = 1u;
    sif1_dma_current = -1;
    sif_subaddr = 0;
    rpc_next_packet = 0;
    sifrpc_next_rid = 0;
    sifrpc_call_seq = 0;
}

static int ee_sif_set_reg(uint32_t reg, uint32_t value) {
    switch (reg) {
    case SIF_REG_MAINADDR: SBUS_MSCOM  = value; return 0;
    case SIF_REG_SUBADDR:  SBUS_SMCOM  = value; return 0;
    case SIF_REG_MSFLAG:   SBUS_MSFLAG = value; return 0;
    case SIF_REG_SMFLAG:   SBUS_SMFLAG = value; return 0;
    default:               sif_sreg_shadow[reg & 0xffu] = value; return 0;
    }
}

static int ee_sif_get_reg(uint32_t reg) {
    switch (reg) {
    case SIF_REG_MAINADDR: return (int)SBUS_MSCOM;
    case SIF_REG_SUBADDR:  return (int)SBUS_SMCOM;
    case SIF_REG_SMFLAG:   return (int)SBUS_SMFLAG;
    case SIF_REG_MSFLAG:   return (int)SBUS_MSFLAG;
    default:               return (int)sif_sreg_shadow[reg & 0xffu];
    }
}

static void ee_sif_dispatch_cmd(volatile ee_sif_cmd_header_t *hdr) {
    switch (hdr->cid) {
        case EE_SIF_CMD_CHANGE_SADDR:
            break;

        case EE_SIF_CMD_SET_SREG: {
            volatile ee_sif_set_sreg_t *pkt =
                (volatile ee_sif_set_sreg_t *)hdr;
            if(pkt->index == 0u)
                ee_sif_set_reg(SIF_SYSREG_RPCINIT, pkt->value);
            break;
        }

        case EE_SIF_CMD_INIT_CMD:
        case EE_SIF_CMD_IOP_RESET:
            break;

        case EE_SIF_CMD_RPC_END:
        case EE_SIF_CMD_RPC_BIND:
        case EE_SIF_CMD_RPC_CALL:
        default:
            break;
    }
}

static void dmac_force_stop(volatile uint32_t *chcr) {
    uint32_t saved = D_ENABLER;
    D_ENABLEW = saved | DMAC_CPND;
    (void)D_ENABLER;
    (void)D_ENABLER;
    *chcr = 0;
    (void)*chcr;            /* MMIO read-back drains WBB before re-enable; matches BIOS isceSifSetDma at 0x80006800. */
    D_ENABLEW = saved;
}

static uint32_t ee_sif_next_dma_id(void) {
    uint32_t id = sif1_dma_next_id++;

    if(sif1_dma_next_id == 0u || sif1_dma_next_id > 0x7fffffffu)
        sif1_dma_next_id = 1u;
    return id;
}

static int ee_sif_find_free_dma_job(void) {
    uint32_t i;

    for (i = 0; i < SIF_DMA_QUEUE_DEPTH; i++) {
        if(sif1_dma_jobs[i].state == SIF_DMA_JOB_FREE)
            return (int)i;
    }

    return -1;
}

static int ee_sif_build_dma_job(ee_sif_dma_job_t *job,
                                ee_sif_dma_transfer_t *dmat,
                                int count) {
    uint32_t scratch_off;
    uint8_t *stream;
    int i;

    if(count <= 0 || count > (int)SIF_DMA_MAX_DESCS)
        return -1;

    scratch_off = 16u * (uint32_t)count;
    stream = job->stream;
    memset(stream, 0, sizeof(job->stream));

    for (i = 0; i < count; i++) {
        uint32_t bytes = (uint32_t)dmat[i].size;
        uint32_t dest = (uint32_t)dmat[i].dest & 0x1fffffffu;
        uint32_t attr = (uint32_t)dmat[i].attr;
        uint32_t block_bytes = bytes;
        uint32_t qbytes;
        uint32_t qwc_data;
        uint32_t scratch_phys;
        uint32_t flags;
        uint32_t id;
        uint8_t *scratch;
        volatile uint32_t *header;
        volatile uint32_t *tag;

        if(dmat[i].size < 0)
            return -1;
        if(bytes > 0u && dmat[i].src == 0)
            return -1;

        if(sif_subaddr != 0u && dest == sif_subaddr)
            block_bytes = SIF_CMD_RECV_BLOCK;
        qbytes = (block_bytes + 15u) & ~15u;
        qwc_data = qbytes >> 4;

        if(bytes > block_bytes)
            return -1;
        if(scratch_off + 16u + qbytes > SIF_DMA_JOB_STREAM_SIZE)
            return -1;

        scratch = &stream[scratch_off];
        if(bytes > 0)
            memcpy(scratch + 16u, (const void *)dmat[i].src, bytes);

        flags = 0;
        if(attr & EE_SIF_DMA_ATTR_INT_O)
            flags |= 0x40000000u;
        if(attr & EE_SIF_DMA_ATTR_ERT)
            flags |= 0x80000000u;

        header = (volatile uint32_t *)scratch;
        header[0] = dest | flags;
        header[1] = qbytes >> 2;

        scratch_phys = EE_PHYS(scratch);
        id = (i == count - 1) ? DMA_TAG_REFE : DMA_TAG_REF;
        tag = (volatile uint32_t *)&stream[i * 16u];
        tag[0] = (id << 28) | (qwc_data + 1u);
        tag[1] = scratch_phys;
        tag[2] = 0;
        tag[3] = 0;

        if(bytes > 0)
            cache_flush_dc(dmat[i].src, bytes);

        scratch_off += 16u + qbytes;
    }

    cache_flush_dc(stream, scratch_off);
    job->stream_size = scratch_off;
    return 0;
}

static void ee_sif_start_dma_job(int slot) {
    ee_sif_dma_job_t *job = &sif1_dma_jobs[slot];

    D6_TADR = EE_PHYS(job->stream);
    D6_QWC = 0;
    D6_CHCR = SIF1_SOURCE_CHCR;
    (void)D6_CHCR;          /* MMIO read-back drains WBB; matches BIOS isceSifSetDma at 0x8000693c. */
    job->state = SIF_DMA_JOB_RUNNING;
    sif1_dma_current = slot;
}

static void ee_sif_dma_pump(void) {
    if(sif1_dma_current >= 0) {
        if(D6_CHCR & CHCR_STR)
            return;

        sif1_dma_jobs[sif1_dma_current].state = SIF_DMA_JOB_FREE;
        sif1_dma_jobs[sif1_dma_current].id = 0u;
        sif1_dma_jobs[sif1_dma_current].stream_size = 0u;
        sif1_dma_current = -1;
    }

    if(sif1_dma_current < 0 &&
        sif1_dma_queue_count > 0u &&
        (D6_CHCR & CHCR_STR) == 0u) {
        int slot = (int)sif1_dma_queue[sif1_dma_queue_head];

        sif1_dma_queue_head =
            (sif1_dma_queue_head + 1u) % SIF_DMA_QUEUE_DEPTH;
        sif1_dma_queue_count--;
        ee_sif_start_dma_job(slot);
    }
}

int ee_sif_set_dma(ee_sif_dma_transfer_t *dmat, int count) {
    int slot;
    uint32_t id;

    ee_sif_dma_pump();

    if(dmat == 0 || count <= 0 || count > (int)SIF_DMA_MAX_DESCS)
        return 0;
    if(sif1_dma_queue_count >= SIF_DMA_QUEUE_DEPTH)
        return 0;

    slot = ee_sif_find_free_dma_job();
    if(slot < 0)
        return 0;

    id = ee_sif_next_dma_id();
    sif1_dma_jobs[slot].id = id;
    sif1_dma_jobs[slot].stream_size = 0u;
    sif1_dma_jobs[slot].state = SIF_DMA_JOB_QUEUED;
    if(ee_sif_build_dma_job(&sif1_dma_jobs[slot], dmat, count) < 0) {
        sif1_dma_jobs[slot].id = 0u;
        sif1_dma_jobs[slot].state = SIF_DMA_JOB_FREE;
        return 0;
    }

    sif1_dma_queue[sif1_dma_queue_tail] = (uint8_t)slot;
    sif1_dma_queue_tail =
        (sif1_dma_queue_tail + 1u) % SIF_DMA_QUEUE_DEPTH;
    sif1_dma_queue_count++;

    ee_sif_dma_pump();
    return (int)id;
}

int ee_sif_dma_stat(int trid) {
    uint32_t id = (uint32_t)trid;
    uint32_t i;

    if(id == 0u)
        return -1;

    ee_sif_dma_pump();

    for (i = 0; i < SIF_DMA_QUEUE_DEPTH; i++) {
        if(sif1_dma_jobs[i].id != id)
            continue;
        if(sif1_dma_jobs[i].state == SIF_DMA_JOB_QUEUED)
            return 1;
        if(sif1_dma_jobs[i].state == SIF_DMA_JOB_RUNNING)
            return 0;
        return -1;
    }

    return -1;
}

void ee_sif_set_dchain(void) {
    if(D5_CHCR & CHCR_STR)
        dmac_force_stop(&D5_CHCR);
    D_STAT = D_STAT_SIF0;
    D5_QWC = 0;
    D5_CHCR = SIF0_DCHAIN_CHCR;
    (void)D5_CHCR;          /* MMIO read-back drains WBB; matches BIOS isceSifSetDChain at 0x8000636c. */
}

static int wait_dma_done(int trid) {
    unsigned int i;

    if(trid <= 0)
        return -1;

    for (i = 0; i < POLL_TIMEOUT; i++) {
        if(ee_sif_dma_stat(trid) < 0)
            return 0;
    }

    return -1;
}

void ee_sif_rearm_receive(void) {
    memset((void *)sif_recv_buf, 0, sizeof(sif_recv_buf));
    cache_flush_dc((const void *)sif_recv_buf, sizeof(sif_recv_buf));
    ee_sif_set_dchain();
}

static void ee_sif_fill_header(ee_sif_cmd_header_t *hdr,
                               uint32_t cid,
                               uint32_t packet_size) {
    hdr->psize = (uint8_t)(packet_size & 0xffu);
    hdr->dsize = 0u;
    hdr->dest = (void *)sif_subaddr;
    hdr->cid = cid;
    hdr->opt = 0;
}

int ee_sif_send_cmd(uint32_t cid, void *packet, uint32_t packet_size,
                    const void *extra_src, void *extra_dest,
                    uint32_t extra_size) {
    ee_sif_dma_transfer_t dmat[2];
    int count = 0;
    int trid;

    if(packet_size == 0u || packet_size > 112u || sif_subaddr == 0u)
        return -1;

    ((ee_sif_cmd_header_t *)packet)->psize = (uint8_t)(packet_size & 0xffu);
    ((ee_sif_cmd_header_t *)packet)->dsize = extra_size;
    ((ee_sif_cmd_header_t *)packet)->dest =
        (extra_size != 0u) ? extra_dest : 0;
    ((ee_sif_cmd_header_t *)packet)->cid = cid;

    if(extra_size != 0u) {
        cache_flush_dc(extra_src, extra_size);
        dmat[count].src = (void *)extra_src;
        dmat[count].dest = extra_dest;
        dmat[count].size = (int32_t)extra_size;
        dmat[count].attr = 0;
        count++;
    }

    cache_flush_dc(packet, packet_size);
    dmat[count].src = packet;
    dmat[count].dest = (void *)sif_subaddr;
    dmat[count].size = (int32_t)packet_size;
    dmat[count].attr = SIF_SEND_ATTR;
    count++;

    trid = ee_sif_set_dma(dmat, count);
    return wait_dma_done(trid);
}

int ee_sif_iop_write(void *iop_dest, const void *src, uint32_t size) {
    ee_sif_dma_transfer_t dmat;
    const uint8_t *src_bytes;
    uint32_t dest_addr;
    uint32_t offset;

    if(size == 0u)
        return 0;

    src_bytes = (const uint8_t *)src;
    dest_addr = (uint32_t)iop_dest;
    offset = 0;
    while (offset < size) {
        uint32_t chunk = size - offset;
        int trid;

        if(chunk > SIF_DMA_MAX_CHUNK)
            chunk = SIF_DMA_MAX_CHUNK;

        cache_flush_dc(src_bytes + offset, chunk);
        dmat.src = (void *)(src_bytes + offset);
        dmat.dest = (void *)(dest_addr + offset);
        dmat.size = (int32_t)chunk;
        dmat.attr = 0;

        trid = ee_sif_set_dma(&dmat, 1);
        if(wait_dma_done(trid) < 0)
            return -1;

        offset += chunk;
    }

    return 0;
}

static int ee_sif_get_other_data(uint32_t src_iop, void *dst_ee, uint32_t size) {
    static volatile uint8_t pkt[64] __attribute__((aligned(64)));
    ee_sif_rpc_other_data_pkt_t *pkt_data;
    volatile uint32_t *uc_dst;
    uint32_t poll_words;
    uint32_t i;
    int rc;

    if(size == 0u || size > 1024u)
        return -1;
    if(((uint32_t)dst_ee & 0xFu) != 0u)
        return -1;

    ee_sif_rearm_receive();

    uc_dst = (volatile uint32_t *)EE_UNCACHED(dst_ee);
    poll_words = (size + 3u) >> 2;
    for (i = 0; i < poll_words; i++)
        uc_dst[i] = 0x5A5A5A5Au;

    memset((void *)pkt, 0, sizeof(pkt));
    pkt_data = (ee_sif_rpc_other_data_pkt_t *)(void *)pkt;
    pkt_data->src = (void *)src_iop;
    pkt_data->dest = (void *)EE_PHYS(dst_ee);
    pkt_data->size = (int32_t)size;

    rc = ee_sif_send_cmd(EE_SIF_CMD_GET_OTHER_DATA,
                         (void *)pkt, 64u, 0, 0, 0);
    if(rc < 0)
        return -2;

    for (i = 0; i < POLL_TIMEOUT; i++) {
        if(uc_dst[0] != 0x5A5A5A5Au) {
            unsigned int j;
            for (j = 0; j < POLL_TIMEOUT / 256u; j++) {
                if((D_STAT & D_STAT_SIF0) != 0u)
                    break;
            }
            D_STAT = D_STAT_SIF0;
            ee_sif_rearm_receive();
            return 0;
        }
    }

    return -3;
}

int ee_sif_iop_read(uint32_t src_iop, void *dst_ee, uint32_t size) {
    static uint8_t scratch[1024] __attribute__((aligned(64)));
    uint8_t *out = (uint8_t *)dst_ee;
    uint32_t offset = 0;

    while (offset < size) {
        uint32_t chunk = size - offset;
        volatile uint8_t *src_uc;
        uint32_t i;

        if(chunk > sizeof(scratch))
            chunk = sizeof(scratch);
        if(ee_sif_get_other_data(src_iop + offset, scratch, chunk) < 0)
            return -1;

        src_uc = (volatile uint8_t *)EE_UNCACHED(scratch);
        for (i = 0; i < chunk; i++)
            out[offset + i] = src_uc[i];

        offset += chunk;
    }

    return 0;
}

int ee_sif_iop_read_word(uint32_t src_iop, uint32_t *out) {
    uint32_t word;

    if(out == 0)
        return -1;
    if(ee_sif_iop_read(src_iop, &word, sizeof(word)) < 0)
        return -1;

    *out = word;
    return 0;
}

volatile ee_sif_cmd_header_t *ee_sif_poll_packet(uint32_t cid,
                                                 unsigned int timeout) {
    volatile ee_sif_cmd_header_t *hdr;
    unsigned int i;

    hdr = (volatile ee_sif_cmd_header_t *)EE_UNCACHED(sif_recv_buf);
    for (i = 0; i < timeout; i++) {
        if((D_STAT & D_STAT_SIF0) == 0u)
            continue;

        D_STAT = D_STAT_SIF0;

        if(hdr->cid == cid)
            return hdr;
        if(hdr->psize != 0u) {
            ee_sif_dispatch_cmd(hdr);
            ee_sif_rearm_receive();
        }
    }

    return 0;
}

int ee_sif_wait_iop_bootend(void) {
    unsigned int i;

    /* IOP readiness can show up two ways:
     *   1. BIOS EESYNC raises BOOTEND in SMFLG after LOADCORE phase 2.
     *      Set on real hardware before any user ELF runs.
     *   2. IOP-side SIFCMD writes its recv-buffer address into SUBADDR
     *      (SMCOM) when its initialisation completes.  This is the only 
     *      one PCSX2 reliably sets — its BIOS HLE skips the BOOTEND 
     *      SMFLG write.
     * Either is sufficient for our purposes; exit on whichever appears
     * first.  On real hardware BOOTEND wins on iteration 0; on PCSX2
     * SUBADDR is already non-zero by the time we get here. */
    for (i = 0; i < POLL_TIMEOUT; i++) {
        if ((uint32_t)ee_sif_get_reg(SIF_REG_SMFLAG) & EE_SIF_STAT_BOOTEND)
            return 0;
        if ((uint32_t)ee_sif_get_reg(SIF_REG_SUBADDR) != 0)
            return 0;
    }

    return -1;
}

int ee_sif_cmd_init(void) {
    ee_sif_saddr_pkt_t saddr_pkt;
    ee_sif_init_pkt_t init_pkt;

    //ee_sif_status("PHASE1: SIFCMD ARM RX");
    memset((void *)sif_recv_buf, 0, sizeof(sif_recv_buf));
    ee_sif_rearm_receive();

    //ee_sif_status("PHASE1: READ SUBADDR");
    sif_subaddr = (uint32_t)ee_sif_get_reg(SIF_REG_SUBADDR);
    if(sif_subaddr == 0u)
        sif_subaddr = (uint32_t)ee_sif_get_reg(SIF_SYSREG_SUBADDR);
    if(sif_subaddr == 0u) {
        //ee_sif_status("PHASE1: SUBADDR=0");
        return -2;
    }

    //ee_sif_status("PHASE1: PUBLISH MAINADDR");
    (void)ee_sif_set_reg(SIF_SYSREG_MAINADDR, EE_PHYS(sif_recv_buf));
    (void)ee_sif_set_reg(SIF_REG_MAINADDR, EE_PHYS(sif_recv_buf));

    //ee_sif_status("PHASE1: BUILD CHANGE_SADDR");
    memset(&saddr_pkt, 0, sizeof(saddr_pkt));
    ee_sif_fill_header(&saddr_pkt.header, EE_SIF_CMD_CHANGE_SADDR,
                       sizeof(saddr_pkt));
    saddr_pkt.buff = (void *)EE_PHYS(sif_recv_buf);

    //ee_sif_status("PHASE1: SEND CHANGE_SADDR");
    if(ee_sif_send_cmd(EE_SIF_CMD_CHANGE_SADDR, &saddr_pkt,
                        sizeof(saddr_pkt), 0, 0, 0) < 0) {
        //ee_sif_status("PHASE1: SEND TIMEOUT");
        return -3;
    }

    //ee_sif_status("PHASE1: SEND INIT_ADDR");
    memset(&init_pkt, 0, sizeof(init_pkt));
    ee_sif_fill_header(&init_pkt.header, EE_SIF_CMD_INIT_CMD,
                       sizeof(init_pkt));
    init_pkt.buff = (void *)EE_PHYS(sif_recv_buf);
    if(ee_sif_send_cmd(EE_SIF_CMD_INIT_CMD, &init_pkt,
                        sizeof(init_pkt), 0, 0, 0) < 0) {
        //ee_sif_status("PHASE1: INIT_ADDR FAIL");
        return -4;
    }

    //ee_sif_status("PHASE1: SIFCMD OK");
    return 0;
}

static volatile uint8_t *rpc_alloc_packet(void) {
    volatile uint8_t *pkt;

    pkt = sif_rpc_packets[rpc_next_packet++ % SIF_RPC_PACKET_COUNT];
    memset((void *)pkt, 0, SIF_PACKET_SIZE);
    return pkt;
}

int ee_sif_rpc_init(void) {
    ee_sif_cmd_header_t pkt;
    unsigned int i;

    //ee_sif_status("PHASE1: SIFRPC INIT");
    memset((void *)sif_rpc_packets, 0, sizeof(sif_rpc_packets));
    memset((void *)&iopheap_client, 0, sizeof(iopheap_client));
    memset((void *)&loadfile_client, 0, sizeof(loadfile_client));
    rpc_next_packet = 0;
    sifrpc_next_rid = 0;
    sifrpc_call_seq = 0;

    //ee_sif_status("PHASE1: SEND INIT_CMD");
    memset(&pkt, 0, sizeof(pkt));
    pkt.opt = 1u;
    if(ee_sif_send_cmd(EE_SIF_CMD_INIT_CMD, &pkt, sizeof(pkt), 0, 0, 0) < 0) {
        //ee_sif_status("PHASE1: INIT_CMD TX FAIL");
        return -1;
    }

    (void)ee_sif_set_reg(SIF_SYSREG_RPCINIT, 0u);

    //ee_sif_status("PHASE1: WAIT RPCINIT");
    for (i = 0; i < POLL_TIMEOUT / 256u; i++) {
        volatile ee_sif_cmd_header_t *hdr =
            ee_sif_poll_packet(EE_SIF_CMD_SET_SREG, 256u);
        if(hdr != 0) {
            ee_sif_dispatch_cmd(hdr);
            ee_sif_rearm_receive();
        }
        if((uint32_t)ee_sif_get_reg(SIF_SYSREG_RPCINIT) != 0u) {
            //ee_sif_status("PHASE1: RPCINIT GOT");
            return 0;
        }
    }

    ee_sif_status("PHASE1: RPCINIT TIMEOUT");
    return -2;
}

void ee_sif_rpc_client_clear(volatile ee_sif_rpc_client_t *client) {
    if(client != 0)
        memset((void *)client, 0, sizeof(*client));
}

int ee_sif_rpc_bind(volatile ee_sif_rpc_client_t *client,
                    uint32_t server_id) {
    volatile ee_sif_rpc_bind_pkt_t *pkt;
    volatile ee_sif_rpc_rend_pkt_t *reply;
    uint32_t rid;

    if(client == 0)
        return -1;

    //ee_sif_status("PHASE1: RPC BIND");
    pkt = (volatile ee_sif_rpc_bind_pkt_t *)rpc_alloc_packet();
    ee_sif_fill_header((ee_sif_cmd_header_t *)&pkt->sifcmd,
                       EE_SIF_CMD_RPC_BIND, SIF_PACKET_SIZE);
    rid = ++sifrpc_next_rid;
    pkt->rec_id = (int32_t)SIFRPC_REC_ID_FOR(rid);
    pkt->pkt_addr = (void *)EE_UNCACHED(pkt);
    pkt->rpc_id = 0;
    pkt->cd = (void *)client;
    pkt->sid = (int32_t)server_id;

    client->pkt_addr = (void *)pkt;
    client->rpc_id = 0;
    client->sema_id = -1;
    client->mode = 0;
    client->server = 0;

    if(ee_sif_send_cmd(EE_SIF_CMD_RPC_BIND, (void *)pkt,
                        SIF_PACKET_SIZE, 0, 0, 0) < 0)
        return -1;

    reply = (volatile ee_sif_rpc_rend_pkt_t *)
        ee_sif_poll_packet(EE_SIF_CMD_RPC_END, POLL_TIMEOUT);
    if(reply == 0) {
        //ee_sif_status("PHASE1: BIND NO REPLY");
        ee_sif_rearm_receive();
        return -2;
    }
    if(reply->cid != EE_SIF_CMD_RPC_BIND) {
        //ee_sif_status("PHASE1: BIND BAD CID");
        ee_sif_rearm_receive();
        return -4;
    }

    client->server = reply->sd;
    client->buf = reply->buf;
    client->cbuf = reply->cbuf;

    ee_sif_rearm_receive();

    if(client->server == 0) {
        //ee_sif_status("PHASE1: BIND NULL SERVER");
        return -3;
    }

    return 0;
}

int ee_sif_rpc_bind_retry(volatile ee_sif_rpc_client_t *client,
                          uint32_t server_id,
                          unsigned int attempts) {
    unsigned int i;
    int rc = -3;

    for (i = 0; i < attempts; i++) {
        rc = ee_sif_rpc_bind(client, server_id);
        if(rc == 0)
            return 0;
        if(rc != -2 && rc != -3)
            return rc;
        ee_delay(500000u);
    }

    return rc;
}

int ee_sif_rpc_call(volatile ee_sif_rpc_client_t *client, int rpc_number,
                    const void *sendbuf, uint32_t send_size,
                    void *recvbuf, uint32_t recv_size) {
    volatile ee_sif_rpc_call_pkt_t *pkt;
    volatile ee_sif_rpc_header_t *reply;
    uint32_t my_seq = ++sifrpc_call_seq;
    static char rpc_msg[24];
    char *p = rpc_msg;
    const char *prefix = "PHASE1: RPC CALL #";
    uint32_t rid;

    for (; *prefix; ++prefix) *p++ = *prefix;
    if(my_seq >= 10u) *p++ = (char)('0' + (my_seq / 10u) % 10u);
    *p++ = (char)('0' + (my_seq % 10u));
    *p = '\0';
    ee_sif_status(rpc_msg);

    if(client == 0 || client->server == 0 || client->buf == 0)
        return -1;

    pkt = (volatile ee_sif_rpc_call_pkt_t *)rpc_alloc_packet();
    ee_sif_fill_header((ee_sif_cmd_header_t *)&pkt->sifcmd,
                       EE_SIF_CMD_RPC_CALL, SIF_PACKET_SIZE);
    rid = ++sifrpc_next_rid;
    pkt->rec_id = (int32_t)SIFRPC_REC_ID_FOR(rid);
    pkt->pkt_addr = (void *)EE_UNCACHED(pkt);
    pkt->rpc_id = 0;
    pkt->cd = (void *)client;
    pkt->rpc_number = rpc_number;
    pkt->send_size = (int32_t)send_size;
    pkt->recvbuf = recvbuf;
    pkt->recv_size = (int32_t)recv_size;
    pkt->rmode = 1;
    pkt->sd = client->server;

    client->pkt_addr = (void *)pkt;
    client->rpc_id = 0;
    client->command = (uint32_t)rpc_number;

    if(recv_size != 0u)
        cache_flush_dc(recvbuf, recv_size);

    if(ee_sif_send_cmd(EE_SIF_CMD_RPC_CALL, (void *)pkt, SIF_PACKET_SIZE,
                        sendbuf, client->buf, send_size) < 0)
        return -2;

    reply = (volatile ee_sif_rpc_header_t *)
        ee_sif_poll_packet(EE_SIF_CMD_RPC_END, RPC_REPLY_TIMEOUT);
    if(reply == 0) {
        static char cnr_msg[56];
        volatile ee_sif_cmd_header_t *recv_hdr =
            (volatile ee_sif_cmd_header_t *)EE_UNCACHED(sif_recv_buf);
        uint32_t recv_cid = recv_hdr->cid;
        uint32_t recv_ds = (uint32_t)recv_hdr->psize |
                           ((uint32_t)recv_hdr->dsize << 8);
        uint32_t dstat = D_STAT;
        char *p = cnr_msg;
        static const char hex[] = "0123456789abcdef";
        int i;

        const char *prefix = "PHASE1: CNR#";
        for (; *prefix; ++prefix) *p++ = *prefix;
        if(my_seq >= 10u) *p++ = (char)('0' + (my_seq / 10u) % 10u);
        *p++ = (char)('0' + (my_seq % 10u));
        *p++ = ' '; *p++ = 'c'; *p++ = '=';
        for (i = 28; i >= 0; i -= 4) *p++ = hex[(recv_cid >> i) & 0xfu];
        *p++ = ' '; *p++ = 'd'; *p++ = '=';
        for (i = 4; i >= 0; i -= 4) *p++ = hex[(recv_ds >> i) & 0xfu];
        *p++ = ' '; *p++ = 's'; *p++ = '=';
        for (i = 28; i >= 0; i -= 4) *p++ = hex[(dstat >> i) & 0xfu];
        *p = '\0';

        ee_sif_status(cnr_msg);
        return -3;
    }

    ee_sif_rearm_receive();
    return 0;
}

void *ee_sif_alloc_iop_heap(uint32_t size) {
    volatile uint32_t *result_view;

    if(iopheap_client.server == 0) {
        //ee_sif_status("PHASE1: HEAP BIND");
        if(ee_sif_rpc_bind(&iopheap_client, EE_SIF_IOPHEAP_RPC_ID) < 0)
            return 0;
    }

    iopheap_arg = size;
    cache_flush_dc((const void *)&iopheap_arg, sizeof(iopheap_arg));
    if(ee_sif_rpc_call(&iopheap_client, IOPHEAP_ALLOC_CID,
                        (const void *)&iopheap_arg, sizeof(iopheap_arg),
                        (void *)&iopheap_arg, sizeof(iopheap_arg)) < 0)
        return 0;

    result_view = (volatile uint32_t *)EE_UNCACHED(&iopheap_arg);
    return (void *)*result_view;
}

int ee_sif_load_module_buffer(const void *irx, uint32_t irx_size,
                              const char *args, uint32_t arg_len,
                              ee_sif_load_module_result_t *out) {
    volatile ee_sif_load_module_result_t *result_view;
    void *irx_iop_addr;
    uint32_t irx_qw_size;

    if(irx == 0 || irx_size == 0u || arg_len > 252u)
        return -1;
    if(arg_len != 0u && args == 0)
        return -1;

    if(loadfile_client.server == 0) {
        if(ee_sif_rpc_bind_retry(&loadfile_client,
                                  EE_SIF_LOADFILE_RPC_ID, 64u) < 0)
            return -2;
    }

    irx_qw_size = (irx_size + 15u) & ~15u;
    irx_iop_addr = ee_sif_alloc_iop_heap(irx_qw_size);
    if(irx_iop_addr == 0)
        return -3;
    if(ee_sif_iop_write(irx_iop_addr, irx, irx_qw_size) < 0)
        return -4;

    memset((void *)&load_args, 0, sizeof(load_args));
    memset((void *)&load_result, 0, sizeof(load_result));
    load_args.module_ptr =
        EE_SIF_IOP_KSEG1_BASE | (((uint32_t)irx_iop_addr) & 0x1fffffu);
    load_args.arg_len = arg_len;
    if(arg_len != 0u)
        memcpy((void *)load_args.args, args, arg_len);

    cache_flush_dc((const void *)&load_args, sizeof(load_args));
    cache_flush_dc((const void *)&load_result, sizeof(load_result));

    if(ee_sif_rpc_call(&loadfile_client, LOADFILE_LOAD_BUF_CID,
                        (const void *)&load_args, sizeof(load_args),
                        (void *)&load_result, sizeof(load_result)) < 0)
        return -5;

    result_view =
        (volatile ee_sif_load_module_result_t *)EE_UNCACHED(&load_result);
    if(out != 0) {
        out->result = result_view->result;
        out->modres = result_view->modres;
    }

    return (int32_t)result_view->result;
}

static inline uint32_t iop_read_word_direct(uint32_t iop_addr) {
    return *(volatile uint32_t *)(IOP_RAM_BASE_EE + (iop_addr & 0x1FFFFFFu));
}

static inline void iop_write_word_direct(uint32_t iop_addr, uint32_t value) {
    *(volatile uint32_t *)(IOP_RAM_BASE_EE + (iop_addr & 0x1FFFFFFu)) = value;
}

static uint32_t lmb_find_loadfile_dispatcher(void) {
    uint32_t addr;
    for (addr = 0x00010000u; addr < EE_SIF_IOP_RAM_SIZE - 16u; addr += 4u) {
        uint32_t w1;
        if(iop_read_word_direct(addr) != LF_DISPATCH_SIG_0) continue;
        w1 = iop_read_word_direct(addr + 4u);
        if(w1 != LF_DISPATCH_SIG_1A && w1 != LF_DISPATCH_SIG_1B) continue;
        if(iop_read_word_direct(addr + 8u) != LF_DISPATCH_SIG_2) continue;
        if(iop_read_word_direct(addr + 12u) != LF_DISPATCH_SIG_3) continue;
        return addr;
    }
    return 0;
}

static uint32_t lmb_walk_for_modload(void) {
    uint32_t head;
    uint32_t entry;
    int safety;

    if(ee_sif_iop_read_word(IOP_LOADCORE_LIBLIST_HEAD, &head) < 0)
        return 0;
    if(head < 0x800u || head >= EE_SIF_IOP_RAM_SIZE)
        return 0;

    entry = head;
    for (safety = 0; safety < 64; safety++) {
        uint32_t next_ptr;
        uint32_t name_w0;
        uint32_t name_w1;

        if(entry == 0u || entry >= EE_SIF_IOP_RAM_SIZE)
            break;

        if(ee_sif_iop_read_word(entry + 0x0Cu, &name_w0) < 0) break;
        if(ee_sif_iop_read_word(entry + 0x10u, &name_w1) < 0) break;

        if(name_w0 == 0x6C646F6Du &&
            (name_w1 & 0x00FFFFFFu) == 0x0064616Fu)
            return entry + LIB_ENTRY_EXPORTS_OFF;

        if(ee_sif_iop_read_word(entry + 0x00u, &next_ptr) < 0) break;
        entry = next_ptr;
    }

    return 0;
}

static uint32_t lmb_scan_for_modload(void) {
    uint32_t addr;
    for (addr = 0x800u; addr < EE_SIF_IOP_RAM_SIZE - 16u; addr += 4u) {
        if(iop_read_word_direct(addr) != 0x6C646F6Du) continue;
        if((iop_read_word_direct(addr + 4u) & 0x00FFFFFFu) !=
            0x0064616Fu) continue;
        return addr + 0x8u;
    }
    return 0;
}

static void lmb_build_stub(uint32_t *stub,
                           uint32_t load_module_buffer,
                           uint32_t start_module,
                           uint32_t result_iop_addr) {
    uint32_t jal_lmb =
        0x0c000000u | ((load_module_buffer >> 2) & 0x03FFFFFFu);
    uint32_t jal_sm =
        0x0c000000u | ((start_module >> 2) & 0x03FFFFFFu);
    uint32_t lui_s1 =
        0x3c110000u | ((result_iop_addr >> 16) & 0xFFFFu);
    uint32_t ori_s1 =
        0x36310000u | (result_iop_addr & 0xFFFFu);

    stub[ 0] = 0x27bdffd8u;
    stub[ 1] = 0xafb00018u;
    stub[ 2] = 0xafbf0020u;
    stub[ 3] = 0x00808021u;
    stub[ 4] = 0x8c840000u;
    stub[ 5] = jal_lmb;
    stub[ 6] = 0xafb1001cu;
    stub[ 7] = lui_s1;
    stub[ 8] = 0x04400008u;
    stub[ 9] = ori_s1;
    stub[10] = 0x00402021u;
    stub[11] = 0x26250008u;
    stub[12] = 0x8e060004u;
    stub[13] = 0x26070104u;
    stub[14] = 0x26280004u;
    stub[15] = jal_sm;
    stub[16] = 0xafa80010u;
    stub[17] = 0xae220000u;
    stub[18] = 0x02201021u;
    stub[19] = 0x8fbf0020u;
    stub[20] = 0x8fb1001cu;
    stub[21] = 0x8fb00018u;
    stub[22] = 0x03e00008u;
    stub[23] = 0x27bd0028u;
    stub[24] = 0x00000000u;
    stub[25] = 0x00000000u;
    stub[26] = 0x7962424cu;
    stub[27] = 0x00004545u;
    stub[28] = 0x00000000u;
    stub[29] = 0x00000000u;
    stub[30] = 0x00000000u;
    stub[31] = 0x00000000u;
}

int ee_sif_apply_lmb_patch(void) {
    uint32_t lf_dispatcher;
    uint32_t modload_exports;
    uint32_t load_module_buffer;
    uint32_t start_module;
    uint32_t lui_at_inst;
    uint32_t lw_v0_inst;
    uint32_t jt_base;
    void *patch_iop;
    uint32_t patch_addr;
    uint32_t result_addr;
    static uint32_t stub[32] __attribute__((aligned(64)));

    lf_dispatcher = lmb_find_loadfile_dispatcher();
    if(lf_dispatcher == 0)
        return -1;

    if(iop_read_word_direct(lf_dispatcher + 4u) == LF_DISPATCH_SIG_1B)
        return 0;

    modload_exports = lmb_walk_for_modload();
    if(modload_exports == 0)
        modload_exports = lmb_scan_for_modload();
    if(modload_exports == 0)
        return -2;

    start_module = iop_read_word_direct(modload_exports + 8 * 4);
    load_module_buffer = iop_read_word_direct(modload_exports + 10 * 4);
    if(start_module == 0 || load_module_buffer == 0)
        return -3;

    lui_at_inst = iop_read_word_direct(lf_dispatcher + 0x1Cu);
    lw_v0_inst = iop_read_word_direct(lf_dispatcher + 0x24u);
    jt_base = ((lui_at_inst & 0xFFFFu) << 16)
            + (uint32_t)(int32_t)(int16_t)(lw_v0_inst & 0xFFFFu);

    patch_iop = ee_sif_alloc_iop_heap(sizeof(stub));
    if(patch_iop == 0)
        return -4;
    patch_addr = (uint32_t)patch_iop;
    result_addr = patch_addr + 96u;

    lmb_build_stub(stub, load_module_buffer, start_module, result_addr);
    cache_flush_dc(stub, sizeof(stub));
    if(ee_sif_iop_write(patch_iop, stub, sizeof(stub)) < 0)
        return -5;

    iop_write_word_direct(jt_base + 0x18u, patch_addr);
    iop_write_word_direct(lf_dispatcher + 4u, 0x2c820007u);
    return 0;
}

int ee_sif_iop_reset(const char *arg) {
    static ee_sif_iop_reset_pkt_t reset_packet __attribute__((aligned(64)));
    int arg_len = 0;
    uint32_t arg_len_padded;
    int rc;
    unsigned int i;

    if(arg != 0) {
        while (arg[arg_len] != '\0' &&
               arg_len < (int)EE_SIF_IOP_RESET_ARG_MAX)
            arg_len++;
    }
    arg_len_padded = ((uint32_t)arg_len + 3u) & ~3u;

    memset(&reset_packet, 0, sizeof(reset_packet));
    if(arg_len > 0) {
        int j;
        for (j = 0; j < arg_len; j++)
            reset_packet.arg[j] = arg[j];
    }
    reset_packet.arg_len = (int32_t)arg_len_padded;
    reset_packet.mode = 0;

    ee_sif_set_reg(SIF_REG_SMFLAG, EE_SIF_STAT_BOOTEND);

    rc = ee_sif_send_cmd(EE_SIF_CMD_IOP_RESET,
                         &reset_packet, sizeof(reset_packet), 0, 0, 0);
    if(rc < 0)
        return -1;

    ee_sif_set_reg(SIF_REG_SMFLAG, 0u);
    ee_delay(2000000u);

    for (i = 0; i < POLL_TIMEOUT; i++) {
        if((uint32_t)ee_sif_get_reg(SIF_REG_SMFLAG) & EE_SIF_STAT_BOOTEND)
            goto bootend_seen;
    }
    return -2;

bootend_seen:
    ee_sif_reset_state();
    return 0;
}
