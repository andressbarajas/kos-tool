/* host/src/main.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <kosload/version.h>
#include <kosload/protocol.h>
#include <kostool/context.h>
#include <kostool/transport.h>
#include <kostool/platform.h>
#include <kostool/binary.h>
#include <kostool/discover.h>
#include <kostool/firmware.h>
#include <kostool/config.h>
#include <kostool/gdb.h>

/* Forward declarations */
uint32_t upload(kostool_context_t *ctx, const char *filename, uint32_t address);
int download(kostool_context_t *ctx, const char *filename, uint32_t address, uint32_t size);
int execute_command(kostool_context_t *ctx, uint32_t address);
int do_console(kostool_context_t *ctx);

static double diag_ms(uint64_t usec) {
    return (double)usec / 1000.0;
}

static double diag_mib_per_sec(uint64_t bytes, uint64_t usec) {
    if (usec == 0)
        return 0.0;
    return ((double)bytes * 1000000.0) / ((double)usec * 1024.0 * 1024.0);
}

static void diag_phase(kostool_context_t *ctx, const char *label,
                       uint64_t usec) {
    if (ctx->diagnostics_enabled)
        printf("[diag] %s: %.3f ms\n", label, diag_ms(usec));
}

static void diag_summary(kostool_context_t *ctx) {
    if (!ctx->diagnostics_enabled)
        return;

    printf("[diag] total elapsed: %.3f ms\n",
           diag_ms(ctx->time_ops->time_usec() - ctx->diagnostics_start_usec));

    if (ctx->diagnostics_uploaded_bytes) {
        printf("[diag] uploaded: %llu bytes in %u sections\n",
               (unsigned long long)ctx->diagnostics_uploaded_bytes,
               ctx->diagnostics_upload_sections);
    }

    if (ctx->diagnostics_downloaded_bytes) {
        printf("[diag] downloaded: %llu bytes\n",
               (unsigned long long)ctx->diagnostics_downloaded_bytes);
    }

    if (ctx->diagnostics_net_send_bytes) {
        printf("[diag] aggregate network RX-on-target: %.2f MiB/s stream, %.2f MiB/s effective\n",
               diag_mib_per_sec(ctx->diagnostics_net_send_bytes,
                                ctx->diagnostics_net_send_stream_usec),
               diag_mib_per_sec(ctx->diagnostics_net_send_bytes,
                                ctx->diagnostics_net_send_total_usec));
    }

    if (ctx->diagnostics_net_recv_bytes) {
        printf("[diag] aggregate network TX-from-target: %.2f MiB/s stream, %.2f MiB/s effective\n",
               diag_mib_per_sec(ctx->diagnostics_net_recv_bytes,
                                ctx->diagnostics_net_recv_stream_usec),
               diag_mib_per_sec(ctx->diagnostics_net_recv_bytes,
                                ctx->diagnostics_net_recv_total_usec));
    }

    if (ctx->diagnostics_net_retransmitted_bytes ||
        ctx->diagnostics_net_recovery_requests ||
        ctx->diagnostics_net_loadbin_retries ||
        ctx->diagnostics_net_recv_rerequests) {
        printf("[diag] recovery: %llu retransmitted bytes, %u upload requests, %u LOADBIN retries, %u download rerequests\n",
               (unsigned long long)ctx->diagnostics_net_retransmitted_bytes,
               ctx->diagnostics_net_recovery_requests,
               ctx->diagnostics_net_loadbin_retries,
               ctx->diagnostics_net_recv_rerequests);
    }
}

/* Detect serial device patterns across platforms */
static int is_serial_device(const char *name) {
    if (!name) return 0;
    if (name[0] == '/') return 1;                          /* Unix: /dev/tty*, /dev/cu.* */
    if ((name[0] == 'C' || name[0] == 'c') &&
        (name[1] == 'O' || name[1] == 'o') &&
        (name[2] == 'M' || name[2] == 'm') &&
        name[3] >= '0' && name[3] <= '9') return 1;       /* Windows: COM1, com3, etc. */
    if (name[0] == '\\' && name[1] == '\\') return 1;     /* Windows: \\.\COM10 */
    return 0;
}

static const char *program_basename(const char *path) {
    const char *base = path;
    const char *slash;
#ifdef _WIN32
    const char *backslash;
#endif

    if (!path || !path[0])
        return path;

    slash = strrchr(path, '/');
    if (slash && slash[1] != '\0')
        base = slash + 1;

#ifdef _WIN32
    backslash = strrchr(path, '\\');
    if (backslash && backslash[1] != '\0' && (!slash || backslash > slash))
        base = backslash + 1;
#endif

    return base;
}

