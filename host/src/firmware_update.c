/* host/src/firmware_update.c */
/*
 * Firmware auto-update for kostool.
 *
 * On connection, queries the remote loader's version string and
 * compares it against the embedded firmware.  If the remote is
 * older (or a different loader, e.g. dcload → dc-load), uploads
 * a trampoline + new firmware binary and reconnects.
 *
 * Supports multiple consoles: DC (SH4 trampoline) and GC (PPC,
 * not yet implemented).  Firmware is embedded per-console per-transport:
 *   firmware_dc_serial, firmware_dc_ip, firmware_gc_serial, firmware_gc_ip
 *
 * The SH4 trampoline copies the firmware to 0x8c004000 (loader
 * base address), invalidates caches, and jumps to the new loader.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include <kosload/version.h>
#include <kosload/protocol.h>
#include <kostool/firmware.h>
#include <kostool/transport.h>
#include <kostool/platform.h>
#include <kostool/discover.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* ===== Firmware data stubs (when not embedded at build time) ===== */

#ifndef HAS_FIRMWARE_DC_SERIAL
const uint8_t firmware_dc_serial_data[] = {0};
const uint32_t firmware_dc_serial_size = 0;
#endif

#ifndef HAS_FIRMWARE_DC_IP
const uint8_t firmware_dc_ip_data[] = {0};
const uint32_t firmware_dc_ip_size = 0;
#endif

#ifndef HAS_FIRMWARE_GC_SERIAL
const uint8_t firmware_gc_serial_data[] = {0};
const uint32_t firmware_gc_serial_size = 0;
#endif

#ifndef HAS_FIRMWARE_GC_IP
const uint8_t firmware_gc_ip_data[] = {0};
const uint32_t firmware_gc_ip_size = 0;
#endif

/* ===== Console detection ===== */

typedef enum {
    CONSOLE_UNKNOWN,
    CONSOLE_DC,
    CONSOLE_GC
} console_type_t;

static console_type_t detect_console(const char *name) {
    if (strncmp(name, "dc-load-", 8) == 0 || strncmp(name, "dcload-", 7) == 0)
        return CONSOLE_DC;
    if (strncmp(name, "gc-load-", 8) == 0 || strncmp(name, "gcload-", 7) == 0)
        return CONSOLE_GC;

    return CONSOLE_UNKNOWN;
}

/* ===== Architecture-specific update parameters ===== */

typedef struct {
    const uint8_t *trampoline;
    uint32_t trampoline_size;
    uint32_t size_patch_offset;
    uint32_t load_addr;
    uint32_t loader_base;
    int big_endian;         /* 0 = LE (SH4), 1 = BE (PPC) */
} arch_update_params_t;

/* ===== SH4 Trampoline (Dreamcast) ===== */

/*
 * Pre-assembled SH4 trampoline (76 bytes, padded to 256).
 *
 * Behavior:
 *   1. Disable interrupts (SR = BL=1, RB=1, IMASK=0xF)
 *   2. Copy N bytes from 0x8c010100 to 0x8c004000
 *   3. Invalidate IC + OC (CCR = 0x0808)
 *   4. Jump to 0xAC004000 (P2 uncached mirror of loader entry)
 *
 * The "size" constant at offset 0x3C must be patched by the host
 * with the actual firmware size (little-endian uint32_t) before upload.
 *
 * Memory layout when uploaded to 0x8c010000:
 *   [0x000-0x0FF]  Trampoline code + constants (256 bytes, zero-padded)
 *   [0x100-...]    Firmware .bin data
 */
