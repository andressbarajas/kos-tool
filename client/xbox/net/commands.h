/* client/xbox/net/commands.h
 *
 * Thin redirect — canonical network-command declarations live in
 * <kosload/commands.h>.  "packet.h" is kept so the include graph still
 * pulls in the platform packet macros (ETHER_H_LEN, checksum helpers, ...).
 */
#ifndef __COMMANDS_H__
#define __COMMANDS_H__

#include "packet.h"
#include <kosload/commands.h>

#endif /* __COMMANDS_H__ */
