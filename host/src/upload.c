/* host/src/upload.c */
#include <stdio.h>
#include <stdint.h>

#include <kostool/transport.h>
#include <kostool/platform.h>
#include <kostool/binary.h>

struct upload_state {
    kostool_context_t *ctx;
    int error;
    uint32_t total_bytes;
};

static int upload_section_cb(const binary_section_t *section, void *user_data) {
    struct upload_state *state = user_data;
    printf("Section %s, lma 0x%08x, size %u\n",
           section->name, section->load_addr, section->size);
    int ret = state->ctx->transport->send_data(
        state->ctx, section->data, section->load_addr, section->size);
    if (ret != 0)
        state->error = 1;
    else
        state->total_bytes += section->size;
    return ret;
}

uint32_t upload(kostool_context_t *ctx, const char *filename, uint32_t address) {
    uint32_t entry_addr = address;

    printf("Upload <%s>\n", filename);

    struct upload_state state = { .ctx = ctx, .error = 0, .total_bytes = 0 };

    uint64_t start = ctx->time_ops->time_usec();

    int ret = binary_auto_load(filename, address, &entry_addr,
                               upload_section_cb, &state);
    if (ret != 0 || state.error) {
        fprintf(stderr, "Upload failed\n");
        return 0;
    }

    uint64_t elapsed = ctx->time_ops->time_usec() - start;
    double secs = elapsed / 1000000.0;
    if (secs > 0)
        printf("Transferred %u bytes at %0.f bytes / sec\n",
               state.total_bytes, state.total_bytes / secs);

    return entry_addr;
}
