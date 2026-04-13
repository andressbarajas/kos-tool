/* host/include/kostool/gdb.h */
#ifndef KOSTOOL_GDB_H
#define KOSTOOL_GDB_H

#include <stddef.h>
#include "context.h"

/* Initialize the GDB server on the specified port */
int gdb_init(kostool_context_t *ctx, uint16_t port);

/* Initialize or tear down the platform socket runtime if needed. */
int gdb_socket_runtime_init(kostool_context_t *ctx);
void gdb_socket_runtime_cleanup(kostool_context_t *ctx);

/* Send a TCP packet fully, handling partial writes. */
int gdb_send_all(kostool_context_t *ctx, int64_t sock, const void *data, size_t len);

/* Close the active GDB client connection, if any. */
void gdb_close_client(kostool_context_t *ctx);

/* Shut down the GDB server */
void gdb_shutdown(kostool_context_t *ctx);

/* Handle a GDB packet from the console */
int gdb_handle_packet(kostool_context_t *ctx);

#endif /* KOSTOOL_GDB_H */
