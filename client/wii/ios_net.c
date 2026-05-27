/* client/wii/ios_net.c
 *
 * Clean-room Wii IOS socket shim.
 *
 * Derived from binary-only inspection of Wii homebrew artifacts.  No
 * third-party headers or source were consulted.
 */

#include "ios_net.h"

#include <stdint.h>
#include <string.h>
#include <kosload/protocol.h>

#include "../gamecube/cache.h"

#define IPC_REG_REQUEST        (*(volatile uint32_t *)0xcd000000)
#define IPC_REG_CONTROL        (*(volatile uint32_t *)0xcd000004)
#define IPC_REG_REPLY          (*(volatile uint32_t *)0xcd000008)
#define IPC_REG_INTERRUPT_ACK  (*(volatile uint32_t *)0xcd000030)

#define IPC_CONTROL_KEEP_MASK  0x30
#define IPC_SEND_REQUEST       0x01
#define IPC_ACK_REQUEST        0x02
#define IPC_ACK_REPLY          0x04
#define IPC_RELEASE_REPLY      0x08
#define IPC_REPLY_READY_MASK   0x14
#define IPC_REQUEST_ACK_MASK   0x22
#define IPC_INTERRUPT_ACK_BIT  0x40000000
#define IPC_MAGIC              0x4c4f4743  /* "LOGC", observed in binaries */

#define IOS_REQ_OPEN           1
#define IOS_REQ_CLOSE          2
#define IOS_REQ_IOCTL          6
#define IOS_REQ_IOCTLV         7

#define IOS_NET_CMD_ACCEPT     1
#define IOS_NET_CMD_BIND       2
#define IOS_NET_CMD_CLOSE      3
#define IOS_NET_CMD_RECVFROM   12
#define IOS_NET_CMD_SENDTO     13
#define IOS_NET_CMD_SOCKET     15
#define IOS_NET_CMD_GETIP      16
#define IOS_NET_CMD_GETIFOPT   0x1c
#define IOS_NET_CMD_STARTUP    31

#define NCD_CMD_GETIFCONFIG    3
#define NCD_CMD_READCONFIG     5
#define NCD_CMD_GETLINKSTATUS  7
#define NCD_CMD_GETWIFIMAC     8

#define IOS_AF_INET            2
#define IOS_SOCK_DGRAM         2

#define IOS_ETIMEDOUT          (-116)
#define IOS_EAGAIN             (-11)
#define IOS_EWOULDBLOCK        (-111)

#define IPC_TIMEOUT_US         5000000
#define NET_START_RETRIES      100
#define NET_START_DELAY_US     100000

#ifndef WII_USE_MEM2_IPC_ARENA
#define WII_USE_MEM2_IPC_ARENA 0
#endif

#define IPC_ARENA_BASE         0x933e0000
#define PATH_BUF_SIZE          64
#define NCD_OUT_SIZE           32
#define NCD_CONFIG_SIZE        7004
#define NCD_CONFIG_FILE_SIZE   6980
#define NCD_HEADER_SIZE        8
#define NCD_CONNECTION_COUNT   3
#define NCD_CONNECTION_SIZE    0x91c
#define NCD_CONN_FLAGS         0x000
#define NCD_FLAG_INTERFACE     0x01
#define NCD_FLAG_IP_DHCP       0x04
#define NCD_FLAG_SELECTED      0x80
#define KD_OUT_SIZE            32
#define IOCTL_WORDS_SIZE       64
#define IOCTL_VECTORS_SIZE     (4 * sizeof(ios_iovec_t))
#define IFOPT_VAL_BUF_SIZE     256
#define SOCKADDR_BUF_SIZE      32
#define SOCKADDR_LEN_BUF_SIZE  32
#define META_BUF_SIZE          64


typedef struct {
    volatile uint32_t word[16];
} ios_request_t;

typedef struct {
    void *data;
    uint32_t len;
} ios_iovec_t;

static const char *last_error = "Wii IOS socket service not initialized";
static char last_error_buf[96];

static int ipc_ready;
static int net_fd = -1;
static int udp_fd = -1;
static uint32_t local_ip;
static const uint8_t fallback_mac[6] =
    { 0x02, 0x57, 0x49, 0x49, 0x00, 0x01 };
/* Display MAC.  Prefer an init-cached real MAC when it is known; otherwise
 * keep the fixed locally-administered fallback used by earlier builds. */
static uint8_t local_mac[6] = { 0x02, 0x57, 0x49, 0x49, 0x00, 0x01 };
static uint8_t wifi_mac[6]  = { 0x02, 0x57, 0x49, 0x49, 0x00, 0x01 };
static uint8_t ethernet_mac[6];
static int wifi_mac_valid;
static int ethernet_mac_valid;
static int ncd_ip_source_dhcp = 1; /* Unknown config falls back to DHCP. */
static int ncd_interface_type = WII_NET_INTERFACE_UNKNOWN;
static int ncd_link_status = WII_NET_LINK_UNKNOWN;
static uint32_t ipc_timeout_valid;
static uint32_t ipc_last_control;
static uint32_t ipc_last_reply;
static uint32_t ipc_last_type;
/* Set around the intentionally-blocking recvfrom: ipc_sync must then
 * wait indefinitely for IOS's reply (a datagram) instead of abandoning
 * the request after 5 s and reusing the shared buffer under it (that
 * abandon was the idle-wedge root cause).  All other IOS calls keep the
 * 5 s safety timeout. */
