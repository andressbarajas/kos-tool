/* host/src/discover.c */
/*
 * Auto-discovery for dcload/kosload devices on the local network.
 *
 * Phase 1: Broadcast VERS to 255.255.255.255 on both ports.
 *   Works for kosload (responds with our_ip as IP source).
 *
 * Phase 2: Subnet scan — send unicast VERS to every host on each
 *   local interface's subnet.  Works for legacy dc-load-ip, which
 *   responds with ntohl(ip->dest) as IP source — correct only when
 *   the incoming packet was unicast (ip->dest == DC's own IP).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include <kosload/protocol.h>
#include <kostool/discover.h>

/* Maximum subnet size to scan (number of host addresses).
 * /22 = 1022 hosts.  Larger subnets are skipped. */
#define MAX_SCAN_HOSTS  1024

/* Check if an IP (network byte order) is a usable unicast address */
static int is_valid_unicast(uint32_t addr_nbo) {
    uint32_t h = ntohl(addr_nbo);
    if (h == 0 || h == 0xFFFFFFFF)
        return 0;
    if ((h >> 28) == 0xE)   /* multicast 224.0.0.0/4 */
        return 0;
    if ((h >> 24) == 127)   /* loopback */
        return 0;
    return 1;
}

/* Build a 12-byte VERS command into buf */
static void build_vers_cmd(uint8_t buf[12]) {
    memcpy(buf, NET_CMD_VERSION, 4);
    uint32_t ver = htonl((2 << 16) | (0 << 8) | 0);  /* v2.0.0 */
    memcpy(buf + 4, &ver, 4);
    memset(buf + 8, 0, 4);
}

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

    uint8_t cmd[12];
    build_vers_cmd(cmd);

    struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family = AF_INET;
    bcast.sin_addr.s_addr = INADDR_BROADCAST;

    uint16_t ports[] = { NET_LEGACY_PORT, NET_DEFAULT_PORT };

    /* Phase 1: broadcast */
    for (int attempt = 0; attempt < 4; attempt++) {
        for (int p = 0; p < 2; p++) {
            bcast.sin_port = htons(ports[p]);
            sendto(sock, (const char *)cmd, 12, 0,
                   (struct sockaddr *)&bcast, sizeof(bcast));
        }

        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);

        for (;;) {
            uint8_t buffer[2048];
            struct sockaddr_in from;
            int fromlen = sizeof(from);

            int rv = recvfrom(sock, (char *)buffer, sizeof(buffer), 0,
                              (struct sockaddr *)&from, &fromlen);
            if (rv >= 12 && memcmp(buffer, NET_CMD_VERSION, 4) == 0 &&
                is_valid_unicast(from.sin_addr.s_addr)) {
                closesocket(sock);
                inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
                return ip_str;
            }

            QueryPerformanceCounter(&now);
            uint64_t elapsed_us = (uint64_t)(now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart;
            if (elapsed_us >= 500000) break;
        }
    }

    /* Phase 2: subnet scan (Windows) */
    {
        ULONG buf_size = 15000;
        PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(buf_size);
        if (!addrs) { closesocket(sock); return NULL; }

        ULONG ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, NULL, addrs, &buf_size);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            addrs = (PIP_ADAPTER_ADDRESSES)realloc(addrs, buf_size);
            ret = GetAdaptersAddresses(AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER, NULL, addrs, &buf_size);
        }
        if (ret != NO_ERROR) { free(addrs); closesocket(sock); return NULL; }

        for (PIP_ADAPTER_ADDRESSES aa = addrs; aa; aa = aa->Next) {
            if (aa->OperStatus != IfOperStatusUp) continue;
            if (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (PIP_ADAPTER_UNICAST_ADDRESS ua = aa->FirstUnicastAddress; ua; ua = ua->Next) {
                struct sockaddr_in *sa = (struct sockaddr_in *)ua->Address.lpSockaddr;
                if (sa->sin_family != AF_INET) continue;

                uint32_t my_ip = ntohl(sa->sin_addr.s_addr);
                uint32_t prefix = ua->OnLinkPrefixLength;
                if (prefix < 22 || prefix > 30) continue;

                uint32_t host_mask = (1u << (32 - prefix)) - 1;
                uint32_t network = my_ip & ~host_mask;

                /* Send VERS to every host on this subnet */
                struct sockaddr_in target;
                memset(&target, 0, sizeof(target));
                target.sin_family = AF_INET;

                for (uint32_t h = 1; h < host_mask; h++) {
                    uint32_t target_ip = network | h;
                    if (target_ip == my_ip) continue;
                    target.sin_addr.s_addr = htonl(target_ip);

                    for (int p = 0; p < 2; p++) {
                        target.sin_port = htons(ports[p]);
                        sendto(sock, (const char *)cmd, 12, 0,
                               (struct sockaddr *)&target, sizeof(target));
                    }
                }
            }
        }
        free(addrs);

        /* Wait for responses */
        LARGE_INTEGER freq2, start2, now2;
        QueryPerformanceFrequency(&freq2);
        QueryPerformanceCounter(&start2);

        for (;;) {
            uint8_t buffer[2048];
            struct sockaddr_in from;
            int fromlen = sizeof(from);

            int rv = recvfrom(sock, (char *)buffer, sizeof(buffer), 0,
                              (struct sockaddr *)&from, &fromlen);
            if (rv >= 12 && memcmp(buffer, NET_CMD_VERSION, 4) == 0 &&
                is_valid_unicast(from.sin_addr.s_addr)) {
                closesocket(sock);
                inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
                return ip_str;
            }

            QueryPerformanceCounter(&now2);
            uint64_t elapsed_us = (uint64_t)(now2.QuadPart - start2.QuadPart) * 1000000 / freq2.QuadPart;
            if (elapsed_us >= 2000000) break;  /* 2s timeout for scan */
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

    uint8_t cmd[12];
    build_vers_cmd(cmd);

    struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family = AF_INET;
    bcast.sin_addr.s_addr = INADDR_BROADCAST;

    uint16_t ports[] = { NET_LEGACY_PORT, NET_DEFAULT_PORT };

    /* Phase 1: broadcast */
    for (int attempt = 0; attempt < 4; attempt++) {
        for (int p = 0; p < 2; p++) {
            bcast.sin_port = htons(ports[p]);
            sendto(sock, cmd, 12, 0,
                   (struct sockaddr *)&bcast, sizeof(bcast));
        }

        struct timeval start, now;
        gettimeofday(&start, NULL);

        for (;;) {
            uint8_t buffer[2048];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);

            ssize_t rv = recvfrom(sock, buffer, sizeof(buffer), 0,
                                  (struct sockaddr *)&from, &fromlen);
            if (rv >= 12 && memcmp(buffer, NET_CMD_VERSION, 4) == 0 &&
                is_valid_unicast(from.sin_addr.s_addr)) {
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

    /* Phase 2: subnet scan using getifaddrs (POSIX) */
    {
        struct ifaddrs *ifap = NULL;
        if (getifaddrs(&ifap) == 0) {
            for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                    continue;
                if (!(ifa->ifa_flags & IFF_UP))
                    continue;
                if (ifa->ifa_flags & IFF_LOOPBACK)
                    continue;
                if (!ifa->ifa_netmask)
                    continue;

                uint32_t my_ip = ntohl(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr);
                uint32_t mask = ntohl(((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr);
                uint32_t host_mask = ~mask;
                uint32_t network = my_ip & mask;

                /* Skip subnets that are too large to scan */
                if (host_mask > MAX_SCAN_HOSTS)
                    continue;

                /* Send VERS to every host on this subnet */
                struct sockaddr_in target;
                memset(&target, 0, sizeof(target));
                target.sin_family = AF_INET;

                for (uint32_t h = 1; h < host_mask; h++) {
                    uint32_t target_ip = network | h;
                    if (target_ip == my_ip)
                        continue;
                    target.sin_addr.s_addr = htonl(target_ip);

                    for (int p = 0; p < 2; p++) {
                        target.sin_port = htons(ports[p]);
                        sendto(sock, cmd, 12, 0,
                               (struct sockaddr *)&target, sizeof(target));
                    }
                }
            }
            freeifaddrs(ifap);
        }

        /* Wait for responses from subnet scan */
        struct timeval scan_start, scan_now;
        gettimeofday(&scan_start, NULL);

        for (;;) {
            uint8_t buffer[2048];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);

            ssize_t rv = recvfrom(sock, buffer, sizeof(buffer), 0,
                                  (struct sockaddr *)&from, &fromlen);
            if (rv >= 12 && memcmp(buffer, NET_CMD_VERSION, 4) == 0 &&
                is_valid_unicast(from.sin_addr.s_addr)) {
                close(sock);
                inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
                return ip_str;
            }

            gettimeofday(&scan_now, NULL);
            uint64_t elapsed = (uint64_t)(scan_now.tv_sec - scan_start.tv_sec) * 1000000
                             + (scan_now.tv_usec - scan_start.tv_usec);
            if (elapsed >= 2000000) break;  /* 2s timeout for scan */
        }
    }

    close(sock);
    return NULL;
#endif
}
