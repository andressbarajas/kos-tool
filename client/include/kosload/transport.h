/* client/include/kosload/transport.h */
#ifndef KOSLOAD_CLIENT_TRANSPORT_H
#define KOSLOAD_CLIENT_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Client-side transport interface.
 *
 * This is the loader-to-host communication boundary for client firmware.
 * Shared client code chooses one transport at startup and should not need to
 * know whether the wire is serial, network, USB, or another future peripheral.
 *
 * Lifecycle:
 * - init() prepares the device/protocol. common_main() may retry it on failure.
 * - loop(true) is the main command loop and may block indefinitely.
 * - syscall_send() sends a console syscall response/request over this transport.
 * - exit_notify() lets the host know the loaded program returned.
 * - stop()/start() bracket periods where a transport must pause or resume
 *   background packet handling around target program execution.
 *
 * Keep protocol additions in include/kosload/protocol.h. Prefer capability
 * bits or transport-private helpers for optional features so new transports do
 * not have to fake unsupported behavior.
 */
typedef struct client_transport_ops {
    const char *name;
    const char *init_error_msg; /* Message shown when init() fails, or NULL */
    int (*init)(void);
    void (*loop)(bool is_main_loop);
    int (*syscall_send)(const char cmd_id[4], const uint8_t *payload, size_t payload_len);
    void (*exit_notify)(void);
    void (*stop)(void);
    void (*start)(void);
} client_transport_ops_t;

/* Transport implementations */
extern const client_transport_ops_t client_serial_transport_ops;
extern const client_transport_ops_t client_network_transport_ops;

#endif /* KOSLOAD_CLIENT_TRANSPORT_H */
