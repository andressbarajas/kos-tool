/* client/common/serial/serial_internal.h */
/*
 * Shared declarations between serial_transport.c and serial_syscalls.c.
 * Not part of the public client API — internal to the serial transport.
 */

#ifndef SERIAL_INTERNAL_H
#define SERIAL_INTERNAL_H

/* Serial byte I/O helpers (defined in serial_transport.c) */
void put_uint(unsigned int val);
unsigned int get_uint(void);

/* Compressed data send (defined in serial_transport.c) */
unsigned int send_data_block_compressed(unsigned char *addr, unsigned int size);

/* Data receive (defined in serial_transport.c) */
void load_data_block_general(unsigned char *addr, unsigned int total, unsigned int verbose);

/* LZO work memory — set by host 'F'/'G' commands or by assign_wrkmem syscall */
extern unsigned char *wrkmem;

#endif /* SERIAL_INTERNAL_H */