static volatile int ipc_block;

#if WII_USE_MEM2_IPC_ARENA
static uint8_t ncd_config_storage[NCD_CONFIG_SIZE] __attribute__((aligned(32)));
#define ipc_request     (*(ios_request_t *)(IPC_ARENA_BASE + 0x0000))
#define path_buf        ((char *)(IPC_ARENA_BASE + 0x0040))
#define ncd_out         ((uint8_t *)(IPC_ARENA_BASE + 0x0080))
#define ncd_config      ncd_config_storage
#define kd_out          ((uint8_t *)(IPC_ARENA_BASE + 0x00a0))
#define ioctl_words     ((uint32_t *)(IPC_ARENA_BASE + 0x00c0))
#define ioctl_vectors   ((ios_iovec_t *)(IPC_ARENA_BASE + 0x0100))
#define sockaddr_buf    ((uint8_t *)(IPC_ARENA_BASE + 0x0120))
#define sockaddr_len_buf ((uint8_t *)(IPC_ARENA_BASE + 0x0140))
#define meta_buf        ((uint8_t *)(IPC_ARENA_BASE + 0x0160))
#define rx_buf          ((uint8_t *)(IPC_ARENA_BASE + 0x01a0))
#define tx_buf          ((uint8_t *)(IPC_ARENA_BASE + 0x0760))
#else
static ios_request_t ipc_request_storage __attribute__((aligned(32)));
static char path_buf_storage[PATH_BUF_SIZE] __attribute__((aligned(32)));
static uint8_t ncd_out_storage[NCD_OUT_SIZE] __attribute__((aligned(32)));
static uint8_t ncd_config_storage[NCD_CONFIG_SIZE] __attribute__((aligned(32)));
static uint8_t kd_out_storage[KD_OUT_SIZE] __attribute__((aligned(32)));
static uint32_t ioctl_words_storage[IOCTL_WORDS_SIZE / sizeof(uint32_t)]
    __attribute__((aligned(32)));
static ios_iovec_t ioctl_vectors_storage[4] __attribute__((aligned(32)));
static uint8_t sockaddr_buf_storage[SOCKADDR_BUF_SIZE]
    __attribute__((aligned(32)));
static uint8_t sockaddr_len_buf_storage[SOCKADDR_LEN_BUF_SIZE]
    __attribute__((aligned(32)));
static uint8_t meta_buf_storage[META_BUF_SIZE] __attribute__((aligned(32)));
static uint8_t rx_buf_storage[WII_NET_MAX_UDP_PAYLOAD]
    __attribute__((aligned(32)));
static uint8_t tx_buf_storage[WII_NET_MAX_UDP_PAYLOAD]
    __attribute__((aligned(32)));

#define ipc_request      ipc_request_storage
#define path_buf         path_buf_storage
#define ncd_out          ncd_out_storage
#define ncd_config       ncd_config_storage
#define kd_out           kd_out_storage
#define ioctl_words      ioctl_words_storage
#define ioctl_vectors    ioctl_vectors_storage
#define sockaddr_buf     sockaddr_buf_storage
#define sockaddr_len_buf sockaddr_len_buf_storage
#define meta_buf         meta_buf_storage
#define rx_buf           rx_buf_storage
#define tx_buf           tx_buf_storage
#endif

/* IOS landing zone for NCD cmd 8 / SOGetInterfaceOpt 0x1004 — both drop the
 * trailing 2 bytes of the 6-byte MAC if the output vector lives in MEM1.  We
 * copy the result straight into MEM1 (wifi_mac/ethernet_mac/local_mac);
 * ifopt_val_buf is also reused as the SOPoll output on the hot path.
 *
 * Placed LOW in the app MEM2 arena: well clear of the IOS-owned/Hollywood-
 * protected high region (Dolphin's MMU-off fast-mem flagged an earlier
 * near-top placement at 0x933dfe00), and only touched by init + the idle
 * poll loop, so timing is unconstrained.
 *
 * Layout from MEM2_MAC_BUFS_BASE, all 32-byte aligned:
 *   +0x000  ncd_mac_buf       (32 B reserved, 6 B used by NCD cmd 8)
 *   +0x020  ifopt_req_buf     (32 B reserved, 8 B used = level + optname)
 *   +0x040  ifopt_val_buf     (IFOPT_VAL_BUF_SIZE = 256 B; up to outlen used)
 *   +0x140  ifopt_len_buf     (32 B reserved, 4 B used = optsize-inout) */
#define NCD_MAC_BUF_SIZE    32
#define IFOPT_REQ_BUF_SIZE  32
#define IFOPT_LEN_BUF_SIZE  32
#define MEM2_MAC_BUFS_SPAN  0x200      /* total span of the four buffers */

#define MEM2_MAC_BUFS_BASE  0x90100000u

