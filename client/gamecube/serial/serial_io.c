/* client/gamecube/serial/serial_io.c */
/*
 * GameCube serial I/O implementation.
 * Wraps the USBGecko driver to provide the generic serial_io interface.
 *
 * Note: The speed parameter in serial_io_init() is ignored for USBGecko
 * since it operates at a fixed USB transfer rate.
 */

#include <kosload/serial_io.h>
#include "usbgecko.h"

int serial_io_init(unsigned int speed)
{
    (void)speed;  /* USBGecko has fixed transfer rate */
    return usbgecko_init();
}

void serial_io_putchar(unsigned char c)
{
    usbgecko_putchar(c);
}

unsigned char serial_io_getchar(void)
{
    return usbgecko_getchar();
}

void serial_io_flush(void)
{
    usbgecko_flush();
}

void serial_io_puts(const unsigned char *str)
{
    /* Send with \n → \n\r conversion to match DC's scif_puts behavior.
     * The host serial transport expects \r after \n. */
    while (*str) {
        usbgecko_putchar(*str);
        if (*str == '\n')
            usbgecko_putchar('\r');
        str++;
    }
}

void serial_io_set_border(unsigned int color)
{
    (void)color;  /* No border register on GameCube */
}

unsigned int serial_io_data_ready(void)
{
    return usbgecko_rx_ready() ? 1 : 0;
}