static const uint8_t sh4_trampoline[256] = {
    /* Instructions (offset 0x00-0x2F) */
    0x0b, 0xd0,             /* mov.l  sr_val_k, r0     */
    0x0e, 0x40,             /* ldc    r0, sr            */
    0x0b, 0xd4,             /* mov.l  src_k, r4         */
    0x0c, 0xd5,             /* mov.l  dst_k, r5         */
    0x0c, 0xd6,             /* mov.l  size_k, r6        */
    /* copy_loop: */
    0x44, 0x60,             /* mov.b  @r4+, r0          */
    0x00, 0x25,             /* mov.b  r0, @r5           */
    0x01, 0x75,             /* add    #1, r5            */
    0x10, 0x46,             /* dt     r6                */
    0xfa, 0x8b,             /* bf     copy_loop         */
    0x0a, 0xd0,             /* mov.l  ccr_addr_k, r0    */
    0x0b, 0xd1,             /* mov.l  ccr_val_k, r1     */
    0x12, 0x20,             /* mov.l  r1, @r0           */
    0x09, 0x00,             /* nop                      */
    0x09, 0x00,             /* nop                      */
    0x09, 0x00,             /* nop                      */
    0x09, 0x00,             /* nop                      */
    0x09, 0x00,             /* nop                      */
    0x09, 0x00,             /* nop                      */
    0x09, 0x00,             /* nop                      */
    0x09, 0x00,             /* nop                      */
    0x07, 0xd0,             /* mov.l  entry_k, r0       */
    0x2b, 0x40,             /* jmp    @r0               */
    0x09, 0x00,             /* nop (delay slot)         */
    /* Constants (offset 0x30-0x4B) */
    0xf0, 0x00, 0x00, 0x50, /* sr_val:   0x500000F0     */
    0x00, 0x01, 0x01, 0x8c, /* src:      0x8c010100     */
    0x00, 0x40, 0x00, 0x8c, /* dst:      0x8c004000     */
    0x00, 0x00, 0x00, 0x00, /* size:     PATCHED        */
    0x1c, 0x00, 0x00, 0xff, /* ccr_addr: 0xFF00001C     */
    0x08, 0x08, 0x00, 0x00, /* ccr_val:  0x00000808     */
    0x00, 0x40, 0x00, 0xac, /* entry:    0xAC004000     */
    /* Remaining bytes zero-padded to 256 */
};

static const arch_update_params_t sh4_params = {
    .trampoline = sh4_trampoline,
    .trampoline_size = 256,
    .size_patch_offset = 0x3C,
    .load_addr = 0x8c010000,
    .loader_base = 0x8c004000,
    .big_endian = 0,
};

/* ===== PPC Trampoline (GameCube) ===== */

/*
 * Pre-assembled PPC trampoline (160 bytes, padded to 256).
 * Assembled from ppc_trampoline.S with powerpc-eabi-gcc.
 *
 * Behavior:
 *   1. Load base register r7 = 0x80100000
 *   2. Disable interrupts (MSR = 0)
 *   3. Set up temporary stack at 0x80200000
 *   4. Copy firmware from 0x80100100 to 0x80003100 (word-at-a-time)
 *   5. Flush D-cache (dcbf) + invalidate I-cache (icbi) over dest range
 *   6. Jump to 0x80003100
 *
 * The "size" constant at offset 0x98 must be patched by the host
 * with the actual firmware size (big-endian uint32_t) before upload.
 *
 * Memory layout when uploaded to 0x80100000:
 *   [0x000-0x0FF]  Trampoline code + constants (256 bytes, zero-padded)
 *   [0x100-...]    Firmware .bin data
 */
