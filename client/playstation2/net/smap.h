/* client/playstation2/net/smap.h
 *
 * Public interface for the PS2 SMAP adapter shim.
 *
 */

#ifndef PS2_NET_SMAP_H
#define PS2_NET_SMAP_H

#include "adapter.h"

extern adapter_t adapter_smap;

const char *smap_get_last_error(void);

#endif /* PS2_NET_SMAP_H */
