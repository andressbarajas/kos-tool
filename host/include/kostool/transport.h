/* host/include/kostool/transport.h */
#ifndef KOSTOOL_TRANSPORT_H
#define KOSTOOL_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "context.h"

/*
 * Transport capability flags.
 *
 * Host code should prefer these flags when deciding whether to expose optional
 * operations. A transport may leave an unsupported callback NULL when the
 * matching capability bit is not set.
 */
#define TRANSPORT_CAP_RESET        (1u << 0)
#define TRANSPORT_CAP_MAPLE        (1u << 1)
#define TRANSPORT_CAP_PMCR         (1u << 2)
#define TRANSPORT_CAP_COMPRESS     (1u << 3)    /* LZO compression (serial) */
#define TRANSPORT_CAP_RECOVERY     (1u << 4)    /* Packet recovery (network) */
#define TRANSPORT_CAP_RTC          (1u << 5)
#define TRANSPORT_CAP_SPEED_CHANGE (1u << 6)

/*
 * Host-side transport interface.
 *
 * This is the host boundary for talking to a loader. High-level code should use
 * this table for upload, download, execute, console, and maintenance commands
 * instead of branching on serial/network implementation details.
 *
 * A new host transport should implement the core data and command callbacks,
 * set capability bits for optional behavior, and leave unsupported optional
 * callbacks NULL. Wire protocol constants and shared packet structures belong
 * in include/kosload/protocol.h so host and client stay in sync. Callers
 * should use the transport_can_* predicates below for optional callbacks.
 */
typedef struct transport_ops {
    const char *name;
    uint32_t capabilities;

    int  (*init)(kostool_context_t *ctx);
    void (*shutdown)(kostool_context_t *ctx);

    int  (*send_data)(kostool_context_t *ctx, const uint8_t *data,
                      uint32_t dest_addr, uint32_t size);
    int  (*recv_data)(kostool_context_t *ctx, uint8_t *data,
                      uint32_t src_addr, uint32_t size, int quiet);

    int  (*send_command)(kostool_context_t *ctx, const char cmd[4],
                         uint32_t addr, uint32_t size,
                         const uint8_t *data, uint32_t data_size);
    int  (*recv_response)(kostool_context_t *ctx, uint8_t *buffer,
                          size_t buffer_size, uint32_t timeout_usec);

    int  (*execute)(kostool_context_t *ctx, uint32_t addr,
                    int console_enabled, int cdfs_redir);
    int  (*reset)(kostool_context_t *ctx);
    int  (*change_speed)(kostool_context_t *ctx, uint32_t new_speed);
    int  (*maple_command)(kostool_context_t *ctx, const uint8_t *cmd,
                          size_t cmd_size, uint8_t *resp, size_t *resp_size);
    int  (*pmcr_command)(kostool_context_t *ctx, const uint8_t *cmd,
                         size_t cmd_size, uint8_t *resp, size_t *resp_size);
    int  (*set_rtc)(kostool_context_t *ctx, uint32_t timestamp);
} transport_ops_t;

static inline int transport_has_capability(const transport_ops_t *ops,
                                           uint32_t capability) {
    return ops && ((ops->capabilities & capability) == capability);
}

static inline int transport_can_reset(const transport_ops_t *ops) {
    return transport_has_capability(ops, TRANSPORT_CAP_RESET) && ops->reset;
}

static inline int transport_can_change_speed(const transport_ops_t *ops) {
    return transport_has_capability(ops, TRANSPORT_CAP_SPEED_CHANGE) &&
           ops->change_speed;
}

static inline int transport_can_maple(const transport_ops_t *ops) {
    return transport_has_capability(ops, TRANSPORT_CAP_MAPLE) &&
           ops->maple_command;
}

static inline int transport_can_pmcr(const transport_ops_t *ops) {
    return transport_has_capability(ops, TRANSPORT_CAP_PMCR) &&
           ops->pmcr_command;
}

static inline int transport_can_set_rtc(const transport_ops_t *ops) {
    return transport_has_capability(ops, TRANSPORT_CAP_RTC) && ops->set_rtc;
}

static inline int transport_uses_compression(const transport_ops_t *ops) {
    return transport_has_capability(ops, TRANSPORT_CAP_COMPRESS);
}

static inline int transport_uses_recovery(const transport_ops_t *ops) {
    return transport_has_capability(ops, TRANSPORT_CAP_RECOVERY);
}

/* Transport implementations */
extern const transport_ops_t serial_transport_ops;
extern const transport_ops_t network_transport_ops;

#endif /* KOSTOOL_TRANSPORT_H */
