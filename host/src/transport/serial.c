/* host/src/transport/serial.c */
/*
 * Serial transport implementation for kostool.
 *
 * Ported from dcload-serial/host-src/tool/dc-tool.c.
 * Implements the dcload serial protocol with LZO compression
 * and XOR checksums.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <kosload/protocol.h>
#include <kostool/transport.h>
#include <kostool/platform.h>
#include "minilzo.h"

#define HEAP_ALLOC(var, size) \
    long __LZO_MMODEL var[((size) + (sizeof(long) - 1)) / sizeof(long)]

static HEAP_ALLOC(wrkmem, LZO1X_1_MEM_COMPRESS);

/* ===== Low-level serial helpers ===== */

static int serial_putc(kostool_context_t *ctx, uint8_t ch) {
    return ctx->serial_ops->write(ctx->serial_handle, &ch, 1);
}

static int serial_getc(kostool_context_t *ctx, uint8_t *ch) {
    int ret = ctx->serial_ops->read(ctx->serial_handle, ch, 1);
    if (ret != 1) {
        fprintf(stderr, "serial_getc: read error\n");
        *ch = 0;
        return -1;
    }
    return 0;
}

/* Blocking read of exactly count bytes */
static void blread(kostool_context_t *ctx, void *buf, int count) {
    uint8_t *tmp = buf;
    while (count > 0) {
        int ret = ctx->serial_ops->read(ctx->serial_handle, tmp, count);
        if (ret <= 0) { fprintf(stderr, "blread: read error (%d)\n", ret); return; }
        tmp += ret;
        count -= ret;
    }
}

/* Send 4-byte little-endian uint, then read back echo for verification */
static void send_uint(kostool_context_t *ctx, uint32_t value) {
    uint8_t b;

    b = value & 0xFF;         serial_putc(ctx, b);
    b = (value >> 8) & 0xFF;  serial_putc(ctx, b);
    b = (value >> 16) & 0xFF; serial_putc(ctx, b);
    b = (value >> 24) & 0xFF; serial_putc(ctx, b);

    uint8_t e0, e1, e2, e3;
    serial_getc(ctx, &e0);
    serial_getc(ctx, &e1);
    serial_getc(ctx, &e2);
    serial_getc(ctx, &e3);
    uint32_t echo = e0 | ((uint32_t)e1 << 8) | ((uint32_t)e2 << 16) | ((uint32_t)e3 << 24);

    if (echo != value)
        fprintf(stderr, "send_uint: echo mismatch (sent 0x%08x, got 0x%08x)\n", value, echo);
}

/* Receive 4-byte little-endian uint */
static uint32_t recv_uint(kostool_context_t *ctx) {
    uint8_t b0, b1, b2, b3;
    serial_getc(ctx, &b0);
    serial_getc(ctx, &b1);
    serial_getc(ctx, &b2);
    serial_getc(ctx, &b3);
    return b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
}

/* ===== LZO send/recv ===== */

/*
 * Send size bytes to DC with LZO compression.
 * Data is sent in SERIAL_BUFFER_SIZE chunks.
 * Each chunk: type('C'/'U') + size + data + XOR checksum + wait for ack
 */
static void lzo_send_data(kostool_context_t *ctx, const uint8_t *addr,
                          uint32_t size, int verbose) {
    uint8_t *buffer = malloc(SERIAL_BUFFER_SIZE + SERIAL_BUFFER_SIZE / 64 + 16 + 3);
    if (!buffer) return;

    if (verbose) {
        printf("send_data: ");
        fflush(stdout);
    }

    while (size > 0) {
        uint32_t sendsize = (size > SERIAL_BUFFER_SIZE) ? SERIAL_BUFFER_SIZE : size;
        lzo_uint csize;

        lzo1x_1_compress(addr, sendsize, buffer, &csize, wrkmem);

        if (csize < sendsize) {
            /* Send compressed */
            if (verbose) { printf("C"); fflush(stdout); }
            serial_putc(ctx, SERIAL_DATA_COMPRESSED);
            send_uint(ctx, csize);
            uint8_t ack = SERIAL_DATA_BAD;
            while (ack != SERIAL_DATA_GOOD) {
                ctx->serial_ops->write(ctx->serial_handle, buffer, csize);
                uint8_t sum = 0;
                for (lzo_uint i = 0; i < csize; i++)
                    sum ^= buffer[i];
                serial_putc(ctx, sum);
                blread(ctx, &ack, 1);
            }
        } else {
            /* Send uncompressed */
            if (verbose) { printf("U"); fflush(stdout); }
            serial_putc(ctx, SERIAL_DATA_UNCOMPRESSED);
            send_uint(ctx, sendsize);
            ctx->serial_ops->write(ctx->serial_handle, addr, sendsize);
            uint8_t sum = 0;
            for (uint32_t i = 0; i < sendsize; i++)
                sum ^= addr[i];
            serial_putc(ctx, sum);
            uint8_t ack;
            blread(ctx, &ack, 1);
        }

        size -= sendsize;
        addr += sendsize;
    }

    if (verbose) {
        printf("\n");
        fflush(stdout);
    }

    free(buffer);
}

