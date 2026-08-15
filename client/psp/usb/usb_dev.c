/* client/psp/usb/usb_dev.c — PSP USB device-controller driver (bare-metal).
 *
 * ============================ PROVENANCE — READ ME ============================
 * Bare-metal USB device driver for the PSP controller at MMIO base 0xBD800000.
 * NO firmware syscall / sceUsbbd.  The register map, the clock/reset/activate
 * init order, the endpoint doorbell/ownership semantics, the endpoint
 * descriptor table, and the EP0 enumeration mechanism were recovered CLEAN-ROOM by
 * disassembling Sony's decrypted 6.61 sceUsbBus_driver (usb.prx) and cross-
 * checked against the psdevwiki hardware-register docs.  No SDK/driver source
 * was read; the descriptors, enumeration state machine, and byte-pipe are ours.
 *
 * Register facts (usb.prx seg0 vaddrs cited inline):
 *   per-EP block, stride 0x20, IN(send) region +0x000.., OUT(recv) region +0x200..
 *     +0x00 control/doorbell (bit3 starts a queued ordinary descriptor);
 *           non-control config-time is (type<<4)|0x102 for IN and
 *           (type<<4)|0x80 for OUT
 *     +0x08 maxpkt words, +0x0C maxpkt bytes, +0x14 ordinary DMA-descriptor
 *           pointer (physical low 29 bits); EP0-OUT +0x10 is the dedicated
 *           SETUP-mailbox descriptor pointer
 *   global: +0x400 activation selector (Sony emits A0/A1/A8/A9 according to
 *     charging mode and descriptor-set availability), +0x404 mode(0x610/0x210),
 *     +0x408 device status, +0x40C int ack, +0x410 int enable,
 *     +0x418 disabled-EP bitmask (1=disabled; low 16 bits are IN endpoints,
 *           high 16 bits are OUT endpoints),
 *     +0x41C reset, +0x504.. endpoint descriptor table (not device address)
 *   canonical gates via sysreg 0xBC100000: +0x4C reset(bit6),
 *     +0x50 bus clock(bit9), +0x54 functional clock field(bits7:4),
 *     +0x78 I/O enable(bit2); retail also enables +0x74 bit8 after the
 *     initial controller-global block and before switching CONFIG to 0x210
 *
 * HARDWARE STATUS: enumeration, both bulk endpoints, and the unchanged kosload
 * serial protocol are proven end to end on a PSP-1000.  Direct checks also
 * resolved the direction-specific endpoint config and enable-mask mapping,
 * including EP0's special pre-connect state.  A second RE pass resolved the 16-byte DMA
 * descriptor itself: word0 carries a 16-bit length, bit27 arms it, and the top
 * nibble is hardware state; word2 carries the data-buffer physical address.
 * The endpoint +0x14 register receives the descriptor address, not the data
 * address.  Type/enable is latched separately, SET_ADDRESS is controller-owned,
 * and the control STATUS phase is a zero-length descriptor on the opposite
 * EP0 direction.
 * =============================================================================
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "usb_dev.h"
#include "cache.h"
#include "hw_trace.h"
#include <kosload/protocol.h>

/* ===== Software byte-pipe rings (serial_io.c sits on these) ===== */

#define RING_SZ 4096
#define RING_MASK (RING_SZ - 1)

static volatile uint8_t rx_ring[RING_SZ];
static volatile uint32_t rx_head, rx_tail;
static uint8_t tx_ring[RING_SZ];
static volatile uint32_t tx_head, tx_tail;
static bool initialized;

static int usb_hw_init_ctrl(void);
static void usb_ep0_poll(void);
static void usb_hw_rx(void);
static void usb_hw_tx(void);
static void usb_diag_accumulate(void);
static void usb_global_events(void);
static void usb_ep0_reconfigure(void);
static void usb_apply_configuration(uint32_t value);

/* Breadcrumb for the NMI crash screen (exception.c prints STEP/ADDR/VALUE).
 * USB bring-up pokes registers that can hang or fault the bus, and an NMI
 * lands a few instructions late, so the last operation to start is the only
 * reliable way to name the one that died.  Bit31 marks "started, trailing sync
 * not reached"; it is cleared once the access retires.
 *
 * Init and EP0 reconfiguration only -- never the bulk data path -- so the
 * paired syncs cost nothing that matters.  noinline because there are ~58 call
 * sites and GCC otherwise expands the stores at each one, for 1.7 KB of
 * duplicated breadcrumb code. */
__attribute__((noinline))
static void usb_diag_begin(uint32_t step, uint32_t addr, uint32_t value) {
    volatile struct psp_hw_trace *trace = psp_hw_trace_uncached();
    trace->addr = addr;
    trace->value = value;
    __asm__ volatile("sync" ::: "memory");
    trace->step = step | 0x80000000u;
    __asm__ volatile("sync" ::: "memory");
}

__attribute__((noinline))
static void usb_diag_end(uint32_t step) {
    volatile struct psp_hw_trace *trace = psp_hw_trace_uncached();
    __asm__ volatile("sync" ::: "memory");
    trace->step = step;
    __asm__ volatile("sync" ::: "memory");
}

static uint32_t usb_diag_read32(uint32_t step, uint32_t addr) {
    uint32_t value;

    usb_diag_begin(step, addr, 0xFFFFFFFFu);
    value = *(volatile uint32_t *)addr;
    psp_hw_trace_uncached()->value = value;
    usb_diag_end(step);
    return value;
}

static void usb_diag_write32(uint32_t step, uint32_t addr,
                                    uint32_t value) {
    usb_diag_begin(step, addr, value);
    *(volatile uint32_t *)addr = value;
    usb_diag_end(step);
}

/* ===== Public byte pipe ===== */

int usb_dev_init(void) {
    volatile struct psp_hw_trace *trace = psp_hw_trace_uncached();
    int result;

    trace->nmi_pre = usb_diag_read32(0x55000001u, 0xBC100000u);
    usb_diag_begin(0x55000006u, 0, 0);
    rx_head = rx_tail = 0;
    tx_head = tx_tail = 0;
    usb_diag_end(0x55000006u);
    usb_diag_begin(0x55000007u, 0, 0);
    result = usb_hw_init_ctrl();
    if(result != USB_DEV_INIT_OK)
        return result;
    usb_diag_end(0x55000007u);
    initialized = true;
    usb_diag_begin(0x55000008u, 0, 0);
    usb_diag_end(0x55000008u);
    return 0;
}

void usb_dev_poll(void) {
    if(!initialized)
        return;
    usb_diag_accumulate(); /* latch transient events before EP0 W1Cs them */
    usb_global_events(); /* bus reset / enum rebuild before any EP0 work */
    usb_ep0_poll();  /* service enumeration */
    usb_hw_rx();
    usb_hw_tx();
}

bool usb_dev_rx_ready(void) {
    usb_dev_poll();
    return rx_head != rx_tail;
}

unsigned char usb_dev_rx_byte(void) {
    while(rx_head == rx_tail)
        usb_dev_poll();
    unsigned char c = rx_ring[rx_tail & RING_MASK];
    rx_tail++;
    return c;
}

void usb_dev_tx_byte(unsigned char c) {
    while((uint32_t)(tx_head - tx_tail) >= RING_SZ)
        usb_dev_poll();
    tx_ring[tx_head & RING_MASK] = c;
    tx_head++;
    if((uint32_t)(tx_head - tx_tail) >= KOSLOAD_USB_MAXPKT)
        usb_hw_tx();
}

void usb_dev_tx_flush(void) {
    usb_hw_tx();
}

/* =========================================================================
 * Hardware layer — register map from usb.prx RE (see PROVENANCE).
 * =========================================================================*/

#define USB_BASE            0xBD800000u
#define USB(off)            (*(volatile uint32_t *)(USB_BASE + (uint32_t)(off)))
#define SYSREG_BASE         0xBC100000u
#define SYSREG(off)         (*(volatile uint32_t *)(SYSREG_BASE + (uint32_t)(off)))

/* Per-endpoint register block. */
#define EP_IN(n)            ((uint32_t)(n) * 0x20u)          /* sending (IN) region */
#define EP_OUT(n)           (0x200u + (uint32_t)(n) * 0x20u) /* receiving (OUT) region */
#define EP_CTRL             0x00u
#define EP_STATUS           0x04u
#define EP_MAXPKT_WORDS     0x08u
#define EP_MAXPKT_BYTES     0x0Cu
#define EP_SETUP_MAILBOX    0x10u /* special only at EP0-OUT (+0x210) */
#define EP_DESC_SUBMIT      0x14u

/* EP control/doorbell bits. */
#define EP_ARM              0x08u        /* endpoint bit3: poll/start queued IN desc */
#define EP_FLUSH            0x02u        /* EP0 IN FIFO flush */
#define EP_NAK              0x040u       /* endpoint is currently NAKing */
#define EP_IN_CONFIG        0x102u       /* retail non-control IN base flags */
#define EP_OUT_CONFIG       0x080u       /* retail non-control OUT base flags */
#define EP_STALL            0x001u       /* endpoint bit0: stall (RE below) */
#define EP_SNAK             0x080u       /* set NAK */
#define EP_CNAK             0x100u       /* clear NAK / expose endpoint */

/* Endpoint-status events.  They gate descriptor inspection in retail 6.61. */
#define EP_STATUS_OUT_DATA  0x010u
#define EP_STATUS_OUT_SETUP 0x020u
#define EP_STATUS_IN_TOKEN  0x040u
#define EP_STATUS_IN_TDC    0x400u
#define EP_STATUS_OUT_W1C   0x2B0u
#define EP_STATUS_IN_W1C    0x6C0u

/* 16-byte ordinary/setup DMA descriptor.  Retail usb.prx text+0x6670 clears
 * word0's top nibble, sets bit27, stores length in its low halfword, stores the
 * data physical address at +8, then writes PHYS(desc) to endpoint +0x14.
 * Hardware reports completion as top-nibble 8 and the actual byte count in the
 * low halfword.  The dedicated SETUP descriptor instead receives its eight
 * request bytes directly in word2/word3. */
