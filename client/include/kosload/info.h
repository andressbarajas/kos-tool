/* client/include/kosload/info.h */
/*
 * kosload info block.
 *
 * Exposes loader metadata at a well-known memory location so loaded
 * programs can read version, capabilities, and network config without
 * making a syscall.
 *
 * The entry point header at entry+0x20 contains a pointer to the
 * kosload_info_t struct.  Programs dereference it to access the data.
 * The 'size' field allows forward compatibility — if the struct grows
 * in a future version, programs can check size before accessing new fields.
 */

#ifndef KOSLOAD_INFO_H
#define KOSLOAD_INFO_H

#include <stdint.h>
#include <kosload/protocol.h>

#define KOSLOAD_INFO_MAGIC      0x4b4f5349  /* "KOSI" */
#define KOSLOAD_MAX_ARGV_DATA   256

/* Transport types */
#define KOSLOAD_TRANSPORT_SERIAL   0
#define KOSLOAD_TRANSPORT_NETWORK  1

typedef struct {
    uint32_t magic;           /* KOSLOAD_INFO_MAGIC — validates pointer */
    uint32_t size;            /* sizeof(kosload_info_t) — forward compat */
    uint32_t version;         /* (major << 16) | (minor << 8) | patch */
    uint32_t capabilities;    /* KOSLOAD_CAP_* bitmask */
    uint32_t transport;       /* KOSLOAD_TRANSPORT_SERIAL or _NETWORK */

    /* Network fields (zero for serial builds) */
    uint32_t console_ip;      /* Console IP address (network byte order) */
    uint32_t host_ip;         /* Host/tool IP address (network byte order) */
    uint16_t host_port;       /* Host UDP port (network byte order) */
    uint8_t  mac[6];          /* Console MAC address */

    /* Serial fields (zero for network builds) */
    uint32_t baud_rate;       /* Serial baud rate (e.g. 1562500) */

    /* Argument vector data (populated by EXEC command from host).
     * argv_data stores NUL-separated strings including argv[0]. */
    uint32_t argc;            /* Final argc including argv[0] (0 = unavailable) */
    char argv_data[KOSLOAD_MAX_ARGV_DATA];
} kosload_info_t;

#endif /* KOSLOAD_INFO_H */
