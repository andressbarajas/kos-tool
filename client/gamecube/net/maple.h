/* client/gamecube/net/maple.h */
/*
 * Stub Maple Bus header for GameCube.
 * The Dreamcast Maple bus does not exist on GC.
 * Provides no-op functions so shared code compiles.
 */

#ifndef __MAPLE_H__
#define __MAPLE_H__

/* Maple response codes used by shared command handlers */
#define MAPLE_RESPONSE_AGAIN   -4

static inline void maple_init(void) { }
static inline void maple_wait_dma(void) { }
static inline void *maple_docmd(int port, int unit, int cmd,
                                int datalen, void *data)
{
    (void)port; (void)unit; (void)cmd; (void)datalen; (void)data;
    return (void *)0;
}

#endif /* __MAPLE_H__ */
