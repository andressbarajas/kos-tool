/* client/dreamcast/serial/scif.c */
/*
 * SCIF serial driver for Dreamcast.
 *
 * Copied from dcload-serial: dcload-serial/target-src/dcload/scif.c
 * (The serial version is a superset of dcload-ip, adding external
 * clock support via the CKE1 bit.)
 */

#include "scif.h"

void scif_flush(void) {
    int v;
    *SCFSR2 &= 0xbf;
    while (!((v = *SCFSR2) & 0x40))
        ;
    *SCFSR2 = v & 0xbf;
}

void scif_init(int bps) {
    int i;

    /* Clear TE and RE bits; if bps==0 (external clock), set CKE1 */
    *SCSCR2 = bps ? 0x0 : 0x02;
    *SCFCR2 = 0x6;     /* Set TFRST and RFRST bits */
    *SCSMR2 = 0x0;     /* 8N1 data format */

    if (bps)
        *SCBRR2 = (50 * 1000000) / (32 * bps) - 1;

    for (i = 0; i < 100000; i++)    /* Delay at least 1 bit interval */
        ;

    *SCFCR2 = 12;
    *SCFCR2 = 0x8;     /* Set MCE */
    *SCSPTR2 = 0;
    *SCFSR2 = 0x60;
    *SCLSR2 = 0;
    *SCSCR2 = bps ? 0x30 : 0x32;   /* Enable TE+RE; if ext clock, CKE1 */

    for (i = 0; i < 100000; i++)
        ;
}

unsigned char scif_getchar(void) {
    unsigned char c;

    while (!(*SCFSR2 & 0x2))       /* Wait for RDF */
        ;
    c = *SCFRDR2;
    *SCFSR2 &= 0xfffd;             /* Clear RDF */

    return c;
}

unsigned int scif_isdata(void) {
    return (*SCFSR2 & 0x2);
}

void scif_putchar(unsigned char c) {
    while (!(*SCFSR2 & 0x20))      /* Wait for TDFE */
        ;
    *SCFTDR2 = c;
    *SCFSR2 &= 0xff9f;             /* Clear TDFE and TEND */
}

void scif_puts(unsigned char *str) {
    int i = 0;
    while (str[i] != 0) {
        scif_putchar(str[i]);
        if (str[i] == '\n')
            scif_putchar('\r');
        i++;
    }
}