struct usb_dma_desc {
    uint32_t control;
    uint32_t reserved1;
    uint32_t data;
    uint32_t reserved3;
};

/* A descriptor must own its complete D-cache line.  Merely aligning a 16-byte
 * object is insufficient: crt0 clears BSS through the cached alias, and a
 * later cached store to a neighbour in the same line could overwrite DMA state
 * maintained through the uncached alias. */
struct usb_dma_slot {
    struct usb_dma_desc desc;
    uint8_t padding[64 - sizeof(struct usb_dma_desc)];
};

#define DESC_OWNER_MASK     0xC0000000u
#define DESC_OWNER_DONE     0x80000000u
#define DESC_RESULT_MASK    0x30000000u
#define DESC_TOP_MASK       (DESC_OWNER_MASK | DESC_RESULT_MASK)
#define DESC_ARM            0x08000000u

/* Global registers. */
#define USB_ACTIVATE        0x400u
#define USB_CONFIG          0x404u
#define USB_INTSTAT         0x408u
#define USB_INTACK          0x40Cu
#define USB_INTEN           0x410u
#define USB_INTEN2          0x414u
#define USB_EP_DISABLE      0x418u
#define USB_RESET           0x41Cu
#define USB_EP_CONFIG(n)    (0x504u + (uint32_t)(n) * 4u)
#define USB_CONFIG_RDE      0x004u /* receive-DMA enable; HW clears per OUT event */
#define USB_CONFIG_DMA_EN   0x00Cu /* receive+transmit DMA enable (retail |0xC) */

/* Global events latched in +0x40C.  Retail's dispatcher (usb.prx text+0x6CF0)
 * tests these one at a time and W1Cs each with its own exact literal. */
#define USB_GEV_SET_CONFIG  0x01u  /* SET_CONFIGURATION, decoded by the core   */
#define USB_GEV_SET_INTF    0x02u  /* SET_INTERFACE, decoded by the core       */
#define USB_GEV_RESET       0x08u  /* bus reset: everything below is lost      */
#define USB_GEV_SUSPEND     0x10u  /* suspend/link teardown; needs a teardown  */
#define USB_GEV_ENUM        0x40u  /* enumeration done, speed latched in +0x408 */
#define USB_SPEED_MASK      0x6000u /* +0x408 speed field read on enum         */
/* +0x408 also carries the values for the requests the core answers by itself:
 * configuration in bits 3:0, interface in 7:4, alternate setting in 11:8
 * (retail text+0x6FA0 and text+0x6F24). */
#define USB_STS_CFG_MASK    0x000Fu
#define USB_INTEN_RUN       0xFFFFFFA4u /* retail mask after (re)configuration */

/* A wedged controller could re-latch an event for as long as the host holds the
 * bus in reset, so servicing is capped.  The cap is deliberately far above a
 * healthy enumeration's two or three resets: hitting it is itself the finding,
 * and the diagnostic's master deadline already bounds the run.
 *
 * The budget is spent per enumeration attempt, NOT over the loader's lifetime.
 * Reaching the configured state proves the path works, so usb_apply_configuration()
 * refunds it.  Counting for the lifetime instead would leave the events latched
 * and permanently stop the device re-enumerating after a few hundred cumulative
 * resets -- recoverable only by a power cycle. */
#define USB_GEV_MAX_SERVICE 256u

/* Public sceUsbActivate selects the non-charging A8 base.  Sony adds bit0 only
 * when its aggregated high-speed descriptor pointer is absent; our USB 2.0
 * personality has the corresponding high-speed/512-byte endpoint set. */
#define USB_ACTIVATE_ON     0xA8u

/* Canonical USB sysreg clock/reset controls (retail 6.61 lowio.prx). */
#define SYSREG_RESET_ENABLE 0x4Cu
#define SYSREG_BUSCLK       0x50u
#define SYSREG_CLK1         0x54u
#define SYSREG_AUX_IO       0x74u
#define SYSREG_IO_ENABLE    0x78u
#define SYSREG_USB_INTR     0x80u
#define SYSREG_USB_RESET    0x00000040u
#define SYSREG_USB_BUSCLK   0x00000200u
#define SYSREG_USB_CLK      0x00000090u /* sceSysregUsbClkEnable(9) */
#define SYSREG_USB_AUX_IO   0x00000100u
#define SYSREG_USB_IO       0x00000004u
#define SYSREG_USB_INTR_MASK 0x0000000Fu
#define SYSREG_USB_READY     0x00000004u /* QueryIntr mask; raw +0x80 bit3 */
#define SYSREG_USB_READY_RAW (SYSREG_USB_READY << 1)

/* Retail 01g schedules controller base init 250,000 us after acquiring the
 * reset-ready event.  Count is inherited from firmware, so use the same
 * deliberately conservative calibration already proven by the Syscon path:
 * 0x00080000 is about 4 ms; x64 remains sub-second and at/above the Sony wait. */
#define USB_READY_DRAIN_TIMEOUT_TICKS 0x00200000u
#define USB_READY_EVENT_TIMEOUT_TICKS 0x02000000u
#define USB_01G_SETTLE_TICKS          0x02000000u

/* Endpoint indices: EP0 = control, EP1 = the bulk pair. */
#define EP0                 0
#define EP_BULK             1

#define PHYS(p)             ((uint32_t)(p) & 0x1FFFFFFFu)
#define UNCACHED(p)         ((volatile void *)((((uint32_t)(p)) & 0x1FFFFFFFu) | 0x40000000u))

static inline void usb_sync(void) { __asm__ volatile("sync" ::: "memory"); }

static inline uint32_t usb_count(void) {
    uint32_t count;
    __asm__ volatile("mfc0 %0, $9" : "=r"(count));
    return count;
}

static uint32_t usb_sysreg_query_intr(uint32_t raw) {
    return (raw >> 1) & SYSREG_USB_INTR_MASK;
}

/* DMA descriptors/buffers are each cache-line isolated, handed from the cached
 * BSS-clearing domain to the uncached/DMA domain once at controller init, and
 * never accessed through their cached aliases afterward. */
static __attribute__((aligned(64))) struct usb_dma_slot setup_slot;
static __attribute__((aligned(64))) struct usb_dma_slot ep0_in_slot;
static __attribute__((aligned(64))) struct usb_dma_slot ep0_out_slot;
static __attribute__((aligned(64))) struct usb_dma_slot bulk_in_slot;
static __attribute__((aligned(64))) struct usb_dma_slot bulk_out_slot;
static __attribute__((aligned(64))) uint8_t ep0_buf[64];
/* Retail gives the zero-length EP0 status-OUT descriptor its own dummy DMA
 * buffer.  Keep that ownership separate from the simultaneously active IN
 * data buffer even though a correct zero-length transfer cannot touch it. */
static __attribute__((aligned(64))) uint8_t ep0_status_buf[64];
static __attribute__((aligned(64))) uint8_t bulk_in_buf[KOSLOAD_USB_MAXPKT];
static __attribute__((aligned(64))) uint8_t bulk_out_buf[KOSLOAD_USB_MAXPKT];

#define SETUP_DESC   (&setup_slot.desc)
#define EP0_IN_DESC  (&ep0_in_slot.desc)
#define EP0_OUT_DESC (&ep0_out_slot.desc)
#define BULK_IN_DESC (&bulk_in_slot.desc)
#define BULK_OUT_DESC (&bulk_out_slot.desc)

static bool configured;
static bool bulk_out_armed;
static bool bulk_in_busy;
/* RESET/ENUM events serviced since the device was last configured.  Distinct
 * from diag_reset_count/diag_enum_count, which are lifetime totals reported in
 * the diagnostic snapshot and must keep accumulating. */
static uint32_t gev_service_used;
static uint32_t diag_events;
static uint32_t diag_error;
static uint32_t diag_setup_hi;
static uint32_t diag_setup_lo;
static uint32_t diag_setup_control;
static uint32_t diag_in_control;
static uint32_t diag_out_control;
static uint32_t diag_in_status;
static uint32_t diag_out_status;
static uint32_t ep0_in_expected;
static bool diag_track_first_data;

/* "Ever observed" accumulators.  A polled driver with interrupts disabled only
 * samples the controller between redraws, so a bus-reset or enum/speed event
 * that hardware raises and a later access clears would otherwise be invisible.
 * These are pure OR accumulators: they never write back to the controller. */
static uint32_t diag_sticky_sysreg;      /* BC100080 raw NMI aggregate */
static uint32_t diag_sticky_intstat;     /* BD800408 device status     */
static uint32_t diag_sticky_intack;      /* BD80040C global events     */
static uint32_t diag_sticky_setup_ctrl;  /* SETUP descriptor control   */
static uint32_t diag_sticky_out_status;  /* EP0-OUT endpoint status    */

/* Global-event bookkeeping.  first_reset_phase answers the question this run
 * exists to settle: did the host's bus reset arrive before the SETUP (normal
 * enumeration) or after our data stage (the host abandoning the transfer)? */
static uint32_t usb_speed;               /* 1 = +0x408 speed bits set, else 2 */
static uint32_t diag_reset_count;
static uint32_t diag_enum_count;
static uint32_t diag_first_reset_phase = 0xFFu;
static uint32_t diag_setup_count;
static uint32_t diag_req_hist0;
static uint32_t diag_req_hist1;
static uint32_t diag_last_setup_hi;
static uint32_t diag_last_setup_lo;
static uint32_t diag_setup_preempts;
static uint32_t diag_stall_count;
static uint32_t diag_set_config_count;
static uint32_t diag_set_intf_count;
static uint32_t diag_config_value;
static uint32_t diag_bulk_out_pkts;
static uint32_t diag_bulk_out_bytes;
static uint32_t diag_bulk_in_pkts;
static uint32_t diag_bulk_in_bytes;
static uint32_t diag_bulk_out_fail;

