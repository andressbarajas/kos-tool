/* host/include/kostool/platform.h */
#ifndef KOSTOOL_PLATFORM_H
#define KOSTOOL_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

/* Serial port operations — one implementation per OS */
typedef struct platform_serial_ops {
    void *(*open)(const char *device, uint32_t initial_baud);
    void  (*close)(void *handle);
    int   (*read)(void *handle, void *buffer, size_t count);
    int   (*write)(void *handle, const void *buffer, size_t count);
    int   (*set_speed)(void *handle, uint32_t baud);
    void  (*flush)(void *handle);
    void  (*drain)(void *handle);
} platform_serial_ops_t;

/* Socket operations — POSIX vs WinSock */
typedef struct platform_socket_ops {
    int     (*init)(void);
    void    (*cleanup)(void);
    int64_t (*udp_socket)(void);
    int64_t (*tcp_socket)(void);
    int     (*bind_listen)(int64_t sock, uint16_t port);
    int64_t (*accept)(int64_t sock);
    int     (*set_nonblocking)(int64_t sock);
    int     (*connect)(int64_t sock, const char *host, uint16_t port);
    int     (*send)(int64_t sock, const void *data, size_t len);
    int     (*recv)(int64_t sock, void *buffer, size_t len);
    int     (*setsockopt_reuse)(int64_t sock);
    void    (*close)(int64_t sock);
} platform_socket_ops_t;

/* Filesystem operations — flag translation, path mapping */
typedef struct platform_fs_ops {
    int         (*translate_open_flags)(int kos_flags);
    const char *(*resolve_path)(const char *dc_path, const char *map_root,
                                char *out, size_t out_size);
    int         (*chroot)(const char *path);
} platform_fs_ops_t;

/* Time operations */
typedef struct platform_time_ops {
    uint64_t (*time_usec)(void);
    void     (*sleep_usec)(uint32_t usec);
} platform_time_ops_t;

/* Platform implementations */
extern const platform_serial_ops_t linux_serial_ops;
extern const platform_serial_ops_t macos_serial_ops;
extern const platform_serial_ops_t windows_serial_ops;
extern const platform_socket_ops_t posix_socket_ops;
extern const platform_socket_ops_t windows_socket_ops;
extern const platform_fs_ops_t posix_fs_ops;
extern const platform_fs_ops_t windows_fs_ops;
extern const platform_time_ops_t posix_time_ops;
extern const platform_time_ops_t windows_time_ops;

/* Get the correct platform ops for the current OS */
const platform_serial_ops_t *platform_get_serial_ops(void);
const platform_socket_ops_t *platform_get_socket_ops(void);
const platform_fs_ops_t *platform_get_fs_ops(void);
const platform_time_ops_t *platform_get_time_ops(void);

#endif /* KOSTOOL_PLATFORM_H */
