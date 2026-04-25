/* host/include/kostool/discover.h */
#ifndef KOSTOOL_DISCOVER_H
#define KOSTOOL_DISCOVER_H

/* Broadcast VERS on the LAN for up to 10 seconds to discover a kosload-ip
 * device. Returns a static string with the discovered IP, or NULL on timeout. */
const char *discover_network_device(void);

#endif /* KOSTOOL_DISCOVER_H */
