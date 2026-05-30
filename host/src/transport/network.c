/* host/src/transport/network.c */
/*
 * Network transport implementation for kostool.
 *
 * Ported from dcload-ip/host-src/tool/dc-tool.c.
 * Implements the dcload-ip v2 UDP protocol with version negotiation,
 * adaptive FIFO pacing, packet recovery, and retry limits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <kosload/protocol.h>
#include <kosload/strutil.h>
#include <kostool/gdb.h>
#include <kostool/transport.h>
#include <kostool/platform.h>
#include <kostool/dedup.h>

/* Host-side pacing.  Some adapters drop packets if the host sends a long
 * burst with no pause.  After N packets, sleep for the matching delay. */
#define BBA_RX_FIFO_DELAY_TIME        1800 /* microseconds */
#define BBA_RX_FIFO_DELAY_COUNT       10   /* packets per burst */
#define LAN_RX_FIFO_DELAY_TIME        1250 /* microseconds */
#define LAN_RX_FIFO_DELAY_COUNT       1    /* packets per burst */
#define W5500_RX_FIFO_DELAY_TIME      7000 /* microseconds */
#define W5500_RX_FIFO_DELAY_COUNT     3    /* packets per burst */
#define W5500_BULK_RX_FIFO_DELAY_TIME 9000 /* microseconds */
#define W5500_UPLOAD_WINDOW_SIZE      (32 * 1024)
#define PS2_BBA_RX_FIFO_DELAY_TIME    1800 /* microseconds */
#define PS2_BBA_RX_FIFO_DELAY_COUNT   10   /* packets per burst */

#define DEFAULT_RX_FIFO_DELAY   (NET_PACKET_TIMEOUT_USEC / 51)
#define DEFAULT_RX_FIFO_COUNT   15

/* Legacy dcload adapter model IDs (octal).
 * Modern kosload uses ADAPTER_* constants from protocol.h. */
#define LEGACY_BBA_MODEL   0400
#define LEGACY_LAN_MODEL   0300

/* Retry limits to prevent infinite loops on persistent failures */
#define MAX_CMD_RETRIES            20       /* max retries for command ack (LOADBIN, DONEBIN, etc.) */
#define MAX_RECV_REREQUESTS        10       /* max full passes over recv bitmap */
#define NET_HANDSHAKE_TIMEOUT_USEC 10000000 /* 10s to find loader at static IP */

/* Wii download (SENDBINQ) tuning.  IOS replies to memory-read requests in
 * single-digit milliseconds when the chunk lands; on a dropped chunk the
 * generic 250 ms wait used to be pure dead time.  Use a short 50 ms
 * timeout for the first N rerequest passes (fast recovery), then fall
 * back to the legacy 250 ms for the final retry so a genuinely slow
 * reply still has time to land.  DC/GC/PS2 with hardware NICs don't have
 * the same asymmetry, so they keep the original 250 ms throughout. */
#define WII_RECV_REREQUEST_TIMEOUT_USEC 50000 /* 50 ms */
#define WII_RECV_FAST_REREQUESTS        10
#define WII_RECV_SLOW_REREQUESTS        1

/* Drain delay after a LOADBIN retry to let stale in-flight PARTBINs expire.
 * This mitigates silent data corruption from attempt N-1's packets arriving
 * during attempt N's transfer. */
#define LOADBIN_DRAIN_DELAY_USEC    (NET_PACKET_TIMEOUT_USEC * 2)

/* Adaptive pacing: increase FIFO delay when recovery reports misses */
#define PACING_INCREASE_FACTOR    2     /* multiply delay on miss */
#define PACING_MAX_DELAY_USEC     50000 /* 50ms cap */

/* Auto-fast threshold: transfers with this many chunks or fewer skip FIFO
 * pacing automatically.  Empirically, 23-chunk (32KB) transfers have zero
 * loss even without pacing, while 205+ chunk transfers start dropping.
 * 50 chunks (~72KB) gives comfortable headroom. */
#define AUTO_FAST_CHUNK_THRESHOLD   50

/* Uncomment to enable per-transfer statistics output */
// #define NET_TRANSFER_STATS

static double diag_ms(uint64_t usec) {
    return (double)usec / 1000.0;
}

static double diag_mib_per_sec(uint64_t bytes, uint64_t usec) {
    if(usec == 0)
        return 0.0;
    return ((double)bytes * 1000000.0) / ((double)usec * 1024.0 * 1024.0);
}

static int adapter_is_w5500(uint32_t adapter) {
    return adapter == ADAPTER_DC_W5500 || adapter == ADAPTER_GC_W5500;
}

static int adapter_is_spi(uint32_t adapter) {
    return adapter_is_w5500(adapter) || adapter == ADAPTER_GC_ENC;
}

/* The Wii has no NIC FIFO — every incoming UDP datagram is drained by a
 * synchronous IOS IPC recvfrom — and IOS-side handling is fussier about
 * non-4-byte-aligned UDP payloads after the first round of traffic.  These
 * call sites pad outbound commands and disable host-side auto_fast.  Keyed
 * on the VERS name rather than installed_adapter so it works even when the
 * post-VERS adapter-ID fallback hasn't recognised ADAPTER_WII_LAN_WIFI. */
static int is_wii_loader(const kostool_context_t *ctx) {
    return ctx && strncmp(ctx->remote_version_string, "wii-load-ip ", 12) == 0;
}

/* Initial streaming-wait timeout for the SENDBIN/SENDBINQ download path.
 * Wii: chunks arrive fast or not at all, so we lower the wait that gates
 * "give up streaming and start re-requesting".  Other consoles keep the
 * legacy 250 ms. */
static uint32_t recv_data_timeout_usec(const kostool_context_t *ctx) {
    return ctx->installed_adapter == ADAPTER_WII_LAN_WIFI ? WII_RECV_REREQUEST_TIMEOUT_USEC
                                                          : NET_PACKET_TIMEOUT_USEC;
}

/* Per-pass timeout for the re-request loop.  Wii uses fast 50 ms passes
 * for the first WII_RECV_FAST_REREQUESTS attempts, then falls back to
 * the conservative 250 ms for the last WII_RECV_SLOW_REREQUESTS.  All
 * other consoles stay on the unchanged 250 ms for every pass. */
static uint32_t recv_data_rerequest_timeout_usec(const kostool_context_t *ctx, int pass) {
    if(ctx->installed_adapter == ADAPTER_WII_LAN_WIFI && pass < WII_RECV_FAST_REREQUESTS)
        return WII_RECV_REREQUEST_TIMEOUT_USEC;
    return NET_PACKET_TIMEOUT_USEC;
}

/* Total rerequest-pass budget.  Wii gets the extra slow pass on top of
 * the fast batch; everyone else stays at MAX_RECV_REREQUESTS = 10. */
