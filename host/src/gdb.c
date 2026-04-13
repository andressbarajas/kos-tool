/* host/src/gdb.c */
/*
 * GDB server for kostool.
 *
 * Creates a TCP listen socket on the GDB port (default 2159).
 * The actual packet relay between DC and GDB client is handled
 * by the gdbpacket syscall handlers in console.c.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kostool/gdb.h>
#include <kostool/platform.h>

int gdb_socket_runtime_init(kostool_context_t *ctx) {
    if (!ctx->socket_ops) {
        fprintf(stderr, "GDB server requires socket operations\n");
        return -1;
    }

    if (!ctx->sockets_initialized) {
        if (ctx->socket_ops->init && ctx->socket_ops->init() != 0) {
            fprintf(stderr, "Socket initialization failed\n");
            return -1;
        }
        ctx->sockets_initialized = 1;
    }

    return 0;
}

void gdb_socket_runtime_cleanup(kostool_context_t *ctx) {
    if (ctx->sockets_initialized) {
        if (ctx->socket_ops && ctx->socket_ops->cleanup)
            ctx->socket_ops->cleanup();
        ctx->sockets_initialized = 0;
    }
}

int gdb_send_all(kostool_context_t *ctx, int64_t sock, const void *data, size_t len) {
    const uint8_t *buf = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        int rv = ctx->socket_ops->send(sock, buf + sent, len - sent);
        if (rv <= 0)
            return -1;
        sent += (size_t)rv;
    }

    return 0;
}

void gdb_close_client(kostool_context_t *ctx) {
    if (ctx->gdb_client_socket >= 0) {
        ctx->socket_ops->close(ctx->gdb_client_socket);
        ctx->gdb_client_socket = -1;
    }
}

void gdb_report_program_exit(kostool_context_t *ctx, int exit_code) {
    if (ctx->gdb_client_socket >= 0) {
        char payload[4];
        char packet[8];
        unsigned char checksum = 0;

        snprintf(payload, sizeof(payload), "W%02x", exit_code & 0xff);

        for (size_t i = 0; payload[i] != '\0'; ++i)
            checksum = (unsigned char)(checksum + (unsigned char)payload[i]);

        snprintf(packet, sizeof(packet), "$%s#%02x", payload, checksum);
        gdb_send_all(ctx, ctx->gdb_client_socket, packet, strlen(packet));
        gdb_close_client(ctx);
    }
}

int gdb_init(kostool_context_t *ctx, uint16_t port) {
    if (gdb_socket_runtime_init(ctx) != 0)
        return -1;

    int64_t sock = ctx->socket_ops->tcp_socket();
    if (sock < 0) {
        fprintf(stderr, "Failed to create GDB server socket\n");
        return -1;
    }

    if (ctx->socket_ops->setsockopt_reuse)
        ctx->socket_ops->setsockopt_reuse(sock);

    if (ctx->socket_ops->bind_listen(sock, port) < 0) {
        fprintf(stderr, "Failed to bind GDB server to port %u\n", port);
        ctx->socket_ops->close(sock);
        ctx->gdb_server_socket = -1;
        return -1;
    }

    ctx->gdb_server_socket = sock;
    ctx->gdb_client_socket = -1;

    printf("GDB server listening on port %u\n", port);
    return 0;
}

void gdb_shutdown(kostool_context_t *ctx) {
    gdb_close_client(ctx);

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