enum ep0_phase {
    EP0_WAIT_SETUP,
    EP0_WAIT_IN_TOKEN,
    EP0_WAIT_IN_DATA,
    EP0_WAIT_OUT_STATUS
};

static enum ep0_phase ep0_phase;

static uint32_t pack_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void diag_fail(uint32_t error) {
    if(!(diag_events & USB_DEV_DIAG_ERROR)) {
        diag_error = error;
        diag_events |= USB_DEV_DIAG_ERROR;
    }
}

/* ===== USB descriptors (ours; VID/PID from protocol.h) ===== */

static const uint8_t dev_desc[18] = {
    18, 0x01,                          /* bLength, DEVICE */
    0x01, 0x02,                        /* bcdUSB 2.01 */
    0xFF, 0x00, 0x00,                  /* vendor class */
    64,                                /* bMaxPacketSize0 */
    KOSLOAD_USB_VID & 0xFF, KOSLOAD_USB_VID >> 8,
    KOSLOAD_USB_PID & 0xFF, KOSLOAD_USB_PID >> 8,
    0x01, 0x01,                        /* bcdDevice 1.01 */
    0, 0, 0,                           /* iManufacturer/iProduct/iSerial */
    1                                  /* bNumConfigurations */
};

/* A device that chirps high speed must answer GET_DESCRIPTOR(DEVICE_QUALIFIER)
 * rather than return a short packet; hosts treat a malformed answer here as a
 * broken device and abandon enumeration.  It mirrors the device descriptor. */
static const uint8_t qualifier_desc[10] = {
    10, 0x06,                          /* bLength, DEVICE_QUALIFIER */
    0x01, 0x02,                        /* bcdUSB 2.01, matching dev_desc */
    0xFF, 0x00, 0x00,                  /* vendor class */
    64,                                /* bMaxPacketSize0 */
    1,                                 /* bNumConfigurations */
    0                                  /* bReserved */
};

/* ===== Microsoft OS 2.0 (WCID) descriptors =====
 *
 * Without these Windows leaves this vendor-class device unbound and -t usb
 * needs a manual Zadig/WinUSB install; Linux and macOS need no setup.
 *
 * Two steps: bcdUSB >= 0x0201 makes Windows fetch the BOS, whose platform
 * capability names a vendor request code; Windows then issues that request to
 * read the set below, whose "WINUSB" compatible ID loads the driver.  This is
 * Windows 8.1+; Windows 7 would need the older 0xEE-string mechanism.
 *
 * Nothing here may exceed the 64-byte ep0_buf: a DeviceInterfaceGUIDs
 * REG_PROPERTY (~132 bytes) would, and libusb does not need one. */

#define MSOS2_VENDOR_CODE          0x20u  /* ours to choose; advertised in the BOS */
#define MSOS2_INDEX_DESCRIPTOR_SET 7u     /* wIndex Windows sends with it */
#define MSOS2_DESC_SET_LEN         30u

static const uint8_t msos2_desc[MSOS2_DESC_SET_LEN] = {
    /* --- descriptor set header (10 bytes) --- */
    0x0A, 0x00,                        /* wLength 10 */
    0x00, 0x00,                        /* MS_OS_20_SET_HEADER_DESCRIPTOR */
    0x00, 0x00, 0x03, 0x06,            /* dwWindowsVersion 0x06030000 (8.1) */
    MSOS2_DESC_SET_LEN, 0x00,          /* wTotalLength */

    /* --- compatible ID (20 bytes) ---
     * Placed directly in the set header's scope, which applies it to the
     * device: correct here because we expose exactly one configuration with
     * one interface.  A composite device would need a function subset. */
    0x14, 0x00,                        /* wLength 20 */
    0x03, 0x00,                        /* MS_OS_20_FEATURE_COMPATIBLE_ID */
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,        /* CompatibleID */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00   /* SubCompatibleID */
};

static const uint8_t bos_desc[33] = {
    0x05, 0x0F,                        /* bLength 5, BOS */
    33, 0x00,                          /* wTotalLength */
    0x01,                              /* bNumDeviceCaps */

    0x1C, 0x10, 0x05, 0x00,            /* bLength 28, DEVICE_CAPABILITY,
                                        * PLATFORM, bReserved */
    /* MS OS 2.0 platform capability UUID {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F},
     * in the wire order the spec requires: first three fields little-endian,
     * last two big-endian. */
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,

    0x00, 0x00, 0x03, 0x06,            /* dwWindowsVersion 0x06030000 */
    MSOS2_DESC_SET_LEN, 0x00,          /* wMSOSDescriptorSetTotalLength */
    MSOS2_VENDOR_CODE,                 /* bMS_VendorCode */
    0x00                               /* bAltEnumCode (0 = no alt enumeration) */
};

/* ep0_send() memcpys into ep0_buf unchecked, so nothing we can be asked for may
 * exceed it.  Both new descriptors are well under 64 bytes; keep it that way. */
_Static_assert(sizeof(bos_desc) <= sizeof(ep0_buf),
               "BOS descriptor does not fit the EP0 buffer");
_Static_assert(sizeof(msos2_desc) <= sizeof(ep0_buf),
               "MS OS 2.0 descriptor set does not fit the EP0 buffer");

/* Bulk wMaxPacketSize for the speed the controller latched.  High speed
 * mandates exactly 512 for bulk endpoints; full speed allows at most 64.
 *
 * usb_speed is 1 when the +0x408 speed bits are set, which is the state a bus
 * reset installs before high-speed chirp detection has run -- i.e. full speed
 * -- and 2 once the controller reports high speed.  Advertising 512 at full
 * speed is illegal and breaks the pipe on a USB 1.1 port or hub. */
static uint32_t usb_bulk_maxpkt(void) {
    return usb_speed == 1 ? 64u : (uint32_t)KOSLOAD_USB_MAXPKT;
}

/* Not const: the two wMaxPacketSize fields are rewritten by
 * usb_refresh_cfg_desc() whenever the latched speed changes. */
static uint8_t cfg_desc[32] = {
    /* configuration */
    9, 0x02, 32, 0, 1, 1, 0, 0xC0, 0,
    /* interface: vendor class, 2 endpoints */
    9, 0x04, 0, 0, 2, 0xFF, 0, 0, 0,
    /* endpoint IN (bulk) */
    7, 0x05, KOSLOAD_USB_EP_IN, 0x02,
    KOSLOAD_USB_MAXPKT & 0xFF, KOSLOAD_USB_MAXPKT >> 8, 0,
    /* endpoint OUT (bulk) */
    7, 0x05, KOSLOAD_USB_EP_OUT, 0x02,
    KOSLOAD_USB_MAXPKT & 0xFF, KOSLOAD_USB_MAXPKT >> 8, 0
};

/* Keep the configuration descriptor's endpoint sizes in step with the latched
 * speed.  Offsets 22/23 and 29/30 are the IN and OUT wMaxPacketSize fields. */
static void usb_refresh_cfg_desc(void) {
    uint32_t mp = usb_bulk_maxpkt();

    cfg_desc[22] = (uint8_t)(mp & 0xFF);
    cfg_desc[23] = (uint8_t)(mp >> 8);
    cfg_desc[29] = (uint8_t)(mp & 0xFF);
    cfg_desc[30] = (uint8_t)(mp >> 8);
}

/* ===== Endpoint primitives ===== */

enum ep_direction {
    EP_DIR_IN,
    EP_DIR_OUT
};

static uint32_t ep_offset(enum ep_direction dir, uint32_t ep_index) {
    return dir == EP_DIR_IN ? EP_IN(ep_index) : EP_OUT(ep_index);
}

/* Non-control endpoints use different config bases in each direction.  Retail
 * usb.prx text+0x39c4 writes (type<<4)|0x102 to IN and (type<<4)|0x80 to OUT. */
static void ep_config_data(enum ep_direction dir, uint32_t ep_index,
                           uint32_t type, uint32_t maxpkt) {
    uint32_t ep_off = ep_offset(dir, ep_index);

    USB(ep_off + EP_CTRL) = (type << 4) |
                            (dir == EP_DIR_IN ? EP_IN_CONFIG : EP_OUT_CONFIG);
    /* +0x08 is IN FIFO size, but OUT receive-frame number. */
    if(dir == EP_DIR_IN)
        USB(ep_off + EP_MAXPKT_WORDS) = maxpkt >> 2;
    USB(ep_off + EP_MAXPKT_BYTES) = maxpkt;
    USB(ep_off + EP_STATUS) = USB(ep_off + EP_STATUS); /* write-back-to-clear */
    usb_sync();
}

/* EP0 is special.  Retail text+0x5f40/+0x5f6c first writes IN=2 and OUT=0,
 * then marks OUT active before posting the SETUP mailbox.  EP0-IN is exposed
 * lazily by the normal IN-start path when a response is actually submitted. */
static void ep0_config_preconnect(uint32_t maxpkt) {
    uint32_t status;

    usb_diag_write32(0x55000601u, USB_BASE + EP_IN(EP0) + EP_CTRL, 2);
    usb_diag_write32(0x55000602u,
                     USB_BASE + EP_IN(EP0) + EP_MAXPKT_WORDS,
                     maxpkt >> 2);
    usb_diag_write32(0x55000603u,
                     USB_BASE + EP_IN(EP0) + EP_MAXPKT_BYTES, maxpkt);
    status = usb_diag_read32(0x55000604u,
                             USB_BASE + EP_IN(EP0) + EP_STATUS);
    usb_diag_write32(0x55000605u,
                     USB_BASE + EP_IN(EP0) + EP_STATUS, status);

    usb_diag_write32(0x55000606u, USB_BASE + EP_OUT(EP0) + EP_CTRL, 0);
    usb_diag_write32(0x55000607u,
                     USB_BASE + EP_OUT(EP0) + EP_MAXPKT_BYTES, maxpkt);
    status = usb_diag_read32(0x55000608u,
                             USB_BASE + EP_OUT(EP0) + EP_STATUS);
    usb_diag_write32(0x55000609u,
                     USB_BASE + EP_OUT(EP0) + EP_STATUS, status);
}

