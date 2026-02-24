/* client/include/kosload/transport.h */
#ifndef KOSLOAD_CLIENT_TRANSPORT_H
#define KOSLOAD_CLIENT_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

/* Client-side transport interface */
typedef struct client_transport_ops {
    const char *name;
    const char *init_error_msg;   /* Message shown when init() fails, or NULL */
    int   (*init)(void);
    void  (*loop)(int is_main_loop);
    int   (*syscall_send)(const char cmd_id[4], const uint8_t *payload,
                          size_t payload_len);
    void  (*exit_notify)(void);
    void  (*stop)(void);
    void  (*start)(void);
} client_transport_ops_t;

/* Transport implementations */
extern const client_transport_ops_t client_serial_transport_ops;
extern const client_transport_ops_t client_network_transport_ops;

#endif /* KOSLOAD_CLIENT_TRANSPORT_H */