/*
 * Receive total bytes from DC with LZO decompression.
 * Each chunk: type('C'/'U') + size + data + checksum + send ack
 */
static void lzo_recv_data(kostool_context_t *ctx, void *data,
                          uint32_t total, int verbose) {
    if (verbose) {
        printf("recv_data: ");
        fflush(stdout);
    }

    uint8_t *out = data;
    while (total > 0) {
        uint8_t type;
        blread(ctx, &type, 1);

        uint32_t size = recv_uint(ctx);

        switch (type) {
        case SERIAL_DATA_UNCOMPRESSED: {
            if (verbose) { printf("U"); fflush(stdout); }
            blread(ctx, out, size);
            uint8_t sum;
            blread(ctx, &sum, 1);
            uint8_t ok = SERIAL_DATA_GOOD;
            serial_putc(ctx, ok);
            total -= size;
            out += size;
            break;
        }
        case SERIAL_DATA_COMPRESSED: {
            if (verbose) { printf("C"); fflush(stdout); }
            uint8_t *tmp = malloc(size);
            if (!tmp) {
                fprintf(stderr, "\nlzo_recv_data: malloc(%u) failed\n", size);
                return;
            }
            blread(ctx, tmp, size);
            uint8_t sum;
            blread(ctx, &sum, 1);
            lzo_uint newsize;
            if (lzo1x_decompress(tmp, size, out, &newsize, 0) == LZO_E_OK) {
                uint8_t ok = SERIAL_DATA_GOOD;
                serial_putc(ctx, ok);
                total -= newsize;
                out += newsize;
            } else {
                uint8_t bad = SERIAL_DATA_BAD;
                serial_putc(ctx, bad);
                fprintf(stderr, "\nrecv_data: decompression failed!\n");
            }
            free(tmp);
            break;
        }
        default:
            break;
        }
    }

    if (verbose) {
        printf("\n");
        fflush(stdout);
    }
}

/* ===== Transport interface implementation ===== */

static int serial_init(kostool_context_t *ctx) {
    if (lzo_init() != LZO_E_OK) {
        fprintf(stderr, "LZO initialization failed\n");
        return -1;
    }

    if (!ctx->device_name) {
        fprintf(stderr, "No serial device specified (use -t)\n");
        return -1;
    }

    /* Open at initial speed (default 57600) */
    ctx->serial_handle = ctx->serial_ops->open(ctx->device_name, SERIAL_DEFAULT_SPEED);
    if (!ctx->serial_handle) {
        fprintf(stderr, "Failed to open serial device %s\n", ctx->device_name);
        return -1;
    }

    ctx->current_speed = SERIAL_DEFAULT_SPEED;

    /* If user requested a different speed, negotiate it */
    if (ctx->initial_speed != SERIAL_DEFAULT_SPEED) {
        if (ctx->transport->change_speed(ctx, ctx->initial_speed) != 0) {
            fprintf(stderr, "Failed to change speed to %u\n", ctx->initial_speed);
            return -1;
        }
    }

    /* Query remote loader version.
     * Protocol: send 'V', DC echoes 'V', sends version string + \n.
     * dcload-serial's scif_puts() also appends \r after \n, so we
     * must consume that trailing byte to keep the buffer clean. */
    {
        uint8_t v_cmd = 'V';
        ctx->serial_ops->write(ctx->serial_handle, &v_cmd, 1);

        /* Consume the command echo byte */
        uint8_t echo;
        serial_getc(ctx, &echo);

        int vi = 0;
        uint8_t ch;
        while (vi < (int)sizeof(ctx->remote_version_string) - 1) {
            if (serial_getc(ctx, &ch) != 0 || ch == '\n')
                break;
            ctx->remote_version_string[vi++] = (char)ch;
        }
        ctx->remote_version_string[vi] = '\0';

        /* Consume trailing \r (dcload-serial's scif_puts adds \r after \n) */
        serial_getc(ctx, &ch);

        ctx->remote_capabilities = 0;
        if (!ctx->quiet_mode)
            printf("%s\n", ctx->remote_version_string);
    }

    return 0;
}