static volatile struct usb_dma_desc *desc_uncached(struct usb_dma_desc *desc) {
    return (volatile struct usb_dma_desc *)UNCACHED(desc);
}

static void desc_clear(struct usb_dma_desc *desc) {
    volatile struct usb_dma_desc *d = desc_uncached(desc);
    d->control = DESC_OWNER_MASK | DESC_ARM;
    d->reserved1 = 0;
    d->data = 0;
    d->reserved3 = PHYS(desc); /* retail initializes ordinary descriptors self-linked */
}

static void setup_desc_clear(void) {
    volatile struct usb_dma_desc *d = desc_uncached(SETUP_DESC);
    d->control = 0;
    d->reserved1 = 0;
    d->data = 0;
    d->reserved3 = 0; /* setup bytes are written directly across word2/word3 */
}

static void desc_prepare(struct usb_dma_desc *desc, const void *buf,
                         uint32_t initial_len) {
    volatile struct usb_dma_desc *d = desc_uncached(desc);
    volatile uint16_t *length = (volatile uint16_t *)&d->control;

    uint32_t control = d->control & ~DESC_TOP_MASK;
    d->control = control | DESC_ARM;
    *length = (uint16_t)initial_len;
    d->data = PHYS(buf);
    usb_sync();
}

/* Retail IN submit (usb.prx text+0x65f8): descriptor low16 starts as the
 * requested byte count, +0x14 receives PHYS(desc), then CTRL bit3 is rung. */
static void ep_submit_in(uint32_t ep_off, struct usb_dma_desc *desc,
                         const void *buf, uint32_t len) {
    desc_prepare(desc, buf, len);

    USB(ep_off + EP_DESC_SUBMIT) = PHYS(desc);
    usb_sync();
    USB(ep_off + EP_CTRL) |= EP_ARM;
    usb_sync();
}

/* Retail OUT submit (text+0x4930): low16 starts at zero and hardware replaces
 * it with the actual received count.  OUT has no bit3 doorbell; exposing the
 * direction with CTRL bit0x100 is the separate start operation. */
static void ep_post_out(uint32_t ep_off, struct usb_dma_desc *desc,
                        const void *buf) {
    desc_prepare(desc, buf, 0);
    USB(ep_off + EP_DESC_SUBMIT) = PHYS(desc);
    usb_sync();
}

static void ep_release_out(uint32_t ep_off) {
    USB(ep_off + EP_CTRL) |= EP_CNAK;
    usb_sync();
}

/* The controller clears DEVCTL.RDE after consuming receive work.  Sony's OUT
 * worker handles/queues the event first, then fresh-RMWs this bit and syncs
 * (retail usb.prx text+0x5174..+0x5190). */
static void usb_receive_dma_enable(void) {
    USB(USB_CONFIG) = USB(USB_CONFIG) | USB_CONFIG_RDE;
    usb_sync();
}

static void ep_submit_out(uint32_t ep_off, struct usb_dma_desc *desc,
                          const void *buf) {
    ep_post_out(ep_off, desc, buf);
    ep_release_out(ep_off);
    usb_receive_dma_enable();
}

static bool desc_finished(struct usb_dma_desc *desc) {
    return (desc_uncached(desc)->control & DESC_OWNER_MASK) == DESC_OWNER_DONE;
}

static bool desc_succeeded(struct usb_dma_desc *desc) {
    return (desc_uncached(desc)->control & DESC_TOP_MASK) == DESC_OWNER_DONE;
}

static uint32_t desc_xfer_len(struct usb_dma_desc *desc) {
    return desc_uncached(desc)->control & 0xFFFFu;
}

static void desc_reclaim(struct usb_dma_desc *desc) {
    /* Sony marks an inspected descriptor software/free with owner 3. */
    desc_uncached(desc)->control |= DESC_OWNER_MASK;
    usb_sync();
}

/* The SETUP mailbox is a dedicated descriptor at EP0-OUT +0x10.  Hardware
 * writes the eight-byte request into descriptor words 2/3 and sets state 8.
 * Clearing the top nibble re-arms the persistent mailbox. */
static void ep0_setup_rearm(void) {
    volatile struct usb_dma_desc *d = desc_uncached(SETUP_DESC);
    d->control &= ~DESC_TOP_MASK;
    usb_sync();
}

static bool ep0_setup_done(void) {
    /* The dedicated setup consumer requires the complete top nibble == 8. */
    return (desc_uncached(SETUP_DESC)->control & DESC_TOP_MASK) ==
           DESC_OWNER_DONE;
}

static void ep_ack_event(uint32_t ep_off, uint32_t status,
                         uint32_t writable_events, uint32_t event) {
    /* Retail preserves the non-event fields and writes one W1C event at a
     * time, so a concurrent SETUP cannot be erased with an OUT-DATA ACK. */
    USB(ep_off + EP_STATUS) = (status & ~writable_events) | event;
    usb_sync();
}

static void ep_enable(enum ep_direction dir, uint32_t ep_index, bool enable) {
    /* Retail +0x3ae8 and +0x4200: low half = IN, high half = OUT. */
    uint32_t m = USB(USB_EP_DISABLE);
    uint32_t bit = 1u << (ep_index + (dir == EP_DIR_OUT ? 16u : 0u));

    /* The IN-start path writes the endpoint bit to +0x414 before exposing it. */
    if(enable && dir == EP_DIR_IN)
        USB(USB_INTEN2) = 1u << ep_index;
    if(enable)
        m &= ~bit;
    else
        m |= bit;
    USB(USB_EP_DISABLE) = m;
    usb_sync();
}

/* ===== Controller bring-up (RE init order) ===== */

