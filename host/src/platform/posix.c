/* host/src/platform/posix.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <kosload/protocol.h>
#include <kosload/strutil.h>
#include <kostool/platform.h>

/* POSIX socket ops */

static int posix_socket_init(void) { return 0; }
static void posix_socket_cleanup(void) { }

static int64_t posix_udp_socket(void) {
    return socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
}

static int64_t posix_tcp_socket(void) {
    return socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
}

static int posix_bind_listen(int64_t sock, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind((int)sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return -1;
    if (listen((int)sock, 1) < 0)
        return -1;
    return 0;
}

static int64_t posix_accept(int64_t sock) {
    return accept((int)sock, NULL, NULL);
}

static int posix_set_nonblocking(int64_t sock) {
    return fcntl((int)sock, F_SETFL, O_NONBLOCK);
}

static int posix_connect(int64_t sock, const char *host, uint16_t port) {
    struct sockaddr_in addr;
    struct hostent *he;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    he = gethostbyname(host);
    if (!he) return -1;
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    return connect((int)sock, (struct sockaddr *)&addr, sizeof(addr));
}

static int posix_send(int64_t sock, const void *data, size_t len) {
    return (int)send((int)sock, data, len, 0);
}

static int posix_recv(int64_t sock, void *buffer, size_t len) {
    return (int)recv((int)sock, buffer, len, 0);
}

static int posix_setsockopt_reuse(int64_t sock) {
    const int enable = 1;
    return setsockopt((int)sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
}

static void posix_socket_close(int64_t sock) {
    close((int)sock);
}

const platform_socket_ops_t posix_socket_ops = {
    .init = posix_socket_init,
    .cleanup = posix_socket_cleanup,
    .udp_socket = posix_udp_socket,
    .tcp_socket = posix_tcp_socket,
    .bind_listen = posix_bind_listen,
    .accept = posix_accept,
    .set_nonblocking = posix_set_nonblocking,
    .connect = posix_connect,
    .send = posix_send,
    .recv = posix_recv,
    .setsockopt_reuse = posix_setsockopt_reuse,
    .close = posix_socket_close,
};

/* POSIX filesystem ops */

static int posix_translate_flags(int kos_flags) {
    int flags = O_RDONLY;
    if (kos_flags & KOS_O_WRONLY) flags = O_WRONLY;
    if (kos_flags & KOS_O_RDWR) flags = O_RDWR;
    if (kos_flags & KOS_O_APPEND) flags |= O_APPEND;
    if (kos_flags & KOS_O_CREAT) flags |= O_CREAT;
    if (kos_flags & KOS_O_TRUNC) flags |= O_TRUNC;
    if (kos_flags & KOS_O_EXCL) flags |= O_EXCL;
    return flags;
}

static const char *posix_resolve_path(const char *dc_path, const char *map_root,
                                      char *out, size_t out_size) {
    size_t len;

    if (!map_root) return dc_path;
    len = compat_str_copy(out, out_size, map_root);
    compat_str_append(out, out_size, len, dc_path);
    return out;
}

static int posix_chroot(const char *path) {
    return chroot(path);
}

const platform_fs_ops_t posix_fs_ops = {
    .translate_open_flags = posix_translate_flags,
    .resolve_path = posix_resolve_path,
    .chroot = posix_chroot,
};

/* POSIX time ops */

static uint64_t posix_time_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void posix_sleep_usec(uint32_t usec) {
    usleep(usec);
}

const platform_time_ops_t posix_time_ops = {
    .time_usec = posix_time_usec,
    .sleep_usec = posix_sleep_usec,
};

/* Platform detection helpers */

const platform_socket_ops_t *platform_get_socket_ops(void) {
    return &posix_socket_ops;
}

const platform_fs_ops_t *platform_get_fs_ops(void) {
    return &posix_fs_ops;
}

const platform_time_ops_t *platform_get_time_ops(void) {
    return &posix_time_ops;
}