#define ncd_mac_buf       ((uint8_t *)(MEM2_MAC_BUFS_BASE + 0x000))
#define ifopt_req_buf     ((uint8_t *)(MEM2_MAC_BUFS_BASE + 0x020))
#define ifopt_val_buf     ((uint8_t *)(MEM2_MAC_BUFS_BASE + 0x040))
#define ifopt_len_buf     ((uint8_t *)(MEM2_MAC_BUFS_BASE + 0x140))

static char *append_text(char *dst, char *end, const char *src)
{
    while (dst + 1 < end && *src)
        *dst++ = *src++;
    *dst = 0;
    return dst;
}

static char *append_int(char *dst, char *end, int value)
{
    char tmp[12];
    unsigned int v;
    int i = 0;

    if (dst + 1 >= end)
        return dst;

    if (value < 0) {
        *dst++ = '-';
        v = (unsigned int)(-value);
    } else {
        v = (unsigned int)value;
    }

    if (v == 0) {
        if (dst + 1 < end)
            *dst++ = '0';
        *dst = 0;
        return dst;
    }

    while (v && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (i > 0 && dst + 1 < end)
        *dst++ = tmp[--i];

    *dst = 0;
    return dst;
}

static char *append_hex32(char *dst, char *end, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    int shift;

    for (shift = 28; shift >= 0 && dst + 1 < end; shift -= 4)
        *dst++ = hex[(value >> shift) & 0xf];

    *dst = 0;
    return dst;
}

static void set_error_code(const char *phase, int code)
{
    char *p = last_error_buf;
    char *end = last_error_buf + sizeof(last_error_buf);

    p = append_text(p, end, phase);
    p = append_text(p, end, ": ");
    p = append_int(p, end, code);

    if (code == IOS_ETIMEDOUT && ipc_timeout_valid) {
        p = append_text(p, end, " t=");
        p = append_int(p, end, (int)ipc_last_type);
        p = append_text(p, end, " c=");
        p = append_hex32(p, end, ipc_last_control);
        p = append_text(p, end, " r=");
        append_hex32(p, end, ipc_last_reply);
    }

    last_error = last_error_buf;
}

static uint32_t ticks(void)
{
    uint32_t tbl;
    __asm__ volatile("mftb %0" : "=r"(tbl));
    return tbl;
}

static void delay_us(uint32_t usec)
{
    uint32_t start = ticks();
    uint32_t wait = usec * 61;

    while ((uint32_t)(ticks() - start) < wait)
        ;
}

static void dcache_invalidate(const void *addr, uint32_t size)
{
    uint32_t start = (uint32_t)addr & ~31;
    uint32_t end = ((uint32_t)addr + size + 31) & ~31;
    uint32_t p;

    if (size == 0)
        return;

    for (p = start; p < end; p += 32)
        __asm__ volatile("dcbi 0,%0" :: "r"(p));

    __asm__ volatile("sync" ::: "memory");
}

static uint32_t phys_addr(const void *addr)
{
    return (uint32_t)addr & 0x3fffffff;
}

static void ipc_ack(uint32_t bit)
{
    uint32_t control = IPC_REG_CONTROL;
    IPC_REG_CONTROL = (control & IPC_CONTROL_KEEP_MASK) | bit;
    IPC_REG_INTERRUPT_ACK = IPC_INTERRUPT_ACK_BIT;
    __asm__ volatile("sync" ::: "memory");
}

static void ipc_init(void)
{
    if (ipc_ready)
        return;

    IPC_REG_CONTROL = 0x38;
    __asm__ volatile("sync" ::: "memory");
    ipc_ready = 1;
}

static int ipc_sync(ios_request_t *req)
{
    uint32_t req_phys = phys_addr(req);
    uint32_t start;

    ipc_init();

    req->word[12] = IPC_MAGIC;
    cache_flush_dc((const void *)req, sizeof(*req));

    IPC_REG_REQUEST = req_phys;
    IPC_REG_CONTROL = (IPC_REG_CONTROL & IPC_CONTROL_KEEP_MASK) | IPC_SEND_REQUEST;
    __asm__ volatile("sync" ::: "memory");

    ipc_timeout_valid = 0;
    ipc_last_type = req->word[0];
    ipc_last_reply = 0;
    start = ticks();
    for (;;) {
        uint32_t control = IPC_REG_CONTROL;

        ipc_last_control = control;

        if ((control & IPC_REQUEST_ACK_MASK) == IPC_REQUEST_ACK_MASK)
            ipc_ack(IPC_ACK_REQUEST);

        control = IPC_REG_CONTROL;
        ipc_last_control = control;
        if ((control & IPC_REPLY_READY_MASK) == IPC_REPLY_READY_MASK) {
            uint32_t reply_phys = IPC_REG_REPLY;
            ipc_last_reply = reply_phys;
            ipc_ack(IPC_ACK_REPLY);

            if (reply_phys == req_phys) {
                int result;

                dcache_invalidate((const void *)req, sizeof(*req));
                result = (int)req->word[1];
                ipc_timeout_valid = 0;
                ipc_ack(IPC_RELEASE_REPLY);
                return result;
            }

            ipc_ack(IPC_RELEASE_REPLY);
        }

        if (!ipc_block &&
            (uint32_t)(ticks() - start) > (IPC_TIMEOUT_US * 61)) {
            ipc_timeout_valid = 1;
            return IOS_ETIMEDOUT;
        }
    }
}

static void request_clear(void)
{
    memset((void *)&ipc_request, 0, sizeof(ipc_request));
}

static unsigned int copy_path(const char *path)
{
    unsigned int i;

    for (i = 0; i < PATH_BUF_SIZE - 1 && path[i]; i++)
        path_buf[i] = path[i];
    path_buf[i] = 0;

    while (++i < PATH_BUF_SIZE)
        path_buf[i] = 0;

    cache_flush_dc(path_buf, PATH_BUF_SIZE);
    return i;
}

static int ios_open(const char *path, int mode)
{
    if (!path)
        return -4;

    copy_path(path);
    request_clear();
    ipc_request.word[0] = IOS_REQ_OPEN;
    ipc_request.word[3] = phys_addr(path_buf);
    ipc_request.word[4] = (uint32_t)mode;
    return ipc_sync(&ipc_request);
}

static int ios_close(int fd)
{
    if (fd < 0)
        return 0;

    request_clear();
    ipc_request.word[0] = IOS_REQ_CLOSE;
    ipc_request.word[2] = (uint32_t)fd;
    return ipc_sync(&ipc_request);
}

static int ios_ioctl(int fd, uint32_t cmd, void *in, uint32_t in_len,
                     void *out, uint32_t out_len)
{
    request_clear();

    if (in && in_len)
        cache_flush_dc(in, in_len);
    if (out && out_len)
        cache_flush_dc(out, out_len);

    ipc_request.word[0] = IOS_REQ_IOCTL;
    ipc_request.word[2] = (uint32_t)fd;
    ipc_request.word[3] = cmd;
    ipc_request.word[4] = in ? phys_addr(in) : 0;
    ipc_request.word[5] = in_len;
    ipc_request.word[6] = out ? phys_addr(out) : 0;
    ipc_request.word[7] = out_len;

    cmd = (uint32_t)ipc_sync(&ipc_request);

    if (out && out_len)
        dcache_invalidate(out, out_len);

    return (int)cmd;
}

static int ios_ioctlv(int fd, uint32_t cmd, int in_count, int out_count,
                      ios_iovec_t *vec)
{
    void *orig[8];
    int total = in_count + out_count;
    int i;
    int result;

    if (total < 0 || total > 8)
        return -22;

    for (i = 0; i < total; i++) {
        orig[i] = vec[i].data;
        if (vec[i].data && vec[i].len)
            cache_flush_dc(vec[i].data, vec[i].len);
        vec[i].data = vec[i].data ? (void *)phys_addr(vec[i].data) : 0;
    }

    cache_flush_dc(vec, (uint32_t)total * sizeof(vec[0]));

    request_clear();
    ipc_request.word[0] = IOS_REQ_IOCTLV;
    ipc_request.word[2] = (uint32_t)fd;
    ipc_request.word[3] = cmd;
    ipc_request.word[4] = (uint32_t)in_count;
    ipc_request.word[5] = (uint32_t)out_count;
    ipc_request.word[6] = phys_addr(vec);

    result = ipc_sync(&ipc_request);

    dcache_invalidate(vec, (uint32_t)total * sizeof(vec[0]));
    for (i = 0; i < total; i++) {
        vec[i].data = orig[i];
        if (i >= in_count && vec[i].data && vec[i].len)
            dcache_invalidate(vec[i].data, vec[i].len);
    }

    return result;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void make_sockaddr(uint8_t *addr, uint16_t port, uint32_t ip)
{
    addr[0] = 8;
    addr[1] = IOS_AF_INET;
    put_u16(addr + 2, port);
    put_u32(addr + 4, ip);
}

static int ios_socket(int domain, int type, int protocol)
{
    ioctl_words[0] = (uint32_t)domain;
    ioctl_words[1] = (uint32_t)type;
    ioctl_words[2] = (uint32_t)protocol;
    return ios_ioctl(net_fd, IOS_NET_CMD_SOCKET, ioctl_words, 12, 0, 0);
}

static int ios_socket_close(int socket_fd)
{
    ioctl_words[0] = (uint32_t)socket_fd;
    return ios_ioctl(net_fd, IOS_NET_CMD_CLOSE, ioctl_words, 4, 0, 0);
}

static int ios_bind(int socket_fd, uint16_t port)
{
    memset(meta_buf, 0, META_BUF_SIZE);
    make_sockaddr(sockaddr_buf, port, 0);

    put_u32(meta_buf + 0, (uint32_t)socket_fd);
    put_u32(meta_buf + 4, 1);
    memcpy(meta_buf + 8, sockaddr_buf, 8);

    return ios_ioctl(net_fd, IOS_NET_CMD_BIND, meta_buf, 36, 0, 0);
}

static int net_ioctlv_out32(int fd, uint32_t cmd, void *out)
{
    ioctl_vectors[0].data = out;
    ioctl_vectors[0].len = NCD_OUT_SIZE;
    return ios_ioctlv(fd, cmd, 0, 1, ioctl_vectors);
}

static int ncd_ioctlv_out2(int fd, uint32_t cmd,
                           void *out0, uint32_t out0_len,
                           void *out1, uint32_t out1_len)
{
    ioctl_vectors[0].data = out0;
    ioctl_vectors[0].len = out0_len;
    ioctl_vectors[1].data = out1;
    ioctl_vectors[1].len = out1_len;
    return ios_ioctlv(fd, cmd, 0, 2, ioctl_vectors);
}

static int mac_is_clear_or_fallback(const uint8_t *mac)
{
    int all_zero = 1;
    int i;

    for (i = 0; i < 6; i++) {
        if (mac[i] != 0) {
            all_zero = 0;
            break;
        }
    }

    return all_zero || memcmp(mac, fallback_mac, 6) == 0;
}

static int mac_is_plausible(const uint8_t *mac)
{
    if (mac_is_clear_or_fallback(mac))
        return 0;
    if ((mac[0] & 0x01) != 0)
        return 0;
    return 1;
}

static int mac_has_tail(const uint8_t *mac)
{
    return mac[4] != 0 || mac[5] != 0;
}

static void ncd_reset_cached_config(void)
{
    ncd_ip_source_dhcp = 1;
    ncd_interface_type = WII_NET_INTERFACE_UNKNOWN;
}

static void ncd_parse_config(void)
{
    int selected = -1;
    int selected_count = 0;
    int i;

    ncd_reset_cached_config();

    for (i = 0; i < (int)NCD_CONNECTION_COUNT; i++) {
        const uint8_t *entry =
            ncd_config + NCD_HEADER_SIZE + (uint32_t)i * NCD_CONNECTION_SIZE;
        uint8_t flags = entry[NCD_CONN_FLAGS];

        if ((flags & NCD_FLAG_SELECTED) != 0) {
            selected_count++;
            selected = i;
        }
    }

    /* Empirically, the NCDGetIfConfig (cmd 3) reply has exactly one entry
     * with NCD_FLAG_SELECTED set on a valid config; treat that as the
     * concrete layout check.  We only need ip_source_dhcp + interface_type
     * from the selected entry. */
    if (selected < 0 || selected_count != 1)
        return;

    {
        const uint8_t *entry = ncd_config + NCD_HEADER_SIZE +
            (uint32_t)selected * NCD_CONNECTION_SIZE;
        uint8_t flags = entry[NCD_CONN_FLAGS];

        ncd_ip_source_dhcp =
            ((flags & NCD_FLAG_IP_DHCP) != 0) ? 1 : 0;
        ncd_interface_type =
            ((flags & NCD_FLAG_INTERFACE) != 0)
                ? WII_NET_INTERFACE_WIRED
                : WII_NET_INTERFACE_WIFI;
    }
}

static void ncd_parse_link_status(const uint8_t *status)
{
    int word0 = (int)get_u32(status + 0);

    /* HBC only special-cases negative first-word statuses (-29/-15) and
     * otherwise proceeds.  Keep this conservative: a negative status means
     * "not up yet"; anything else is still an undecoded/unknown snapshot. */
    if (word0 < 0)
        ncd_link_status = WII_NET_LINK_DOWN;
    else
        ncd_link_status = WII_NET_LINK_UNKNOWN;
}

/* NCDGetWirelessMacAddress: shape verified from System Menu RE.
 * Opens /dev/net/ncd/manage fresh per query (System Menu pattern),
 * 0 in / 2 out vectors with status (32 B) + MAC (exactly 6 B).
 * Caller invokes this after IOS_NET_CMD_STARTUP so the NCD/IP stack
 * is fully settled.  */
static void ncd_probe_wifi_mac(void)
{
    int fd;
    int ret;

    fd = ios_open("/dev/net/ncd/manage", 0);
    if (fd < 0)
        return;

    memset(ncd_out, 0, NCD_OUT_SIZE);
    memset(ncd_mac_buf, 0, NCD_MAC_BUF_SIZE);
    ret = ncd_ioctlv_out2(fd, NCD_CMD_GETWIFIMAC,
                          ncd_out, NCD_OUT_SIZE, ncd_mac_buf, 6);
    ios_close(fd);

    if (ret != 0 || ncd_out[0] != 0)
        return;

    if (mac_is_plausible(ncd_mac_buf) && mac_has_tail(ncd_mac_buf)) {
        memcpy(wifi_mac, ncd_mac_buf, 6);
        wifi_mac_valid = 1;
    }
}

static void ncd_probe_config(int ncd_fd)
{
    int ret;

    memset(ncd_config, 0, NCD_CONFIG_SIZE);
    ret = ncd_ioctlv_out2(ncd_fd, NCD_CMD_GETIFCONFIG,
                          ncd_config, NCD_CONFIG_SIZE,
                          ncd_out, NCD_OUT_SIZE);
    if (ret < 0) {
        memset(ncd_config, 0, NCD_CONFIG_SIZE);
        ret = ncd_ioctlv_out2(ncd_fd, NCD_CMD_READCONFIG,
                              ncd_config, NCD_CONFIG_SIZE,
                              ncd_out, NCD_OUT_SIZE);
    }

    if (ret == 0)
        ncd_parse_config();
    else
        ncd_reset_cached_config();
}

static int open_network_devices(void)
{
    int ncd_fd;
    int kd_fd;
    int ret;
    int i;

    ncd_reset_cached_config();
    ncd_link_status = WII_NET_LINK_UNKNOWN;
    wifi_mac_valid = 0;
    ethernet_mac_valid = 0;

    ncd_fd = -1;
    for (i = 0; i < 16; i++) {
        ncd_fd = ios_open("/dev/net/ncd/manage", 0);
        if (ncd_fd >= 0)
            break;
        if (ncd_fd != -6) {
            set_error_code("open ncd failed", ncd_fd);
            return ncd_fd;
        }
        delay_us(NET_START_DELAY_US);
    }
    if (ncd_fd < 0) {
        set_error_code("open ncd timed out", ncd_fd);
        return ncd_fd;
    }

    ret = net_ioctlv_out32(ncd_fd, NCD_CMD_GETLINKSTATUS, ncd_out);
    if (ret < 0) {
        ios_close(ncd_fd);
        set_error_code("ncd ioctlv 7 failed", ret);
        return ret;
    }
    ncd_parse_link_status(ncd_out);

    /* MAC retrieval is deferred to after IOS_NET_CMD_STARTUP completes,
     * per AGENT/wii_sysmenu_mac_re.md: the System Menu only queries the
     * MAC once NCD's link state has settled.  Config (cmd 3/5) still
     * happens here because we need its contents to drive interface-type
     * selection before the rest of bring-up. */
    ncd_probe_config(ncd_fd);
    ios_close(ncd_fd);

    net_fd = ios_open("/dev/net/ip/top", 0);
    if (net_fd < 0) {
        set_error_code("open ip/top failed", net_fd);
        return net_fd;
    }

    kd_fd = ios_open("/dev/net/kd/request", 0);
    if (kd_fd < 0) {
        set_error_code("open kd failed", kd_fd);
        return kd_fd;
    }

    ret = ios_ioctl(kd_fd, 6, 0, 0, kd_out, KD_OUT_SIZE);
    ios_close(kd_fd);
    if (ret < 0) {
        set_error_code("kd ioctl 6 failed", ret);
        return ret;
    }

    ret = ios_ioctl(net_fd, IOS_NET_CMD_STARTUP, 0, 0, 0, 0);
    if (ret < 0)
        set_error_code("ip startup 31 failed", ret);
    return ret;
}

/* GETIP returns the IPv4 (which looks signed-negative on PPC for 192.x/
 * 10.x, so sign is useless), 0 while DHCP is still pending, or a small
 * negative IOS error (e.g. -116 ETIMEDOUT once the lease lapses).  Every
 * IOS error is 0xFFFFFFxx (first octet 0xFF) and no valid host IPv4 has
 * first octet 0 or 255 — that cleanly separates a usable address from
 * "not up / error". */
static int getip_ok(int ret)
{
    uint32_t v = (uint32_t)ret;
    uint32_t hi = v >> 24;

    return v != 0 && hi != 0x00 && hi != 0xFF;
}

static int poll_local_ip(uint32_t *ip)
{
    uint32_t i;

    for (i = 0; i < NET_START_RETRIES; i++) {
        int ret = ios_ioctl(net_fd, IOS_NET_CMD_GETIP, 0, 0, 0, 0);
        if (getip_ok(ret)) {
            *ip = (uint32_t)ret;
            return 0;
        }
        delay_us(NET_START_DELAY_US);
    }

    ipc_timeout_valid = 0;
    set_error_code("ip query 16 timed out", IOS_ETIMEDOUT);
    return IOS_ETIMEDOUT;
}

/* Forward declaration: definition lives below, after the public init/recvfrom
 * cluster, but probe_active_link_state and probe_active_mac_from_getifopt
 * both need to call it during the init sequence. */
static int wii_ios_net_getifopt(uint32_t optname, uint8_t *out, uint32_t outlen);

static int probe_active_link_state(void)
{
    uint8_t raw[4];
    uint32_t state;
    int ret;

    memset(raw, 0, sizeof(raw));
    ret = wii_ios_net_getifopt(0x1005, raw, sizeof(raw));
    if (ret < 0)
        return ret;

    state = get_u32(raw);
    ncd_link_status = state ? WII_NET_LINK_UP : WII_NET_LINK_DOWN;
    return ncd_link_status;
}

static int probe_active_mac_from_getifopt(uint8_t *mac)
{
    uint8_t raw[6];
    int ret;

    /* System Menu RE: Nintendo calls SOGetInterfaceOpt(0x1004) with the
     * caller's exact size = 6 and accepts the raw 6 bytes as the
     * active-interface MAC.  Match that. */
    memset(raw, 0, sizeof(raw));
    ret = wii_ios_net_getifopt(0x1004, raw, sizeof(raw));
    if (ret < 0)
        return ret;

    if (!mac_is_plausible(raw) || !mac_has_tail(raw))
        return -1;

    memcpy(mac, raw, 6);
    return 0;
}

static void choose_display_mac(void)
{
    if (ncd_interface_type == WII_NET_INTERFACE_WIFI && wifi_mac_valid) {
        memcpy(local_mac, wifi_mac, 6);
        return;
    }

    if (ncd_interface_type == WII_NET_INTERFACE_WIRED &&
        ethernet_mac_valid) {
        memcpy(local_mac, ethernet_mac, 6);
        return;
    }
}

const char *wii_ios_net_last_error(void)
{
    return last_error;
}

int wii_ios_net_init(wii_net_config_t *config)
{
    int ret;

    if (udp_fd >= 0) {
        if (config) {
            config->ip = local_ip;
            memcpy(config->mac, local_mac, 6);
            config->ip_source_dhcp = ncd_ip_source_dhcp;
            config->interface_type = ncd_interface_type;
        }
        return 0;
    }

    memcpy(local_mac, fallback_mac, 6);

    ret = open_network_devices();
    if (ret < 0)
        goto fail;

    ret = poll_local_ip(&local_ip);
    if (ret < 0)
        goto fail;

    /* MAC probes go HERE — after open_network_devices() ran IOS_NET_CMD_STARTUP
     * and poll_local_ip() saw a valid IP.  Mirrors the System Menu pattern of
     * waiting for the NCD/IP stack to be ready before sampling either MAC. */
    ncd_probe_wifi_mac();
    (void)probe_active_link_state();

    if (ncd_interface_type == WII_NET_INTERFACE_WIRED) {
        uint8_t mac[6];
        if (probe_active_mac_from_getifopt(mac) == 0) {
            memcpy(ethernet_mac, mac, 6);
            ethernet_mac_valid = 1;
        }
    }

    udp_fd = ios_socket(IOS_AF_INET, IOS_SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        ret = udp_fd;
        set_error_code("udp socket 15 failed", ret);
        goto fail;
    }

    ret = ios_bind(udp_fd, NET_DEFAULT_PORT);
    if (ret < 0) {
        set_error_code("udp bind 2 failed", ret);
        goto fail;
    }

    /* Socket stays BLOCKING.  The idle-wedge was never blocking itself —
     * it was ipc_sync abandoning the recv after 5 s and reusing the
     * shared buffer.  ipc_block makes recvfrom's ipc_sync wait forever
     * for the datagram instead, so an idle loader just blocks (correct,
     * like DC/GC) with no abandon, no poll-flood, full upload speed. */

    choose_display_mac();

    if (config) {
        config->ip = local_ip;
        memcpy(config->mac, local_mac, 6);
        config->ip_source_dhcp = ncd_ip_source_dhcp;
        config->interface_type = ncd_interface_type;
    }

    last_error = "ok";
    return 0;

fail:
    wii_ios_net_shutdown();
    return ret;
}

void wii_ios_net_shutdown(void)
{
    if (udp_fd >= 0) {
        ios_socket_close(udp_fd);
        udp_fd = -1;
    }

    if (net_fd >= 0) {
        ios_close(net_fd);
        net_fd = -1;
    }
}

int wii_ios_net_recvfrom(uint8_t *payload, uint32_t payload_capacity,
                         uint32_t *src_ip, uint16_t *src_port)
{
    uint32_t cap;
    int ret;

    if (udp_fd < 0 || !payload || payload_capacity == 0)
        return -1;

    cap = payload_capacity;
    if (cap > WII_NET_MAX_UDP_PAYLOAD)
        cap = WII_NET_MAX_UDP_PAYLOAD;

    memset(meta_buf, 0, 8);
    put_u32(meta_buf + 0, (uint32_t)udp_fd);
    put_u32(meta_buf + 4, 0);

    make_sockaddr(sockaddr_buf, 0, 0);
    sockaddr_len_buf[0] = 8;
    sockaddr_len_buf[1] = 0;
    sockaddr_len_buf[2] = 0;
    sockaddr_len_buf[3] = 0;

    ioctl_vectors[0].data = meta_buf;
    ioctl_vectors[0].len = 8;
    ioctl_vectors[1].data = rx_buf;
    ioctl_vectors[1].len = cap;
    ioctl_vectors[2].data = sockaddr_buf;
    ioctl_vectors[2].len = 8;

    /* Blocking recv: ipc_sync must wait for the datagram, never abandon. */
    ipc_block = 1;
    ret = ios_ioctlv(net_fd, IOS_NET_CMD_RECVFROM, 1, 2, ioctl_vectors);
    ipc_block = 0;
    if (ret == IOS_EAGAIN || ret == IOS_EWOULDBLOCK)
        return -1;
    if (ret <= 0)
        return ret;
    if ((uint32_t)ret > cap)
        ret = (int)cap;

    memcpy(payload, rx_buf, (uint32_t)ret);
    if (src_ip)
        *src_ip = get_u32(sockaddr_buf + 4);
    if (src_port)
        *src_port = get_u16(sockaddr_buf + 2);

    return ret;
}

int wii_ios_net_sendto(const uint8_t *payload, uint32_t payload_len,
                       uint32_t dest_ip, uint16_t dest_port)
{
    int ret;

    if (udp_fd < 0 || !payload)
        return -1;
    if (payload_len > WII_NET_MAX_UDP_PAYLOAD)
        return -1;

    memcpy(tx_buf, payload, payload_len);
    make_sockaddr(sockaddr_buf, dest_port, dest_ip);

    memset(meta_buf, 0, 40);
    put_u32(meta_buf + 0, (uint32_t)udp_fd);
    put_u32(meta_buf + 4, 0);
    put_u32(meta_buf + 8, 1);
    memcpy(meta_buf + 12, sockaddr_buf, 8);

    ioctl_vectors[0].data = tx_buf;
    ioctl_vectors[0].len = payload_len;
    ioctl_vectors[1].data = meta_buf;
    ioctl_vectors[1].len = 40;

    /* Same no-abandon rule as recvfrom: under load (e.g. the -t dhcp
     * VERS flood) IOS is slow but still replies; abandoning the sendto
     * at 5 s and reusing the shared ipc_request buffer under it is what
     * corrupts the IPC ring and wedges the loader after a while. */
    ipc_block = 1;
    ret = ios_ioctlv(net_fd, IOS_NET_CMD_SENDTO, 2, 0, ioctl_vectors);
    ipc_block = 0;
    return ret;
}

/* SOGetInterfaceOpt: ip/top ioctlv 0x1C, level 0xFFFE.  1 in / 2 out:
 *   in[0]  = { u32 level=0xFFFE, u32 optname }   (big-endian words)
 *   out[0] = optval (caller buffer)
 *   out[1] = optval size (caller passes `outlen` here and reads the
 *           IOS-updated value back as the "actual size written").
 * WiiBrew /dev/net/ip/top "Socket" page; shape verified from System Menu RE. */
static int wii_ios_net_getifopt(uint32_t optname, uint8_t *out, uint32_t outlen)
{
    int ret;

    if (net_fd < 0 || !out || outlen == 0 || outlen > IFOPT_VAL_BUF_SIZE)
        return -1;

    put_u32(ifopt_req_buf + 0, 0xFFFE);
    put_u32(ifopt_req_buf + 4, optname);
    memset(ifopt_len_buf, 0, IFOPT_LEN_BUF_SIZE);
    put_u32(ifopt_len_buf, outlen);
    memset(ifopt_val_buf, 0, outlen);

    ioctl_vectors[0].data = ifopt_req_buf; ioctl_vectors[0].len = 8;
    ioctl_vectors[1].data = ifopt_val_buf; ioctl_vectors[1].len = outlen;
    ioctl_vectors[2].data = ifopt_len_buf; ioctl_vectors[2].len = 4;

    ret = ios_ioctlv(net_fd, IOS_NET_CMD_GETIFOPT, 1, 2, ioctl_vectors);
    if (ret < 0)
        return ret;

    memcpy(out, ifopt_val_buf, outlen);
    return 0;
}

/* Opt 0xC001 = "DHCP lease time remaining?" (WiiBrew, example 0x29EF ≈
 * 10735 s).  Returns remaining seconds, or a negative IOS rc on failure
 * (so the on-screen LEASE: field doubles as an error probe). */
int wii_ios_net_lease_secs(void)
{
    uint8_t b[4];
    int r;

    if (net_fd < 0)
        return -1;
    r = wii_ios_net_getifopt(0xC001, b, 4);
    if (r < 0)
        return r;
    return (int)get_u32(b);
}

int wii_ios_net_poll_link_state(void)
{
    if (net_fd < 0)
        return -1;
    return probe_active_link_state();
}

/* SOPoll: /dev/net/ip/top ioctl 0x0B.
 *   in[0]  = u64 timeout in ms (8 B, BE)
 *   out[0] = array of struct pollsd { s32 socket; s32 events; s32 revents; }
 *
 * Reuses the MEM2 ifopt_req_buf (>= 8 B) for the timeout input and the
 * MEM2 ifopt_val_buf (>= 12 B) for the single-pollfd output.  Same MEM2
 * placement rule as the MAC retrieval — short non-word-aligned writes
 * back to MEM1 BSS were lossy on this Wii. */
int wii_ios_net_poll_recv(int timeout_ms)
{
    int ret;
    uint32_t revents;

    if (net_fd < 0 || udp_fd < 0)
        return -1;

    /* IN: u64 timeout (BE). */
    put_u32(ifopt_req_buf + 0, 0);
    put_u32(ifopt_req_buf + 4, (uint32_t)timeout_ms);

    /* OUT: one struct pollsd { s32 socket; s32 events; s32 revents; } = 12 B.
     * POLLIN = 0x0001 (standard POSIX value). */
    put_u32(ifopt_val_buf + 0, (uint32_t)udp_fd);
    put_u32(ifopt_val_buf + 4, 0x0001);
    put_u32(ifopt_val_buf + 8, 0);

    ret = ios_ioctl(net_fd, 0x0B, ifopt_req_buf, 8, ifopt_val_buf, 12);
    if (ret < 0)
        return ret;

    revents = get_u32(ifopt_val_buf + 8);
    return (revents & 0x0001) ? 1 : 0;
}

