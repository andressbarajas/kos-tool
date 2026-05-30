/* include/kosload/protocol.h */
#ifndef KOSLOAD_PROTOCOL_H
#define KOSLOAD_PROTOCOL_H

#include <stdint.h>

/*
 * Shared protocol definitions for kosload.
 *
 * Keep command IDs, packet structures, capability-related constants, adapter
 * IDs, and memory-layout constants centralized here so host and client code
 * stay in sync as new consoles or transports are added.
 *
 * Serial protocol: Preserved from dcload-serial
 *   - Single-byte commands ('A'-'V'), LZO compression, XOR checksums
 *
 * Network protocol: Preserved from dcload-ip v2
 *   - UDP command_t packets, 1440-byte payloads, packet recovery
 */

/* ===== Serial Protocol Command Bytes ===== */

#define SERIAL_CMD_EXECUTE      'A'     /* Execute at address */
#define SERIAL_CMD_LOAD_BEGIN   'B'     /* Begin binary load (address + size) */
#define SERIAL_CMD_DOWNLOAD     'F'     /* Download from DC (verbose) */
#define SERIAL_CMD_DOWNLOAD_Q   'G'     /* Download from DC (quiet) */
#define SERIAL_CMD_CDFS_REDIR   'H'     /* Enable CDFS redirection */
#define SERIAL_CMD_CAPABILITIES 'I'     /* Query capability bitmask */
#define SERIAL_CMD_SPEED        'S'     /* Change serial speed */
#define SERIAL_CMD_SETRTC       'W'     /* Set RTC to host time */

/* Serial data transfer types */
#define SERIAL_DATA_UNCOMPRESSED 'U'
#define SERIAL_DATA_COMPRESSED   'C'
#define SERIAL_DATA_GOOD         'G'
#define SERIAL_DATA_BAD          'B'

/* Serial console syscall command IDs (numeric) */
#define SERIAL_SYSCALL_EXIT         0
#define SERIAL_SYSCALL_FSTAT        1
#define SERIAL_SYSCALL_WRITE        2
#define SERIAL_SYSCALL_READ         3
#define SERIAL_SYSCALL_OPEN         4
#define SERIAL_SYSCALL_CLOSE        5
#define SERIAL_SYSCALL_CREAT        6
#define SERIAL_SYSCALL_LINK         7
#define SERIAL_SYSCALL_UNLINK       8
#define SERIAL_SYSCALL_CHDIR        9
#define SERIAL_SYSCALL_CHMOD        10
#define SERIAL_SYSCALL_LSEEK        11
#define SERIAL_SYSCALL_TIME         12
#define SERIAL_SYSCALL_STAT         13
#define SERIAL_SYSCALL_UTIME        14
#define SERIAL_SYSCALL_BAD          15
#define SERIAL_SYSCALL_OPENDIR      16
#define SERIAL_SYSCALL_CLOSEDIR     17
#define SERIAL_SYSCALL_READDIR      18
#define SERIAL_SYSCALL_CDFSREAD     19
#define SERIAL_SYSCALL_GDBPACKET    20
#define SERIAL_SYSCALL_REWINDDIR    21
#define SERIAL_SYSCALL_PROGEXIT     22
#define SERIAL_SYSCALL_MKDIR        23

/* ===== Network Protocol Command IDs (4-byte ASCII) ===== */

#define NET_CMD_EXECUTE      "EXEC" /* Execute at address */
#define NET_CMD_LOADBIN      "LBIN" /* Begin receiving binary */
#define NET_CMD_PARTBIN      "PBIN" /* Part of a binary */
#define NET_CMD_DONEBIN      "DBIN" /* End receiving binary */
#define NET_CMD_SENDBIN      "SBIN" /* Send a binary */
#define NET_CMD_SENDBINQ     "SBIQ" /* Send a binary, quiet */
#define NET_CMD_VERSION      "VERS" /* Version info exchange */
#define NET_CMD_CAPABILITIES "CAPS" /* Query capability bitmask */
#define NET_CMD_RETVAL       "RETV" /* Return value */
#define NET_CMD_REBOOT       "RBOT" /* Reboot console */
#define NET_CMD_MAPLE        "MAPL" /* Maple bus passthrough */
#define NET_CMD_PMCR         "PMCR" /* Performance counter */
#define NET_CMD_SETRTC       "SRTC" /* Set RTC to host time */