static const uint8_t ppc_trampoline[256] = {
    /* 0x00: lis r7, 0x8010 */
    0x3c, 0xe0, 0x80, 0x10,
    /* 0x04: li r0, 0 */
    0x38, 0x00, 0x00, 0x00,
    /* 0x08: mtmsr r0 */
    0x7c, 0x00, 0x01, 0x24,
    /* 0x0C: isync */
    0x4c, 0x00, 0x01, 0x2c,
    /* 0x10: lis r1, 0x8020 */
    0x3c, 0x20, 0x80, 0x20,
    /* 0x14: ori r1, r1, 0 */
    0x60, 0x21, 0x00, 0x00,
    /* 0x18: stwu r0, -16(r1) */
    0x94, 0x01, 0xff, 0xf0,
    /* 0x1C: lwz r4, 0x90(r7)  -- source */
    0x80, 0x87, 0x00, 0x90,
    /* 0x20: lwz r5, 0x94(r7)  -- dest */
    0x80, 0xa7, 0x00, 0x94,
    /* 0x24: lwz r6, 0x98(r7)  -- size (PATCHED) */
    0x80, 0xc7, 0x00, 0x98,
    /* 0x28: lwz r3, 0x9C(r7)  -- entry */
    0x80, 0x67, 0x00, 0x9c,
    /* 0x2C: mr r9, r5  -- save dest */
    0x7c, 0xa9, 0x2b, 0x78,
    /* 0x30: mr r10, r6  -- save size */
    0x7c, 0xca, 0x33, 0x78,
    /* 0x34: srwi r6, r6, 2 */
    0x54, 0xc6, 0xf0, 0xbe,
    /* 0x38: mtctr r6 */
    0x7c, 0xc9, 0x03, 0xa6,
    /* copy_loop (0x3C): */
    /* 0x3C: lwz r0, 0(r4) */
    0x80, 0x04, 0x00, 0x00,
    /* 0x40: stw r0, 0(r5) */
    0x90, 0x05, 0x00, 0x00,
    /* 0x44: addi r4, r4, 4 */
    0x38, 0x84, 0x00, 0x04,
    /* 0x48: addi r5, r5, 4 */
    0x38, 0xa5, 0x00, 0x04,
    /* 0x4C: bdnz copy_loop */
    0x42, 0x00, 0xff, 0xf0,
    /* 0x50: mr r4, r9  -- reload dest */
    0x7d, 0x24, 0x4b, 0x78,
    /* 0x54: add r5, r9, r10  -- dest end */
    0x7c, 0xa9, 0x52, 0x14,
    /* dcbf_loop (0x58): */
    /* 0x58: dcbf 0, r4 */
    0x7c, 0x00, 0x20, 0xac,
    /* 0x5C: addi r4, r4, 32 */
    0x38, 0x84, 0x00, 0x20,
    /* 0x60: cmplw r4, r5 */
    0x7c, 0x04, 0x28, 0x40,
    /* 0x64: blt dcbf_loop */
    0x41, 0x80, 0xff, 0xf4,
    /* 0x68: sync */
    0x7c, 0x00, 0x04, 0xac,
    /* 0x6C: mr r4, r9  -- reload dest */
    0x7d, 0x24, 0x4b, 0x78,
    /* icbi_loop (0x70): */
    /* 0x70: icbi 0, r4 */
    0x7c, 0x00, 0x27, 0xac,
    /* 0x74: addi r4, r4, 32 */
    0x38, 0x84, 0x00, 0x20,
    /* 0x78: cmplw r4, r5 */
    0x7c, 0x04, 0x28, 0x40,
    /* 0x7C: blt icbi_loop */
    0x41, 0x80, 0xff, 0xf4,
    /* 0x80: sync */
    0x7c, 0x00, 0x04, 0xac,
    /* 0x84: isync */
    0x4c, 0x00, 0x01, 0x2c,
    /* 0x88: mtctr r3 */
    0x7c, 0x69, 0x03, 0xa6,
    /* 0x8C: bctr */
    0x4e, 0x80, 0x04, 0x20,
    /* Constant pool (0x90-0x9F): */
    /* 0x90: source = 0x80100100 */
    0x80, 0x10, 0x01, 0x00,
    /* 0x94: dest   = 0x80003100 */
    0x80, 0x00, 0x31, 0x00,
    /* 0x98: size   = PATCHED */
    0x00, 0x00, 0x00, 0x00,
    /* 0x9C: entry  = 0x80003100 */
    0x80, 0x00, 0x31, 0x00,
    /* Remaining bytes zero-padded to 256 */
};

static const arch_update_params_t ppc_params = {
    .trampoline = ppc_trampoline,
    .trampoline_size = 256,
    .size_patch_offset = 0x98,
    .load_addr = 0x80100000,
    .loader_base = 0x80003100,
    .big_endian = 1,
};

/* ===== Version parsing ===== */

/* Parse "name X.Y.Z" or "name X.Y" from a version string.
 * Returns 1 if parsed successfully, 0 if not. */
static int parse_version_string(const char *str, char *name, size_t name_size,
                                int *major, int *minor, int *patch) {
    if (!str || !str[0])
        return 0;

    /* Find the space before the version number */
    const char *space = strchr(str, ' ');
    if (!space)
        return 0;

    /* Copy name */
    size_t nlen = (size_t)(space - str);
    if (nlen >= name_size) nlen = name_size - 1;
    memcpy(name, str, nlen);
    name[nlen] = '\0';

    /* Parse X.Y.Z or X.Y (patch defaults to 0) */
    *patch = 0;
    int fields = sscanf(space + 1, "%d.%d.%d", major, minor, patch);
    if (fields < 2)
        return 0;

    return 1;
}

/* Compare two version tuples.
 * Returns: <0 if a < b, 0 if equal, >0 if a > b */
static int compare_versions(int a_major, int a_minor, int a_patch,
                            int b_major, int b_minor, int b_patch) {
    if (a_major != b_major) return a_major - b_major;
    if (a_minor != b_minor) return a_minor - b_minor;
    return a_patch - b_patch;
}

/* Check if a loader name is a "serial" type */
static int is_serial_loader(const char *name) {
    return strstr(name, "serial") != NULL;
}

