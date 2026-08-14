/* client/common/core/info.c */
/*
 * kosload info block instance.
 *
 * Version comes from mk/version.mk as -D flags (KOSLOAD_VERSION_DEFS).
 * Transport-specific fields (capabilities, transport type, IP, MAC,
 * baud rate) are populated by each transport's init function.
 */

#include <kosload/info.h>
#include <kosload/version.h>

kosload_info_t kosload_info = {
    .magic   = KOSLOAD_INFO_MAGIC,
    .size    = sizeof(kosload_info_t),
    .version = KOSLOAD_VERSION_ENCODED,
};