static int recv_data_max_rerequests(const kostool_context_t *ctx) {
    if(ctx->installed_adapter == ADAPTER_WII_LAN_WIFI)
        return WII_RECV_FAST_REREQUESTS + WII_RECV_SLOW_REREQUESTS;
    return MAX_RECV_REREQUESTS;
}

/*
 * Decide whether a freshly-received packet is the ACK we're waiting for.
 *
 * Plain memcmp on the command id isn't enough on a lossy link: a delayed
 * duplicate of a previous LOADBIN/EXEC/SETRTC reply (same 4-byte id, but
 * for the prior request's addr/size) would otherwise satisfy the match
 * and either be misread as the current ACK or — more commonly — consume
 * the recv slot for this attempt, burning the full 250 ms timeout before
 * the next retry.
 *
 * Filter on addr/size for the commands that have a deterministic echo
 * (LOADBIN ACK echoes back load_address + load_size; EXEC ACK echoes
 * back the exec address + flags; SETRTC echoes back the timestamp).
 * Other commands (DONEBIN, VERSION, ...) are still id-matched only
 * because their replies don't carry a meaningful addr/size.
 */
static int command_ack_matches(const uint8_t *buffer, int rv, const char expected[4], uint32_t addr,
                               uint32_t size) {
    const net_command_t *reply;

    if(rv < NET_COMMAND_LEN || memcmp(buffer, expected, 4) != 0)
        return 0;

    reply = (const net_command_t *)buffer;
    if(memcmp(expected, NET_CMD_LOADBIN, 4) == 0 || memcmp(expected, NET_CMD_EXECUTE, 4) == 0 ||
       memcmp(expected, NET_CMD_SETRTC, 4) == 0)
        return ntohl(reply->address) == addr && ntohl(reply->size) == size;

    return 1;
}

/* ===== Low-level UDP helpers ===== */

/*
 * Send a command packet: 4-byte command ID + 4-byte address + 4-byte size +
 * optional data. All multi-byte fields in network byte order.
 */
static int send_cmd(kostool_context_t *ctx, const char cmd[4], uint32_t addr, uint32_t size,
                    const uint8_t *data, uint32_t dsize) {
    uint8_t  buf[2048];
    uint32_t wire_size;
    uint32_t tmp;

    /* Reserve 3 extra bytes for the Wii 4-byte tail-pad below. */
    if(dsize > sizeof(buf) - 12 - 3)
        dsize = sizeof(buf) - 12 - 3;

    memcpy(buf, cmd, 4);
    tmp = htonl(addr);
    memcpy(buf + 4, &tmp, 4);
    tmp = htonl(size);
    memcpy(buf + 8, &tmp, 4);
    if(data && dsize > 0)
        memcpy(buf + 12, data, dsize);

    wire_size = 12 + dsize;
    /* Wii: pad the wire size up to a multiple of 4.  HW-confirmed required
     * for repeat -t <ip> uploads: without this, the second kos-tool
     * invocation gets "No network loader response" even though the first
     * succeeded.  The actual symptom is in IOS-side recvfrom handling of
     * trailing non-word bytes (e.g. the 30-byte EXEC + 14-byte argv that
     * fires on every run).  Header is already 4-byte aligned; the pad
     * only fires when dsize is non-word and zero-fills the slop. */
    if(is_wii_loader(ctx)) {
        uint32_t padded = (wire_size + 3) & ~3;
        if(padded > wire_size)
            memset(buf + wire_size, 0, padded - wire_size);
        wire_size = padded;
    }

    int ret = ctx->socket_ops->send(ctx->global_socket, buf, wire_size);
    if(ret < 0)
        return -1;
    /* During a dedup capture, save outbound packets so dedup_try_replay()
     * can resend them on a retransmit (only the Wii retransmits; wired
     * consoles wait forever, so this is dormant there, not dead).
     *
     * But never save SENDBINQ/SENDBIN: replays must have no side effects,
     * and these *pull* guest memory — replaying one makes the guest re-send
     * its whole buffer.  read()'s LOADBIN/PARTBIN *pushes* to a fixed
     * address, so it's safe to replay; the write() pull is not. */
    if(memcmp(cmd, NET_CMD_SENDBINQ, 4) != 0 && memcmp(cmd, NET_CMD_SENDBIN, 4) != 0)
        dedup_capture_pkt(buf, wire_size);
    return 0;
}

/*
 * Receive a response packet with timeout.
 * Returns bytes received, or -1 on timeout.
 */
static int recv_resp(kostool_context_t *ctx, uint8_t *buffer, size_t buffer_size, uint32_t timeout_usec) {
    uint64_t start = ctx->time_ops->time_usec();

    while((ctx->time_ops->time_usec() - start) < timeout_usec) {
        int rv = ctx->socket_ops->recv(ctx->global_socket, buffer, buffer_size);
        if(rv > 0)
            return rv;
    }

    return -1;
}

/*
 * Drain any stale packets from the receive buffer.
 * Call after a LOADBIN retry to discard in-flight PARTBINs from the
 * previous attempt that could corrupt the new transfer.
 */
static void drain_rx_buffer(kostool_context_t *ctx, uint32_t drain_usec) {
    uint8_t  discard[2048];
    uint64_t start = ctx->time_ops->time_usec();

    while((ctx->time_ops->time_usec() - start) < drain_usec) {
        ctx->socket_ops->recv(ctx->global_socket, discard, sizeof(discard));
    }
}

/*
 * Send a command and wait for a matching response, with retry limit.
 * Returns 0 on success (buffer contains the response), -1 on failure.
 *
 * Within each 250 ms attempt window, this drains every datagram that
 * arrives and filters with command_ack_matches() — instead of stopping
 * at the first packet recv_resp() hands back.  Critical on lossy links
 * (Wii internal Wi-Fi especially): a single stray duplicate from a
 * prior command used to consume the recv slot, fail the memcmp, and
 * waste the rest of the timeout before the next retry, which is what
 * stretched LOADBIN/DONEBIN into 5–15-retry territory and made EXEC
 * fail outright when the channel had any backlog.
 */
static int send_and_wait(kostool_context_t *ctx, const char cmd[4], uint32_t addr, uint32_t size,
                         const uint8_t *data, uint32_t dsize, uint8_t *buffer, size_t buffer_size,
                         const char expected[4]) {
    for(int attempt = 0; attempt < MAX_CMD_RETRIES; attempt++) {
        uint64_t start;

        send_cmd(ctx, cmd, addr, size, data, dsize);
        start = ctx->time_ops->time_usec();
        while((ctx->time_ops->time_usec() - start) < NET_PACKET_TIMEOUT_USEC) {
            int rv = ctx->socket_ops->recv(ctx->global_socket, buffer, buffer_size);
            if(rv <= 0)
                continue;
            if(command_ack_matches(buffer, rv, expected, addr, size))
                return 0;
            /* Wrong-id or wrong-addr/size packet: drop it and keep
             * looking within this attempt's window. */
        }
        if(!ctx->fast_mode && attempt > 0 && (attempt % 5) == 0)
            fprintf(stderr, "send_and_wait: retrying %s (attempt %d/%d)...\n", cmd, attempt + 1,
                    MAX_CMD_RETRIES);
    }
    fprintf(stderr, "send_and_wait: %s failed after %d attempts\n", cmd, MAX_CMD_RETRIES);
    return -1;
}