static int usb_hw_init_ctrl(void) {
    volatile struct psp_hw_trace *trace = psp_hw_trace_uncached();

    /* crt0 cleared BSS through cached addresses.  Transfer exclusive ownership
     * of every DMA cache line before its first uncached access or publication;
     * otherwise a later eviction of those dirty zeros could corrupt DMA state. */
    usb_diag_begin(0x55000101u, (uint32_t)&setup_slot, sizeof(setup_slot));
    cache_flush_range(&setup_slot, sizeof(setup_slot));
    usb_diag_end(0x55000101u);
    usb_diag_begin(0x55000102u, (uint32_t)&ep0_in_slot, sizeof(ep0_in_slot));
    cache_flush_range(&ep0_in_slot, sizeof(ep0_in_slot));
    usb_diag_end(0x55000102u);
    usb_diag_begin(0x55000103u, (uint32_t)&ep0_out_slot, sizeof(ep0_out_slot));
    cache_flush_range(&ep0_out_slot, sizeof(ep0_out_slot));
    usb_diag_end(0x55000103u);
    usb_diag_begin(0x55000104u, (uint32_t)&bulk_in_slot, sizeof(bulk_in_slot));
    cache_flush_range(&bulk_in_slot, sizeof(bulk_in_slot));
    usb_diag_end(0x55000104u);
    usb_diag_begin(0x55000105u, (uint32_t)&bulk_out_slot, sizeof(bulk_out_slot));
    cache_flush_range(&bulk_out_slot, sizeof(bulk_out_slot));
    usb_diag_end(0x55000105u);
    usb_diag_begin(0x55000106u, (uint32_t)ep0_buf, sizeof(ep0_buf));
    cache_flush_range(ep0_buf, sizeof(ep0_buf));
    usb_diag_end(0x55000106u);
    usb_diag_begin(0x55000107u, (uint32_t)ep0_status_buf, sizeof(ep0_status_buf));
    cache_flush_range(ep0_status_buf, sizeof(ep0_status_buf));
    usb_diag_end(0x55000107u);
    usb_diag_begin(0x55000108u, (uint32_t)bulk_in_buf, sizeof(bulk_in_buf));
    cache_flush_range(bulk_in_buf, sizeof(bulk_in_buf));
    usb_diag_end(0x55000108u);
    usb_diag_begin(0x55000109u, (uint32_t)bulk_out_buf, sizeof(bulk_out_buf));
    cache_flush_range(bulk_out_buf, sizeof(bulk_out_buf));
    usb_diag_end(0x55000109u);

    /* 1. Canonical USB clocks/reset via sysreg 0xBC100000.  The old C4/F0/B8
     * sequence was reconstructed from model-specific PHY and suspend paths;
     * those registers are not substitutes for these lowio USB controls.
     * Keep reset asserted while opening I/O and both clocks, then release it.
     * The 01g still requires Sony's reset-ready acquire and 250-ms deferred
     * init boundary before the first controller MMIO access. */
    uint32_t value = usb_diag_read32(0x55000211u,
                                     SYSREG_BASE + SYSREG_RESET_ENABLE);
    usb_diag_write32(0x55000212u, SYSREG_BASE + SYSREG_RESET_ENABLE,
                     value | SYSREG_USB_RESET);
    trace->post_gate_4c = usb_diag_read32(0x55000213u,
                                         SYSREG_BASE + SYSREG_RESET_ENABLE);

    value = usb_diag_read32(0x55000214u,
                            SYSREG_BASE + SYSREG_IO_ENABLE);
    usb_diag_write32(0x55000215u, SYSREG_BASE + SYSREG_IO_ENABLE,
                     value | SYSREG_USB_IO);
    trace->post_gate_78 = usb_diag_read32(0x55000216u,
                                         SYSREG_BASE + SYSREG_IO_ENABLE);

    /* Sony startup clears inherited USB sysreg causes while reset is still
     * asserted.  AcquireIntr(mask) writes only causes that were observed:
     * ((BC100080 >> 1) & mask) << 1.  Clearing all currently reported USB
     * causes here establishes a fresh edge for reset-ready below without
     * touching connect status or the unrelated MemoryStick bits. */
    trace->usb_80_preclear = usb_diag_read32(0x55000230u,
                                             SYSREG_BASE + SYSREG_USB_INTR);
    value = usb_sysreg_query_intr(trace->usb_80_preclear);
    if(value != 0)
        usb_diag_write32(0x55000231u,
                         SYSREG_BASE + SYSREG_USB_INTR, value << 1);
    trace->usb_80_after_clear = usb_diag_read32(
        0x55000232u, SYSREG_BASE + SYSREG_USB_INTR);
    if(trace->usb_80_after_clear & SYSREG_USB_READY_RAW) {
        usb_diag_begin(0x550002F2u, SYSREG_BASE + SYSREG_USB_INTR,
                       trace->usb_80_after_clear);
        usb_diag_end(0x550002F2u);
        return USB_DEV_INIT_STALE_ACK_ERROR;
    }

    value = usb_diag_read32(0x55000217u, SYSREG_BASE + SYSREG_BUSCLK);
    usb_diag_write32(0x55000218u, SYSREG_BASE + SYSREG_BUSCLK,
                     value | SYSREG_USB_BUSCLK);
    trace->post_gate_50 = usb_diag_read32(0x55000219u,
                                         SYSREG_BASE + SYSREG_BUSCLK);

    value = usb_diag_read32(0x5500021Au, SYSREG_BASE + SYSREG_CLK1);
    usb_diag_write32(0x5500021Bu, SYSREG_BASE + SYSREG_CLK1,
                     value | SYSREG_USB_CLK);
    trace->post_gate_54 = usb_diag_read32(0x5500021Cu,
                                         SYSREG_BASE + SYSREG_CLK1);

    value = usb_diag_read32(0x5500021Du,
                            SYSREG_BASE + SYSREG_RESET_ENABLE);
    usb_diag_write32(0x5500021Eu, SYSREG_BASE + SYSREG_RESET_ENABLE,
                     value & ~SYSREG_USB_RESET);
    trace->post_gate_4c = usb_diag_read32(0x5500021Fu,
                                         SYSREG_BASE + SYSREG_RESET_ENABLE);
    usb_sync();

    /* A failed gate write makes every BD800xxx access unsafe.  Fail before
     * activation and preserve a bitmask in VALUE for the diagnostic screen:
     * bit0 reset held, bit1 I/O off, bit2 bus clock off, bit3 clock mask off. */
    value = 0;
    if(trace->post_gate_4c & SYSREG_USB_RESET)
        value |= 1u << 0;
    if(!(trace->post_gate_78 & SYSREG_USB_IO))
        value |= 1u << 1;
    if(!(trace->post_gate_50 & SYSREG_USB_BUSCLK))
        value |= 1u << 2;
    if((trace->post_gate_54 & SYSREG_USB_CLK) != SYSREG_USB_CLK)
        value |= 1u << 3;
    if(value != 0) {
        usb_diag_begin(0x550002FFu, 0, value);
        usb_diag_end(0x550002FFu);
        return USB_DEV_INIT_GATE_ERROR;
    }

    /* Reset release is asynchronous on the PSP-1000.  Retail first drains
     * any cause 4 already pending after ResetDisable, then arms IRQ57.  A
     * subsequent cause 4 enters the IRQ57 handler, which acquires that fresh
     * cause and defers controller base init by 250,000 us on old Tachyon.
     * Poll the same two phases with bounded waits because CPU IE stays off.
     * Accessing BD800400 before this boundary raises source-9 NMI with the
     * physical target address in BC100014, as confirmed on hardware. */
    uint32_t start = usb_count();
    usb_diag_begin(0x55000233u, SYSREG_BASE + SYSREG_USB_INTR, 0);
    for(;;) {
        value = *(volatile uint32_t *)(SYSREG_BASE + SYSREG_USB_INTR);
        trace->value = value;
        if(!(value & SYSREG_USB_READY_RAW))
            break;

        /* AcquireIntr(4) is the literal W1C value 8.  Never write the full
         * R80 snapshot because bit0 is connect state and other bits belong
         * to independent causes. */
        *(volatile uint32_t *)(SYSREG_BASE + SYSREG_USB_INTR) =
            SYSREG_USB_READY_RAW;
        usb_sync();
        if((uint32_t)(usb_count() - start) >=
           USB_READY_DRAIN_TIMEOUT_TICKS) {
            usb_diag_end(0x55000233u);
            usb_diag_begin(0x550002F3u,
                           SYSREG_BASE + SYSREG_USB_INTR, value);
            usb_diag_end(0x550002F3u);
            return USB_DEV_INIT_READY_DRAIN_TIMEOUT;
        }
    }
    usb_diag_end(0x55000233u);

    /* Wait for the new cause 4 that retail would deliver to IRQ57. */
    start = usb_count();
    usb_diag_begin(0x55000234u, SYSREG_BASE + SYSREG_USB_INTR,
                   SYSREG_USB_READY_RAW);
    for(;;) {
        value = *(volatile uint32_t *)(SYSREG_BASE + SYSREG_USB_INTR);
        trace->value = value;
        if(value & SYSREG_USB_READY_RAW)
            break;
        if((uint32_t)(usb_count() - start) >=
           USB_READY_EVENT_TIMEOUT_TICKS) {
            usb_diag_end(0x55000234u);
            usb_diag_begin(0x550002F0u,
                           SYSREG_BASE + SYSREG_USB_INTR, value);
            usb_diag_end(0x550002F0u);
            return USB_DEV_INIT_READY_TIMEOUT;
        }
    }
    usb_diag_end(0x55000234u);

    /* Acquire only that freshly observed reset-ready cause. */
    usb_diag_write32(0x55000235u, SYSREG_BASE + SYSREG_USB_INTR,
                     SYSREG_USB_READY_RAW);
    trace->usb_80_after_ack = usb_diag_read32(
        0x55000236u, SYSREG_BASE + SYSREG_USB_INTR);
    if(trace->usb_80_after_ack & SYSREG_USB_READY_RAW) {
        usb_diag_begin(0x550002F1u, SYSREG_BASE + SYSREG_USB_INTR,
                       trace->usb_80_after_ack);
        usb_diag_end(0x550002F1u);
        return USB_DEV_INIT_READY_ACK_ERROR;
    }

    usb_diag_begin(0x55000237u, 0, USB_01G_SETTLE_TICKS);
    start = usb_count();
    while((uint32_t)(usb_count() - start) < USB_01G_SETTLE_TICKS)
        __asm__ volatile("");
    usb_diag_end(0x55000237u);

    /* 2. Controller base init, matching usb.prx text+0x52a4..+0x533c. */
    usb_diag_write32(0x55000301u, USB_BASE + USB_ACTIVATE, USB_ACTIVATE_ON);
    usb_diag_write32(0x55000302u, USB_BASE + USB_CONFIG, 0x610u);
    usb_diag_write32(0x55000303u, USB_BASE + USB_RESET, 0);
    value = usb_diag_read32(0x55000304u, USB_BASE + USB_INTACK);
    usb_diag_write32(0x55000305u, USB_BASE + USB_INTACK, value);
    value = usb_diag_read32(0x55000306u, USB_BASE + USB_INTEN2);
    usb_diag_write32(0x55000307u, USB_BASE + USB_INTEN2, value);
    usb_diag_write32(0x55000308u, USB_BASE + USB_INTEN, 0xFFFFFFB7u);
    usb_diag_write32(0x55000309u, USB_BASE + USB_EP_DISABLE, 0xFFFFFFFFu);

    /* Retail usb.prx calls lowio NID 0x1561BCD2 here: after the initial
     * controller-global block, but before CONFIG becomes 0x210.  Its 6.61
     * implementation RMW-enables BC100074 bit0x100. */
    value = usb_diag_read32(0x5500030Au, SYSREG_BASE + SYSREG_AUX_IO);
    usb_diag_write32(0x5500030Bu, SYSREG_BASE + SYSREG_AUX_IO,
                     value | SYSREG_USB_AUX_IO);
    trace->post_aux_74 = usb_diag_read32(0x5500030Cu,
                                        SYSREG_BASE + SYSREG_AUX_IO);
    if(!(trace->post_aux_74 & SYSREG_USB_AUX_IO)) {
        usb_diag_begin(0x550003FEu, 0, SYSREG_USB_AUX_IO);
        usb_diag_end(0x550003FEu);
        return USB_DEV_INIT_AUX_ERROR;
    }

    usb_diag_write32(0x5500030Du, USB_BASE + USB_CONFIG, 0x210u);

    /* 3. Reset software and descriptor state before registering endpoints. */
    usb_diag_begin(0x55000400u, (uint32_t)SETUP_DESC, 0);
    configured = false;
    bulk_out_armed = bulk_in_busy = false;
    ep0_phase = EP0_WAIT_SETUP;
    diag_events = 0;
    diag_error = USB_DEV_DIAG_ERROR_NONE;
    diag_setup_hi = diag_setup_lo = diag_setup_control = 0;
    diag_in_control = diag_out_control = 0;
    diag_in_status = diag_out_status = 0;
    ep0_in_expected = 0;
    diag_track_first_data = false;

    setup_desc_clear();
    desc_clear(EP0_IN_DESC);
    desc_clear(EP0_OUT_DESC);
    desc_clear(BULK_IN_DESC);
    desc_clear(BULK_OUT_DESC);
    usb_diag_end(0x55000400u);

    /* 4/5. Endpoint table, EP0 registration and the SETUP mailbox.  A bus reset
     * destroys all of it, so the identical sequence is reachable from the
     * global-event handler below. */
    usb_ep0_reconfigure();
    diag_events |= USB_DEV_DIAG_ACTIVE;
    usb_diag_begin(0x550007FFu, 0, 0);
    usb_diag_end(0x550007FFu);
    return USB_DEV_INIT_OK;
}

/* ===== Global events (bus reset / enumeration), polled =====
 *
 * Interrupts stay masked, so nothing services +0x40C unless we do.  Retail's
 * dispatcher lives at usb.prx text+0x6CF0 and its reset arm reaches text+0x7160,
 * which calls the reconfigure routine at text+0x5EC0 and only then W1Cs 0x08.
 */

