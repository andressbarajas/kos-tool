/* host/src/platform/windows.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <kosload/protocol.h>
#include <kosload/strutil.h>
#include <kostool/platform.h>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ===== Windows Socket Ops ===== */

static int win_socket_init(void) {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) != NO_ERROR ? -1 : 0;
}

static void win_socket_cleanup(void) {
    WSACleanup();
}

static int64_t win_udp_socket(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    return (s == INVALID_SOCKET) ? -1 : (int64_t)s;
}

static int64_t win_tcp_socket(void) {
    SOCKET s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    return (s == INVALID_SOCKET) ? -1 : (int64_t)s;
}

static int win_bind_listen(int64_t sock, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind((SOCKET)sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
        return -1;
    if (listen((SOCKET)sock, 1) == SOCKET_ERROR)
        return -1;
    return 0;
}

static int64_t win_accept(int64_t sock) {
    SOCKET s = accept((SOCKET)sock, NULL, NULL);
    return (s == INVALID_SOCKET) ? -1 : (int64_t)s;
}

static int win_set_nonblocking(int64_t sock) {
    unsigned long flags = 1;
    return ioctlsocket((SOCKET)sock, FIONBIO, &flags) == SOCKET_ERROR ? -1 : 0;
}

static int win_connect(int64_t sock, const char *host, uint16_t port) {
    struct sockaddr_in addr;
    struct hostent *he;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    he = gethostbyname(host);
    if (!he) return -1;
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    return connect((SOCKET)sock, (struct sockaddr *)&addr, sizeof(addr));
}

static int win_send(int64_t sock, const void *data, size_t len) {
    return send((SOCKET)sock, (const char *)data, (int)len, 0);
}

static int win_recv(int64_t sock, void *buffer, size_t len) {
    return recv((SOCKET)sock, (char *)buffer, (int)len, 0);
}

static int win_setsockopt_reuse(int64_t sock) {
    const char enable = 1;
    return setsockopt((SOCKET)sock, SOL_SOCKET, SO_REUSEADDR,
                      &enable, sizeof(enable));
}

static void win_socket_close(int64_t sock) {
    closesocket((SOCKET)sock);
}

const platform_socket_ops_t windows_socket_ops = {
    .init = win_socket_init,
    .cleanup = win_socket_cleanup,
    .udp_socket = win_udp_socket,
    .tcp_socket = win_tcp_socket,
    .bind_listen = win_bind_listen,
    .accept = win_accept,
    .set_nonblocking = win_set_nonblocking,
    .connect = win_connect,
    .send = win_send,
    .recv = win_recv,
    .setsockopt_reuse = win_setsockopt_reuse,
    .close = win_socket_close,
};

/* ===== Windows Filesystem Ops ===== */

static int win_translate_flags(int kos_flags) {
    int flags = _O_RDONLY;
    if (kos_flags & KOS_O_WRONLY) flags = _O_WRONLY;
    if (kos_flags & KOS_O_RDWR)  flags = _O_RDWR;
    if (kos_flags & KOS_O_APPEND) flags |= _O_APPEND;
    if (kos_flags & KOS_O_CREAT)  flags |= _O_CREAT;
    if (kos_flags & KOS_O_TRUNC)  flags |= _O_TRUNC;
    if (kos_flags & KOS_O_EXCL)   flags |= _O_EXCL;
    flags |= _O_BINARY;
    return flags;
}

static const char *win_resolve_path(const char *dc_path, const char *map_root,
                                    char *out, size_t out_size) {
    size_t len;

    if (!map_root) return dc_path;
    len = compat_str_copy(out, out_size, map_root);
    compat_str_append(out, out_size, len, dc_path);
    return out;
}

static int win_chroot(const char *path) {
    (void)path;
    /* chroot not available on Windows */
    return -1;
}

const platform_fs_ops_t windows_fs_ops = {
    .translate_open_flags = win_translate_flags,
    .resolve_path = win_resolve_path,
    .chroot = win_chroot,
};

/* ===== Windows Time Ops ===== */

static uint64_t win_time_usec(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000000 / freq.QuadPart);
}

static void win_sleep_usec(uint32_t usec) {
    if (usec >= 1000) {
        /* Use Sleep() for >= 1ms; round to nearest ms */
        Sleep((usec + 500) / 1000);
    } else {
        /* Spin-wait for sub-millisecond precision */
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        LONGLONG target = (LONGLONG)usec * freq.QuadPart / 1000000;
        do {
            QueryPerformanceCounter(&now);
        } while ((now.QuadPart - start.QuadPart) < target);
    }
}

const platform_time_ops_t windows_time_ops = {
    .time_usec = win_time_usec,
    .sleep_usec = win_sleep_usec,
};

/* Platform detection for Windows */
const platform_socket_ops_t *platform_get_socket_ops(void) {
    return &windows_socket_ops;
}

const platform_fs_ops_t *platform_get_fs_ops(void) {
    return &windows_fs_ops;
}

const platform_time_ops_t *platform_get_time_ops(void) {
    return &windows_time_ops;
}

#endif /* _WIN32 */
