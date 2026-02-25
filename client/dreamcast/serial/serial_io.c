/* client/dreamcast/serial/serial_io.c */
/*
 * Dreamcast serial I/O implementation.
 * Wraps the SH4 SCIF driver to provide the generic serial_io interface.
 */

#include <kosload/serial_io.h>
#include "scif.h"

int serial_io_init(unsigned int speed)
{
    scif_init((int)speed);
    return 0;  /* SCIF is always present on Dreamcast */
}

void serial_io_putchar(unsigned char c)
{
    scif_putchar(c);
}

unsigned char serial_io_getchar(void)
{
    return scif_getchar();
}

void serial_io_flush(void)
{
    scif_flush();
}

void serial_io_puts(const unsigned char *str)
{
    scif_puts((unsigned char *)str);
}

void serial_io_set_border(unsigned int color)
{
    *(volatile unsigned int *)0xa05f8040 = color;
}

unsigned int serial_io_data_ready(void)
{
    return scif_isdata();
}
