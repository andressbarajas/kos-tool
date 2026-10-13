/* host/src/upload.c */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <kostool/transport.h>
#include <kostool/platform.h>
#include <kostool/binary.h>

struct upload_state {
    kostool_context_t *ctx;
    int filename_scrambled;
    int saw_raw;
    int error;
    uint32_t total_bytes;
    uint32_t sections;
};

/* Match the basename on both POSIX and Windows paths. */
static int is_scrambled_filename(const char *filename) {
    const char *name = filename;
    for(const char *p = filename; *p; p++) {
        if(*p == '/' || *p == '\\')
            name = p + 1;
    }

    const char *expected = "1st_read.bin";
    while(*name && *expected) {
        if(tolower((unsigned char)*name++) != *expected++)
            return 0;
    }
    return *name == '\0' && *expected == '\0';
}

/* Returns a descrambled copy (caller frees), or NULL to send as-is */
static uint8_t *maybe_descramble(struct upload_state *state, const binary_section_t *section) {
    if(state->ctx->console_type != CONSOLE_DC || strcmp(section->name, ".raw") != 0)
        return NULL;

    state->saw_raw = 1;

    if(!state->ctx->force_descramble && !state->filename_scrambled)
        return NULL;

    uint8_t *descrambled = malloc(section->size);
    if(!descrambled || dc_descramble(section->data, descrambled, section->size) != 0) {
        free(descrambled);
        return NULL;
    }

    if(state->ctx->force_descramble)
        printf("Descrambling binary (--scrambled)\n");
    else
        printf("Descrambling binary (1st_read.bin filename)\n");

    return descrambled;
}

static int upload_section_cb(const binary_section_t *section, void *user_data) {
    struct upload_state *state = user_data;
    uint8_t *descrambled = maybe_descramble(state, section);
    const uint8_t *data = descrambled ? descrambled : section->data;

    printf("Section %s, lma 0x%08x, size %u\n", section->name, section->load_addr, section->size);
    int ret = state->ctx->transport->send_data(state->ctx, data, section->load_addr, section->size);
    free(descrambled);
    if(ret != 0)
        state->error = 1;
    else {
        state->total_bytes += section->size;
        state->sections++;
    }
    return ret;
}

uint32_t upload(kostool_context_t *ctx, const char *filename, uint32_t address) {
    uint32_t entry_addr = address;

    printf("Upload <%s>\n", filename);

    struct upload_state state = { .ctx = ctx, .filename_scrambled = is_scrambled_filename(filename) };

    uint64_t start = ctx->time_ops->time_usec();

    int ret = binary_auto_load(filename, address, &entry_addr, upload_section_cb, &state);
    if(ret != 0 || state.error) {
        fprintf(stderr, "Upload failed\n");
        return 0;
    }

    if(ctx->force_descramble && ctx->console_type != CONSOLE_DC)
        printf("Ignoring --scrambled: only Dreamcast binaries are scrambled\n");
    else if(ctx->force_descramble && !state.saw_raw)
        printf("Ignoring --scrambled: %s is not a raw binary\n", filename);

    uint64_t elapsed = ctx->time_ops->time_usec() - start;
    double   secs = elapsed / 1000000.0;
    if(secs > 0)
        printf("Transferred %u bytes at %0.f bytes / sec\n", state.total_bytes, state.total_bytes / secs);

    if(ctx->diagnostics_enabled) {
        ctx->diagnostics_uploaded_bytes += state.total_bytes;
        ctx->diagnostics_upload_sections += state.sections;
        printf("[diag] upload total: %u bytes, %u sections, %.3f ms, %.2f MiB/s\n", state.total_bytes,
               state.sections, (double)elapsed / 1000.0,
               secs > 0 ? ((double)state.total_bytes / secs) / (1024.0 * 1024.0) : 0.0);
    }

    return entry_addr;
}
