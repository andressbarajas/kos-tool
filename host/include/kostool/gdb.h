/* host/include/kostool/gdb.h */
#ifndef KOSTOOL_GDB_H
#define KOSTOOL_GDB_H

#include "context.h"

/* Initialize the GDB server on the specified port */
int gdb_init(kostool_context_t *ctx, uint16_t port);

/* Shut down the GDB server */
void gdb_shutdown(kostool_context_t *ctx);

/* Handle a GDB packet from the console */
int gdb_handle_packet(kostool_context_t *ctx);

#endif /* KOSTOOL_GDB_H */
