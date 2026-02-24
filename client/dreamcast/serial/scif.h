/* client/dreamcast/serial/scif.h */
#ifndef KOSLOAD_DC_SCIF_H
#define KOSLOAD_DC_SCIF_H

/*
 * Copied from dcload-serial: dcload-serial/target-src/dcload/scif.h
 *
 * SH4 SCIF (Serial Communication Interface with FIFO) registers.
 */

#define SCSMR2  (volatile unsigned short *) 0xffe80000  /* Serial mode */
#define SCBRR2  (volatile unsigned char *)  0xffe80004  /* Bit rate */
#define SCSCR2  (volatile unsigned short *) 0xffe80008  /* Serial control */
#define SCFTDR2 (volatile unsigned char *)  0xffe8000c  /* Transmit FIFO data */
#define SCFSR2  (volatile unsigned short *) 0xffe80010  /* Serial status */
#define SCFRDR2 (volatile unsigned char *)  0xffe80014  /* Receive FIFO data */
#define SCFCR2  (volatile unsigned short *) 0xffe80018  /* FIFO control */
#define SCFDR2  (volatile unsigned short *) 0xffe8001c  /* FIFO data count */
#define SCSPTR2 (volatile unsigned short *) 0xffe80020  /* Serial port */
#define SCLSR2  (volatile unsigned short *) 0xffe80024  /* Line status */

void scif_init(int bps);
void scif_flush(void);
unsigned char scif_getchar(void);
unsigned int scif_isdata(void);
void scif_putchar(unsigned char c);
void scif_puts(unsigned char *str);

#endif /* KOSLOAD_DC_SCIF_H */