/* Network console syscall command IDs (4-byte ASCII) */
#define NET_SYSCALL_EXIT      "DC00"
#define NET_SYSCALL_FSTAT     "DC01"
#define NET_SYSCALL_WRITE_OLD "DD02" /* Legacy write command */
#define NET_SYSCALL_WRITE     "DC02"
#define NET_SYSCALL_READ      "DC03"
#define NET_SYSCALL_OPEN      "DC04"
#define NET_SYSCALL_CLOSE     "DC05"
#define NET_SYSCALL_CREAT     "DC06"
#define NET_SYSCALL_LINK      "DC07"
#define NET_SYSCALL_UNLINK    "DC08"
#define NET_SYSCALL_CHDIR     "DC09"
#define NET_SYSCALL_CHMOD     "DC10"
#define NET_SYSCALL_LSEEK     "DC11"
#define NET_SYSCALL_TIME      "DC12"
#define NET_SYSCALL_STAT      "DC13"
#define NET_SYSCALL_UTIME     "DC14"
#define NET_SYSCALL_BAD       "DC15"
#define NET_SYSCALL_OPENDIR   "DC16"
#define NET_SYSCALL_CLOSEDIR  "DC17"
#define NET_SYSCALL_READDIR   "DC18"
#define NET_SYSCALL_CDFSREAD  "DC19"
#define NET_SYSCALL_GDBPACKET "DC20"
#define NET_SYSCALL_REWINDDIR "DC21"
#define NET_SYSCALL_PROGEXIT  "DC22"
#define NET_SYSCALL_MKDIR     "DC23"

/* ===== Loader-side capabilities (kosload -> kos-tool) ===== */

#define KOSLOAD_CAP_SERIAL     (1 << 0)
#define KOSLOAD_CAP_NETWORK    (1 << 1)
#define KOSLOAD_CAP_GDB        (1 << 2)
#define KOSLOAD_CAP_DHCP       (1 << 3)
#define KOSLOAD_CAP_CDFS_REDIR (1 << 4)
#define KOSLOAD_CAP_ARGV       (1 << 5)

/* ===== Host-side capabilities (kos-tool -> kosload) */
#define KOSTOOL_CAP_GDB_ACTIVE (1 << 0) /* kos-tool started with -g */

#define KOSLOAD_EXCEPTION_TAG "EXPT" /* Exception frame marker (both serial and network) */

/* ===== NET_CMD_EXECUTE flag bits (carried in command->size) ===== */

#define KOSLOAD_EXEC_CONSOLE (1 << 0) /* enable console redirection while program runs */
#define KOSLOAD_EXEC_CDFS    (1 << 1) /* enable CDFS redir while program runs */
#define KOSLOAD_EXEC_FW_UPDATE                                                                               \
    (1 << 2) /* target is a firmware-update                                                                  \
              * trampoline: skip the console/CDFS                                                            \
              * setup meant for user programs.                                                               \
              * The execute path is otherwise the                                                            \
              * same as a normal program launch. */

/* ===== Network Packet Structures ===== */

#define NET_COMMAND_LEN   12

/* Network command packet (UDP payload) */
typedef struct {
    uint8_t  id[4];
    uint32_t address;
    uint32_t size;
    uint8_t  data[];
} __attribute__((packed, aligned(4))) net_command_t;

/* Compatibility alias for DC net stack code */
#define COMMAND_LEN NET_COMMAND_LEN

/* Extended command types for syscall requests */
typedef struct {
    uint8_t  id[4];
    uint32_t value0;
} __attribute__((packed)) net_command_int_t;

