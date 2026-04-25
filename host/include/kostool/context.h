/* host/include/kostool/context.h */
#ifndef KOSTOOL_CONTEXT_H
#define KOSTOOL_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations — full definitions in their respective headers */
struct transport_ops;
struct platform_serial_ops;
struct platform_socket_ops;
struct platform_fs_ops;
struct platform_time_ops;

typedef struct kostool_context {
    /* Transport layer */
    const struct transport_ops *transport;
    void *transport_data;

    /* Platform abstractions */
    const struct platform_serial_ops *serial_ops;
    const struct platform_socket_ops *socket_ops;
    const struct platform_fs_ops *fs_ops;
    const struct platform_time_ops *time_ops;

    /* Serial port state */
    void *serial_handle;
    const char *device_name;
    uint32_t initial_speed;
    uint32_t current_speed;
    int speedhack;
    int use_extclk;

    /* Network state */
    int64_t socket_fd;
    int64_t socket_legacy;
    int64_t global_socket;
    int sockets_initialized;
    const char *hostname;
    uint16_t port;
    uint32_t installed_adapter;
    int legacy_mode;
    int force_legacy;
    int fast_mode;
    uint32_t rx_fifo_delay;
    uint32_t rx_fifo_delay_count;

    /* GDB state */
    int gdb_enabled;
    int64_t gdb_server_socket;
    int64_t gdb_client_socket;
    int gdb_detach_pending;
    struct {
        int in_packet;
        int checksum_bytes;
        size_t payload_len;
        char payload[32];
    } gdb_client_probe;
    struct {
        int in_packet;
        int checksum_bytes;
        size_t payload_len;
        char payload[32];
    } gdb_target_probe;

    /* CDFS state */
    int cdfs_enabled;
    int cdfs_fd;
    const char *iso_filename;

    /* Fileserver state */
    const char *chroot_path;
    const char *map_path;
    int use_chroot;

    /* Command state */
    uint32_t load_address;
    uint32_t download_size;
    int console_enabled;
    int quiet_mode;
    int dumb_terminal;

    /* Diagnostics / timing */
    int diagnostics_enabled;
    uint64_t diagnostics_start_usec;
    uint64_t diagnostics_uploaded_bytes;
    uint64_t diagnostics_downloaded_bytes;
    uint64_t diagnostics_net_send_bytes;
    uint64_t diagnostics_net_recv_bytes;
    uint64_t diagnostics_net_send_stream_usec;
    uint64_t diagnostics_net_recv_stream_usec;
    uint64_t diagnostics_net_send_total_usec;
    uint64_t diagnostics_net_recv_total_usec;
    uint64_t diagnostics_net_retransmitted_bytes;
    uint32_t diagnostics_upload_sections;
    uint32_t diagnostics_net_recovery_requests;
    uint32_t diagnostics_net_loadbin_retries;
    uint32_t diagnostics_net_recv_rerequests;

    /* Argument vector data for the loaded program.
     * Serialized as: "argv0\0argv1\0...argvN\0". */
    uint32_t prog_argc;
    char     prog_argv_data[256];

    /* RTC sync */
    int rtc_sync;

    /* Program execution state */
    int program_executed;

    /* addr2line (configured via kos-tool.cfg) */
    const char *loaded_binary_path;
    char sh4_addr2line[512];        /* Full path to SH4 addr2line (from config) */
    char ppc_addr2line[512];        /* Full path to PPC addr2line (from config) */

    /* Remote loader info (populated during transport init/handshake) */
    char     remote_version_string[128];
    uint32_t remote_capabilities;
    int      target_big_endian;     /* 0 = LE (DC/SH4), 1 = BE (GC/PPC) */

    /* Firmware update */
    int skip_update;
    const char *firmware_path;
    const char *switch_target;
    int switch_target_is_network;
    int switch_target_is_dhcp;
} kostool_context_t;

#endif /* KOSTOOL_CONTEXT_H */