/* Retail usb.prx text+0x5EC0.  Rebuilds everything the controller loses across
 * a bus reset or a speed change: endpoint table, EP0 packet sizes, the run
 * interrupt mask, the endpoint enable mask (IN0 disabled / OUT0 enabled), the
 * SETUP mailbox, the receive+transmit DMA enables and finally OUT0's CNAK.
 * The write order and the trace breadcrumbs are the proven cold-boot ones. */
static void usb_ep0_reconfigure(void) {
    uint32_t value;

    /* Retail uses +0x504 for EP0's max packet size and one following word per
     * descriptor; it never writes a USB device address here.  Our descriptor
     * order is EP1 IN then EP1 OUT, both bulk, configuration/interface zero. */
    usb_diag_write32(0x55000501u, USB_BASE + USB_EP_CONFIG(0), 64u << 19);
    usb_diag_write32(0x55000502u, USB_BASE + USB_EP_CONFIG(1), 0x100000D1u);
    usb_diag_write32(0x55000503u, USB_BASE + USB_EP_CONFIG(2), 0x100000C1u);

    usb_diag_begin(0x55000600u, USB_BASE + EP_IN(EP0), 64);
    ep0_config_preconnect(64);
    usb_diag_end(0x55000600u);
    usb_diag_write32(0x55000701u, USB_BASE + USB_INTEN, USB_INTEN_RUN);
    /* Retail stores the whole 0xFFFEFFFF: every endpoint disabled except OUT0.
     * Re-disabling IN0 matters on re-entry, where a data stage exposed it. */
    usb_diag_begin(0x55000702u, USB_BASE + USB_EP_DISABLE, 0xFFFEFFFFu);
    ep_enable(EP_DIR_IN, EP0, false);
    ep_enable(EP_DIR_OUT, EP0, true);
    usb_diag_end(0x55000702u);
    usb_diag_write32(0x55000703u,
                     USB_BASE + EP_OUT(EP0) + EP_SETUP_MAILBOX,
                     PHYS(SETUP_DESC));
    value = usb_diag_read32(0x55000704u, USB_BASE + USB_CONFIG);
    usb_diag_write32(0x55000705u, USB_BASE + USB_CONFIG,
                     value | USB_CONFIG_DMA_EN);
    value = usb_diag_read32(0x55000706u,
                            USB_BASE + EP_OUT(EP0) + EP_CTRL);
    usb_diag_write32(0x55000707u, USB_BASE + EP_OUT(EP0) + EP_CTRL,
                     value | EP_CNAK);
}

/* A bus reset voids every queued transfer, so drop our side of them and go back
 * to waiting for the first SETUP of the new enumeration attempt. */
static void usb_ep0_restart(void) {
    setup_desc_clear();
    desc_clear(EP0_IN_DESC);
    desc_clear(EP0_OUT_DESC);
    configured = false;
    bulk_out_armed = bulk_in_busy = false;
    ep0_in_expected = 0;
    ep0_phase = EP0_WAIT_SETUP;
}

static void usb_global_events(void) {
    uint32_t events = USB(USB_INTACK);

    if(!(events & (USB_GEV_RESET | USB_GEV_ENUM |
                   USB_GEV_SET_CONFIG | USB_GEV_SET_INTF)))
        return;

    /* SET_CONFIGURATION and SET_INTERFACE never arrive as SETUP packets: the
     * controller decodes and answers them itself, publishes the requested
     * values in +0x408 and raises these events (retail text+0x6F98/+0x6F04).
     * Ignoring them means never becoming configured no matter what the host
     * does, which looks exactly like the host giving up after enumeration. */
    if(events & USB_GEV_SET_CONFIG) {
        diag_set_config_count++;
        diag_config_value = USB(USB_INTSTAT) & USB_STS_CFG_MASK;
        usb_apply_configuration(diag_config_value);
        USB(USB_INTACK) = USB_GEV_SET_CONFIG; /* exact W1C, text+0x6FC8 */
        usb_sync();
    }

    if(events & USB_GEV_SET_INTF) {
        /* Only interface 0, alternate setting 0 exists, so there is nothing to
         * switch; the event still has to be acknowledged. */
        diag_set_intf_count++;
        USB(USB_INTACK) = USB_GEV_SET_INTF;
        usb_sync();
    }

    if(!(events & (USB_GEV_RESET | USB_GEV_ENUM)))
        return;
    if(gev_service_used >= USB_GEV_MAX_SERVICE)
        return;
    gev_service_used++;

    if(events & USB_GEV_RESET) {
        if(diag_first_reset_phase == 0xFFu)
            diag_first_reset_phase = (uint32_t)ep0_phase;
        diag_reset_count++;
        usb_ep0_restart();
        /* text+0x7160 enters the reconfigure with speed 1 before acknowledging. */
        usb_speed = 1;
        usb_ep0_reconfigure();
        USB(USB_INTACK) = USB_GEV_RESET; /* exact W1C, text+0x6E10 */
        usb_sync();
    }

    if(events & USB_GEV_ENUM) {
        uint32_t speed;

        diag_enum_count++;
        /* text+0x6E24 acknowledges before it inspects the speed field. */
        USB(USB_INTACK) = USB_GEV_ENUM;
        usb_sync();
        speed = (USB(USB_INTSTAT) & USB_SPEED_MASK) ? 1u : 2u;
        if(speed != usb_speed) {
            usb_speed = speed;
            usb_ep0_restart();
            usb_ep0_reconfigure();
        } else {
            /* text+0x7150: unchanged speed only refreshes the run mask. */
            USB(USB_INTEN) = USB_INTEN_RUN;
            usb_sync();
        }
    }

    /* USB_GEV_SUSPEND is deliberately left pending.  Retail runs a teardown
     * routine (text+0x741C) before its W1C and we have no equivalent, so
     * acknowledging it here would only hide the event. */
}

/* ===== EP0 control-transfer / enumeration (polled) ===== */

/* Device->host control transfer: send the IN data on EP0-IN, then arm the
 * zero-length OUT status stage on EP0-OUT (RE A3). */
static void ep0_send(const void *data, uint32_t len, uint32_t wLength) {
    if(len > wLength)
        len = wLength;
    /* The memcpy below is unchecked; truncate rather than overrun ep0_buf
     * into the adjacent DMA buffers. */
    if(len > sizeof(ep0_buf))
        len = sizeof(ep0_buf);
    if(len != 0)
        memcpy((void *)UNCACHED(ep0_buf), data, len);

    /* Retail preposts the zero-length OUT status descriptor while OUT0 remains
     * NAKed, then starts the IN data.  OUT0 is CNAKed only after IN completes. */
    ep_post_out(EP_OUT(EP0), EP0_OUT_DESC, ep0_status_buf);

    /* Sony exposes EP0-IN if hardware is NAKing, then waits for the host's IN
     * token before publishing/ringing the descriptor.  The common SETUP path
     * already issued the endpoint flush for every request direction. */
    uint32_t in_control = USB(EP_IN(EP0) + EP_CTRL);
    if(in_control & EP_NAK) {
        /* Retail re-reads CTRL for the command write instead of replaying the
         * earlier decision snapshot, preserving any intervening live bits. */
        USB(EP_IN(EP0) + EP_CTRL) =
            USB(EP_IN(EP0) + EP_CTRL) | EP_CNAK;
        usb_sync();
    }
    /* A lingering NAK is recoverable in Sony's start path: it SNAKs the OUT
     * side, then continues enabling IN.  The bounded IN-token timeout remains
     * the authority on whether endpoint exposure actually progressed. */
    if(USB(EP_IN(EP0) + EP_CTRL) & EP_NAK) {
        USB(EP_OUT(EP0) + EP_CTRL) =
            USB(EP_OUT(EP0) + EP_CTRL) | EP_SNAK;
        usb_sync();
    }
    ep_enable(EP_DIR_IN, EP0, true);
    ep0_in_expected = len;
    if(diag_track_first_data) {
        diag_out_control = desc_uncached(EP0_OUT_DESC)->control;
        diag_events |= USB_DEV_DIAG_OUT_POSTED;
    }
    ep0_phase = EP0_WAIT_IN_TOKEN;
}

/* No-data / host->device request: Sony triggers the controller's special EP0
 * IN-status handshake with CTRL bit0x100; it does not queue a DMA descriptor. */
static void ep0_send_status(void) {
    USB(EP_IN(EP0) + EP_CTRL) |= EP_CNAK;
    usb_sync();
    ep0_phase = EP0_WAIT_SETUP;
}

/* Enable or tear down the bulk pair for a configuration value.  Reached from
 * the controller's own SET_CONFIGURATION decode, and from the EP0 fallback. */
static void usb_apply_configuration(uint32_t value) {
    configured = value != 0;
    if(configured) {
        /* Enumeration completed, so the storm budget starts over. */
        gev_service_used = 0;
        /* Must match what cfg_desc advertised for this speed. */
        uint32_t mp = usb_bulk_maxpkt();
        ep_config_data(EP_DIR_OUT, EP_BULK, 2, mp);
        ep_enable(EP_DIR_OUT, EP_BULK, true);
        ep_config_data(EP_DIR_IN, EP_BULK, 2, mp);
        ep_enable(EP_DIR_IN, EP_BULK, true);
        diag_events |= USB_DEV_DIAG_CONFIGURED;
    } else {
        ep_enable(EP_DIR_OUT, EP_BULK, false);
        ep_enable(EP_DIR_IN, EP_BULK, false);
        bulk_out_armed = bulk_in_busy = false;
    }
}

/* Protocol stall.  Retail usb.prx text+0x3FC4 flushes the IN FIFO then sets
 * endpoint control bit0, and text+0x3F84 sets the same bit on the OUT side
 * (endpoint stride 0x20 from the +0x200 receive base).  A control endpoint's
 * stall is cleared by the controller when the next SETUP arrives, so there is
 * nothing to undo here. */