/* Check if a loader name is an "ip" / network type */
static int is_ip_loader(const char *name) {
    return strstr(name, "ip") != NULL;
}

/* ===== IP config patching ===== */

/*
 * The client firmware contains a patchable IP config block:
 *   struct { char magic[8]; char ip[16]; }
 * where magic = "KOSLD_IP" and ip is "0.0.0.0" for DHCP or a
 * dotted-quad string for static IP.
 *
 * The host scans the embedded firmware binary for this magic and 
 * patches the IP field before uploading, allowing a single DHCP 
 * build to serve as static-IP firmware too.
 */
#define IP_CONFIG_MAGIC     "KOSLD_IP"
#define IP_CONFIG_MAGIC_LEN 8
#define IP_CONFIG_IP_LEN    16

/* Find the IP config block in fw_data and overwrite the IP string.
 * Returns 0 on success, -1 if the magic was not found. */
static int patch_firmware_ip(uint8_t *fw_data, uint32_t fw_size,
                             const char *ip_str) {
    for (uint32_t i = 0; i + IP_CONFIG_MAGIC_LEN + IP_CONFIG_IP_LEN <= fw_size; i++) {
        if (memcmp(fw_data + i, IP_CONFIG_MAGIC, IP_CONFIG_MAGIC_LEN) == 0) {
            uint8_t *ip_field = fw_data + i + IP_CONFIG_MAGIC_LEN;
            memset(ip_field, 0, IP_CONFIG_IP_LEN);
            strncpy((char *)ip_field, ip_str, IP_CONFIG_IP_LEN - 1);
            return 0;
        }
    }
    return -1;
}

/* ===== External firmware loading ===== */

/* Load a firmware .bin file from disk.  Caller must free() the returned buffer. */
static uint8_t *load_firmware_file(const char *path, uint32_t *out_size) {
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open firmware file: %s\n", path);
        return NULL;
    }

    off_t fsize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 512 * 1024) {
        fprintf(stderr, "Invalid firmware file size: %lld\n", (long long)fsize);
        close(fd);
        return NULL;
    }

    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) {
        close(fd);
        return NULL;
    }

    ssize_t n = read(fd, buf, (size_t)fsize);
    close(fd);

    if (n != fsize) {
        fprintf(stderr, "Short read on firmware file\n");
        free(buf);
        return NULL;
    }

    *out_size = (uint32_t)fsize;
    return buf;
}

/* ===== Core update logic ===== */

static int perform_update(kostool_context_t *ctx, const uint8_t *fw_data,
                          uint32_t fw_size, const char *patch_ip,
                          int dhcp_reconnect,
                          const arch_update_params_t *arch) {
    /* Build combined blob: trampoline (256 bytes) + firmware .bin */
    uint32_t blob_size = arch->trampoline_size + fw_size;
    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        fprintf(stderr, "Failed to allocate update blob (%u bytes)\n", blob_size);
        return -1;
    }

    memcpy(blob, arch->trampoline, arch->trampoline_size);
    memcpy(blob + arch->trampoline_size, fw_data, fw_size);

    /* Always patch the IP config block in the embedded firmware.
     * The embedded binary may have been built with any IP setting;
     * we must override it to match the desired mode:
     *   DHCP:   "0.0.0.0"
     *   Static: the current console IP (ctx->hostname) */
    if (patch_ip) {
        if (patch_firmware_ip(blob + arch->trampoline_size, fw_size, patch_ip) != 0)
            fprintf(stderr, "Warning: IP config block not found in firmware\n");
    }

    /* Patch the size constant in the trampoline.
     * SH4 is little-endian (host-native on x86/ARM), PPC is big-endian. */
    if (arch->big_endian) {
        uint8_t be[4] = {
            (fw_size >> 24) & 0xFF,
            (fw_size >> 16) & 0xFF,
            (fw_size >>  8) & 0xFF,
            (fw_size      ) & 0xFF,
        };
        memcpy(blob + arch->size_patch_offset, be, 4);
    } else {
        memcpy(blob + arch->size_patch_offset, &fw_size, 4);
    }

    /* Upload combined blob to program load area */
    int ret = ctx->transport->send_data(ctx, blob, arch->load_addr, blob_size);
    if (ret != 0) {
        fprintf(stderr, "Firmware upload failed\n");
        free(blob);
        return -1;
    }

    /* Execute the trampoline (no console, no CDFS) */
    ret = ctx->transport->execute(ctx, arch->load_addr, 0, 0);
    if (ret != 0) {
        fprintf(stderr, "Trampoline execute failed\n");
        free(blob);
        return -1;
    }

    free(blob);
    printf("Firmware updated\n");
    printf("Reconnecting...\n");

    if (strcmp(ctx->transport->name, "serial") == 0) {
        /* Serial: console restarts at default baud rate after trampoline.
         * Must close and reinit to renegotiate speed. */
        ctx->current_speed = SERIAL_DEFAULT_SPEED;  /* skip speed restore in shutdown */
        ctx->transport->shutdown(ctx);

        ctx->remote_capabilities = 0;
        memset(ctx->remote_version_string, 0, sizeof(ctx->remote_version_string));

        if (ctx->transport->init(ctx) != 0) {
            fprintf(stderr, "Failed to reconnect after firmware update\n");
            return -1;
        }
    } else {
        /* Network (static IP or DHCP): keep existing socket alive.
         * Lazy prepare_comms() will reconnect on the next operation.
         * DHCP servers typically reassign the same IP for the same MAC,
         * so reusing the original hostname works in practice. */
        ctx->installed_adapter = 0;
        ctx->legacy_mode = 0;
        ctx->remote_capabilities = 0;
        memset(ctx->remote_version_string, 0, sizeof(ctx->remote_version_string));
    }

    return 1;
}

