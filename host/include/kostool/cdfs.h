/* host/include/kostool/cdfs.h */
#ifndef KOSTOOL_CDFS_H
#define KOSTOOL_CDFS_H

#include <stddef.h>
#include <stdint.h>
#include "context.h"

/* Open an ISO image for CDFS redirection */
int cdfs_open(kostool_context_t *ctx, const char *iso_filename);

/* Close the ISO image */
void cdfs_close(kostool_context_t *ctx);

/* Read sectors from the ISO (called by syscall handler) */
int cdfs_read_sectors(kostool_context_t *ctx, int start_sector, int num_sectors,
                      uint8_t *buffer, size_t buffer_size);

#endif /* KOSTOOL_CDFS_H */