static bool network_remote_supports_capabilities(const kostool_context_t *ctx) {
    return strncmp(ctx->remote_version_string, "dc-load-ip ", 11) == 0 ||
           strncmp(ctx->remote_version_string, "gc-load-ip ", 11) == 0 ||
           strncmp(ctx->remote_version_string, "ps2-load-ip ", 12) == 0 ||
           strncmp(ctx->remote_version_string, "wii-load-ip ", 12) == 0;
}

static int query_capabilities(kostool_context_t *ctx, uint32_t *capabilities) {
    uint8_t buffer[2048];

    for(int attempt = 0; attempt < 2; attempt++) {
        uint64_t start = ctx->time_ops->time_usec();

        send_cmd(ctx, NET_CMD_CAPABILITIES, ctx->kostool_capabilities, 0, NULL, 0);

        while((ctx->time_ops->time_usec() - start) < NET_PACKET_TIMEOUT_USEC) {
            uint64_t elapsed = ctx->time_ops->time_usec() - start;
            uint32_t remaining = (uint32_t)(NET_PACKET_TIMEOUT_USEC - elapsed);
            int rv;

            if(remaining == 0)
                break;

            rv = recv_resp(ctx, buffer, sizeof(buffer), remaining);
            if(rv <= 0)
                break;

            if(memcmp(buffer, NET_CMD_CAPABILITIES, 4) != 0)
                continue;

            *capabilities = ntohl(((net_command_t *)buffer)->address);
            return 0;
        }
    }

    return -1;
}

/*
 * Encode kos-tool version into a uint32 for handshake:
 * (major<<16)|(minor<<8)|patch Returns 0 if force_legacy is set.
 *
 * send_cmd() already applies htonl() to the address field, so we must NOT
 * apply htonl() here — that would double byte-swap and corrupt the version.
 * Version must be >= 2.0.0 so firmware's DCTOOL_MAJOR >= 2 for v2 features
 * (1440-byte payloads, v2 syscall port, "DC02" write commands).
 *
 * 3.0.0 is the version that introduced the KSQ0 syscall-sequencing trailer
 * and per-seq dedup cache (see include/kosload/protocol.h).  Clients gate
 * their bounded-wait retransmit on tool_version >= 0x00030000; against an
 * older host they emit the harmless trailer but keep the legacy
 * wait-forever semantics, since the legacy host has no dedup cache and a
 * retransmit would re-run non-idempotent syscalls.
 *
 * Sourced from version.mk via the generated <kosload/version.h> so the
 * VERS-encoded protocol version tracks the release version automatically.
 */
#include <kosload/version.h>
static uint32_t make_encoded_version(int force_legacy) {
    if(force_legacy)
        return 0;
    return ((uint32_t)KOSLOAD_VERSION_MAJOR << 16) | ((uint32_t)KOSLOAD_VERSION_MINOR << 8) |
           ((uint32_t)KOSLOAD_VERSION_PATCH);
}

/*
 * Version negotiation and adapter detection.
 * Dual-socket strategy: legacy port 31313 and v2 port 53535.
 * The DC will respond on whichever port it understands.
 */
static int prepare_comms(kostool_context_t *ctx) {
    if(ctx->installed_adapter != 0)
        return 0; /* Already initialized */

    uint32_t encoded_ver = make_encoded_version(ctx->force_legacy);
    uint8_t buffer[2048];
    uint64_t start = ctx->time_ops->time_usec();
    int flip = 0;
    int recv_len = -1;

    /* Start with v2 socket unless forcing legacy */
    if(ctx->force_legacy)
        ctx->global_socket = ctx->socket_legacy;
    else
        ctx->global_socket = ctx->socket_fd;

    /* Send version commands, alternating between v2 and legacy sockets. */
    while((ctx->time_ops->time_usec() - start) < NET_HANDSHAKE_TIMEOUT_USEC) {
        send_cmd(ctx, NET_CMD_VERSION, encoded_ver, 0, NULL, 0);
        recv_len = recv_resp(ctx, buffer, sizeof(buffer), NET_PACKET_TIMEOUT_USEC);
        if(recv_len > 0 && memcmp(buffer, NET_CMD_VERSION, 4) == 0)
            goto got_version;

        /* Try the other socket */
        flip ^= 1;
        ctx->global_socket = flip ? ctx->socket_legacy : ctx->socket_fd;
    }

    fprintf(stderr, "No network loader response from %s\n", ctx->hostname);
    return -1;

got_version:
    /* Close the socket we don't need */
    if(ctx->global_socket == ctx->socket_fd) {
        ctx->socket_ops->close(ctx->socket_legacy);
        ctx->socket_legacy = -1;
    } else {
        ctx->socket_ops->close(ctx->socket_fd);
        ctx->socket_fd = -1;
    }

    /* Extract the adapter type from the VERSION response. */
    net_command_t *cmd = (net_command_t *)buffer;
    uint32_t       raw_addr = ntohl(cmd->address);
    ctx->installed_adapter = raw_addr;
    ctx->remote_capabilities = 0;

    /* Store version string for firmware update decisions */
    if(recv_len > NET_COMMAND_LEN) {
        compat_str_copy_bytes(ctx->remote_version_string, sizeof(ctx->remote_version_string), cmd->data,
                              (size_t)(recv_len - NET_COMMAND_LEN));
    } else {
        ctx->remote_version_string[0] = '\0';
    }

    /* Modern network loaders can answer a dedicated capability query.
     * Legacy dcload-ip does not advertise CAPS, so only probe loaders
     * whose version string identifies them as kosload. */
    if(network_remote_supports_capabilities(ctx))
        query_capabilities(ctx, &ctx->remote_capabilities);

    /* Accept legacy octal IDs and modern ADAPTER_* IDs.
     * Old loaders sometimes report octal 0400/0300 as decimal 256/192. */
    int is_bba = (ctx->installed_adapter == LEGACY_BBA_MODEL || ctx->installed_adapter == ADAPTER_DC_BBA ||
                  ctx->installed_adapter == ADAPTER_GC_BBA || ctx->installed_adapter == ADAPTER_PS2_BBA);
    int is_lan = (ctx->installed_adapter == LEGACY_LAN_MODEL || ctx->installed_adapter == ADAPTER_DC_LAN);
    int is_w5500 = adapter_is_w5500(ctx->installed_adapter);
    int is_spi = adapter_is_spi(ctx->installed_adapter);

    printf("%s\n", ctx->remote_version_string);

    if(ctx->force_legacy) {
        printf("Forcing 1024-byte payloads...\n");
        ctx->legacy_mode = 1;
    }

    if(is_bba) {
        if(!ctx->fast_mode) {
            ctx->rx_fifo_delay = BBA_RX_FIFO_DELAY_TIME;
            ctx->rx_fifo_delay_count = BBA_RX_FIFO_DELAY_COUNT;
        } else {
            ctx->rx_fifo_delay = 0;
        }
    } else if(is_lan) {
        if(!ctx->fast_mode) {
            ctx->rx_fifo_delay = LAN_RX_FIFO_DELAY_TIME;
            ctx->rx_fifo_delay_count = LAN_RX_FIFO_DELAY_COUNT;
        } else {
            ctx->rx_fifo_delay = 0;
        }
    } else if(is_w5500) {
        ctx->rx_fifo_delay = W5500_RX_FIFO_DELAY_TIME;
        ctx->rx_fifo_delay_count = W5500_RX_FIFO_DELAY_COUNT;
    } else if(is_spi) {
        // if (!ctx->fast_mode) {
        ctx->rx_fifo_delay = DEFAULT_RX_FIFO_DELAY;
        ctx->rx_fifo_delay_count = DEFAULT_RX_FIFO_COUNT;
        // } else {
        //     ctx->rx_fifo_delay = 0;
        // }
    } else if(is_wii_loader(ctx)) {
        /* Wii reports ADAPTER_WII_LAN_WIFI (0x0E58); none of the DC/GC/PS2
         * is_bba/is_lan/is_spi predicates match, so without this branch
         * we'd fall through and clobber the adapter ID to ADAPTER_DC_BBA.
         * Keep the reported ID and apply Wii's structural IOS-IPC pacing:
         * 4000us after every chunk, not relaxed by -F/fast_mode (legacy
         * 1024-byte payloads). */
        ctx->legacy_mode = 1;
        ctx->rx_fifo_delay = 4000;
        ctx->rx_fifo_delay_count = 1;
    } else {
        ctx->installed_adapter = ADAPTER_DC_BBA;
        ctx->legacy_mode = 1;
        ctx->rx_fifo_delay = DEFAULT_RX_FIFO_DELAY;
        ctx->rx_fifo_delay_count = DEFAULT_RX_FIFO_COUNT;
    }

    return 0;
}

