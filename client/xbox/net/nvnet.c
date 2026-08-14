/* client/xbox/net/nvnet.c
 *
 * Minimal polling driver for the original Xbox's onboard nForce (NVnet)
 * Ethernet MAC — the "forcedeth" hardware.  Single RX ring + single TX ring,
 * no interrupts (the shared loader loop polls).
 *
 * ============================ PROVENANCE — READ ME ============================
 * The nForce MAC has no public datasheet.  This register map, descriptor layout,
 * control bits, and init sequence were recovered CLEAN-ROOM by disassembling the
 * NIC driver inside a binary the Xbox itself ships: the "XNET" section of an
 * Xbox dashboard XBE (UnleashX, xboxdash.xbe).  RE of binaries is the permitted
 * route; no driver source (forcedeth/nxdk/xbox-linux) was read.  Each value
 * below is annotated with the XNET disassembly address it came from.
 *
 * Key facts proven by that disassembly:
 *   - the MAC register file is at a FIXED absolute MMIO base 0xFEF00000; the
 *     driver uses literal `ds:0xFEF000xx` accesses and does NO PCI config-space
 *     (CF8/CFC) enumeration — the Xbox is a fixed platform and the kernel has
 *     already PCI-initialised the onboard NIC before an XBE runs;
 *   - the descriptor is the 8-byte v1 form
 *     { u32 buffer_phys; u16 length; u16 flags };
 *
 * Still NOT proven by this binary (kept from the forcedeth model, marked OPEN):
 *   PollingInterval 0x00c (unreferenced); PFF_PROMISC 0x80 (promisc never taken);
 *   the exact TX "last packet" bit (bit16 is used in the status path but its
 *   sole meaning wasn't isolated).  These do not affect basic RX/TX bring-up.
 *
 * PHY access was recovered from the Xbox kernel's PhyGetLinkState implementation.
 * The MAC's MII control/data registers are driven directly here; no kernel thunk
 * is required.
 *
 * Xbox specifics:
 *   - the loader relocates itself to run IDENTITY-mapped (VA == PA) at physical
 *     0x11000 before networking starts (xbox_relocate_identity() in target.c),
 *     so dma_phys() is just the identity.  A kernel-loaded XBE's low RAM is
 *     scattered across arbitrary physical pages, so without that relocation a
 *     plain VA cast makes the NIC DMA into phantom memory (rx stays 0);
 *   - x86 DMA is cache-coherent, so no explicit cache flushing is required.
 * =============================================================================
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "adapter.h"
#include "nvnet.h"
#include <kosload/net_stack.h>
#include "packet.h"
#include "kosload.h"
#include "../video.h"

#include <kosload/dhcp.h>
#include <kosload/target.h>
#include <kosload/net_stack.h>
#include <kosload/screensaver.h>

/* ===== Fixed MMIO base (RE: literal ds:0xFEF00000 in XNET @ 1e9388/1e9b74) ===== */

#define NVNET_MMIO_BASE           0xFEF00000u

/* ===== REGISTER MAP — RE-confirmed from xboxdash.xbe XNET (see PROVENANCE).
 * Trailing comment = XNET disassembly VMA the offset was observed at. */