static void serial_shutdown(kostool_context_t *ctx) {
    if (!ctx->serial_handle) return;

    /* Restore initial speed if we changed it */
    if (ctx->current_speed != SERIAL_DEFAULT_SPEED) {
        /* Try to change back - but don't fail if it doesn't work */
        uint8_t c = SERIAL_CMD_SPEED;
        ctx->serial_ops->write(ctx->serial_handle, &c, 1);
        blread(ctx, &c, 1);
        send_uint(ctx, SERIAL_DEFAULT_SPEED);

        ctx->serial_ops->close(ctx->serial_handle);
        ctx->serial_handle = ctx->serial_ops->open(ctx->device_name, SERIAL_DEFAULT_SPEED);
        if (ctx->serial_handle) {
            uint32_t rv = 0xdeadbeef;
            send_uint(ctx, rv);
            recv_uint(ctx);
        }
    }

    /* Close GDB socket if started */
    if (ctx->gdb_enabled && ctx->gdb_client_socket >= 0) {
        char gdb_buf[] = "+$X0f#ee";
        ctx->socket_ops->send(ctx->gdb_client_socket, gdb_buf, strlen(gdb_buf));
        ctx->time_ops->sleep_usec(1000000);
        ctx->socket_ops->close(ctx->gdb_client_socket);
        ctx->gdb_client_socket = -1;
    }
    if (ctx->gdb_enabled && ctx->gdb_server_socket >= 0) {
        ctx->socket_ops->close(ctx->gdb_server_socket);
        ctx->gdb_server_socket = -1;
    }

    if (ctx->serial_handle) {
        ctx->serial_ops->flush(ctx->serial_handle);
        ctx->serial_ops->close(ctx->serial_handle);
        ctx->serial_handle = NULL;
    }
}

/*
 * Send binary data to DC target at dest_addr.
 * Serial protocol: send 'B' command, read ack, send addr, send size, send data w/ LZO.
 */
static int serial_send_data(kostool_context_t *ctx, const uint8_t *data,
                            uint32_t dest_addr, uint32_t size) {
    uint8_t c = SERIAL_CMD_LOAD_BEGIN;
    ctx->serial_ops->write(ctx->serial_handle, &c, 1);
    blread(ctx, &c, 1);

    send_uint(ctx, dest_addr);
    send_uint(ctx, size);

    lzo_send_data(ctx, data, size, 1);
    return 0;
}

/*
 * Download data from DC target at src_addr.
 * Serial protocol: send 'F'/'G' command, read ack, send addr/size/wrkmem, recv w/ LZO.
 */
static int serial_recv_data(kostool_context_t *ctx, uint8_t *data,
                            uint32_t src_addr, uint32_t size, int quiet) {
    uint8_t c = quiet ? SERIAL_CMD_DOWNLOAD_Q : SERIAL_CMD_DOWNLOAD;
    ctx->serial_ops->write(ctx->serial_handle, &c, 1);
    blread(ctx, &c, 1);

    send_uint(ctx, src_addr);
    send_uint(ctx, size);
    send_uint(ctx, ctx->target_big_endian ? GC_LZO_WRKMEM_ADDR : DC_LZO_WRKMEM_ADDR);

    lzo_recv_data(ctx, data, size, 1);
    return 0;
}

/*
 * Serial protocol doesn't use 4-byte command IDs.
 * This maps the generic interface to single-byte serial commands.
 */
static int serial_send_command(kostool_context_t *ctx, const char cmd[4],
                               uint32_t addr, uint32_t size,
                               const uint8_t *data, uint32_t data_size) {
    (void)addr; (void)size; (void)data; (void)data_size;

    /* Map network-style command IDs to serial command bytes */
    uint8_t c;
    if (memcmp(cmd, NET_CMD_EXECUTE, 4) == 0)
        c = SERIAL_CMD_EXECUTE;
    else if (memcmp(cmd, NET_CMD_LOADBIN, 4) == 0)
        c = SERIAL_CMD_LOAD_BEGIN;
    else if (memcmp(cmd, NET_CMD_REBOOT, 4) == 0)
        c = SERIAL_CMD_SPEED;  /* No direct reboot command in serial */
    else {
        fprintf(stderr, "serial: unsupported command %.4s\n", cmd);
        return -1;
    }

    ctx->serial_ops->write(ctx->serial_handle, &c, 1);
    return 0;
}

static int serial_recv_response(kostool_context_t *ctx, uint8_t *buffer,
                                size_t buffer_size, uint32_t timeout_usec) {
    (void)timeout_usec;
    if (buffer_size < 1) return -1;
    blread(ctx, buffer, 1);
    return 1;
}