/* ===== Transport interface implementation ===== */

static int network_init(kostool_context_t *ctx) {
    if(!ctx->hostname) {
        fprintf(stderr, "No hostname specified (use -t)\n");
        return -1;
    }

    /* Initialize socket subsystem (WinSock on Windows, no-op on POSIX) */
    if(!ctx->sockets_initialized) {
        if(ctx->socket_ops->init && ctx->socket_ops->init() != 0) {
            fprintf(stderr, "Socket initialization failed\n");
            return -1;
        }
        ctx->sockets_initialized = 1;
    }

    /* Parse hostname:port if present */
    char *host_copy = strdup(ctx->hostname);
    uint16_t port = NET_DEFAULT_PORT;
    char *colon = strchr(host_copy, ':');
    if(colon) {
        *colon = '\0';
        port = (uint16_t)atoi(colon + 1);
    }
    ctx->port = port;

    /* Create both sockets: legacy (31313) and v2 (user port or 53535) */
    ctx->socket_fd = ctx->socket_ops->udp_socket();
    ctx->socket_legacy = ctx->socket_ops->udp_socket();
    if(ctx->socket_fd < 0 || ctx->socket_legacy < 0) {
        fprintf(stderr, "Failed to create UDP sockets\n");
        free(host_copy);
        return -1;
    }

    /* Connect legacy socket first */
    int rc;
    rc = ctx->socket_ops->connect(ctx->socket_legacy, host_copy, NET_LEGACY_PORT);
    if(rc < 0) {
        fprintf(stderr, "Failed to connect to %s:%d (legacy)\n", host_copy, NET_LEGACY_PORT);
        free(host_copy);
        return -1;
    }

    /* Connect v2 socket */
    rc = ctx->socket_ops->connect(ctx->socket_fd, host_copy, port);
    if(rc < 0) {
        fprintf(stderr, "Failed to connect to %s:%d\n", host_copy, port);
        free(host_copy);
        return -1;
    }

    free(host_copy);

    /* Set both sockets non-blocking */
    ctx->socket_ops->set_nonblocking(ctx->socket_fd);
    ctx->socket_ops->set_nonblocking(ctx->socket_legacy);

    /* Initialize FIFO delay defaults */
    ctx->rx_fifo_delay = DEFAULT_RX_FIFO_DELAY;
    ctx->rx_fifo_delay_count = DEFAULT_RX_FIFO_COUNT;

    /* Run VERS handshake now so remote_version_string and
     * remote_capabilities are available for auto-update. */
    printf("Connecting to %s...\n", ctx->hostname);
    fflush(stdout);
    if(prepare_comms(ctx) != 0)
        return -1;

    return 0;
}

static void network_shutdown(kostool_context_t *ctx) {
    /* Close GDB sockets */
    if(ctx->gdb_enabled)
        gdb_shutdown(ctx);

    /* Close main sockets.
     * global_socket aliases either socket_fd or socket_legacy after
     * prepare_comms(), so clear the matching one to avoid double-close. */
    if(ctx->global_socket > 0) {
        if(ctx->global_socket == ctx->socket_fd)
            ctx->socket_fd = -1;
        else if(ctx->global_socket == ctx->socket_legacy)
            ctx->socket_legacy = -1;
        ctx->socket_ops->close(ctx->global_socket);
        ctx->global_socket = -1;
    }
    if(ctx->socket_fd > 0) {
        ctx->socket_ops->close(ctx->socket_fd);
        ctx->socket_fd = -1;
    }
    if(ctx->socket_legacy > 0) {
        ctx->socket_ops->close(ctx->socket_legacy);
        ctx->socket_legacy = -1;
    }

    gdb_socket_runtime_cleanup(ctx);
}

/*
 * Send binary data to DC via UDP.
 * Protocol:
 * 1. Send LOADBIN with dest_addr and size, wait for ack
 * 2. Send data in PARTBIN chunks (1024 or 1440 bytes) with adaptive FIFO pacing
 * 3. Send DONEBIN, wait for ack
 * 4. If ack indicates missing data, resend those chunks (with retry limit)
 *
 * Reliability improvements over legacy dc-tool:
 * - Retry limits on all loops to prevent hangs
 * - Drain delay after LOADBIN retry to discard stale in-flight packets
 * - Adaptive pacing: increase FIFO delay when packet loss detected
 */
