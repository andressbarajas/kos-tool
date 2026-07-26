/* client/xbox/net/nvnet.h — Xbox onboard nForce (NVnet) Ethernet driver. */
#ifndef KOSLOAD_XBOX_NVNET_H
#define KOSLOAD_XBOX_NVNET_H

#include <kosload/net_adapter.h>

extern adapter_t adapter_nvnet;

const char *nvnet_get_last_error(void);

#endif /* KOSLOAD_XBOX_NVNET_H */
