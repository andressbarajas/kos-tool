/* client/gamecube/serial/usbgecko.h */
/*
 * USBGecko serial driver for GameCube.
 *
 * The USBGecko is a serial debug adapter that connects via the
 * EXI bus (device 0 on channel 0 or 1). It provides USB-to-serial
 * communication with the host PC.
 *
 * Device ID: 0x01010000
 */

#ifndef KOSLOAD_GC_USBGECKO_H
#define KOSLOAD_GC_USBGECKO_H

/* USBGecko EXI commands */
#define USBGECKO_CMD_TX_STATUS  0xC0000000  /* Check TX FIFO status */
#define USBGECKO_CMD_RX_STATUS  0xD0000000  /* Check RX FIFO status */
#define USBGECKO_CMD_TX_DATA    0xB0000000  /* Send byte (data in bits 23:16) */
#define USBGECKO_CMD_RX_DATA    0xA0000000  /* Receive byte (data in bits 23:16) */

/* Status bits (bit 10 of 16-bit EXI response) */
#define USBGECKO_TX_READY       0x0400      /* TX FIFO has space */
#define USBGECKO_RX_READY       0x0400      /* RX FIFO has data */

/* USBGecko device ID */
#define USBGECKO_ID             0x01010000

/* Preferred EXI location for USBGecko (runtime detection probes both slots). */
#define USBGECKO_CHANNEL        1
#define USBGECKO_DEVICE         0

/* Initialize USBGecko. Returns 0 on success, -1 if not detected. */
int usbgecko_init(void);

/* Check if USBGecko is present */
int usbgecko_detect(void);

/* Send a single byte (blocks until TX ready) */
void usbgecko_putchar(unsigned char c);

/* Receive a single byte (blocks until RX has data) */
unsigned char usbgecko_getchar(void);

/* Check if data is available for reading */
int usbgecko_rx_ready(void);

/* Check if transmit buffer has space */
int usbgecko_tx_ready(void);

/* Send a null-terminated string */
void usbgecko_puts(const unsigned char *str);

/* Flush: wait for all pending TX data to be sent */
void usbgecko_flush(void);

#endif /* KOSLOAD_GC_USBGECKO_H */
