/* host/src/discover.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#endif

#include <kosload/protocol.h>
#include <kostool/discover.h>

const char *discover_network_device(void) {
#ifdef _WIN32
    static char ip_str[INET_ADDRSTRLEN];

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return NULL;

    BOOL enable = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&enable, sizeof(enable));

    /* Set non-blocking */
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);

    /* Build VERS command: "VERS" + htonl(version) + htonl(0) */
    uint8_t cmd[12];
    memcpy(cmd, NET_CMD_VERSION, 4);
    uint32_t ver = htonl((2 << 16) | (0 << 8) | 0);  /* v2.0.0 */
    memcpy(cmd + 4, &ver, 4);
    memset(cmd + 8, 0, 4);

    struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family = AF_INET;
    bcast.sin_addr.s_addr = INADDR_BROADCAST;

    uint16_t ports[] = { NET_LEGACY_PORT, NET_DEFAULT_PORT };

    for (int attempt = 0; attempt < 4; attempt++) {
        /* Send VERS to both ports */
        for (int p = 0; p < 2; p++) {
            bcast.sin_port = htons(ports[p]);
            sendto(sock, (const char *)cmd, 12, 0,
                   (struct sockaddr *)&bcast, sizeof(bcast));
        }

        /* Wait up to 500ms for a response */
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);

        for (;;) {
            uint8_t buffer[2048];
            struct sockaddr_in from;
            int fromlen = sizeof(from);

            int rv = recvfrom(sock, (char *)buffer, sizeof(buffer), 0,
                              (struct sockaddr *)&from, &fromlen);
            if (rv >= 12 && memcmp(buffer, NET_CMD_VERSION, 4) == 0) {
                closesocket(sock);
                inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
                return ip_str;
            }

            QueryPerformanceCounter(&now);
            uint64_t elapsed_us = (uint64_t)(now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart;
            if (elapsed_us >= 500000) break;
        }
    }

    closesocket(sock);
    return NULL;
#else
    static char ip_str[INET_ADDRSTRLEN];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return NULL;

    int enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    fcntl(sock, F_SETFL, O_NONBLOCK);

    /* Build VERS command: "VERS" + htonl(version) + htonl(0) */
    uint8_t cmd[12];
    memcpy(cmd, NET_CMD_VERSION, 4);
    uint32_t ver = htonl((2 << 16) | (0 << 8) | 0);  /* v2.0.0 */
    memcpy(cmd + 4, &ver, 4);
    memset(cmd + 8, 0, 4);

    struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family = AF_INET;
    bcast.sin_addr.s_addr = INADDR_BROADCAST;

    uint16_t ports[] = { NET_LEGACY_PORT, NET_DEFAULT_PORT };

    for (int attempt = 0; attempt < 4; attempt++) {
        /* Send VERS to both ports */
        for (int p = 0; p < 2; p++) {
            bcast.sin_port = htons(ports[p]);
            sendto(sock, cmd, 12, 0,
                   (struct sockaddr *)&bcast, sizeof(bcast));
        }

        /* Wait up to 500ms for a response */
        struct timeval start, now;
        gettimeofday(&start, NULL);

        for (;;) {
            uint8_t buffer[2048];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);

            ssize_t rv = recvfrom(sock, buffer, sizeof(buffer), 0,
                                  (struct sockaddr *)&from, &fromlen);
            if (rv >= 12 && memcmp(buffer, NET_CMD_VERSION, 4) == 0) {
                close(sock);
                inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
                return ip_str;
            }

            gettimeofday(&now, NULL);
            uint64_t elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000
                             + (now.tv_usec - start.tv_usec);
            if (elapsed >= 500000) break;
        }
    }

    close(sock);
    return NULL;
#endif
}