static void ep0_stall(void) {
    USB(EP_IN(EP0) + EP_CTRL) |= EP_FLUSH;
    usb_sync();
    USB(EP_IN(EP0) + EP_CTRL) |= EP_STALL;
    USB(EP_OUT(EP0) + EP_CTRL) |= EP_STALL;
    usb_sync();
    diag_stall_count++;
    ep0_phase = EP0_WAIT_SETUP;
}

static void handle_setup(const uint8_t *s) {
    uint8_t bmRequestType = s[0];
    uint8_t bRequest = s[1];
    uint16_t wValue = (uint16_t)(s[2] | (s[3] << 8));
    uint16_t wIndex = (uint16_t)(s[4] | (s[5] << 8));
    uint16_t wLength = (uint16_t)(s[6] | (s[7] << 8));
    static const uint8_t zero_status[2] = { 0, 0 };

    /* Every request is recorded; the sixteen-deep bRequest history is what
     * shows how far the host actually got through enumeration. */
    diag_setup_count++;
    diag_last_setup_hi = pack_be32(s);
    diag_last_setup_lo = pack_be32(s + 4);
    /* One byte per request: bRequest in the high nibble, the descriptor type
     * from wValue in the low one.  61 = GET_DESCRIPTOR DEVICE, 62 = CONFIG,
     * 66 = DEVICE_QUALIFIER, 63 = STRING, 50 = SET_ADDRESS, 90 = SET_CONFIG. */
    diag_req_hist1 = (diag_req_hist1 << 8) | (diag_req_hist0 >> 24);
    diag_req_hist0 = (diag_req_hist0 << 8) |
                     ((uint32_t)(bRequest & 0x0Fu) << 4) |
                     ((wValue >> 8) & 0x0Fu);

    if(!(diag_events & USB_DEV_DIAG_SETUP)) {
        diag_setup_hi = diag_last_setup_hi;
        diag_setup_lo = diag_last_setup_lo;
        diag_events |= USB_DEV_DIAG_SETUP;
        diag_track_first_data =
            bmRequestType == 0x80u && bRequest == 6u &&
            wValue == 0x0100u && wLength != 0;
        /* Not an error any more: this build runs a whole enumeration, and a
         * host retrying after a reset may legitimately open with something
         * other than GET_DESCRIPTOR DEVICE.  Only the one-shot pass/fail
         * bookkeeping for the very first transfer depends on it. */
        if(diag_track_first_data)
            diag_events |= USB_DEV_DIAG_EXPECTED_SETUP;
    }

    /* Vendor requests are identified by bmRequestType, not by bRequest.  The
     * switch below dispatches on bRequest alone, so this has to be decided
     * first: MSOS2_VENDOR_CODE would otherwise be run as whatever standard
     * request happens to share its number. */
    if((bmRequestType & 0x60) == 0x40) {
        /* 0xC0 exactly: device-to-host, vendor type, device recipient -- the
         * only form the MS OS 2.0 spec defines for this request.  Answering an
         * OUT-direction request with an IN data stage would desync EP0. */
        if(bmRequestType == 0xC0 && bRequest == MSOS2_VENDOR_CODE &&
           wIndex == MSOS2_INDEX_DESCRIPTOR_SET)
            ep0_send(msos2_desc, sizeof(msos2_desc), wLength);
        else
            ep0_stall();
        return;
    }

    switch(bRequest) {
        case 6: /* GET_DESCRIPTOR */
            switch(wValue >> 8) {
                case 1: /* DEVICE */
                    ep0_send(dev_desc, sizeof(dev_desc), wLength);
                    break;
                case 2: /* CONFIGURATION */
                    usb_refresh_cfg_desc();
                    ep0_send(cfg_desc, sizeof(cfg_desc), wLength);
                    break;
                case 6: /* DEVICE_QUALIFIER */
                    ep0_send(qualifier_desc, sizeof(qualifier_desc), wLength);
                    break;
                case 0x0F: /* BOS -- carries the MS OS 2.0 platform capability */
                    ep0_send(bos_desc, sizeof(bos_desc), wLength);
                    break;
                default: /* strings, other-speed config: not supported */
                    ep0_stall();
                    break;
            }
            break;
        case 5: /* SET_ADDRESS: the controller applies the setup request after
                 * its status stage.  +0x504 is the endpoint table, not address. */
            diag_events |= USB_DEV_DIAG_ADDRESSED;
            ep0_send_status();
            break;
        case 9: /* SET_CONFIGURATION.  The controller normally answers this
                 * itself and raises the global event instead of delivering a
                 * SETUP, so this path is only a fallback. */
            usb_apply_configuration(wValue & 0xFFu);
            ep0_send_status();
            break;
        case 0: /* GET_STATUS */
            ep0_send(zero_status, sizeof(zero_status), wLength);
            break;
        case 8: { /* GET_CONFIGURATION */
            uint8_t value = configured ? 1 : 0;
            ep0_send(&value, sizeof(value), wLength);
            break;
        }
        case 10: { /* GET_INTERFACE: only alternate setting zero exists. */
            uint8_t value = 0;
            ep0_send(&value, sizeof(value), wLength);
            break;
        }
        case 11: /* SET_INTERFACE: only alternate setting zero exists. */
            ep0_send_status();
            break;
        default:
            /* An unsupported request must be stalled.  Answering a control-IN
             * read with a zero-length packet instead reads to the host as a
             * valid but empty descriptor, which is what stalls enumeration. */
            ep0_stall();
            break;
    }
}

/* Consume the SETUP mailbox.  A SETUP arriving mid-transfer legally supersedes
 * it -- the host has abandoned the old control transfer, which is ordinary
 * during enumeration -- so drop our queued descriptors and service the new
 * request instead of wedging.  Callers keep retail's per-phase ordering: the
 * status-OUT phase completes its data event first, everything else checks
 * SETUP first. */
static bool ep0_take_setup(uint32_t out_status) {
    uint8_t setup[8];
    const volatile uint8_t *src;

    if(!(out_status & EP_STATUS_OUT_SETUP) || !ep0_setup_done())
        return false;

    src = (const volatile uint8_t *)&desc_uncached(SETUP_DESC)->data;
    if(!(diag_events & USB_DEV_DIAG_SETUP))
        diag_setup_control = desc_uncached(SETUP_DESC)->control;
    for(unsigned int i = 0; i < sizeof(setup); i++)
        setup[i] = src[i];

    if(ep0_phase != EP0_WAIT_SETUP) {
        diag_setup_preempts++;
        desc_reclaim(EP0_IN_DESC);
        desc_reclaim(EP0_OUT_DESC);
        ep0_phase = EP0_WAIT_SETUP;
    }

    ep_ack_event(EP_OUT(EP0), out_status,
                 EP_STATUS_OUT_W1C, EP_STATUS_OUT_SETUP);
    USB(EP_IN(EP0) + EP_CTRL) |= EP_FLUSH;
    usb_sync();
    ep0_setup_rearm();
    handle_setup(setup);
    usb_receive_dma_enable();
    return true;
}

static void usb_ep0_poll(void) {
    uint32_t out_status = USB(EP_OUT(EP0) + EP_STATUS);

    switch(ep0_phase) {
        case EP0_WAIT_SETUP:
            ep0_take_setup(out_status);
            break;

        case EP0_WAIT_IN_TOKEN: {
            uint32_t in_status = USB(EP_IN(EP0) + EP_STATUS);

            if((out_status & EP_STATUS_OUT_SETUP) && ep0_setup_done()) {
                diag_out_status = out_status;
                ep0_take_setup(out_status);
                break;
            }

            if(in_status & EP_STATUS_IN_TOKEN) {
                ep_submit_in(EP_IN(EP0), EP0_IN_DESC,
                             ep0_buf, ep0_in_expected);
                if(diag_track_first_data) {
                    diag_in_status = in_status;
                    diag_in_control = desc_uncached(EP0_IN_DESC)->control;
                    diag_events |= USB_DEV_DIAG_IN_POSTED;
                }
                ep_ack_event(EP_IN(EP0), in_status,
                             EP_STATUS_IN_W1C, EP_STATUS_IN_TOKEN);
                ep0_phase = EP0_WAIT_IN_DATA;
            }
            break;
        }

        case EP0_WAIT_IN_DATA: {
            uint32_t in_status = USB(EP_IN(EP0) + EP_STATUS);

            /* A new SETUP supersedes the old control transfer.  For this
             * bounded first-transfer diagnostic, preserve it as an exact
             * failure rather than silently proving the wrong transaction. */
            if((out_status & EP_STATUS_OUT_SETUP) && ep0_setup_done()) {
                diag_out_status = out_status;
                ep0_take_setup(out_status);
                break;
            }

            if((in_status & EP_STATUS_IN_TDC) && desc_finished(EP0_IN_DESC)) {
                diag_in_status = in_status;
                diag_in_control = desc_uncached(EP0_IN_DESC)->control;
                if(diag_track_first_data) {
                    diag_events |= USB_DEV_DIAG_IN_DONE;
                    if(!desc_succeeded(EP0_IN_DESC))
                        diag_fail(USB_DEV_DIAG_ERROR_IN_RESULT);
                    else if(desc_xfer_len(EP0_IN_DESC) != ep0_in_expected)
                        diag_fail(USB_DEV_DIAG_ERROR_IN_LENGTH);
                }
                ep_ack_event(EP_IN(EP0), in_status,
                             EP_STATUS_IN_W1C, EP_STATUS_IN_TDC);
                desc_reclaim(EP0_IN_DESC);

                if(!(diag_events & USB_DEV_DIAG_ERROR)) {
                    ep_release_out(EP_OUT(EP0));
                    if(diag_track_first_data)
                        diag_events |= USB_DEV_DIAG_STATUS_RELEASED;
                    ep0_phase = EP0_WAIT_OUT_STATUS;
                }
                /* Retail ACKs the token only after its completion handler has
                 * reclaimed IN and released the preposted status-OUT stage. */
                ep_ack_event(EP_IN(EP0), in_status,
                             EP_STATUS_IN_W1C, EP_STATUS_IN_TOKEN);
            }
            break;
        }

        case EP0_WAIT_OUT_STATUS:
            /* Retail gates OUT descriptor completion on the OUT-DATA event.
             * Process DATA before SETUP when both arrive in one snapshot. */
            if((out_status & EP_STATUS_OUT_DATA) &&
               desc_finished(EP0_OUT_DESC)) {
                diag_out_status = out_status;
                diag_out_control = desc_uncached(EP0_OUT_DESC)->control;
                if(diag_track_first_data) {
                    diag_events |= USB_DEV_DIAG_OUT_DONE;
                    if(!desc_succeeded(EP0_OUT_DESC))
                        diag_fail(USB_DEV_DIAG_ERROR_OUT_RESULT);
                    else if(desc_xfer_len(EP0_OUT_DESC) != 0)
                        diag_fail(USB_DEV_DIAG_ERROR_OUT_LENGTH);
                }
                ep_ack_event(EP_OUT(EP0), out_status,
                             EP_STATUS_OUT_W1C, EP_STATUS_OUT_DATA);
                desc_reclaim(EP0_OUT_DESC);
                usb_receive_dma_enable();
                if(diag_track_first_data && !(diag_events & USB_DEV_DIAG_ERROR))
                    diag_events |= USB_DEV_DIAG_EP0_PASS;
                diag_track_first_data = false;
                ep0_phase = EP0_WAIT_SETUP;
                break;
            }

            if((out_status & EP_STATUS_OUT_SETUP) && ep0_setup_done()) {
                diag_out_status = out_status;
                ep0_take_setup(out_status);
            }
            break;
    }
}