#define NvRegIrqStatus            0x000  /* read-to-ack in ISR      1e9f64 */
#define NvRegIrqMask              0x004  /* =0x5f enable / 0 poll   1e9f77 */
#define NvRegPollingInterval      0x00c  /* OPEN: unreferenced             */
#define NvRegMisc1                0x080  /* =0x3b0f3e; duplex bit1  1e9e83 */
#define NvRegTransmitterControl   0x084  /* start=1 / stop=0        1e92e6 */
#define NvRegTransmitterStatus    0x088  /* poll bit0 = idle        1e9333 */
#define NvRegPacketFilterFlags    0x08c  /* =0x7f0020               1e9e79 */
#define NvRegReg090               0x090  /* filter aux =0x5ee       1e9e6f */
#define NvRegReceiverControl      0x094  /* start=1 / stop=0        1e92eb */
#define NvRegReceiverStatus       0x098  /* poll bit0 = idle        1e932a */
#define NvRegReg09c               0x09c  /* mcast hash | 0x7f00     1e9eae */
#define NvRegReg0a0               0x0a0  /* backoff cfg =0x16070f   1e9e..  */
#define NvRegReg0a4               0x0a4  /* backoff cfg =0x16       1e9e..  */
#define NvRegMacAddrA             0x0a8  /* MAC bytes [0..3]        1e9e2d */
#define NvRegMacAddrB             0x0ac  /* MAC bytes [4..5] (u16)  1e9e39 */
#define NvRegMulticastAddrA       0x0b0  /* =0xffffffff             1e9e52 */
#define NvRegMulticastAddrB       0x0b4  /* =0xffff                 1e9e5c */
#define NvRegMulticastMaskA       0x0b8  /* =0xffffffff             1e9e3e */
#define NvRegMulticastMaskB       0x0bc  /* =0xffff                 1e9e4d */
#define NvRegTxRingPhysAddr       0x100  /* TX ring base            1e9ed5 */
#define NvRegRxRingPhysAddr       0x104  /* RX ring base            1e9ee6 */
#define NvRegRingSizes            0x108  /* (rx-1)<<16 | (tx-1)     1e9eeb */
#define NvRegLinkSpeed            0x110  /* speed | 0x10000         1e92de */
#define NvRegReg13c               0x13c  /* TX ring watermark 0x300010 1e9f0e */
#define NvRegReg140               0x140  /* TX ring limit    0x300010  1e9f09 */
#define NvRegTxRxControl          0x144  /* main control            1e9344 */
#define NvRegMIIStatus            0x180  /* PHY event, write-1-clear 1e9b8a */
#define NvRegReg184               0x184  /* =0x8 after ring init    1e9f68 */
#define NvRegAdapterControl       0x188  /* PHY-select + start      1e9f13 */
#define NvRegMIISpeed             0x18c  /* PHY select =0x105       1e9f1d */
#define NvRegMIIControl           0x190  /* MII command/status      2d599 */
#define NvRegMIIData              0x194  /* MII read/write data     2d5df */

#define NVREG_MIICTL_READ         0x0020
#define NVREG_MIICTL_INUSE        0x8000
#define NVREG_MII_LINK_STATUS     0x0004 /* IEEE 802.3 BMSR link bit */
#define NVREG_MII_BMSR            0x0001
#define NVREG_MII_TIMEOUT_TICKS   7333333u /* 10 ms at 733.33 MHz */

/* TxRxControl (0x144) bits — RE @ 1e9344/1e934f/1e937c/1e986c. */
#define NVREG_TXRXCTL_KICK        0x0001 /* TX kick             1e986c */
#define NVREG_TXRXCTL_BIT1        0x0002 /* used in link path   1e9ba5 */
#define NVREG_TXRXCTL_BIT2        0x0004 /* pre-reset           1e9344 */
#define NVREG_TXRXCTL_IDLE        0x0008 /* poll = idle ack     1e934f */
#define NVREG_TXRXCTL_RESET       0x0010 /* soft reset          1e937c */

#define NVREG_XMITCTL_START       0x01   /* TransmitterControl  1e92e6 */
#define NVREG_RCVCTL_START        0x01   /* ReceiverControl     1e92eb */
#define NVREG_STAT_BUSY           0x01   /* TX/RX Status bit0 = engine busy */

/* Interrupt-status bits still latch while NvRegIrqMask is zero.  Polling code
 * must acknowledge them, and RX_NOBUF requires an explicit descriptor GET. */
#define NVREG_IRQ_RX_NOBUF        0x0004

/* PacketFilterFlags (0x08c): 0x7f0020 = ALWAYS(0x7f0000) | MYADDR(0x20). */
#define NVREG_PFF_ALWAYS          0x7F0000
#define NVREG_PFF_MYADDR          0x20
#define NVREG_PFF_PROMISC         0x80   /* OPEN: promisc path never taken */

