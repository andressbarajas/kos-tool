/* client/wii/net/maple.h - no Dreamcast Maple bus on Wii. */
#ifndef KOSLOAD_WII_MAPLE_H
#define KOSLOAD_WII_MAPLE_H

#define MAPLE_RESPONSE_AGAIN    -4
#define MAPLE_RESPONSE_BADCMD   -3

static inline void maple_init(void) {
}

static inline void maple_wait_dma(void) {
}

static inline void *maple_docmd(int port, int unit, int cmd, int datalen, void *data) {
    static unsigned char no_maple_response[4] = {(unsigned char)MAPLE_RESPONSE_BADCMD, 0, 0, 0};

    (void)port;
    (void)unit;
    (void)cmd;
    (void)datalen;
    (void)data;
    return no_maple_response;
}

#endif /* KOSLOAD_WII_MAPLE_H */