/* ===== Public API ===== */

int auto_update_firmware(kostool_context_t *ctx) {
    /* Parse remote version */
    char remote_name[64] = {0};
    int remote_major = 0, remote_minor = 0, remote_patch = 0;

    if (!parse_version_string(ctx->remote_version_string, remote_name,
                              sizeof(remote_name),
                              &remote_major, &remote_minor, &remote_patch)) {
        /* Can't parse version — might be garbage or empty string */
        if (!ctx->firmware_path) {
            if (!ctx->quiet_mode && ctx->remote_version_string[0])
                printf("Cannot parse remote version: %s\n",
                       ctx->remote_version_string);
            return 0;
        }
    }

    /* Determine which firmware to use based on transport type and remote loader */
    const uint8_t *fw_data = NULL;
    uint32_t fw_size = 0;
    int is_serial = (strcmp(ctx->transport->name, "serial") == 0);

    /* Detect which console we're talking to */
    console_type_t console = detect_console(remote_name);

    /* Select architecture-specific update parameters */
    const arch_update_params_t *arch;
    if (console == CONSOLE_GC)
        arch = &ppc_params;
    else
        arch = &sh4_params;  /* DC or unknown (unknown won't reach perform_update) */

    /* -U flag: use external firmware file, skip all safety checks */
    uint8_t *ext_fw = NULL;
    if (ctx->firmware_path) {
        ext_fw = load_firmware_file(ctx->firmware_path, &fw_size);
        if (!ext_fw)
            return -1;
        fw_data = ext_fw;

        printf("Updating firmware...\n");

        int ret = perform_update(ctx, fw_data, fw_size, NULL, 0, arch);
        free(ext_fw);
        return ret;
    }

    if (console == CONSOLE_UNKNOWN) {
        if (!ctx->quiet_mode)
            printf("Unknown console type: %s\n", remote_name);
        return 0;
    }

    const char *prefix = (console == CONSOLE_DC) ? "dc" : "gc";

    /* Select embedded firmware based on console + transport */
    if (console == CONSOLE_DC) {
        if (is_serial) {
            fw_data = firmware_dc_serial_data;
            fw_size = firmware_dc_serial_size;
        } else {
            fw_data = firmware_dc_ip_data;
            fw_size = firmware_dc_ip_size;
        }
    } else {
        if (is_serial) {
            fw_data = firmware_gc_serial_data;
            fw_size = firmware_gc_serial_size;
        } else {
            fw_data = firmware_gc_ip_data;
            fw_size = firmware_gc_ip_size;
        }
    }

    /* No embedded firmware → skip */
    if (fw_size == 0)
        return 0;

    /* Skip if -N flag */
    if (ctx->skip_update)
        return 0;

    /* Check if update is needed:
     * - Different loader name (dcload → dc-load, kosload → dc-load): update
     * - Same name but older version: update
     * - Same name and same/newer version: skip */
    char expected_name[32];
    snprintf(expected_name, sizeof(expected_name), "%s-load-%s",
             prefix, is_serial ? "serial" : "ip");
    int need_update = 0;

    if (strcmp(remote_name, expected_name) != 0) {
        /* Different name — either dcload → dc-load upgrade,
         * or serial/ip mismatch.  Check if types match. */
        if (is_serial && !is_serial_loader(remote_name)) {
            /* Serial transport but remote is IP loader?  Skip. */
            return 0;
        }
        if (!is_serial && !is_ip_loader(remote_name)) {
            /* Network transport but remote is serial loader?  Skip. */
            return 0;
        }
        need_update = 1;
    } else {
        /* Same name — compare versions */
        int cmp = compare_versions(KOSLOAD_VERSION_MAJOR,
                                   KOSLOAD_VERSION_MINOR,
                                   KOSLOAD_VERSION_PATCH,
                                   remote_major, remote_minor, remote_patch);
        if (cmp > 0)
            need_update = 1;  /* Embedded is newer */
        else
            return 0;  /* Same or older — skip */
    }

    if (!need_update)
        return 0;

    /* Determine the IP to patch into the embedded firmware.
     *
     * The embedded firmware may have been built with any IP setting
     * (static or DHCP).  We always patch the IP config block to match
     * the desired mode:
     *   DHCP:   patch to "0.0.0.0" so the new loader does DHCP
     *   Static: patch to ctx->hostname so the IP is preserved
     *
     * dhcp_reconnect controls the post-update reconnection strategy:
     *   1 = full shutdown → discover → reinit (IP may change)
     *   0 = socket reuse with lazy prepare_comms (IP stays the same) */
    const char *patch_ip = NULL;
    int dhcp_reconnect = 0;

    if (!is_serial) {
        /* New-style loader with capabilities: check DHCP flag */
        if (ctx->remote_capabilities != 0) {
            if (!(ctx->remote_capabilities & (1 << 3))) {
                /* Static IP — patch firmware to preserve it */
                patch_ip = ctx->hostname;
            } else {
                patch_ip = "0.0.0.0";
                dhcp_reconnect = 1;
            }
        }
        /* Legacy dcload-ip: probe memory to detect DHCP vs static.
         * The DREAMCAST_IP string literal lives in .rodata. */
        else if (strstr(remote_name, "dcload") != NULL) {
            #define PROBE_SIZE 24576
            uint8_t *loader_mem = malloc(PROBE_SIZE);
            int probed = 0;
            if (loader_mem &&
                ctx->transport->recv_data(ctx, loader_mem,
                                          arch->loader_base, PROBE_SIZE, 1) == 0) {
                /* Search for NUL-terminated "0.0.0.0" — the DHCP sentinel */
                static const uint8_t dhcp_sentinel[8] = "0.0.0.0";
                for (uint32_t i = 0; i + 8 <= PROBE_SIZE; i++) {
                    if (memcmp(loader_mem + i, dhcp_sentinel, 8) == 0) {
                        probed = 1;  /* DHCP mode */
                        break;
                    }
                }
                if (!probed) {
                    /* No "0.0.0.0" found — static IP build */
                    patch_ip = ctx->hostname;
                    probed = 1;
                } else {
                    patch_ip = "0.0.0.0";
                    dhcp_reconnect = 1;
                }
            }
            free(loader_mem);
            #undef PROBE_SIZE

            if (!probed) {
                printf("Could not read loader memory — skipping update\n");
                printf("Use -U <file> to force update\n");
                return 0;
            }
        }
    }

    /* Save adapter description (e.g. " using Broadband Adapter (HIT-0400)")
     * before the update clears remote_version_string. */
    const char *using_suffix = strstr(ctx->remote_version_string, " using ");
    char adapter_desc[128] = "";
    if (using_suffix)
        snprintf(adapter_desc, sizeof(adapter_desc), "%s", using_suffix);

    if (strstr(remote_name, "dcload") != NULL)
        printf("Legacy loader found...\n");
    printf("Updating firmware...\n");

    int result = perform_update(ctx, fw_data, fw_size, patch_ip, dhcp_reconnect, arch);

    /* Network static IP: print the expected new version and pre-fill
     * remote_version_string so prepare_comms() skips its duplicate printf.
     * Serial and DHCP paths do a full reinit which prints the real version. */
    if (result > 0 && !dhcp_reconnect && !is_serial) {
        snprintf(ctx->remote_version_string,
                 sizeof(ctx->remote_version_string),
                 "%s-load-%s %s%s", prefix, "ip",
                 KOSLOAD_VERSION_STRING, adapter_desc);
        printf("%s\n", ctx->remote_version_string);
    }

    return result;
}
