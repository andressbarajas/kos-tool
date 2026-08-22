/* host/src/transport/usb.c — libusb byte pipe for the PSP USB loader.
 *
 * The PSP loader (psp-load-usb) tunnels the kosload serial byte protocol over
 * a vendor-specific USB bulk IN/OUT endpoint pair.  Rather than duplicate the
 * whole serial protocol, this file provides a libusb-backed platform_serial_ops
 * so the existing serial transport (host/src/transport/serial.c) runs over USB
 * unchanged: main.c selects serial_transport_ops with ctx->serial_ops set to
 * usb_serial_ops when the user passes "-t usb".
 *
 * Only built when libusb-1.0 is available (host/Makefile autodetect sets
 * HAVE_LIBUSB); otherwise -t usb reports a clear error in main.c.
 */

#ifdef HAVE_LIBUSB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libusb.h>

#include <kostool/platform.h>
#include <kosload/protocol.h>

#define USB_IO_TIMEOUT_MS   3000   /* per bulk transfer */
#define USB_POLL_TIMEOUT_MS 20     /* non-blocking-ish availability probe */

typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *dev;
    /* Two packets' worth: usb_topup() compacts a partial packet to the front
     * and appends a whole new one, so there must be room for both. */
    uint8_t  rxbuf[KOSLOAD_USB_MAXPKT * 2];
    int      rxlen;
    int      rxpos;
} usb_handle_t;

static void *usb_open(const char *device, uint32_t initial_baud) {
    (void)device;
    (void)initial_baud; /* USB has no baud rate */

    usb_handle_t *h = calloc(1, sizeof(*h));
    if(!h)
        return NULL;

    if(libusb_init(&h->ctx) != 0) {
        free(h);
        return NULL;
    }

    h->dev = libusb_open_device_with_vid_pid(h->ctx, KOSLOAD_USB_VID, KOSLOAD_USB_PID);
    if(!h->dev) {
        if(!kostool_quiet_open)
            fprintf(stderr, "usb: no PSP loader found (VID:PID %04x:%04x). Is it in USB mode?\n",
                    KOSLOAD_USB_VID, KOSLOAD_USB_PID);
        libusb_exit(h->ctx);
        free(h);
        return NULL;
    }

    libusb_set_auto_detach_kernel_driver(h->dev, 1);

    /* macOS deliberately leaves a vendor-specific device unconfigured when no
     * driver matches it: the OS reads the device and configuration descriptors
     * and then stops, never issuing SET_CONFIGURATION.  Claiming an interface
     * on an unconfigured device fails, so select the configuration ourselves.
     * LIBUSB_ERROR_BUSY means another program holds the device; a device that
     * is already in this configuration succeeds as a no-op. */
    int cfg = -1;
    if(libusb_get_configuration(h->dev, &cfg) != 0 ||
       cfg != KOSLOAD_USB_CONFIG) {
        int r = libusb_set_configuration(h->dev, KOSLOAD_USB_CONFIG);
        if(r != 0) {
            fprintf(stderr, "usb: could not select configuration %d (%s)\n",
                    KOSLOAD_USB_CONFIG, libusb_error_name(r));
            libusb_close(h->dev);
            libusb_exit(h->ctx);
            free(h);
            return NULL;
        }
    }

    if(libusb_claim_interface(h->dev, KOSLOAD_USB_IFACE) != 0) {
        fprintf(stderr, "usb: could not claim interface %d\n", KOSLOAD_USB_IFACE);
        libusb_close(h->dev);
        libusb_exit(h->ctx);
        free(h);
        return NULL;
    }

    return h;
}

static void usb_close(void *handle) {
    usb_handle_t *h = handle;
    if(!h)
        return;
    libusb_release_interface(h->dev, KOSLOAD_USB_IFACE);
    libusb_close(h->dev);
    libusb_exit(h->ctx);
    free(h);
}

/* Refill the RX buffer from the bulk IN endpoint.
 *
 * Returns >0 with bytes available, 0 when the transfer simply timed out with
 * nothing to report, or -1 when the device has gone away.  Keeping the last
 * two apart matters: an idle console is the normal case (the loaded program
 * is busy and not calling syscalls), and callers must wait it out rather than
 * treat it as a failure. */
static int usb_fill(usb_handle_t *h, unsigned int timeout_ms) {
    if(h->rxpos < h->rxlen)
        return h->rxlen - h->rxpos;

    int transferred = 0;
    int r = libusb_bulk_transfer(h->dev, KOSLOAD_USB_EP_IN, h->rxbuf, (int)sizeof(h->rxbuf),
                                 &transferred, timeout_ms);
    if((r == 0 || r == LIBUSB_ERROR_TIMEOUT) && transferred > 0) {
        h->rxpos = 0;
        h->rxlen = transferred;
        return transferred;
    }
    if(r == 0 || r == LIBUSB_ERROR_TIMEOUT)
        return 0; /* idle, not broken */
    return -1;    /* NO_DEVICE, PIPE, IO, ... */
}

