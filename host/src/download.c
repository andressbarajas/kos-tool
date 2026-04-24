/* host/src/download.c */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include <kostool/transport.h>
#include <kostool/platform.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

int download(kostool_context_t *ctx, const char *filename,
             uint32_t address, uint32_t size) {
    printf("Download %u bytes at 0x%08x to <%s>\n", size, address, filename);

    uint8_t *data = malloc(size);
    if (!data) {
        fprintf(stderr, "Failed to allocate %u bytes\n", size);
        return -1;
    }

    uint64_t start = ctx->time_ops->time_usec();

    int ret = ctx->transport->recv_data(ctx, data, address, size, ctx->quiet_mode);
    if (ret != 0) {
        fprintf(stderr, "Download failed\n");
        free(data);
        return -1;
    }

    uint64_t elapsed = ctx->time_ops->time_usec() - start;

    int fd = open(filename, O_BINARY | O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror(filename);
        free(data);
        return -1;
    }

    write(fd, data, size);
    close(fd);
    free(data);

    printf("Received %u bytes\n", size);
    double secs = elapsed / 1000000.0;
    if (secs > 0)
        printf("Transferred at %.0f bytes / sec\n", size / secs);

    if (ctx->diagnostics_enabled) {
        ctx->diagnostics_downloaded_bytes += size;
        printf("[diag] download total: %u bytes, %.3f ms, %.2f MiB/s\n",
               size, (double)elapsed / 1000.0,
               secs > 0 ? ((double)size / secs) / (1024.0 * 1024.0) : 0.0);
    }

    return 0;
}
