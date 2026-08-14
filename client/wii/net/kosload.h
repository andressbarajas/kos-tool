/* client/wii/net/kosload.h */
/*
 * Wii display/timing constants for the network stack.
 * Shared state and display helpers live in <kosload/display.h>.
 */
#ifndef __KOSLOAD_H__
#define __KOSLOAD_H__

#include <kosload/display.h>
#include <kosload/protocol.h>

#define BBA_BG_COLOR   0x00100530
#define ENC_BG_COLOR   0x00100530
#define W5500_BG_COLOR 0x00100530
#define ERROR_BG_COLOR 0x00400000
#define STR_COLOR      0x00FFFFFF

#define WII_TBR_FREQUENCY 60750000
#define PERFCOUNTER_SCALE WII_TBR_FREQUENCY

#endif /* __KOSLOAD_H__ */