static void usage(void) {
    printf("\nkostool %s — Unified console loader\n\n", KOSLOAD_VERSION_STRING);
    printf("Usage: kostool [options] -t <device|ip|auto>\n\n");
    printf("Commands:\n");
    printf("  -x <file>    Upload and execute <file>\n");
    printf("     [-- args] Pass arguments to loaded program (must be last)\n");
    printf("  -u <file>    Upload <file>\n");
    printf("  -d <file>    Download to <file>\n");
    printf("  -r           Reset console\n\n");
    printf("Options:\n");
    printf("  -a <addr>    Set address (default: 0x8c010000)\n");
    printf("  -s <size>    Set size for download\n");
    printf("  -t <device>  Serial device, IP address, or 'auto'\n");
    printf("  -b <baud>    Serial baud rate (default: %d)\n", SERIAL_DEFAULT_SPEED);
    printf("  -n           Disable console/fileserver\n");
    printf("  -p           Dumb terminal mode\n");
    printf("  -q           Quiet mode\n");
    printf("  -c <path>    Chroot to <path>\n");
    printf("  -m <path>    Map /pc/ to <path>\n");
    printf("  -i <iso>     Enable CDFS redirection with <iso>\n");
    printf("  -g           Start GDB server on port %d\n", NET_GDB_PORT);
    printf("  -e           Alternate 115200/230400 baud\n");
    printf("  -E           Use external clock\n");
    printf("  -l           Force legacy 1024-byte payloads\n");
    printf("  -f           Fast mode (no FIFO delays)\n");
    printf("  -P, --diag   Print performance diagnostics\n");
    printf("  -w           Sync console RTC to host time\n");
    printf("  -U <file>    Update firmware from external file\n");
    printf("  -F           Enable automatic firmware update\n");
    printf("  -h           Show this help\n\n");
}