/* ===== Bulk data path (the byte pipe) ===== */

static void usb_hw_rx(void) {
    if(!configured)
        return;

    if(!bulk_out_armed) {
        ep_submit_out(EP_OUT(EP_BULK), BULK_OUT_DESC, bulk_out_buf);
        bulk_out_armed = true;
        return;
    }
    uint32_t status = USB(EP_OUT(EP_BULK) + EP_STATUS);
    if((status & EP_STATUS_OUT_DATA) && desc_finished(BULK_OUT_DESC)) {
        ep_ack_event(EP_OUT(EP_BULK), status,
                     EP_STATUS_OUT_W1C, EP_STATUS_OUT_DATA);
        if(!desc_succeeded(BULK_OUT_DESC)) {
            /* The packet's bytes are gone and a raw byte stream cannot resync,
             * so host and loader are now desynchronised at an arbitrary offset.
             * Nothing can recover it here -- re-arming silently was the bug --
             * so raise the diagnostic error state that the crash screen and the
             * snapshot both report, instead of only bumping a private counter.
             * Data blocks are still covered by the protocol's checksum and
             * re-request; command traffic has no such backstop. */
            diag_bulk_out_fail++;
            diag_fail(USB_DEV_DIAG_ERROR_BULK_OUT_RESULT);
            desc_reclaim(BULK_OUT_DESC);
            bulk_out_armed = false;
            return;
        }
        uint32_t n = desc_xfer_len(BULK_OUT_DESC);
        diag_bulk_out_pkts++;
        diag_bulk_out_bytes += n;
        if(n > KOSLOAD_USB_MAXPKT)
            n = KOSLOAD_USB_MAXPKT;
        const volatile uint8_t *src = (const volatile uint8_t *)UNCACHED(bulk_out_buf);
        for(uint32_t i = 0; i < n; i++) {
            if((uint32_t)(rx_head - rx_tail) >= RING_SZ)
                break;
            rx_ring[rx_head & RING_MASK] = src[i];
            rx_head++;
        }
        desc_reclaim(BULK_OUT_DESC);
        bulk_out_armed = false; /* re-armed next call */
    }
}

static void usb_hw_tx(void) {
    if(!configured)
        return;

    /* Reclaim a finished IN transfer. */
    uint32_t status = USB(EP_IN(EP_BULK) + EP_STATUS);
    if(bulk_in_busy && (status & EP_STATUS_IN_TDC) &&
       desc_finished(BULK_IN_DESC)) {
        ep_ack_event(EP_IN(EP_BULK), status,
                     EP_STATUS_IN_W1C, EP_STATUS_IN_TDC);
        desc_reclaim(BULK_IN_DESC);
        bulk_in_busy = false;
    }

    if(bulk_in_busy || tx_head == tx_tail)
        return;

    uint32_t n = 0;
    volatile uint8_t *dst = (volatile uint8_t *)UNCACHED(bulk_in_buf);
    while(n < KOSLOAD_USB_MAXPKT && tx_tail != tx_head) {
        dst[n++] = tx_ring[tx_tail & RING_MASK];
        tx_tail++;
    }
    ep_submit_in(EP_IN(EP_BULK), BULK_IN_DESC, bulk_in_buf, n);
    diag_bulk_in_pkts++;
    diag_bulk_in_bytes += n;
    bulk_in_busy = true;
}

/* Read-only sampling of the registers whose transitions decide whether the
 * host ever drove the bus.  Called from every usb_dev_poll(). */
static void usb_diag_accumulate(void) {
    diag_sticky_sysreg |= SYSREG(SYSREG_USB_INTR);
    diag_sticky_intstat |= USB(USB_INTSTAT);
    diag_sticky_intack |= USB(USB_INTACK);
    diag_sticky_setup_ctrl |= desc_uncached(SETUP_DESC)->control;
    diag_sticky_out_status |= USB(EP_OUT(EP0) + EP_STATUS);
}

void usb_dev_diag_snapshot(struct usb_dev_diag_snapshot *snapshot) {
    if(!snapshot)
        return;

    snapshot->events = diag_events;
    snapshot->error = diag_error;
    snapshot->setup_hi = diag_setup_hi;
    snapshot->setup_lo = diag_setup_lo;
    snapshot->setup_control = (diag_events & USB_DEV_DIAG_SETUP) ?
                              diag_setup_control : desc_uncached(SETUP_DESC)->control;
    snapshot->setup_desc_phys = PHYS(SETUP_DESC);
    snapshot->in_desc_phys = PHYS(EP0_IN_DESC);
    snapshot->out_desc_phys = PHYS(EP0_OUT_DESC);
    snapshot->in_control = (diag_events & USB_DEV_DIAG_IN_DONE) ?
                           diag_in_control : desc_uncached(EP0_IN_DESC)->control;
    snapshot->out_control = (diag_events & USB_DEV_DIAG_OUT_DONE) ?
                            diag_out_control : desc_uncached(EP0_OUT_DESC)->control;
    snapshot->in_status = (diag_events & USB_DEV_DIAG_IN_DONE) ?
                          diag_in_status : USB(EP_IN(EP0) + EP_STATUS);
    snapshot->out_status = diag_out_status ?
                           diag_out_status : USB(EP_OUT(EP0) + EP_STATUS);
    snapshot->usb_status = USB(USB_INTSTAT);
    snapshot->usb_pending = USB(USB_INTACK);
    snapshot->usb_config = USB(USB_CONFIG);
    snapshot->ep_disable = USB(USB_EP_DISABLE);
    snapshot->in0_control = USB(EP_IN(EP0) + EP_CTRL);
    snapshot->out0_control = USB(EP_OUT(EP0) + EP_CTRL);
    snapshot->ep_table0 = USB(USB_EP_CONFIG(0));
    snapshot->ep_table1 = USB(USB_EP_CONFIG(1));
    snapshot->ep_table2 = USB(USB_EP_CONFIG(2));
    snapshot->sticky_sysreg = diag_sticky_sysreg;
    snapshot->sticky_intstat = diag_sticky_intstat;
    snapshot->sticky_intack = diag_sticky_intack;
    snapshot->sticky_setup_control = diag_sticky_setup_ctrl;
    snapshot->sticky_out_status = diag_sticky_out_status;
    snapshot->reset_count = diag_reset_count;
    snapshot->enum_count = diag_enum_count;
    snapshot->speed = usb_speed;
    snapshot->first_reset_phase = diag_first_reset_phase;
    snapshot->ep0_phase = (uint32_t)ep0_phase;
    snapshot->setup_count = diag_setup_count;
    snapshot->req_hist0 = diag_req_hist0;
    snapshot->req_hist1 = diag_req_hist1;
    snapshot->last_setup_hi = diag_last_setup_hi;
    snapshot->last_setup_lo = diag_last_setup_lo;
    snapshot->setup_preempts = diag_setup_preempts;
    snapshot->stall_count = diag_stall_count;
    snapshot->set_config_count = diag_set_config_count;
    snapshot->set_intf_count = diag_set_intf_count;
    snapshot->config_value = diag_config_value;
    snapshot->bulk_out_pkts = diag_bulk_out_pkts;
    snapshot->bulk_out_bytes = diag_bulk_out_bytes;
    snapshot->bulk_in_pkts = diag_bulk_in_pkts;
    snapshot->bulk_in_bytes = diag_bulk_in_bytes;
    snapshot->bulk_out_fail = diag_bulk_out_fail;
    snapshot->bulk_out_armed = bulk_out_armed ? 1u : 0u;
    snapshot->rx_pending = (uint32_t)(rx_head - rx_tail);
}

void usb_dev_prepare_handoff(void) {
    if(!initialized)
        return; /* controller clock may not even be on — nothing to retire */

    /* Deconfigure, which is the driver's own teardown of the bulk pair: both
     * bulk endpoints disabled and the armed flags dropped.  The bulk-OUT
     * descriptor is the one pointing DDMA at a buffer in this loader's BSS,
     * and the trampoline is about to overwrite that memory, so it must not
     * still be armed when the copy runs.
     *
     * EP0 and the DMA enables are deliberately left up.  The host's reconnect
     * path is built on the observation that the PSP does NOT drop off the bus
     * across an update -- it polls for the new loader to answer rather than
     * waiting for a disconnect -- so the control endpoint keeps answering
     * exactly as it does today, and only the data path is retired. */
    usb_apply_configuration(0);
}