static int network_send_data_once(kostool_context_t *ctx, const uint8_t *data, uint32_t dest_addr,
                                  uint32_t size) {
    uint8_t  buffer[2048];
    uint64_t diag_total_start = 0;
    uint64_t diag_loadbin_done = 0;
    uint64_t diag_stream_start = 0;
    uint64_t diag_stream_end = 0;
    uint64_t diag_pacing_wait = 0;
    uint64_t diag_post_stream_wait = 0;
    uint32_t diag_total_chunks = 0;
    uint32_t diag_recovery_requests = 0;
    uint32_t diag_retransmitted_bytes = 0;
    uint32_t diag_loadbin_retries = 0;

    if(!size)
        return -1;

    if(ctx->diagnostics_enabled)
        diag_total_start = ctx->time_ops->time_usec();

#ifdef NET_TRANSFER_STATS
    uint32_t stat_total_chunks = 0;
    uint32_t stat_recovery_rounds = 0;
    uint32_t stat_retransmitted_bytes = 0;
    uint32_t stat_loadbin_retries = 0;
    uint64_t stat_start_time = ctx->time_ops->time_usec();
#endif

    /* Send LOADBIN and wait for ack */
    if(send_and_wait(ctx, NET_CMD_LOADBIN, dest_addr, size, NULL, 0, buffer, sizeof(buffer),
                     NET_CMD_LOADBIN) != 0) {
        fprintf(stderr, "send_data: LOADBIN failed, giving up\n");
        return -1;
    }
    if(ctx->diagnostics_enabled)
        diag_loadbin_done = ctx->time_ops->time_usec();

    /* Send data in chunks with adaptive pacing */
    uint32_t chunk_size = ctx->legacy_mode ? NET_LEGACY_PAYLOAD_SIZE : NET_PAYLOAD_SIZE;
    uint32_t pacing_count = 0;
    uint32_t current_fifo_delay = ctx->rx_fifo_delay;

    uint32_t num_chunks = (size + chunk_size - 1) / chunk_size;
    diag_total_chunks = num_chunks;
#ifdef NET_TRANSFER_STATS
    stat_total_chunks = num_chunks;
#endif

    /* Auto-fast: skip FIFO pacing for small transfers (e.g. syscall I/O).
     * These fit in the BBA's RX FIFO without loss, so pacing is pure waste.
     * The Wii has no such FIFO (synchronous IOS IPC per datagram), so even
     * a few-chunk burst can overrun it — never auto_fast that path. */
    int auto_fast = (!adapter_is_spi(ctx->installed_adapter) && !is_wii_loader(ctx) &&
                     num_chunks <= AUTO_FAST_CHUNK_THRESHOLD);
    if(auto_fast)
        current_fifo_delay = 0;

    if(ctx->diagnostics_enabled)
        diag_stream_start = ctx->time_ops->time_usec();

    for(uint32_t offset = 0; offset < size; offset += chunk_size) {
        uint32_t remaining = size - offset;
        uint32_t send_size = (remaining >= chunk_size) ? chunk_size : remaining;

        send_cmd(ctx, NET_CMD_PARTBIN, dest_addr + offset, send_size, data + offset, send_size);

        /* FIFO pacing: pause periodically to let DC empty its RX buffer */
        pacing_count++;
        if(ctx->rx_fifo_delay_count && pacing_count >= ctx->rx_fifo_delay_count) {
            uint64_t start = ctx->time_ops->time_usec();
            while((ctx->time_ops->time_usec() - start) < current_fifo_delay)
                ;
            if(ctx->diagnostics_enabled)
                diag_pacing_wait += ctx->time_ops->time_usec() - start;
            pacing_count = 0;
        }
    }
    if(ctx->diagnostics_enabled)
        diag_stream_end = ctx->time_ops->time_usec();

    /* Brief delay before DONEBIN to ensure data packets are sent */
    if(!ctx->fast_mode && !auto_fast) {
        uint64_t start = ctx->time_ops->time_usec();
        while((ctx->time_ops->time_usec() - start) < NET_PACKET_TIMEOUT_USEC / 10)
            ;
        if(ctx->diagnostics_enabled)
            diag_post_stream_wait += ctx->time_ops->time_usec() - start;
    }

    /* Send DONEBIN and handle packet recovery with retry limit */
    int loadbin_retries = 0;

    if(send_and_wait(ctx, NET_CMD_DONEBIN, 0, 0, NULL, 0, buffer, sizeof(buffer), NET_CMD_DONEBIN) != 0) {
        /* DONEBIN failed — retry the entire LOADBIN sequence */
        fprintf(stderr, "send_data: DONEBIN failed, retrying LOADBIN...\n");

        /* Drain stale packets before retrying */
        drain_rx_buffer(ctx, LOADBIN_DRAIN_DELAY_USEC);
#ifdef NET_TRANSFER_STATS
        stat_loadbin_retries++;
        stat_retransmitted_bytes += size;
#endif
        diag_loadbin_retries++;
        diag_retransmitted_bytes += size;

        if(send_and_wait(ctx, NET_CMD_LOADBIN, dest_addr, size, NULL, 0, buffer, sizeof(buffer),
                         NET_CMD_LOADBIN) != 0) {
            fprintf(stderr, "send_data: LOADBIN retry failed, giving up\n");
            return -1;
        }

        /* Resend all data */
        pacing_count = 0;
        for(uint32_t offset = 0; offset < size; offset += chunk_size) {
            uint32_t remaining = size - offset;
            uint32_t send_size = (remaining >= chunk_size) ? chunk_size : remaining;
            send_cmd(ctx, NET_CMD_PARTBIN, dest_addr + offset, send_size, data + offset, send_size);
            pacing_count++;
            if(ctx->rx_fifo_delay_count && pacing_count >= ctx->rx_fifo_delay_count) {
                uint64_t start = ctx->time_ops->time_usec();
                while((ctx->time_ops->time_usec() - start) < current_fifo_delay)
                    ;
                if(ctx->diagnostics_enabled)
                    diag_pacing_wait += ctx->time_ops->time_usec() - start;
                pacing_count = 0;
            }
        }

        if(!ctx->fast_mode) {
            uint64_t start = ctx->time_ops->time_usec();
            while((ctx->time_ops->time_usec() - start) < NET_PACKET_TIMEOUT_USEC / 10)
                ;
            if(ctx->diagnostics_enabled)
                diag_post_stream_wait += ctx->time_ops->time_usec() - start;
        }

        if(send_and_wait(ctx, NET_CMD_DONEBIN, 0, 0, NULL, 0, buffer, sizeof(buffer), NET_CMD_DONEBIN) != 0) {
            fprintf(stderr, "send_data: DONEBIN retry failed, giving up\n");
            return -1;
        }
    }

    /* Resend any missing chunks reported by DC.
     * No limit on recovery rounds — match legacy dc-tool-ip behavior where
     * the loop runs until all chunks are received.  Adaptive pacing increases
     * FIFO delay on each miss, so packet loss naturally converges to zero. */
    net_command_t *cmd = (net_command_t *)buffer;
    while(ntohl(cmd->size) != 0) {
        uint32_t miss_addr = ntohl(cmd->address);
        uint32_t miss_size = ntohl(cmd->size);
        uint32_t miss_off = miss_addr - dest_addr;

        /* Validate the miss range */
        if(miss_off + miss_size > size) {
            fprintf(stderr, "send_data: invalid miss range 0x%x+%u, aborting recovery\n", miss_addr,
                    miss_size);
            break;
        }

        /* Adapt pacing: packet loss means we're sending too fast.
         * Skip in fast mode — user explicitly accepted the risk. */
        if(!ctx->fast_mode && current_fifo_delay < PACING_MAX_DELAY_USEC) {
            current_fifo_delay *= PACING_INCREASE_FACTOR;
            if(current_fifo_delay > PACING_MAX_DELAY_USEC)
                current_fifo_delay = PACING_MAX_DELAY_USEC;
        }

        send_cmd(ctx, NET_CMD_PARTBIN, miss_addr, miss_size, data + miss_off, miss_size);

#ifdef NET_TRANSFER_STATS
        stat_recovery_rounds++;
        stat_retransmitted_bytes += miss_size;
#endif
        diag_recovery_requests++;
        diag_retransmitted_bytes += miss_size;

        if(send_and_wait(ctx, NET_CMD_DONEBIN, 0, 0, NULL, 0, buffer, sizeof(buffer), NET_CMD_DONEBIN) != 0) {
            /* DONEBIN failed during recovery — retry LOADBIN with drain */
            loadbin_retries++;
#ifdef NET_TRANSFER_STATS
            stat_loadbin_retries++;
#endif
            diag_loadbin_retries++;
            if(loadbin_retries > 3) {
                fprintf(stderr, "send_data: too many LOADBIN retries during "
                                "recovery, giving up\n");
                return -1;
            }

            fprintf(stderr, "send_data: DONEBIN failed during recovery, "
                            "draining and retrying...\n");
            drain_rx_buffer(ctx, LOADBIN_DRAIN_DELAY_USEC);

            if(send_and_wait(ctx, NET_CMD_LOADBIN, dest_addr, size, NULL, 0, buffer, sizeof(buffer),
                             NET_CMD_LOADBIN) != 0) {
                fprintf(stderr, "send_data: LOADBIN retry failed during recovery\n");
                return -1;
            }
            break; /* Restart from scratch would need full resend — let caller
                      retry */
        }
    }

#ifdef NET_TRANSFER_STATS
    /* Print packet loss statistics */
    uint64_t stat_elapsed = ctx->time_ops->time_usec() - stat_start_time;
    double   elapsed_ms = (double)stat_elapsed / 1000.0;
    double   throughput_kbps = (elapsed_ms > 0) ? ((double)size * 8.0) / elapsed_ms : 0;

    printf("  Transfer stats: %u bytes in %.1f ms (%.1f kbit/s)\n", size, elapsed_ms, throughput_kbps);
    printf("  Chunks: %u total, %u retransmitted (%u recovery rounds)\n", stat_total_chunks,
           stat_recovery_rounds, stat_recovery_rounds);
    if(stat_retransmitted_bytes > 0)
        printf("  Retransmitted: %u bytes (%.2f%% overhead)\n", stat_retransmitted_bytes,
               (double)stat_retransmitted_bytes * 100.0 / (double)size);
    if(stat_loadbin_retries > 0)
        printf("  LOADBIN retries: %u\n", stat_loadbin_retries);
#endif

    if(ctx->diagnostics_enabled) {
        uint64_t diag_total_usec = ctx->time_ops->time_usec() - diag_total_start;
        uint64_t diag_loadbin_usec = diag_loadbin_done - diag_total_start;
        uint64_t diag_stream_usec = diag_stream_end - diag_stream_start;
        uint64_t diag_done_usec = diag_total_usec - diag_loadbin_usec - diag_stream_usec;

        ctx->diagnostics_net_send_bytes += size;
        ctx->diagnostics_net_send_stream_usec += diag_stream_usec;
        ctx->diagnostics_net_send_total_usec += diag_total_usec;
        ctx->diagnostics_net_retransmitted_bytes += diag_retransmitted_bytes;
        ctx->diagnostics_net_recovery_requests += diag_recovery_requests;
        ctx->diagnostics_net_loadbin_retries += diag_loadbin_retries;

        printf("[diag] network upload section 0x%08x: %u bytes, %u chunks x "
               "%u, auto_fast=%d\n",
               dest_addr, size, diag_total_chunks, chunk_size, auto_fast);
        printf("[diag]   steady stream: %.3f ms, %.2f MiB/s, pacing %.3f ms, "
               "post-wait %.3f ms\n",
               diag_ms(diag_stream_usec), diag_mib_per_sec(size, diag_stream_usec), diag_ms(diag_pacing_wait),
               diag_ms(diag_post_stream_wait));
        printf("[diag]   effective: %.3f ms, %.2f MiB/s; LOADBIN %.3f ms, "
               "DONE/recovery %.3f ms\n",
               diag_ms(diag_total_usec), diag_mib_per_sec(size, diag_total_usec), diag_ms(diag_loadbin_usec),
               diag_ms(diag_done_usec));
        if(diag_retransmitted_bytes || diag_recovery_requests || diag_loadbin_retries) {
            printf("[diag]   recovery: %u retransmitted bytes, %u requests, %u "
                   "LOADBIN retries, final_delay=%u us\n",
                   diag_retransmitted_bytes, diag_recovery_requests, diag_loadbin_retries,
                   current_fifo_delay);
        }
    }

    return 0;
}