typedef struct {
    uint8_t  id[4];
    uint32_t value0;
    uint32_t value1;
    uint32_t value2;
} __attribute__((packed)) net_command_3int_t;

typedef struct {
    uint8_t  id[4];
    uint32_t value0;
    uint32_t value1;
    char     string[];
} __attribute__((packed)) net_command_2int_string_t;

typedef struct {
    uint8_t  id[4];
    uint32_t value0;
    char     string[];
} __attribute__((packed)) net_command_int_string_t;

typedef struct {
    uint8_t id[4];
    char string[];
} __attribute__((packed)) net_command_string_t;

typedef struct {
    uint8_t  id[4];
    uint32_t value0;
    uint32_t value1;
    uint32_t value2;
    char     string[];
} __attribute__((packed)) net_command_3int_string_t;

/* ===== Network Constants ===== */

#define NET_LEGACY_PORT         31313
#define NET_DEFAULT_PORT        53535
#define NET_LEGACY_PAYLOAD_SIZE 1024
#define NET_PAYLOAD_SIZE        1440
#define NET_PACKET_TIMEOUT_USEC 250000 /* 250ms */

#define NET_GDB_PORT    2159

/* ===== Optional syscall extensions ===== */
/*
 * Two optional trailers may follow the legacy syscall payload of a UDP
 * datagram.  Old hosts/clients (e.g. dc-load-ip 2.0.1) size the payload
 * from its in-band length fields and ignore trailing bytes, so both are
 * safe to add in either direction.
 *
 * KSQ0 (8 bytes: magic + big-endian seq id)
 *   A request tag.  The client puts a unique seq id on each syscall
 *   request; a 3.0.0+ host echoes it on the RETVAL and caches the reply
 *   under that id (host/src/dedup.c).  If the same request arrives again
 *   (its first copy or the reply was lost), the host replays the cached
 *   reply instead of re-running the syscall — which is what keeps
 *   non-idempotent calls (read, lseek, readdir) correct under packet
 *   loss.  The client only retransmits against a 3.0.0+ host; against a
 *   legacy host it sends the tag but never retries (a retry there would
 *   re-run the syscall).  Same idea as NFS's reply cache (RFC 1813) and
 *   CoAP's message-id dedup (RFC 7252).
 *
 * KIR0 (4-byte magic)
 *   Added to fstat/stat/readdir requests: "you may inline the result in
 *   the RETVAL instead of a separate SENDBINQ+DONEBIN trip."  Turns a
 *   2-packet reply into 1, halving loss odds.  Like CoAP's piggybacked
 *   response.
 *
 * Both sit at the very end of the UDP payload, after any inline data:
 *   request:  [legacy payload] [KIR0?] [KSQ0]
 *   response: [legacy payload] [KIR0 + inline buffer?] [KSQ0]
 */
#define NET_SEQ_MAGIC            "KSQ0"
#define NET_SEQ_MAGIC_LEN        4
#define NET_SEQ_TRAILER_LEN      8   /* magic (4) + seq id BE (4) */

#define NET_INLINE_RET_MAGIC     "KIR0"
#define NET_INLINE_RET_MAGIC_LEN 4

/* ===== Adapter Type Constants ===== */

#define ADAPTER_NONE         0x0000
#define ADAPTER_DC_BBA          0x0400 /* Dreamcast Broadband Adapter (RTL8139C) */
#define ADAPTER_DC_LAN          0x0300 /* Dreamcast LAN Adapter */
#define ADAPTER_DC_W5500        0x5500 /* Dreamcast W5500 (SCIF-SPI) */
#define ADAPTER_GC_BBA          0x0015 /* GameCube Broadband Adapter */
#define ADAPTER_GC_ENC          0x2860 /* GameCube ENC28J60 (EXI-SPI) */
#define ADAPTER_GC_W5500        0x5501 /* GameCube W5500 (EXI-SPI) */
#define ADAPTER_PS2_BBA         0x0500 /* PlayStation 2 Broadband Adapter */
#define ADAPTER_WII_LAN_WIFI    0x0E58 /* Wii IOS net — USB LAN Adapter (RVL-015) or internal Wi-Fi */

