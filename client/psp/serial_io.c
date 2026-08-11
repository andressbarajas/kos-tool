/* client/psp/serial_io.c — bridges the kosload serial byte I/O API onto the
 * PSP USB bulk pipe.
 *
 * The shared serial transport (client/common/serial) is written entirely
 * against serial_io_*; backing those calls with the USB device controller's
 * bulk IN/OUT endpoints tunnels the whole serial protocol (LZO, checksums,
 * commands, syscalls) over USB with no changes to the transport.  "speed" is
 * meaningless over USB and is ignored.
 */

#include <kosload/protocol.h>
#include <kosload/serial_io.h>
#include "usb/usb_dev.h"

extern volatile unsigned int installed_adapter;

int serial_io_init(unsigned int speed) {
    (void)speed;
    installed_adapter = ADAPTER_PSP_USB;
    return usb_dev_init();
}

void serial_io_putchar(unsigned char c) {
    usb_dev_tx_byte(c);
}

unsigned char serial_io_getchar(void) {
    return usb_dev_rx_byte();
}

void serial_io_flush(void) {
    usb_dev_tx_flush();
}

void serial_io_puts(const unsigned char *str) {
    /* Send with \n -> \n\r conversion to match DC's scif_puts behavior.
     * The host serial transport expects \r after \n (same as GC). */
    while(*str) {
        usb_dev_tx_byte(*str);
        if(*str == '\n')
            usb_dev_tx_byte('\r');
        str++;
    }
    usb_dev_tx_flush();
}

void serial_io_set_border(unsigned int color) {
    (void)color; /* no border register on PSP */
}

unsigned int serial_io_data_ready(void) {
    return usb_dev_rx_ready() ? 1u : 0u;
}
