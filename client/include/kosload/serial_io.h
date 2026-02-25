/* client/include/kosload/serial_io.h */
/*
 * Serial byte I/O abstraction for the kosload serial transport.
 *
 * Platform-specific implementations:
 *   - Dreamcast: client/dreamcast/serial_io.c (wraps SCIF)
 *   - GameCube:  client/gamecube/serial_io.c  (wraps USBGecko)
 */

#ifndef KOSLOAD_SERIAL_IO_H
#define KOSLOAD_SERIAL_IO_H

int serial_io_init(unsigned int speed);
void serial_io_putchar(unsigned char c);
unsigned char serial_io_getchar(void);
void serial_io_flush(void);
void serial_io_puts(const unsigned char *str);
void serial_io_set_border(unsigned int color);
unsigned int serial_io_data_ready(void);

#endif /* KOSLOAD_SERIAL_IO_H */