/*
 * Execute at address on DC.
 * Serial protocol: optionally send 'H' for CDFS redir, then send 'A' + addr + console.
 */
static int serial_execute(kostool_context_t *ctx, uint32_t addr,
                          int console_enabled, int cdfs_redir) {
    uint8_t c;

    if (cdfs_redir) {
        c = SERIAL_CMD_CDFS_REDIR;
        ctx->serial_ops->write(ctx->serial_handle, &c, 1);
        blread(ctx, &c, 1);
    }

    printf("Sending execute command (0x%08x, console=%d)...", addr, console_enabled);
    if (ctx->prog_argc > 0)
        printf("args(%u): \"%s\"...", ctx->prog_argc, ctx->prog_command_line);

    c = SERIAL_CMD_EXECUTE;
    ctx->serial_ops->write(ctx->serial_handle, &c, 1);
    blread(ctx, &c, 1);

    send_uint(ctx, addr);

    /* Encode "args follow" flag in bit 31 of console field.
     * kosload-serial checks this bit and reads argc + cmdline after console.
     * Legacy dcload-serial just does if(console), so 0x80000001 is still
     * truthy (console enabled) — harmless.  Extra bytes are only sent when
     * args are present, so with no args the protocol is identical to legacy. */
    uint32_t console_flags = (uint32_t)console_enabled;
    if (ctx->prog_argc > 0)
        console_flags |= (1u << 31);
    send_uint(ctx, console_flags);

    if (ctx->prog_argc > 0) {
        send_uint(ctx, ctx->prog_argc);
        uint32_t cmdline_len = (uint32_t)strlen(ctx->prog_command_line) + 1;
        send_uint(ctx, cmdline_len);
        ctx->serial_ops->write(ctx->serial_handle,
                               (const uint8_t *)ctx->prog_command_line, cmdline_len);
    }

    printf("executing\n");
    return 0;
}

/*
 * Change serial port speed.
 * Protocol: send 'S', read ack, send speed (with speedhack/extclk adjustments),
 * close port, reopen at new speed, verify with 0xdeadbeef exchange.
 */
static int serial_change_speed(kostool_context_t *ctx, uint32_t new_speed) {
    uint8_t c = SERIAL_CMD_SPEED;
    ctx->serial_ops->write(ctx->serial_handle, &c, 1);
    blread(ctx, &c, 1);

    /* Apply speedhack or external clock adjustments */
    uint32_t wire_speed = new_speed;
    if (ctx->speedhack && new_speed == 115200)
        wire_speed = 111607;  /* N=13 instead of N=12, -3.0% error vs 4.3% */
    else if (ctx->speedhack && new_speed == 230400)
        wire_speed = 223214;  /* N=6 instead of N=5, -2.8% error vs 11.5% */
    else if (ctx->use_extclk)
        wire_speed = 0;

    send_uint(ctx, wire_speed);

    printf("Changing speed to %u bps... ", new_speed);

    /* Close and reopen at the new speed */
    ctx->serial_ops->close(ctx->serial_handle);
    ctx->serial_handle = ctx->serial_ops->open(ctx->device_name, new_speed);
    if (!ctx->serial_handle) {
        fprintf(stderr, "Failed to reopen serial at %u bps\n", new_speed);
        return -1;
    }

    /* Verify the link with a 0xdeadbeef exchange */
    uint32_t rv = 0xdeadbeef;
    send_uint(ctx, rv);
    rv = recv_uint(ctx);

    ctx->current_speed = new_speed;
    printf("done\n");

    return 0;
}

static int serial_set_rtc(kostool_context_t *ctx, uint32_t timestamp) {
    uint8_t c = SERIAL_CMD_SETRTC;
    ctx->serial_ops->write(ctx->serial_handle, &c, 1);
    blread(ctx, &c, 1);
    send_uint(ctx, timestamp);
    printf("RTC synced\n");
    return 0;
}

const transport_ops_t serial_transport_ops = {
    .name = "serial",
    .capabilities = TRANSPORT_CAP_COMPRESS,
    .init = serial_init,
    .shutdown = serial_shutdown,
    .send_data = serial_send_data,
    .recv_data = serial_recv_data,
    .send_command = serial_send_command,
    .recv_response = serial_recv_response,
    .execute = serial_execute,
    .reset = NULL,
    .change_speed = serial_change_speed,
    .maple_command = NULL,
    .pmcr_command = NULL,
    .set_rtc = serial_set_rtc,
};
