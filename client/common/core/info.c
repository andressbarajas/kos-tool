/* client/common/core/info.c */
/*
 * kosload info block instance.
 *
 * Version is set at compile time. Transport-specific fields
 * (capabilities, transport type, IP, MAC, baud rate) are
 * populated by each transport's init function.
 */

#include <kosload/info.h>
#include <kosload/version.h>

kosload_info_t kosload_info = {
    .magic   = KOSLOAD_INFO_MAGIC,
    .size    = sizeof(kosload_info_t),
    .version = (KOSLOAD_VERSION_MAJOR << 16) |
               (KOSLOAD_VERSION_MINOR << 8) |
               KOSLOAD_VERSION_PATCH,
};