static int network_send_data(kostool_context_t *ctx, const uint8_t *data, uint32_t dest_addr, uint32_t size) {
    uint32_t offset = 0;
    uint32_t saved_fifo_delay;
    int ret = 0;

    if(!size)
        return -1;

    prepare_comms(ctx);

    if(!adapter_is_w5500(ctx->installed_adapter) || size <= W5500_UPLOAD_WINDOW_SIZE) {
        return network_send_data_once(ctx, data, dest_addr, size);
    }

    if(ctx->diagnostics_enabled) {
        printf("[diag] W5500 upload split: %u bytes into %u-byte windows\n", size, W5500_UPLOAD_WINDOW_SIZE);
    }

    saved_fifo_delay = ctx->rx_fifo_delay;

    /* Large W5500 uploads are split into windows, but each window is still
     * large enough to benefit from slightly slower FIFO pacing. Keep this
     * upload-only delay out of smaller syscall/console transfers. */
    ctx->rx_fifo_delay = W5500_BULK_RX_FIFO_DELAY_TIME;

    while(offset < size) {
        uint32_t chunk = size - offset;

        if(chunk > W5500_UPLOAD_WINDOW_SIZE)
            chunk = W5500_UPLOAD_WINDOW_SIZE;

        if(network_send_data_once(ctx, data + offset, dest_addr + offset, chunk) != 0) {
            ret = -1;
            break;
        }

        offset += chunk;
    }

    ctx->rx_fifo_delay = saved_fifo_delay;
    return ret;
}