/* Grow the buffered byte count past a partial packet.
 *
 * usb_fill() deliberately returns the current packet's residual and stops
 * there, which is right for usb_read() but wrong for an availability probe:
 * the device coalesces the console byte stream into <=512-byte bulk transfers,
 * so a 4-byte value can straddle a packet boundary.  ser_try_recv_uint() polls
 * for 4 bytes, and with a 1-3 byte residual usb_fill() would hand back the same
 * short count forever -- the program's exit code was then silently reported as
 * 0.  The termios backend this replaces used FIONREAD, which does accumulate.
 *
 * Compacts what is left to the front, then appends one more transfer. */
static int usb_topup(usb_handle_t *h, unsigned int timeout_ms) {
    if(h->rxpos > 0) {
        int residual = h->rxlen - h->rxpos;
        if(residual > 0)
            memmove(h->rxbuf, h->rxbuf + h->rxpos, (size_t)residual);
        h->rxlen = residual;
        h->rxpos = 0;
    }

    /* A bulk IN buffer shorter than one maxpacket risks LIBUSB_ERROR_OVERFLOW,
     * so only append when a whole packet still fits. */
    if((int)sizeof(h->rxbuf) - h->rxlen < KOSLOAD_USB_MAXPKT)
        return h->rxlen - h->rxpos;

    int transferred = 0;
    int r = libusb_bulk_transfer(h->dev, KOSLOAD_USB_EP_IN, h->rxbuf + h->rxlen,
                                 (int)sizeof(h->rxbuf) - h->rxlen, &transferred, timeout_ms);
    if(transferred > 0)
        h->rxlen += transferred;
    if(r != 0 && r != LIBUSB_ERROR_TIMEOUT && transferred == 0)
        return -1; /* NO_DEVICE, PIPE, IO, ... */
    return h->rxlen - h->rxpos;
}

static int usb_read(void *handle, void *buffer, size_t count) {
    usb_handle_t *h = handle;
    uint8_t *out = buffer;
    size_t n = 0;

    if(count == 0)
        return 0;

    /* Block for at least one byte, then drain whatever the current packet
     * holds (tty-like semantics the serial protocol expects).
     *
     * "Block" has to mean it.  The layers above this pipe were written against
     * a termios port opened VMIN=1/VTIME=0, which never returns 0 -- so a
     * short read is read as a hard error.  A bulk-IN timeout is not an error
     * here, it just means the program on the PSP is off doing its own work and
     * has not made a syscall recently, which is exactly what a demo that holds
     * a screen for 30 seconds does.  Returning 0 there made the console loop
     * dispatch an uninitialised command byte every USB_IO_TIMEOUT_MS, which is
     * where the "blread: read error (0)" flood and the desynchronised
     * "send_uint: echo mismatch" came from.  So keep retrying an idle pipe and
     * only report failure when the device actually goes away. */
    while(n < count) {
        if(h->rxpos >= h->rxlen) {
            if(n > 0)
                break; /* return what we have rather than block again */

            int r;
            while((r = usb_fill(h, USB_IO_TIMEOUT_MS)) == 0)
                ; /* idle console: keep waiting, same as a blocking tty */

            if(r < 0)
                return -1; /* device gone -- a real error, report it as one */
        }
        out[n++] = h->rxbuf[h->rxpos++];
    }
    return (int)n;
}

/* A short write desynchronises the byte protocol, and every caller of this
 * transport discards the return value (matching the termios backend, which
 * cannot short-write).  So a timeout that made no progress is retried rather
 * than silently truncating, and a genuine failure is reported loudly instead of
 * surfacing later as an unrelated checksum or echo mismatch. */
#define USB_WRITE_STALL_RETRIES 3

static int usb_write(void *handle, const void *buffer, size_t count) {
    usb_handle_t *h = handle;
    const uint8_t *in = buffer;
    size_t sent = 0;
    int stalls = 0;

    while(sent < count) {
        int transferred = 0;
        int chunk = (int)(count - sent);
        int r = libusb_bulk_transfer(h->dev, KOSLOAD_USB_EP_OUT, (unsigned char *)(in + sent), chunk,
                                     &transferred, USB_IO_TIMEOUT_MS);
        if(transferred > 0) {
            sent += (size_t)transferred;
            stalls = 0;
            continue;
        }
        if(r == LIBUSB_ERROR_TIMEOUT && ++stalls <= USB_WRITE_STALL_RETRIES)
            continue; /* device busy, not gone: try the remainder again */

        fprintf(stderr, "usb: write failed after %zu of %zu bytes (%s)\n",
                sent, count, libusb_error_name(r));
        return -1;
    }
    return (int)sent;
}

static int usb_bytes_available(void *handle) {
    usb_handle_t *h = handle;
    return usb_topup(h, USB_POLL_TIMEOUT_MS);
}

static int usb_set_speed(void *handle, uint32_t baud) {
    (void)handle;
    (void)baud;
    return 0; /* no-op over USB */
}

static void usb_flush(void *handle) {
    (void)handle; /* bulk OUT is sent synchronously in usb_write */
}

static void usb_drain(void *handle) {
    (void)handle;
}

const platform_serial_ops_t usb_serial_ops = {
    .open = usb_open,
    .close = usb_close,
    .read = usb_read,
    .write = usb_write,
    .bytes_available = usb_bytes_available,
    .set_speed = usb_set_speed,
    .flush = usb_flush,
    .drain = usb_drain,
};

#endif /* HAVE_LIBUSB */