/* AdapterControl (0x188). */
#define NVREG_ADAPTCTL_INIT       0x1040000 /* PHY-select value  1e9f13 */
#define NVREG_ADAPTCTL_RUNNING    0x100000  /* start/running     1e9f37 */

/* LinkSpeed (0x110) values seen: 0x3e8 (1000) = 10 Mbit, 0x64 (100) = 100 Mbit. */
#define NVREG_LINKSPEED_100       100
#define NVREG_LINKSPEED_FLAG      0x10000

/* ===== Descriptor ring ===== */

/* Original 32-bit NVnet descriptor: physical buffer pointer + flags/length. */
typedef struct {
    volatile uint32_t buf;      /* physical address of the packet buffer */
    volatile uint16_t length;
    volatile uint16_t flags;
} __attribute__((packed)) nv_desc_t;

#define RX_RING   16
#define TX_RING   4
#define NV_BUFSZ  2048          /* per-descriptor buffer (>= max frame)   */

/* RX flags */
#define NV_RX_DESCRIPTORVALID (1u << 0)
#define NV_RX_SUBTRACT1       (1u << 2)
#define NV_RX_ERROR_MASK      0x1f80u
#define NV_RX_AVAIL           (1u << 15) /* owned by NIC when set          */

/* TX flags */
#define NV_TX_LASTPACKET     (1u << 0)
#define NV_TX_VALID          (1u << 15)

extern uint8_t __nicpersist_start__[] __asm__("__nicpersist_start__");
#define NICPERSIST_BASE ((uintptr_t)__nicpersist_start__)

static uint8_t (*const rx_buf)[NV_BUFSZ] =
    (uint8_t (*)[NV_BUFSZ])(NICPERSIST_BASE + 0x00000u);   /* 16*2048 = 0x8000 */
static uint8_t (*const tx_buf)[NV_BUFSZ] =
    (uint8_t (*)[NV_BUFSZ])(NICPERSIST_BASE + 0x08000u);   /*  4*2048 = 0x2000 */
static nv_desc_t *const rx_ring = (nv_desc_t *)(NICPERSIST_BASE + 0x0A000u);
static nv_desc_t *const tx_ring = (nv_desc_t *)(NICPERSIST_BASE + 0x0B000u);

#define cur_rx (*(volatile unsigned int *)(NICPERSIST_BASE + 0x0C000u))
#define cur_tx (*(volatile unsigned int *)(NICPERSIST_BASE + 0x0C004u))
#define nic_magic (*(volatile uint32_t *)(NICPERSIST_BASE + 0x0C008u))
#define NIC_MAGIC  0x6b4e4943u /* 'kNIC' */

static volatile uint8_t *mmio;
static bool link_up;
static uint64_t next_link_poll;
static const char *last_error = "NO ETHERNET ADAPTER DETECTED!";

/* Crash-screen telemetry.  Kept as simple globals so the exception handler
 * can report ring corruption without calling back into the NIC driver. */
volatile uint32_t nvnet_dbg_cur_rx;
volatile uint32_t nvnet_dbg_cur_tx;
volatile uint32_t nvnet_dbg_bad_desc;
volatile uint32_t nvnet_dbg_tx_timeout;

static inline uint32_t reg_rd(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }
static inline void reg_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }

static uint32_t dma_phys(const void *ptr) {
    /*
     * The loader relocates itself to run IDENTITY-mapped (VA == PA) at physical
     * 0x11000 before networking starts — see xbox_relocate_identity() in
     * target.c.  A DMA buffer's physical address is therefore simply its virtual
     * address: no MmGetPhysicalAddress-style page-walk is needed, and we avoid
     * depending on the kernel's self-mapped page tables.  (The NIC's rings and
     * buffers are all loader-internal, so they live in the identity region;
     * guests never DMA.)
     */
    return (uint32_t)(uintptr_t)ptr;
}