/*
 * Receive binary data from console via UDP.
 * Protocol:
 * 1. Send SENDBIN/SENDBINQ with src_addr and size
 * 2. Receive PARTBIN packets, track which chunks arrived in a map
 * 3. On DONEBIN or timeout, re-request missing chunks
 *
 * Improvements: retry limit on re-request loop to prevent infinite loops.
 */
static int network_recv_data(kostool_context_t *ctx, uint8_t *data, uint32_t src_addr, uint32_t size,
                             int quiet) {
    uint8_t buffer[2048];
    int retval;
    uint32_t recv_timeout;
    int max_rerequests;
    uint64_t diag_total_start = 0;
    uint64_t diag_stream_start = 0;
    uint64_t diag_stream_end = 0;
    uint32_t diag_packets = 0;
    uint32_t diag_rerequests = 0;

    prepare_comms(ctx);
    recv_timeout = recv_data_timeout_usec(ctx);
    max_rerequests = recv_data_max_rerequests(ctx);

    if(ctx->diagnostics_enabled)
        diag_total_start = ctx->time_ops->time_usec();

    uint32_t chunk_size = ctx->legacy_mode ? NET_LEGACY_PAYLOAD_SIZE : NET_PAYLOAD_SIZE;
    uint32_t num_chunks = (size + chunk_size - 1) / chunk_size;

    uint8_t *map = calloc(1, num_chunks);
    if(!map)
        return -1;

    /* Request the binary */
    const char *cmd_id = quiet ? NET_CMD_SENDBINQ : NET_CMD_SENDBIN;
    if(ctx->diagnostics_enabled)
        diag_stream_start = ctx->time_ops->time_usec();
    send_cmd(ctx, cmd_id, src_addr, size, NULL, 0);

    /* Receive packets */
    int packets = 0;
    uint64_t start = ctx->time_ops->time_usec();

    while((ctx->time_ops->time_usec() - start) < recv_timeout && packets < (int)(num_chunks + 1)) {
        memset(buffer, 0, 2048);

        while((retval = ctx->socket_ops->recv(ctx->global_socket, buffer, 2048)) < 0 &&
              (ctx->time_ops->time_usec() - start) < recv_timeout)
            ;

        if(retval > 0) {
            start = ctx->time_ops->time_usec();
            net_command_t *pkt = (net_command_t *)buffer;

            if(memcmp(pkt->id, NET_CMD_SENDBIN, 4) == 0) {
                uint32_t pkt_addr = ntohl(pkt->address);
                uint32_t pkt_size = ntohl(pkt->size);
                uint32_t chunk_idx = (pkt_addr - src_addr) / chunk_size;

                /* Drop SENDBINs whose addr/size doesn't fall inside the
                 * transfer we asked for — a stray duplicate from a prior
                 * read() would otherwise scribble random offsets of
                 * `data`. */
                if(pkt_addr < src_addr || pkt_addr >= src_addr + size || pkt_size > chunk_size ||
                   pkt_size > src_addr + size - pkt_addr || chunk_idx >= num_chunks)
                    continue;

                map[chunk_idx] = 1;
                memcpy(data + (pkt_addr - src_addr), buffer + NET_COMMAND_LEN, pkt_size);
            } else if(memcmp(pkt->id, NET_CMD_DONEBIN, 4) != 0) {
                /* Neither SENDBIN nor DONEBIN: stray response from some
                 * unrelated command.  Drop and keep waiting. */
                continue;
            }
            packets++;
            diag_packets++;
        }
    }
    if(ctx->diagnostics_enabled)
        diag_stream_end = ctx->time_ops->time_usec();

    /* Re-request any missing chunks with retry limit */
    int passes = 0;
    for(uint32_t c = 0; c < num_chunks; c++) {
        if(map[c])
            continue;

        uint32_t req_addr = src_addr + c * chunk_size;
        uint32_t req_size = ((size - c * chunk_size) >= chunk_size) ? chunk_size : (size - c * chunk_size);
        uint32_t pass_timeout = recv_data_rerequest_timeout_usec(ctx, passes);

        send_cmd(ctx, NET_CMD_SENDBINQ, req_addr, req_size, NULL, 0);
        diag_rerequests++;

        start = ctx->time_ops->time_usec();
        while((ctx->time_ops->time_usec() - start) < pass_timeout) {
            while((retval = ctx->socket_ops->recv(ctx->global_socket, buffer, 2048)) < 0 &&
                  (ctx->time_ops->time_usec() - start) < pass_timeout)
                ;

            if(retval <= 0)
                break;

            net_command_t *pkt = (net_command_t *)buffer;
            if(memcmp(pkt->id, NET_CMD_SENDBIN, 4) == 0) {
                uint32_t pkt_addr = ntohl(pkt->address);
                uint32_t pkt_size = ntohl(pkt->size);
                uint32_t idx = (pkt_addr - src_addr) / chunk_size;
                if(pkt_addr < src_addr || pkt_addr >= src_addr + size || pkt_size > chunk_size ||
                   pkt_size > src_addr + size - pkt_addr || idx >= num_chunks)
                    continue;

                map[idx] = 1;
                memcpy(data + (pkt_addr - src_addr), buffer + NET_COMMAND_LEN, pkt_size);
            } else if(memcmp(pkt->id, NET_CMD_DONEBIN, 4) == 0) {
                /* DONEBIN is the "end of stream" marker.  If our target
                 * chunk has landed, we're done with this pass. */
                if(map[c])
                    break;
            } else {
                /* Unrelated reply — keep draining. */
                continue;
            }

            if(map[c])
                break;
        }

        /* Restart check from beginning, but limit total passes */
        passes++;
        if(passes >= max_rerequests) {
            fprintf(stderr,
                    "recv_data: exceeded %d re-request passes, transfer may be "
                    "incomplete\n",
                    max_rerequests);
            break;
        }
        c = (uint32_t)-1;
    }

    free(map);

    if(ctx->diagnostics_enabled) {
        uint64_t diag_total_usec = ctx->time_ops->time_usec() - diag_total_start;
        uint64_t diag_stream_usec = diag_stream_end - diag_stream_start;

        ctx->diagnostics_net_recv_bytes += size;
        ctx->diagnostics_net_recv_stream_usec += diag_stream_usec;
        ctx->diagnostics_net_recv_total_usec += diag_total_usec;
        ctx->diagnostics_net_recv_rerequests += diag_rerequests;

        printf("[diag] network download 0x%08x: %u bytes, %u chunks x %u, "
               "packets=%u\n",
               src_addr, size, num_chunks, chunk_size, diag_packets);
        printf("[diag]   steady stream: %.3f ms, %.2f MiB/s\n", diag_ms(diag_stream_usec),
               diag_mib_per_sec(size, diag_stream_usec));
        printf("[diag]   effective: %.3f ms, %.2f MiB/s, rerequests=%u\n", diag_ms(diag_total_usec),
               diag_mib_per_sec(size, diag_total_usec), diag_rerequests);
    }

    return 0;
}