int main(int argc, char *argv[]) {
    kostool_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.cdfs_fd = -1;
    ctx.load_address = DC_DEFAULT_LOAD_ADDR;
    ctx.initial_speed = SERIAL_DEFAULT_SPEED;
    ctx.console_enabled = 1;
    ctx.socket_fd = -1;
    ctx.socket_legacy = -1;
    ctx.global_socket = -1;
    ctx.gdb_server_socket = -1;
    ctx.gdb_client_socket = -1;
    ctx.skip_update = 1;

    /* Set up platform ops */
    ctx.serial_ops = platform_get_serial_ops();
    ctx.socket_ops = platform_get_socket_ops();
    ctx.fs_ops = platform_get_fs_ops();
    ctx.time_ops = platform_get_time_ops();

    uint64_t diagnostics_start_usec = ctx.time_ops->time_usec();

    /* Load config file (sets addr2line defaults) */
    config_load(&ctx);

    char command = 0;
    const char *filename = NULL;

    if (argc < 2) {
        usage();
        return 1;
    }

    int prog_args_start = 0; /* index into argv where program args begin (after --) */

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        if (argv[i][1] == '-' && argv[i][2] == '\0') {
            /* -- separator: everything after this is program arguments */
            prog_args_start = i + 1;
            break;
        }
        if (strcmp(argv[i], "--diag") == 0 ||
            strcmp(argv[i], "--diagnostics") == 0 ||
            strcmp(argv[i], "--perf") == 0) {
            ctx.diagnostics_enabled = 1;
            continue;
        }
        switch (argv[i][1]) {
        case 'x':
            command = 'x';
            if (++i < argc) filename = argv[i];
            break;
        case 'u':
            command = 'u';
            if (++i < argc) filename = argv[i];
            break;
        case 'd':
            command = 'd';
            if (++i < argc) filename = argv[i];
            break;
        case 'r':
            command = 'r';
            break;
        case 'a':
            if (++i < argc) ctx.load_address = strtoul(argv[i], NULL, 0);
            break;
        case 's':
            if (++i < argc) ctx.download_size = strtoul(argv[i], NULL, 0);
            break;
        case 't':
            if (++i < argc) {
                ctx.device_name = argv[i];
                ctx.hostname = argv[i];
            }
            break;
        case 'b':
            if (++i < argc) ctx.initial_speed = strtoul(argv[i], NULL, 0);
            break;
        case 'n':
            ctx.console_enabled = 0;
            break;
        case 'p':
            ctx.dumb_terminal = 1;
            ctx.console_enabled = 0;
            break;
        case 'q':
            ctx.quiet_mode = 1;
            break;
        case 'c':
            if (++i < argc) {
                ctx.chroot_path = argv[i];
                ctx.use_chroot = 1;
            }
            break;
        case 'm':
            if (++i < argc) ctx.map_path = argv[i];
            break;
        case 'i':
            if (++i < argc) {
                ctx.iso_filename = argv[i];
                ctx.cdfs_enabled = 1;
            }
            break;
        case 'g':
            ctx.gdb_enabled = 1;
            break;
        case 'e':
            ctx.speedhack = 1;
            break;
        case 'E':
            ctx.use_extclk = 1;
            break;
        case 'l':
            ctx.force_legacy = 1;
            break;
        case 'f':
            ctx.fast_mode = 1;
            printf("Enabling fast transfer mode\n");
            break;
        case 'P':
            ctx.diagnostics_enabled = 1;
            break;
        case 'w':
            ctx.rtc_sync = 1;
            break;
        case 'U':
            if (++i < argc) ctx.firmware_path = argv[i];
            break;
        case 'F':
            ctx.skip_update = 0;
            break;
        case 'h':
            usage();
            return 0;
        default:
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    if (ctx.diagnostics_enabled) {
        ctx.diagnostics_start_usec = diagnostics_start_usec;
        printf("[diag] performance diagnostics enabled\n");
    }

    /* Build argv data for EXEC: "argv0\0argv1\0...".
     * Always synthesize argv[0] from the uploaded filename so KOS can
     * expose a real argc/argv pair to the loaded program. */
    if (command == 'x' && filename) {
        size_t offset = 0;
        const char *argv0 = program_basename(filename);
        size_t len = strlen(argv0) + 1;

        if (len > sizeof(ctx.prog_argv_data)) {
            fprintf(stderr, "Warning: argv[0] too long for loader argv buffer; argv disabled\n");
        } else {
            memcpy(ctx.prog_argv_data + offset, argv0, len);
            offset += len;
            ctx.prog_argc = 1;

            if (prog_args_start > 0 && prog_args_start < argc) {
                for (int i = prog_args_start; i < argc; i++) {
                    len = strlen(argv[i]) + 1;
                    if (offset + len > sizeof(ctx.prog_argv_data)) {
                        fprintf(stderr, "Warning: program arguments truncated to %zu bytes\n",
                                sizeof(ctx.prog_argv_data));
                        break;
                    }

                    memcpy(ctx.prog_argv_data + offset, argv[i], len);
                    offset += len;
                    ctx.prog_argc++;
                }
            }
        }
    }

    /* Handle -t auto: broadcast VERS to discover dcload/kosload on the network */
    if (ctx.device_name && strcmp(ctx.device_name, "auto") == 0) {
        ctx.device_name = NULL;
        ctx.hostname = NULL;

        printf("Scanning the network...\n");
        const char *found_ip = discover_network_device();
        if (found_ip) {
            printf("Found at %s\n", found_ip);
            ctx.hostname = found_ip;
        } else {
            fprintf(stderr, "No devices found on the network\n");
            fprintf(stderr, "You can also specify the IP directly with -t <ip>\n");
            return 1;
        }
    }

    /* Select transport */
    if (is_serial_device(ctx.device_name)) {
        ctx.transport = &serial_transport_ops;
    } else if (ctx.hostname) {
        ctx.transport = &network_transport_ops;
    } else {
        fprintf(stderr, "Error: specify -t <device|ip|auto>\n");
        return 1;
    }

    /* Initialize transport */
    uint64_t phase_start = ctx.time_ops->time_usec();
    if (ctx.transport->init(&ctx) != 0) {
        fprintf(stderr, "Failed to initialize %s transport\n", ctx.transport->name);
        return 1;
    }
    diag_phase(&ctx, "transport init", ctx.time_ops->time_usec() - phase_start);
    if (ctx.diagnostics_enabled && strcmp(ctx.transport->name, "network") == 0) {
        printf("[diag] network: adapter=0x%04x legacy=%d fast=%d fifo_delay=%u us/%u packets\n",
               ctx.installed_adapter, ctx.legacy_mode, ctx.fast_mode,
               ctx.rx_fifo_delay, ctx.rx_fifo_delay_count);
    }

    /* Detect target endianness from version string (gc-load = BE, dc-load = LE) */
    ctx.target_big_endian = (strncmp(ctx.remote_version_string, "gc-load-", 8) == 0 ||
                             strncmp(ctx.remote_version_string, "gcload-", 7) == 0);

    /* Firmware update if requested */
    if (!ctx.skip_update || ctx.firmware_path) {
        phase_start = ctx.time_ops->time_usec();
        int updated = auto_update_firmware(&ctx);
        diag_phase(&ctx, "firmware update check", ctx.time_ops->time_usec() - phase_start);
        if (updated < 0) {
            fprintf(stderr, "Firmware update failed\n");
            ctx.transport->shutdown(&ctx);
            return 1;
        }
    }

    /* CDFS redirection requires the console/fileserver loop to serve
     * read requests from the DC.  Force console on if -i was specified,
     * matching legacy dc-tool-ip behaviour. */
    if (ctx.cdfs_enabled && !ctx.console_enabled)
        ctx.console_enabled = 1;

    if (ctx.console_enabled)
        printf("Console enabled\n");
    if (ctx.use_chroot && ctx.chroot_path)
        printf("Chrooting to <%s>\n", ctx.chroot_path);

    /* Sync RTC if requested — works standalone or with any command.
     * The DC BIOS expects local time in the AICA RTC, not UTC,
     * so add the host's UTC offset (tm_gmtoff) to the timestamp. */
    if (ctx.rtc_sync && transport_can_set_rtc(ctx.transport)) {
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        time_t local_time = now + lt->tm_gmtoff;
        phase_start = ctx.time_ops->time_usec();
        ctx.transport->set_rtc(&ctx, (uint32_t)local_time);
        diag_phase(&ctx, "RTC sync", ctx.time_ops->time_usec() - phase_start);
    }

    int ret = 0;
    switch (command) {
    case 'x':
        if (!filename) { fprintf(stderr, "No filename specified\n"); ret = 1; break; }
        ctx.load_address = upload(&ctx, filename, ctx.load_address);
        if (ctx.load_address == 0) { ret = 1; break; }
        ctx.loaded_binary_path = filename;
        if (ctx.gdb_enabled && (ctx.console_enabled || ctx.dumb_terminal)) {
            phase_start = ctx.time_ops->time_usec();
            if (gdb_init(&ctx, NET_GDB_PORT) != 0) {
                ret = 1;
                break;
            }
            diag_phase(&ctx, "GDB relay init", ctx.time_ops->time_usec() - phase_start);
        }
        phase_start = ctx.time_ops->time_usec();
        ret = execute_command(&ctx, ctx.load_address);
        diag_phase(&ctx, "execute command", ctx.time_ops->time_usec() - phase_start);
        if (ret == 0 && (ctx.console_enabled || ctx.dumb_terminal)) {
            if (ctx.diagnostics_enabled)
                printf("[diag] entering console at %.3f ms\n",
                       diag_ms(ctx.time_ops->time_usec() - ctx.diagnostics_start_usec));
            phase_start = ctx.time_ops->time_usec();
            ret = do_console(&ctx);
            diag_phase(&ctx, "console session", ctx.time_ops->time_usec() - phase_start);
        }
        break;
    case 'u':
        if (!filename) { fprintf(stderr, "No filename specified\n"); ret = 1; break; }
        upload(&ctx, filename, ctx.load_address);
        break;
    case 'd':
        if (!filename) { fprintf(stderr, "No filename specified\n"); ret = 1; break; }
        if (!ctx.download_size) {
            fprintf(stderr, "Must specify size with -s for download\n");
            ret = 1;
            break;
        }
        phase_start = ctx.time_ops->time_usec();
        ret = download(&ctx, filename, ctx.load_address, ctx.download_size);
        diag_phase(&ctx, "download command", ctx.time_ops->time_usec() - phase_start);
        break;
    case 'r':
        if (transport_can_reset(ctx.transport))
            ret = ctx.transport->reset(&ctx);
        break;
    default:
        if (!ctx.rtc_sync) {
            usage();
            ret = 1;
        }
        break;
    }

    diag_summary(&ctx);

    phase_start = ctx.time_ops->time_usec();
    ctx.transport->shutdown(&ctx);
    diag_phase(&ctx, "transport shutdown", ctx.time_ops->time_usec() - phase_start);
    return ret;
}
