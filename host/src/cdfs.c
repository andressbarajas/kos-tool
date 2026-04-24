/* host/src/cdfs.c */
/*
 * CDFS redirection for kostool.
 *
 * Opens an ISO image file so the DC can read CD sectors from it
 * via the cdfs_read syscall. Sector reads are handled inline by
 * the console.c syscall handlers using ctx->cdfs_fd.
 */

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include <kosload/file_compat.h>
#include <kostool/cdfs.h>

int cdfs_open(kostool_context_t *ctx, const char *iso_filename) {
    int fd = open(iso_filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        perror(iso_filename);
        return -1;
    }

    ctx->cdfs_fd = fd;
    ctx->iso_filename = iso_filename;
    printf("Opened ISO image %s for CDFS redirection\n", iso_filename);
    return 0;
}

void cdfs_close(kostool_context_t *ctx) {
    if (ctx->cdfs_fd >= 0) {
        close(ctx->cdfs_fd);
        ctx->cdfs_fd = -1;
    }
}

int cdfs_read_sectors(kostool_context_t *ctx, int start_sector, int num_sectors,
                      uint8_t *buffer, size_t buffer_size) {
    if (ctx->cdfs_fd < 0)
        return -1;

    size_t bytes = (size_t)num_sectors * 2048;
    if (bytes > buffer_size)
        bytes = buffer_size;

    /* Adjust for 150-sector pregap */
    off_t offset = (off_t)(start_sector - 150) * 2048;
    if (lseek(ctx->cdfs_fd, offset, SEEK_SET) < 0)
        return -1;

    ssize_t ret = read(ctx->cdfs_fd, buffer, bytes);
    if (ret < 0)
        return -1;

    return (int)ret;
}