/* ===== Serial Constants ===== */

#define SERIAL_DEFAULT_SPEED    57600
#define SERIAL_BUFFER_SIZE      16384

/* Default load addresses */
#define DC_DEFAULT_LOAD_ADDR    0x8c010000
#define GC_DEFAULT_LOAD_ADDR    0x80003100
#define WII_DEFAULT_LOAD_ADDR   0x80004000
#define PS2_DEFAULT_LOAD_ADDR   0x00100000

/* Memory layout constants.  Wii MEM1/MEM2 ceilings deliberately omitted:
 * the build system passes the channel-safe loader ceiling via -DWII_MEM1_TOP
 * (see mk/memory.mk), which differs from the hardware-architectural ceiling.
 * Defining the architectural value here too would shadow the build-system
 * value with a redefinition warning, and no source consumer needs it. */
#define DC_RAM_TOP     0x8d000000 /* 16 MB SH4 RAM */
#define GC_RAM_TOP     0x81800000 /* 24 MB MEM1 */
#define PS2_RAM_TOP    0x02000000 /* 32 MB EE RAM */

#ifndef GC_LOADER_BASE
#define GC_LOADER_BASE     0x817EC000 /* keep in sync with mk/memory.mk */
#endif
#ifndef PS2_LOADER_BASE
#define PS2_LOADER_BASE    0x00100000 /* keep in sync with mk/memory.mk */
#endif

#define LZO_WRKMEM_SIZE    0x10000 /* 64 KB — LZO1X_1_MEM_COMPRESS */

/* LZO work memory addresses for serial downloads.
 * Placed just below the top of usable RAM on each console. */
#define DC_LZO_WRKMEM_ADDR   (DC_RAM_TOP - LZO_WRKMEM_SIZE)     /* 0x8cff0000 */
#define GC_LZO_WRKMEM_ADDR   (GC_LOADER_BASE - LZO_WRKMEM_SIZE) /* 0x817DC000 */

/* ===== KOS Open Flags (for flag translation) ===== */

#define KOS_O_WRONLY    0x0001
#define KOS_O_RDWR      0x0002
#define KOS_O_APPEND    0x0008
#define KOS_O_CREAT     0x0200
#define KOS_O_TRUNC     0x0400
#define KOS_O_EXCL      0x0800

/* ===== Network Packet Headers (for client side) ===== */
/* Alignment attributes match the DC network stack's packet buffer alignment
 * strategy (2-byte offset from 32-byte aligned raw buffer). */

typedef struct {
    uint8_t dest[6];
    uint8_t src[6];
    uint8_t type[2];
} __attribute__((packed, aligned(2))) ether_header_t;

typedef struct {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t length;
    uint16_t packet_id;
    uint16_t flags_frag_offset;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dest;
} __attribute__((packed, aligned(8))) ip_header_t;

typedef struct {
    uint16_t src;
    uint16_t dest;
    uint16_t length;
    uint16_t checksum;
    uint8_t  data[];
} __attribute__((packed, aligned(4))) udp_header_t;

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint32_t misc;
} __attribute__((packed, aligned(4))) icmp_header_t;

typedef struct {
    uint16_t hw_addr_space;
    uint16_t proto_addr_space;
    uint8_t  hw_addr_len;
    uint8_t  proto_addr_len;
    uint16_t opcode;
    uint8_t  hw_sender[6];
    uint8_t  proto_sender[4];
    uint8_t  hw_target[6];
    uint8_t  proto_target[4];
} __attribute__((packed, aligned(8))) arp_header_t;

/* Protocol identifiers */
#define IP_PROTO_ICMP   1
#define IP_PROTO_UDP    17

/* Ethernet types */
#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_ARP    0x0806

/* RX/TX buffer sizes */
#define RAW_PKT_BUF_SIZE    1536
#define PKT_BUF_SIZE        1514

#endif /* KOSLOAD_PROTOCOL_H */
