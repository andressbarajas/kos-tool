/* host/src/execute.c */
#include <stdio.h>
#include <stdint.h>

#include <kostool/transport.h>

int execute_command(kostool_context_t *ctx, uint32_t address) {
    printf("Executing at 0x%08x\n", address);
    return ctx->transport->execute(ctx, address,
                                   ctx->console_enabled,
                                   ctx->cdfs_enabled);
}
