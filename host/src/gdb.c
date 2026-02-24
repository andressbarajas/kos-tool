/* host/src/gdb.c */
/*
 * GDB server for kostool.
 *
 * Creates a TCP listen socket on the GDB port (default 2159).
 * The actual packet relay between DC and GDB client is handled
 * by the gdbpacket syscall handlers in console.c.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <kostool/gdb.h>
#include <kostool/platform.h>

int gdb_init(kostool_context_t *ctx, uint16_t port) {
    if (!ctx->socket_ops) {
        fprintf(stderr, "GDB server requires socket operations\n");
        return -1;
    }

    int64_t sock = ctx->socket_ops->tcp_socket();
    if (sock < 0) {
        fprintf(stderr, "Failed to create GDB server socket\n");
        return -1;
    }

    if (ctx->socket_ops->bind_listen(sock, port) < 0) {
        fprintf(stderr, "Failed to bind GDB server to port %u\n", port);
        ctx->socket_ops->close(sock);
        return -1;
    }

    ctx->gdb_server_socket = sock;
    ctx->gdb_client_socket = -1;

    printf("GDB server listening on port %u\n", port);
    return 0;
}

void gdb_shutdown(kostool_context_t *ctx) {
    if (ctx->gdb_client_socket >= 0) {
        /* Send SIGTERM to GDB client: +$X0f#ee */
        const char sigterm[] = "+$X0f#ee";
        ctx->socket_ops->send(ctx->gdb_client_socket, sigterm, strlen(sigterm));
        if (ctx->time_ops)
            ctx->time_ops->sleep_usec(500000);
        ctx->socket_ops->close(ctx->gdb_client_socket);
        ctx->gdb_client_socket = -1;
    }

    if (ctx->gdb_server_socket >= 0) {
        ctx->socket_ops->close(ctx->gdb_server_socket);
        ctx->gdb_server_socket = -1;
    }
}

int gdb_handle_packet(kostool_context_t *ctx) {
    /* Packet relay is handled inline by console.c syscall handlers */
    (void)ctx;
    return 0;
}
