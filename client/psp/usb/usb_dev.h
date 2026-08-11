/* client/psp/usb/usb_dev.h — PSP USB device-controller HAL.
 *
 * Presents a vendor-specific USB device with one bulk IN and one bulk OUT
 * endpoint (IDs in <kosload/protocol.h>).  The rest of the loader treats this
 * as a byte pipe via serial_io.c, so the kosload serial protocol runs over it
 * unchanged.
 */

#ifndef KOSLOAD_PSP_USB_DEV_H
#define KOSLOAD_PSP_USB_DEV_H

#include <stdbool.h>
#include <stdint.h>

/* Nonblocking bring-up telemetry.  The production driver records the first
 * EP0 transaction so a hardware build can expose progress without relying on
 * USB itself for diagnostics. */
enum usb_dev_diag_event {
    USB_DEV_DIAG_ACTIVE          = 1u << 0,
    USB_DEV_DIAG_SETUP           = 1u << 1,
    USB_DEV_DIAG_EXPECTED_SETUP  = 1u << 2,
    USB_DEV_DIAG_IN_POSTED       = 1u << 3,
    USB_DEV_DIAG_OUT_POSTED      = 1u << 4,
    USB_DEV_DIAG_IN_DONE         = 1u << 5,
    USB_DEV_DIAG_OUT_DONE        = 1u << 6,
    USB_DEV_DIAG_STATUS_RELEASED = 1u << 7,
    USB_DEV_DIAG_EP0_PASS        = 1u << 8,
    USB_DEV_DIAG_ADDRESSED       = 1u << 9,
    USB_DEV_DIAG_CONFIGURED      = 1u << 10,
    USB_DEV_DIAG_ERROR           = 1u << 31
};

enum usb_dev_diag_error {
    USB_DEV_DIAG_ERROR_NONE = 0,
    USB_DEV_DIAG_ERROR_UNEXPECTED_SETUP = 1,
    USB_DEV_DIAG_ERROR_IN_RESULT = 2,
    USB_DEV_DIAG_ERROR_OUT_RESULT = 3,
    USB_DEV_DIAG_ERROR_IN_LENGTH = 4,
    USB_DEV_DIAG_ERROR_OUT_LENGTH = 5,
    USB_DEV_DIAG_ERROR_SETUP_PREEMPT = 6,
    USB_DEV_DIAG_ERROR_BULK_OUT_RESULT = 7
};

struct usb_dev_diag_snapshot {
    uint32_t events;
    uint32_t error;
    uint32_t setup_hi;
    uint32_t setup_lo;
    uint32_t setup_control;
    uint32_t setup_desc_phys;
    uint32_t in_desc_phys;
    uint32_t out_desc_phys;
    uint32_t in_control;
    uint32_t out_control;
    uint32_t in_status;
    uint32_t out_status;
    uint32_t usb_status;
    uint32_t usb_pending;
    uint32_t usb_config;
    uint32_t ep_disable;
    uint32_t in0_control;
    uint32_t out0_control;
    uint32_t ep_table0;
    uint32_t ep_table1;
    uint32_t ep_table2;
    /* Monotonic OR accumulators sampled on every usb_dev_poll(), so events the
     * controller raises between two redraws are still reported. */
    uint32_t sticky_sysreg;         /* BC100080 */
    uint32_t sticky_intstat;        /* BD800408 */
    uint32_t sticky_intack;         /* BD80040C */
    uint32_t sticky_setup_control;  /* SETUP descriptor control word */
    uint32_t sticky_out_status;     /* EP0-OUT endpoint status */
    /* Global-event servicing.  first_reset_phase is the EP0 phase captured at
     * the first bus reset, or 0xFF if no reset has been seen. */
    uint32_t reset_count;
    uint32_t enum_count;
    uint32_t speed;
    uint32_t first_reset_phase;
    uint32_t ep0_phase;
    /* Enumeration progress.  req_hist packs the bRequest code of the last
     * sixteen SETUPs, one nibble each, oldest first: req_hist1 then req_hist0
     * read left to right is the host's actual request sequence. */
    uint32_t setup_count;
    uint32_t req_hist0;
    uint32_t req_hist1;
    uint32_t last_setup_hi;
    uint32_t last_setup_lo;
    uint32_t setup_preempts;
    uint32_t stall_count;
    /* SET_CONFIGURATION / SET_INTERFACE are answered by the controller itself
     * and reported as global events, never as SETUP packets. */
    uint32_t set_config_count;
    uint32_t set_intf_count;
    uint32_t config_value;
    /* Bulk pipe activity, so a run that configures but never moves data can be
     * told apart from one where the endpoints are simply idle. */
    uint32_t bulk_out_pkts;
    uint32_t bulk_out_bytes;
    uint32_t bulk_in_pkts;
    uint32_t bulk_in_bytes;
    uint32_t bulk_out_fail;
    uint32_t bulk_out_armed;
    uint32_t rx_pending;
};

enum usb_dev_init_result {
    USB_DEV_INIT_OK = 0,
    USB_DEV_INIT_GATE_ERROR = -2,
    USB_DEV_INIT_AUX_ERROR = -3,
    USB_DEV_INIT_READY_TIMEOUT = -4,
    USB_DEV_INIT_READY_ACK_ERROR = -5,
    USB_DEV_INIT_STALE_ACK_ERROR = -6,
    USB_DEV_INIT_READY_DRAIN_TIMEOUT = -7
};

/* Bring up the USB device controller + endpoints.  Returns 0 on success or a
 * negative usb_dev_init_result without touching the inaccessible controller. */
int usb_dev_init(void);

/* Pump hardware endpoint FIFOs <-> the software ring buffers.  Safe to call
 * often; called from the byte helpers so the pipe drains/fills. */
void usb_dev_poll(void);

/* Snapshot the first EP0 transfer plus live controller state.  Call only after
 * usb_dev_init(), when the controller clock is known to be enabled. */
void usb_dev_diag_snapshot(struct usb_dev_diag_snapshot *snapshot);

/* Quiesce the bulk data path ahead of a one-way firmware handoff.
 *
 * Retires the bulk endpoints so no armed descriptor is still pointing DDMA at
 * the outgoing loader's buffers while the trampoline overwrites that memory
 * with the replacement image.  EP0, the DMA enables and the bus attachment are
 * deliberately left alone, so the device keeps answering control traffic and
 * never leaves the bus -- which is what the host's reconnect path expects.
 *
 * The result is the same state a SET_CONFIGURATION(0) leaves behind, which the
 * replacement loader's usb_dev_init() already rebuilds from. */
void usb_dev_prepare_handoff(void);

/* Byte pipe used by serial_io.c. */
bool usb_dev_rx_ready(void);          /* true if a received byte is available */
unsigned char usb_dev_rx_byte(void);  /* blocking: next byte from the host    */
void usb_dev_tx_byte(unsigned char c);/* queue a byte to the host            */
void usb_dev_tx_flush(void);          /* push any buffered TX to the host     */

#endif /* KOSLOAD_PSP_USB_DEV_H */