static int network_send_command(kostool_context_t *ctx, const char cmd[4], uint32_t addr, uint32_t size,
                                const uint8_t *data, uint32_t data_size) {
    prepare_comms(ctx);
    return send_cmd(ctx, cmd, addr, size, data, data_size);
}

static int network_recv_response(kostool_context_t *ctx, uint8_t *buffer, size_t buffer_size,
                                 uint32_t timeout_usec) {
    return recv_resp(ctx, buffer, buffer_size, timeout_usec);
}

static bool network_remote_supports_argv(const kostool_context_t *ctx) {
    return (ctx->remote_capabilities & KOSLOAD_CAP_ARGV) != 0;
}

static int network_execute(kostool_context_t *ctx, uint32_t addr, int console_enabled, int cdfs_redir) {
    uint8_t buffer[2048];
    bool send_argv;

    prepare_comms(ctx);
    send_argv = (ctx->prog_argc > 0) && network_remote_supports_argv(ctx);

    uint32_t flags = ((uint32_t)cdfs_redir << 1) | (uint32_t)console_enabled;

    /* Build EXEC data payload: [argc (4 bytes BE)] [argv data blob]
     * where argv data = "argv0\0argv1\0...". */
    uint8_t  exec_data[4 + sizeof(ctx->prog_argv_data)];
    uint32_t exec_data_len = 0;

    if(send_argv) {
        uint32_t argc_be = htonl(ctx->prog_argc);
        uint32_t argv_data_len = 0;
        memcpy(exec_data, &argc_be, 4);
        for(uint32_t i = 0; i < ctx->prog_argc; i++)
            argv_data_len += (uint32_t)strlen(ctx->prog_argv_data + argv_data_len) + 1;
        memcpy(exec_data + 4, ctx->prog_argv_data, argv_data_len);
        exec_data_len = 4 + argv_data_len;
    }

    /* For v2+ dcload, use uncached address */
    if(!ctx->legacy_mode || ctx->force_legacy)
        printf("Sending execute command (0x%08x, console=%d, cdfsredir=%d)...", addr | 0xa0000000,
               console_enabled, cdfs_redir);
    else
        printf("Sending execute command (0x%08x, console=%d, cdfsredir=%d)...", addr, console_enabled,
               cdfs_redir);

    if(send_argv)
        printf("argv(%u, argv0=\"%s\")...", ctx->prog_argc, ctx->prog_argv_data);

    if(send_and_wait(ctx, NET_CMD_EXECUTE, addr, flags, exec_data_len ? exec_data : NULL, exec_data_len,
                     buffer, sizeof(buffer), NET_CMD_EXECUTE) != 0) {
        fprintf(stderr, "execute failed\n");
        return -1;
    }

    printf("executing\n");
    return 0;
}

static int network_reset(kostool_context_t *ctx) {
    prepare_comms(ctx);
    printf("Resetting...\n");
    return send_cmd(ctx, NET_CMD_REBOOT, 0, 0, NULL, 0);
}

static int network_maple(kostool_context_t *ctx, const uint8_t *cmd, size_t cmd_size, uint8_t *resp,
                         size_t *resp_size) {
    uint8_t buffer[2048];

    prepare_comms(ctx);

    send_cmd(ctx, NET_CMD_MAPLE, 0, 0, cmd, (uint32_t)cmd_size);
    int rv = recv_resp(ctx, buffer, sizeof(buffer), NET_PACKET_TIMEOUT_USEC);
    if(rv <= 0)
        return -1;

    net_command_t *pkt = (net_command_t *)buffer;
    uint32_t       data_len = ntohl(pkt->size);
    if(resp && resp_size) {
        if(data_len > *resp_size)
            data_len = (uint32_t)*resp_size;
        memcpy(resp, buffer + NET_COMMAND_LEN, data_len);
        *resp_size = data_len;
    }

    return 0;
}

static int network_pmcr(kostool_context_t *ctx, const uint8_t *cmd, size_t cmd_size, uint8_t *resp,
                        size_t *resp_size) {
    uint8_t buffer[2048];

    prepare_comms(ctx);

    send_cmd(ctx, NET_CMD_PMCR, 0, 0, cmd, (uint32_t)cmd_size);
    int rv = recv_resp(ctx, buffer, sizeof(buffer), NET_PACKET_TIMEOUT_USEC);
    if(rv <= 0)
        return -1;

    net_command_t *pkt = (net_command_t *)buffer;
    uint32_t data_len = ntohl(pkt->size);
    if(resp && resp_size) {
        if(data_len > *resp_size)
            data_len = (uint32_t)*resp_size;
        memcpy(resp, buffer + NET_COMMAND_LEN, data_len);
        *resp_size = data_len;
    }

    return 0;
}

static int network_set_rtc(kostool_context_t *ctx, uint32_t timestamp) {
    uint8_t buffer[2048];

    prepare_comms(ctx);

    printf("Syncing RTC...");
    if(send_and_wait(ctx, NET_CMD_SETRTC, timestamp, 0, NULL, 0, buffer, sizeof(buffer), NET_CMD_SETRTC) !=
       0) {
        fprintf(stderr, "failed\n");
        return -1;
    }
    printf("done\n");

    return 0;
}

const transport_ops_t network_transport_ops = {
    .name = "network",
    .capabilities = TRANSPORT_CAP_RESET | TRANSPORT_CAP_MAPLE | TRANSPORT_CAP_PMCR | TRANSPORT_CAP_RECOVERY |
                    TRANSPORT_CAP_RTC,
    .init = network_init,
    .shutdown = network_shutdown,
    .send_data = network_send_data,
    .recv_data = network_recv_data,
    .send_command = network_send_command,
    .recv_response = network_recv_response,
    .execute = network_execute,
    .reset = network_reset,
    .change_speed = NULL,
    .maple_command = network_maple,
    .pmcr_command = network_pmcr,
    .set_rtc = network_set_rtc,
};
