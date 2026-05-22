/* client/wii/net/scif.h - no SH4 SCIF on Wii. */
#ifndef KOSLOAD_WII_SCIF_H
#define KOSLOAD_WII_SCIF_H

static inline void scif_puts(const unsigned char *s)
{
    (void)s;
}

#endif /* KOSLOAD_WII_SCIF_H */
