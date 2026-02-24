/* host/src/upload.c */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include <kostool/transport.h>
#include <kostool/platform.h>
#include <kostool/binary.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* Auto-detect toolchain prefix from ELF e_machine field */
static const char *detect_addr2line_prefix(const char *filename) {
    int fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) return "";

    uint8_t hdr[20];
    int n = (int)read(fd, hdr, 20);
    close(fd);

    if (n < 20) return "";
    /* Check ELF magic */
    if (hdr[0] != 0x7f || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F')
        return "";

    /* e_machine is at offset 18 (2 bytes, little-endian for SH/PPC) */
    uint16_t e_machine = hdr[18] | ((uint16_t)hdr[19] << 8);

    /* ELF is big-endian if EI_DATA == 2 */
    if (hdr[5] == 2)
        e_machine = ((uint16_t)hdr[18] << 8) | hdr[19];

    switch (e_machine) {
    case 42:  return "sh-elf-";           /* EM_SH */
    case 20:  return "powerpc-eabi-";     /* EM_PPC */
    default:  return "";
    }
}

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

    /* Auto-detect addr2line toolchain prefix if not user-specified */
    if (ctx->addr2line_enabled && !ctx->addr2line_prefix)
        ctx->addr2line_prefix = detect_addr2line_prefix(filename);

    return entry_addr;
}
