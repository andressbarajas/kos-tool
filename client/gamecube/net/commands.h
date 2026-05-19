/* client/gamecube/net/commands.h
 *
 * Thin redirect — canonical network-command declarations live in
 * <kosload/commands.h>.  "packet.h" is kept here so the "commands.h"
 * include graph is unchanged: consumers (e.g. client/common/core/commands.c)
 * still get the platform packet macros (ETHER_H_LEN, checksum helpers,
 * ...) transitively via "commands.h", exactly as before.
 */
#ifndef __COMMANDS_H__
#define __COMMANDS_H__

#include "packet.h"
#include <kosload/commands.h>

#endif /* __COMMANDS_H__ */