static uint64_t mii_read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool mii_wait_idle(void) {
    uint64_t deadline = mii_read_tsc() + NVREG_MII_TIMEOUT_TICKS;

    while(reg_rd(NvRegMIIControl) & NVREG_MIICTL_INUSE) {
        if(mii_read_tsc() >= deadline)
            return false;
        __asm__ volatile("pause");
    }

    return true;
}

static bool mii_read(uint8_t phy_reg, uint16_t *value) {
    /* Match the kernel's recovery path: a transaction inherited from the
     * dashboard is cancelled by writing INUSE before waiting for idle. */
    if(reg_rd(NvRegMIIControl) & NVREG_MIICTL_INUSE) {
        reg_wr(NvRegMIIControl, NVREG_MIICTL_INUSE);
        if(!mii_wait_idle())
            return false;
    }

    reg_wr(NvRegMIIControl, (uint32_t)phy_reg | NVREG_MIICTL_READ);
    if(!mii_wait_idle())
        return false;

    *value = (uint16_t)reg_rd(NvRegMIIData);
    return true;
}

static bool phy_link_is_up(void) {
    uint16_t status;

    /*
     * Mirror the kernel's PhyGetLinkState (RE @ 0x8002d659): it refreshes its
     * cached composite link state ONLY on a successful MII read and otherwise
     * returns the previously cached value.  The MAC autonomously polls the PHY
     * (NvRegAdapterControl RUNNING is set in nvnet_init), so a manual BMSR read
     * frequently collides with an in-flight auto-poll and has to take the abort
     * path or time out.  Reporting "link down" on such a transient failure made
     * link_up flap on nearly every 100 ms poll; each flap fires the transition
     * handler in nvnet_poll_link(), which sets escape_loop and zeroes
     * dhcp_attempts — tearing down the DHCP OFFER/ACK receive-wait before a
     * reply can arrive, so DHCP never completes.  Keep the last known state on
     * failure (sticky), exactly as the kernel returns its cached value.  A real
     * cable pull still reports link=0 through a SUCCESSFUL read, because the PHY
     * keeps answering MDIO with the cable out — so genuine unplugs still work.
     *
     * BMSR link status is latch-low, so read it twice to surface a link that
     * recovered since the previous poll.
     */
    if(!mii_read(NVREG_MII_BMSR, &status) ||
       !mii_read(NVREG_MII_BMSR, &status))
        return link_up;

    return (status & NVREG_MII_LINK_STATUS) != 0;
}

static void nvnet_poll_link(const target_ops_t *t) {
    uint64_t now = t->get_ticks();
    bool new_link;

    if(next_link_poll && now < next_link_poll)
        return;
    next_link_poll = now + t->ticks_per_second / 10u;

    new_link = phy_link_is_up();
    if(new_link == link_up)
        return;

    link_up = new_link;
    screensaver_wake();

    if(booted && !running)
        disp_status(link_up ? "idle..." : "link lost...");

    if(link_up) {
        /* Restart ring processing after the PHY reacquires carrier. */
        reg_wr(NvRegReceiverControl, NVREG_RCVCTL_START);
        reg_wr(NvRegTransmitterControl, NVREG_XMITCTL_START);
        reg_wr(NvRegTxRxControl, NVREG_TXRXCTL_BIT1 | NVREG_TXRXCTL_KICK);
    }

    /* A cable transition during a DHCP receive wait should retry immediately
     * instead of consuming the remainder of the old timeout. */
    if(timeout_loop > 0) {
        dhcp_attempts = 0;
        timeout_loop = -1;
        escape_loop = 1;
    }
}

const char *nvnet_get_last_error(void) { return last_error; }

/* ===== Bring-up ===== */

static int nvnet_detect(void) {
    /* The onboard NIC is always present and its MMIO BAR is hard-mapped at a
     * fixed address; the kernel has already PCI-initialised it (bus-master +
     * memory space) before an XBE runs, so — matching the dashboard driver —
     * there is no PCI config probe and no bus-master enable to do here. */
    mmio = (volatile uint8_t *)NVNET_MMIO_BASE;

    global_bg_color = NVNET_BG_COLOR;
    installed_adapter = ADAPTER_XBOX_NFORCE;
    return 0;
}

static void nvnet_read_mac(unsigned char *mac) {
    /* MAC is in MacAddrA (low 4 bytes) + MacAddrB (high 2 bytes), programmed
     * by the kernel/EEPROM.  Xbox hardware exposes the bytes in this
     * little-endian A/B layout. */
    uint32_t a = reg_rd(NvRegMacAddrA);
    uint32_t b = reg_rd(NvRegMacAddrB);
    mac[0] = (a >> 0) & 0xff;
    mac[1] = (a >> 8) & 0xff;
    mac[2] = (a >> 16) & 0xff;
    mac[3] = (a >> 24) & 0xff;
    mac[4] = (b >> 0) & 0xff;
    mac[5] = (b >> 8) & 0xff;

    /* Some environments leave the NIC's MAC registers unprogrammed — notably
     * xemu, which doesn't emulate the EEPROM MAC, so they read all-zero.  An
     * all-zero source MAC is invalid (DHCP servers may reject it and ARP can't
     * resolve it), so synthesise one under the genuine Xbox (Microsoft) OUI
     * 00:0D:3A with TSC-derived low bytes (unique per boot, so two emulator
     * instances on one LAN don't collide) and program it into the NIC so the
     * MYADDR RX filter matches what we transmit.  Never triggers on real
     * hardware, where the kernel programs a valid EEPROM MAC before the XBE runs. */
    if((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) {
        uint64_t t = mii_read_tsc();
        mac[0] = 0x00; mac[1] = 0x0d; mac[2] = 0x3a;   /* Xbox (Microsoft) OUI */
        mac[3] = (uint8_t)(t >> 16);
        mac[4] = (uint8_t)(t >> 8);
        mac[5] = (uint8_t)t;
        reg_wr(NvRegMacAddrA,
               mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((uint32_t)mac[3] << 24));
        reg_wr(NvRegMacAddrB, mac[4] | (mac[5] << 8));
    }
}

static void nvnet_setup_rings(void) {
    unsigned int i;
    for(i = 0; i < RX_RING; i++) {
        rx_ring[i].buf = dma_phys(rx_buf[i]);
        rx_ring[i].length = NV_BUFSZ;
        rx_ring[i].flags = NV_RX_AVAIL;
    }
    for(i = 0; i < TX_RING; i++) {
        tx_ring[i].buf = dma_phys(tx_buf[i]);
        tx_ring[i].length = 0;
        tx_ring[i].flags = 0; /* owned by host until we transmit */
    }
    cur_rx = 0;
    cur_tx = 0;

    reg_wr(NvRegTxRingPhysAddr, dma_phys(tx_ring));
    reg_wr(NvRegRxRingPhysAddr, dma_phys(rx_ring));
    /* RingSizes packs (rx-1) in the high half, (tx-1) in the low half. */
    reg_wr(NvRegRingSizes, ((RX_RING - 1) << 16) | (TX_RING - 1));
}

/* Stop the engines and soft-reset the TX/RX control block, following the
 * dashboard XNET sequence: stop TX/RX + wait for the status idle bits, then
 * TxRxControl = BIT2, wait for the IDLE ack (bit3), pulse RESET, clear. */
static void nvnet_reset_engines(void) {
    unsigned int t;

    reg_wr(NvRegTransmitterControl, 0);          /* 1e931d */
    reg_wr(NvRegReceiverControl, 0);             /* 1e9313 */
    for(t = 100000; t && (reg_rd(NvRegTransmitterStatus) & NVREG_STAT_BUSY); t--)
        ;                                        /* 1e9333: wait TX idle */
    for(t = 100000; t && (reg_rd(NvRegReceiverStatus) & NVREG_STAT_BUSY); t--)
        ;                                        /* 1e932a: wait RX idle */

    reg_wr(NvRegTxRxControl, NVREG_TXRXCTL_BIT2);            /* 1e9344 */
    for(t = 100000; t && !(reg_rd(NvRegTxRxControl) & NVREG_TXRXCTL_IDLE); t--)
        ;                                                   /* 1e934f: idle ack */
    reg_wr(NvRegTxRxControl, NVREG_TXRXCTL_RESET);           /* 1e937c */
    reg_wr(NvRegTxRxControl, 0);                             /* 1e9391 */
}

static int nvnet_init(void) {
    if(nic_magic == NIC_MAGIC) {
        /*
         * Preserve the live rings and both hardware/software cursors across
         * guest return and firmware re-entry.  The replacement flat image
         * reinitializes adapter_nvnet.mac to zero, so restore it from the
         * still-programmed MAC registers before the common stack builds or
         * filters any packets.
         */
        nvnet_read_mac(adapter_nvnet.mac);
        link_up = phy_link_is_up();
        next_link_poll = 0;
        return 0;
    }

    nvnet_reset_engines();

    nvnet_read_mac(adapter_nvnet.mac);
    nvnet_setup_rings();

    /* Chip config values as written by the dashboard driver (XNET @ 1e9e..). */
    reg_wr(NvRegMisc1, 0x3b0f3e);
    reg_wr(NvRegReg090, 0x5ee);
    reg_wr(NvRegReg09c, 0x7f00);
    reg_wr(NvRegReg0a0, 0x16070f);
    reg_wr(NvRegReg0a4, 0x16);
    reg_wr(NvRegReg13c, 0x300010);
    reg_wr(NvRegReg140, 0x300010);

    /* Accept all multicast + our own address (dashboard writes all-ones). */
    reg_wr(NvRegMulticastAddrA, 0xffffffff);
    reg_wr(NvRegMulticastAddrB, 0xffff);
    reg_wr(NvRegMulticastMaskA, 0xffffffff);
    reg_wr(NvRegMulticastMaskB, 0xffff);
    reg_wr(NvRegPacketFilterFlags, NVREG_PFF_ALWAYS | NVREG_PFF_MYADDR); /* 0x7f0020 */

    /* The dashboard configures this path for 100 Mbit full-duplex. */
    reg_wr(NvRegLinkSpeed, NVREG_LINKSPEED_100 | NVREG_LINKSPEED_FLAG);

    /* No interrupts — we poll.  Ack any pending status. */
    reg_wr(NvRegIrqMask, 0);
    reg_wr(NvRegIrqStatus, reg_rd(NvRegIrqStatus));
    reg_wr(NvRegMIIStatus, reg_rd(NvRegMIIStatus)); /* write-1-to-clear */

    /* PHY-select then start the adapter (AdapterControl). */
    reg_wr(NvRegAdapterControl, NVREG_ADAPTCTL_INIT);
    reg_wr(NvRegMIISpeed, 0x105);
    reg_wr(NvRegAdapterControl, NVREG_ADAPTCTL_INIT | NVREG_ADAPTCTL_RUNNING);
    reg_wr(NvRegReg184, 0x8);

    /* Start the receiver + transmitter engines. */
    reg_wr(NvRegReceiverControl, NVREG_RCVCTL_START);
    reg_wr(NvRegTransmitterControl, NVREG_XMITCTL_START);
    reg_wr(NvRegTxRxControl, NVREG_TXRXCTL_BIT1 | NVREG_TXRXCTL_KICK);

    link_up = phy_link_is_up();
    next_link_poll = 0;
    nic_magic = NIC_MAGIC;
    return 0;
}

static void nvnet_start(void) {
    reg_wr(NvRegReceiverControl, NVREG_RCVCTL_START);
}

static void nvnet_stop(void) {
    /* Guests use the loader's network syscalls, so RX must remain active while
     * guest code executes.  Firmware replacement preserves the live rings. */
}

static bool nvnet_is_firmware_exec_ack(const unsigned char *pkt, int len) {
    const unsigned int command_offset = ETHER_H_LEN + IP_H_LEN + UDP_H_LEN;
    const ip_header_t *ip;
    const udp_header_t *udp;
    const net_command_t *command;

    if(len < (int)(command_offset + NET_COMMAND_LEN))
        return false;

    ip = (const ip_header_t *)(pkt + ETHER_H_LEN);
    if(ip->protocol != IP_UDP_PROTOCOL)
        return false;

    udp = (const udp_header_t *)(pkt + ETHER_H_LEN + IP_H_LEN);
    if(ntohs(udp->length) < UDP_H_LEN + NET_COMMAND_LEN)
        return false;

    command = (const net_command_t *)(pkt + command_offset);
    return memcmp(command->id, NET_CMD_EXECUTE, 4) == 0 &&
           (ntohl(command->size) & KOSLOAD_EXEC_FW_UPDATE) != 0;
}

static int nvnet_tx(unsigned char *pkt, int len) {
    nv_desc_t *d = &tx_ring[cur_tx];
    unsigned int timeout;
    int payload_len;

    if(len <= 0)
        return 0;

    /* Firmware-update EXEC is fire-and-forget on the host.  Unlike the other
     * console adapters, NVnet normally waits for TX completion, which made this
     * otherwise-unneeded acknowledgement reliably survive the trampoline and
     * sit ahead of the replacement loader's VERSION reply.  Suppress only this
     * response; normal EXEC acknowledgements still use the synchronous path. */
    if(nvnet_is_firmware_exec_ack(pkt, len))
        return 1;

    nvnet_dbg_cur_tx = cur_tx;
    if(d->buf != dma_phys(tx_buf[cur_tx]))
        nvnet_dbg_bad_desc++;

    /* Wait for the NIC to release this descriptor. */
    timeout = 1000000;
    while((d->flags & NV_TX_VALID) && timeout)
        timeout--;
    if(!timeout) {
        nvnet_dbg_tx_timeout++;
        return 0;
    }

    payload_len = len > NV_BUFSZ ? NV_BUFSZ : len;
    len = payload_len < 60 ? 60 : payload_len;

    memcpy(tx_buf[cur_tx], pkt, payload_len);
    if(len > payload_len)
        memset(tx_buf[cur_tx] + payload_len, 0, (size_t)(len - payload_len));
    d->buf = dma_phys(tx_buf[cur_tx]);
    d->length = (uint16_t)(len - 1);
    __asm__ volatile("" ::: "memory");
    d->flags = NV_TX_VALID | NV_TX_LASTPACKET;

    /* Kick the transmit engine. */
    reg_wr(NvRegTxRxControl, NVREG_TXRXCTL_KICK);

    /* This loader is polling-only and command handlers may immediately stop
     * RX or jump into guest code after sending their acknowledgment.  Wait for
     * ownership to return so the packet is on the wire before either happens. */
    timeout = 1000000;
    while((d->flags & NV_TX_VALID) && timeout)
        timeout--;
    if(!timeout)
        nvnet_dbg_tx_timeout++;

    {
        uint32_t irq = reg_rd(NvRegIrqStatus);
        if(irq)
            reg_wr(NvRegIrqStatus, irq);
        if(irq & NVREG_IRQ_RX_NOBUF)
            reg_wr(NvRegTxRxControl, NVREG_TXRXCTL_BIT1);
    }

    cur_tx = (cur_tx + 1) % TX_RING;
    return timeout != 0;
}

static int nvnet_rx(void) {
    int processed = 0;

    for(;;) {
        nv_desc_t *d = &rx_ring[cur_rx];
        uint16_t flags = d->flags;
        uint32_t len;
        bool deliver;

        nvnet_dbg_cur_rx = cur_rx;
        if(d->buf != dma_phys(rx_buf[cur_rx]))
            nvnet_dbg_bad_desc++;

        /* Still owned by the NIC → nothing received in this slot. */
        if(flags & NV_RX_AVAIL)
            break;

        len = d->length;
        if((flags & NV_RX_SUBTRACT1) && len)
            len--;
        deliver = (flags & NV_RX_DESCRIPTORVALID) &&
                  !(flags & NV_RX_ERROR_MASK) &&
                  len >= 14 && len <= RX_PKT_BUF_SIZE;
        if(deliver)
            memcpy(current_pkt, rx_buf[cur_rx], len);

        /*
         * Hand the descriptor back and advance the software head before
         * dispatching the packet.  EXEC is synchronous: process_pkt() can
         * enter a guest which immediately performs loader network syscalls,
         * recursively polling this same ring.  Dispatching first left this
         * descriptor host-owned and let the inner poll advance cur_rx; the
         * outer poll then advanced it a second time on guest return.  One RX
         * slot was skipped after every execution until the NIC ran out of
         * descriptors.  MSDash/nvnetdrv likewise removes a descriptor from
         * the RX ring before invoking its upper-layer callback.
         */
        d->buf = dma_phys(rx_buf[cur_rx]);
        d->length = NV_BUFSZ;
        __asm__ volatile("" ::: "memory");
        d->flags = NV_RX_AVAIL;
        reg_wr(NvRegTxRxControl, NVREG_TXRXCTL_BIT1);

        cur_rx = (cur_rx + 1) % RX_RING;
        processed++;

        if(deliver)
            process_pkt(current_pkt);

        if(processed > RX_RING)
            break;
    }

    {
        uint32_t irq = reg_rd(NvRegIrqStatus);
        if(irq)
            reg_wr(NvRegIrqStatus, irq);
        if(irq & NVREG_IRQ_RX_NOBUF)
            reg_wr(NvRegTxRxControl, NVREG_TXRXCTL_BIT1);
    }

    return processed;
}

static void nvnet_loop(bool is_main_loop) {
    const target_ops_t *t = target_get_ops();
    uint64_t last_sec_tick = 0;
    unsigned int secs = 0;

    if(is_main_loop && !(booted || running))
        disp_info();

    if(timeout_loop > 0)
        last_sec_tick = t->get_ticks();

    while(!escape_loop) {
        int got_rx;

        /* Keep our framebuffer on screen: the kernel's boot-animation thread
         * resets NV2A scanout ~1 s after title handoff, so re-assert it each
         * iteration (cheap, pure MMIO).  See client/xbox/video.c. */
        xbox_video_kick();
        nvnet_poll_link(t);

        got_rx = nvnet_rx();

        /* The Xbox polling path can lose a syscall request/reply while rings
         * are being restarted around guest execution.  Honor the shared
         * bounded-wait deadline so the syscall layer can retransmit the same
         * sequence-numbered request instead of waiting forever. */
        if(!got_rx && loop_deadline_ticks &&
           t->get_ticks() >= loop_deadline_ticks)
            break;

        if(is_main_loop)
            dhcp_tick();

        /* PHY status is advisory.  Never let a failed/stale MII read suppress
         * DHCP: the MAC can receive and transmit as soon as its engines are
         * running, and DHCP's own bounded retry handles a disconnected cable. */
        if(is_main_loop)
            dhcp_poll();

        if(timeout_loop > 0 && !escape_loop) {
            uint64_t now = t->get_ticks();
            if((now - last_sec_tick) >= t->ticks_per_second) {
                last_sec_tick = now;
                secs++;
                if(dhcp_attempts > 1) {
                    disp_dhcp_attempts_count();
                    disp_dhcp_next_attempt(timeout_loop - secs + 1);
                }
                if(secs > (unsigned int)timeout_loop) {
                    timeout_loop = -1;
                    escape_loop = 1;
                }
            }
        }

        if(is_main_loop && !escape_loop)
            screensaver_poll();

        /* A non-main invocation is the synchronous receive wait used by DHCP
         * and network syscalls.  Stay here until process_pkt() sets
         * escape_loop or the timeout above expires; returning after one poll
         * makes DHCP send REQUEST before an OFFER has arrived. */
    }
    escape_loop = 0;
}

adapter_t adapter_nvnet = {
    "nForce Ethernet (NVnet)",
    {0},        /* MAC (filled by init) */
    {0},        /* pad */
    nvnet_detect,
    nvnet_init,
    nvnet_start,
    nvnet_stop,
    nvnet_loop,
    nvnet_tx,
    true,       /* bounded deduplicated retries prevent permanent syscall stalls */
};
