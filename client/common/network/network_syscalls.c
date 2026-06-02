/* client/common/network/network_syscalls.c */
/*
 * Network transport syscall implementations for kosload client.
 * Based on dcload-ip: dcload-ip/target-src/dcload/syscalls.c
 *
 * These functions implement POSIX-like file I/O by sending UDP command
 * packets to the host tool and waiting for NET_CMD_RETVAL responses.
 * Each syscall builds a packet in pkt_buf, calls build_send_packet()
 * to transmit it, then enters bb->loop(0) to wait for the response.
 *
 * The crt0.S syscall table maps slots 0-21 to these functions.
 */

#include <string.h>
#include <kosload/protocol.h>
#include <kosload/types.h>
#include <kosload/net_adapter.h>
#include <kosload/net_stack.h>
#include <kosload/target.h>
#include "commands.h"
#include "packet.h"

/* ===== Syscall shared state ===== */

unsigned short kosload_syscall_port = 31313;
unsigned int syscall_retval = 0;
unsigned char *syscall_data = 0;
unsigned int syscall_data_len = 0;

/* Per-syscall bounded-wait deadline, armed only for a lossy adapter
 * (adapter_t.lossy — today the Wii's internal Wi-Fi).
 * kos_syscall_wait_for_retval() sets it before bb->loop(0); reliable
 * adapters never enter that path, so it stays 0 for them.  Defined here so
 * the variable has exactly one definition across all builds — the extern in
 * <kosload/net_adapter.h> lets the lossy adapter's loop reach it. */
volatile uint64_t loop_deadline_ticks = 0;

/* Sequencing + retransmit state.  The KSQ0 trailer (see
 * include/kosload/protocol.h) is appended to every outgoing syscall request
 * regardless of which console this is — old hosts ignore the extra 8 bytes,
 * new hosts use them to dedup.  The bounded-wait retransmit half
 * (kos_syscall_wait_for_retval) only runs for a lossy adapter
 * (adapter_t.lossy) AND additionally needs a dedup-aware host (kosload
 * >= 3.0.0, see host_supports_dedup() below).  Reliable adapters and legacy
 * hosts keep the original wait-forever semantics: reliable links don't drop
 * packets, and retransmitting to a non-dedup host would risk re-running a
 * non-idempotent syscall (read, lseek, readdir).
 *
 * The saved-packet buffer caches the full Ethernet/IP/UDP frame of the most
 * recent build_send_packet() so the retransmit can re-tx the exact same
 * bytes (including the original seq id, so the host can recognize the
 * duplicate). */
#define KOS_SYSCALL_SAVE_MAX 2048
static unsigned char kos_syscall_save_pkt[KOS_SYSCALL_SAVE_MAX] __attribute__((aligned(4)));
static unsigned int  kos_syscall_save_len = 0;

/* Bounded-wait interval (ticks).  Stored so the RX path can push the
 * deadline forward whenever a packet arrives (see
 * net_bump_syscall_deadline) — the timeout then means "no progress for
 * this long", not "total syscall time exceeded this", which is what lets
 * a large multi-packet read() over a slow link complete instead of
 * spuriously timing out mid-transfer. */
static uint64_t kos_syscall_timeout_ticks = 0;

unsigned int kos_syscall_seq = 0;          /* echoed-back seq id of in-flight req */
static unsigned int kos_syscall_next_seq = 1;

/* Retransmit timing (used only for a lossy adapter — today the Wii's
 * internal Wi-Fi).  Per-attempt *idle* timeout: net_bump_syscall_deadline
 * pushes the deadline forward on every inbound bulk-data packet, so it only
 * fires after a full interval of no progress (lost request or lost RETVAL),
 * never mid-transfer.
 *
 * MUST stay above the host's recovery cadence (NET_PACKET_TIMEOUT_USEC =
 * 250 ms, the rate send_data re-sends DONEBIN while gap-filling a lossy
 * read()).  Equal values raced: the guest retransmitted mid-recovery,
 * forcing the host to restart the transfer instead of finishing its
 * gap-fill.  600 ms keeps a 2-interval margin so the host re-arms us first. */
#define KOS_SYSCALL_RETRY_MS    600
#define KOS_SYSCALL_MAX_RETRIES 6

#define NET_WRITE_INLINE_MAX 512

/* Private dirent buffer for readdir */
typedef struct {
    long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
} dc_dirent_t;

static dc_dirent_t our_dir;

/* ===== strlen (no libc available) ===== */

static unsigned int my_strlen(const char *s) {
    unsigned int c = 0;
    while(s[c] != 0)
        c++;
    return c;
}

/* True iff the host advertised dedup support in its VERS reply (kosload
 * 3.0.0 or newer).  Talking to legacy dc-tool 2.0.x the trailer still goes
 * out (harmless) but bb->loop(0) reverts to wait-forever. */
static inline int host_supports_dedup(void) {
    return tool_version >= 0x00030000;
}

/* ===== KIR0 inline-return helpers (small fixed-size out-buffer syscalls) ===== */

static inline void append_inline_ret_magic(unsigned char *dst) {
    memcpy(dst, NET_INLINE_RET_MAGIC, NET_INLINE_RET_MAGIC_LEN);
}

/* True iff cmd_retval() captured a KIR0 inline payload that matches the
 * expected size for the small-fixed-size out-buffer of the just-completed
 * syscall.  On match, copies the inline bytes into the guest's buffer.
 * Returns 1 on hit, 0 on miss (legacy host that did the separate
 * SENDBINQ + DONEBIN bulk-transfer leg). */
static inline int copy_inline_ret(void *dst, unsigned int count) {
    if(syscall_data_len != NET_INLINE_RET_MAGIC_LEN + count)
        return 0;
    if(memcmp(syscall_data, NET_INLINE_RET_MAGIC, NET_INLINE_RET_MAGIC_LEN) != 0)
        return 0;
    memcpy(dst, syscall_data + NET_INLINE_RET_MAGIC_LEN, count);
    return 1;
}

/* ===== Packet construction helper ===== */

void build_send_packet(int command_len) {
    ether_header_t *ether = (ether_header_t *)pkt_buf;
    ip_header_t *ip = (ip_header_t *)(pkt_buf + ETHER_H_LEN);
    udp_header_t *udp = (udp_header_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN);
    unsigned int total_len;

    /* Append the KSQ0 trailer at the very end of the UDP payload.  Old
     * hosts (dc-tool 2.0.x, pre-3.0) ignore the extra 8 bytes; new hosts
     * use them to dedup retransmits of this exact syscall request.  See
     * include/kosload/protocol.h for the wire format.  Increment BEFORE
     * sending so kos_syscall_seq names the request we just put on the
     * wire — cmd_retval() will compare against this on the way back. */
    unsigned char *trailer = pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + command_len;

    kos_syscall_seq = kos_syscall_next_seq++;
    if(kos_syscall_next_seq == 0)
        kos_syscall_next_seq = 1;

    memcpy(trailer, NET_SEQ_MAGIC, NET_SEQ_MAGIC_LEN);
    trailer[NET_SEQ_MAGIC_LEN + 0] = (unsigned char)(kos_syscall_seq >> 24);
    trailer[NET_SEQ_MAGIC_LEN + 1] = (unsigned char)(kos_syscall_seq >> 16);
    trailer[NET_SEQ_MAGIC_LEN + 2] = (unsigned char)(kos_syscall_seq >> 8);
    trailer[NET_SEQ_MAGIC_LEN + 3] = (unsigned char)kos_syscall_seq;
    command_len += (int)NET_SEQ_TRAILER_LEN;

    make_ether(tool_mac, bb->mac, ether);
    make_ip(tool_ip, our_ip, UDP_H_LEN + command_len, IP_UDP_PROTOCOL, ip, 0);
    make_udp(tool_port, kosload_syscall_port, command_len, ip, udp);
    bb->start();
    total_len = ETHER_H_LEN + IP_H_LEN + UDP_H_LEN + command_len;
    bb->tx(pkt_buf, total_len);

    /* Save a copy for the retransmit path.  Oversize would mean the
     * syscall built a packet bigger than KOS_SYSCALL_SAVE_MAX (currently
     * impossible: max command < 1500); leave len 0 so the retransmit loop
     * just gives up rather than re-tx'ing garbage. */
    if(total_len <= KOS_SYSCALL_SAVE_MAX) {
        memcpy(kos_syscall_save_pkt, pkt_buf, total_len);
        kos_syscall_save_len = total_len;
    } else {
        kos_syscall_save_len = 0;
    }
}

/* Push the bounded-wait deadline forward by one interval.  Called from the
 * bulk-data command handlers (cmd_loadbin/partbin/donebin/sendbin) each
 * time a chunk arrives during a syscall wait, so a long multi-packet
 * read()/write() that keeps making progress never trips the timeout.
 * No-op outside a bounded wait (loop_deadline_ticks == 0): the idle main
 * loop and program-upload path are unaffected. */
void net_bump_syscall_deadline(void) {
    const target_ops_t *t;

    if(!loop_deadline_ticks)
        return;
    t = target_get_ops();
    if(t && t->get_ticks)
        loop_deadline_ticks = t->get_ticks() + kos_syscall_timeout_ticks;
}

/* Wait for the host's RETVAL.
 *
 * Reliable transports (bb->lossy == false — the raw-NIC drivers on
 * DC/GC/PS2, and the Wii's wired LAN Adapter): just bb->loop(0) and wait
 * forever, exactly like dc-load 2.x.  The bounded-wait retransmit is
 * skipped — on a link that doesn't drop packets its per-syscall deadline
 * would fire spuriously and flood the host with duplicate requests (one
 * retransmit for every non-bulk syscall — open/lseek/close/inline write),
 * which collapses write()-heavy programs.
 *
 * Lossy transports (bb->lossy == true — the Wii's internal Wi-Fi over IOS
 * sockets): bounded retries + retransmit, so a single lost request/reply
 * recovers.  This also needs a dedup-aware host (kosload >= 3.0.0); against
 * a legacy host it falls back to wait-forever, since retransmitting could
 * re-execute a non-idempotent syscall (read, lseek, readdir).  On timeout
 * exhaustion the syscall returns -1 by leaving syscall_retval = -1; we clear
 * loop_deadline_ticks before returning so the next syscall starts clean. */
static void kos_syscall_wait_for_retval(void) {
    const target_ops_t *t;
    uint64_t timeout_ticks;
    int retry;

    /* Reliable transport: wait forever for the RETVAL (see above). */
    if(!bb->lossy) {
        bb->loop(0);
        return;
    }

    if(!host_supports_dedup()) {
        /* Legacy host — old wait-forever semantics. */
        bb->loop(0);
        return;
    }

    t = target_get_ops();
    if(!t || !t->get_ticks || !t->ticks_per_second) {
        /* No timer — falling back to wait-forever is safer than spinning
         * tight without a deadline mechanism. */
        bb->loop(0);
        return;
    }

    timeout_ticks = ((uint64_t)t->ticks_per_second * KOS_SYSCALL_RETRY_MS) / 1000;
    if(timeout_ticks == 0)
        timeout_ticks = 1;
    kos_syscall_timeout_ticks = timeout_ticks;

    for(retry = 0; retry <= KOS_SYSCALL_MAX_RETRIES; retry++) {
        uint64_t deadline;

        escape_loop = 0;
        deadline = t->get_ticks() + timeout_ticks;
        loop_deadline_ticks = deadline;
        bb->loop(0);
        loop_deadline_ticks = 0;

        /* Did cmd_retval fire, or did the adapter break out on a stall?
         * We can't read `escape_loop` here because the adapter clears it
         * back to 0 right before returning from loop().  Use the deadline
         * as the authoritative signal: a return before the deadline means
         * the only thing that could have ended the loop was
         * `escape_loop = 1` (set by cmd_retval, the upload handlers,
         * etc.).  A return at/after the deadline means no packet arrived
         * for a full interval (the bulk-data handlers push the deadline
         * forward on every chunk via net_bump_syscall_deadline, so this
         * only trips on a genuine stall — a lost request or reply — not
         * on a long-but-progressing transfer). */
        if(t->get_ticks() < deadline) {
            /* RETVAL (or other in-band terminator) fired. */
            return;
        }

        /* Stalled.  Retransmit the saved request — same seq id, so a host
         * that already serviced it replays the cached response without
         * re-running the syscall.  Only a lossy adapter (the Wii wifi)
         * reaches here.  Copy the saved frame back into pkt_buf and TX
         * from there — pkt_buf is the buffer every send uses but gets
         * clobbered by response-building during the wait, hence the
         * separate save buffer. */
        if(kos_syscall_save_len == 0 || retry == KOS_SYSCALL_MAX_RETRIES)
            break;
        memcpy(pkt_buf, kos_syscall_save_pkt, kos_syscall_save_len);
        bb->tx(pkt_buf, kos_syscall_save_len);
    }

    kos_syscall_timeout_ticks = 0;

    /* Final timeout: surface as -1 to the guest. */
    syscall_retval = (unsigned int)-1;
    syscall_data = 0;
    syscall_data_len = 0;
}

/* Syscall 14: assign_wrkmem.  Network transport needs no LZO work memory, but
 * the return value is the host/loader transport probe: KOS's kosload driver
 * calls assign_wrkmem(0) at init and reads -1 => IP mode, 0 => serial (which
 * then gets a 64 KB compression buffer).  MUST return -1 here — returning void
 * (garbage r0) makes KOS mis-detect serial mode and drive the IP loader down a
 * compression/wrkmem path it doesn't implement, corrupting memory.  Classic
 * dcload-ip returns -1 here too, which is why legacy loaders work. */
int assign_wrkmem(unsigned char *user_buffer) {
    (void)user_buffer;
    return -1;
}

/* ===== Syscall implementations ===== */

/* progexit is the one syscall with no request/response round-trip: it
 * fires the PROGEXIT notification and the guest halts immediately, so
 * there is no bb->loop(0) parked to recover a dropped datagram the way
 * every other syscall implicitly does.  On wired raw-Ethernet that lone
 * datagram effectively never goes missing; on a lossy/offloaded path
 * (Wii IOS/WiFi) it can, and the host then waits for it forever.
 *
 * Give the notification delivery robustness of its own: emit it several
 * times before halting (this is the one case wired consoles benefit from
 * too, since they have no retransmit at all).  Idempotent — the host
 * terminates on the first copy received and has already left its console
 * loop, so the duplicates are discarded. */
#define PROGEXIT_RETRANSMIT_COUNT 5

void progexit(int ret_code) {
    int i;

    bb->stop();

    net_command_t *command = (net_command_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_PROGEXIT, 4);
    command->address = htonl((unsigned int)ret_code);
    command->size = 0;

    for(i = 0; i < PROGEXIT_RETRANSMIT_COUNT; i++)
        build_send_packet(COMMAND_LEN);
}

int read(int fd, void *buf, unsigned int count) {
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_READ, 4);
    command->value0 = htonl(fd);
    command->value1 = htonl((unsigned int)buf);
    command->value2 = htonl(count);
    build_send_packet(sizeof(net_command_3int_t));
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int write(int fd, const void *buf, unsigned int count) {
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    unsigned int inline_count = 0;

    if(DCTOOL_MAJOR < 2)
        memcpy(command->id, NET_SYSCALL_WRITE_OLD, 4);
    else
        memcpy(command->id, NET_SYSCALL_WRITE, 4);

    command->value0 = htonl(fd);
    command->value1 = htonl((unsigned int)buf);
    command->value2 = htonl(count);

    /* Inline small write buffers right after the command struct.  A
     * dedup-aware host (kosload >= 3.0.0) sees the extra bytes and uses
     * them directly, avoiding the SENDBINQ + DONEBIN bulk-read round-trip
     * back to the guest's buf address.  A legacy host ignores them and
     * does the original two-trip read — wire-safe in both directions.
     * Gated on host_supports_dedup() because pre-3.0 hosts didn't
     * special-case write inlining either way; the saving is only realized
     * with a new host. */
    if(host_supports_dedup() && count > 0 && count <= NET_WRITE_INLINE_MAX &&
       count <= NET_PAYLOAD_SIZE - sizeof(net_command_3int_t)) {
        inline_count = count;
        memcpy((unsigned char *)command + sizeof(net_command_3int_t), buf, inline_count);
    }

    build_send_packet(sizeof(net_command_3int_t) + inline_count);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int open(const char *pathname, int flags, int mode) {
    net_command_2int_string_t *command =
        (net_command_2int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(pathname);

    memcpy(command->id, NET_SYSCALL_OPEN, 4);
    command->value0 = htonl(flags);
    command->value1 = htonl(mode);
    memcpy(command->string, pathname, namelen + 1);

    build_send_packet(sizeof(net_command_2int_string_t) + namelen + 1);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int close(int fd) {
    net_command_int_t *command = (net_command_int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_CLOSE, 4);
    command->value0 = htonl(fd);

    build_send_packet(sizeof(net_command_int_t));
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int creat(const char *pathname, int mode) {
    net_command_int_string_t *command =
        (net_command_int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(pathname);

    memcpy(command->id, NET_SYSCALL_CREAT, 4);
    command->value0 = htonl(mode);
    memcpy(command->string, pathname, namelen + 1);

    build_send_packet(sizeof(net_command_int_string_t) + namelen + 1);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int link(const char *oldpath, const char *newpath) {
    net_command_string_t *command = (net_command_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen1 = my_strlen(oldpath);
    int namelen2 = my_strlen(newpath);

    memcpy(command->id, NET_SYSCALL_LINK, 4);
    memcpy(command->string, oldpath, namelen1 + 1);
    memcpy(command->string + namelen1 + 1, newpath, namelen2 + 1);

    build_send_packet(sizeof(net_command_string_t) + namelen1 + namelen2 + 2);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int unlink(const char *pathname) {
    net_command_string_t *command = (net_command_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(pathname);

    memcpy(command->id, NET_SYSCALL_UNLINK, 4);
    memcpy(command->string, pathname, namelen + 1);

    build_send_packet(sizeof(net_command_string_t) + namelen + 1);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int chdir(const char *path) {
    net_command_string_t *command = (net_command_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(path);

    memcpy(command->id, NET_SYSCALL_CHDIR, 4);
    memcpy(command->string, path, namelen + 1);

    build_send_packet(sizeof(net_command_string_t) + namelen + 1);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int chmod(const char *path, int mode) {
    net_command_int_string_t *command =
        (net_command_int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(path);

    memcpy(command->id, NET_SYSCALL_CHMOD, 4);
    command->value0 = htonl(mode);
    memcpy(command->string, path, namelen + 1);

    build_send_packet(sizeof(net_command_int_string_t) + namelen + 1);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int mkdir(const char *path, int mode) {
    net_command_int_string_t *command =
        (net_command_int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(path);

    memcpy(command->id, NET_SYSCALL_MKDIR, 4);
    command->value0 = htonl(mode);
    memcpy(command->string, path, namelen + 1);

    build_send_packet(sizeof(net_command_int_string_t) + namelen + 1);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int lseek(int fd, int offset, int whence) {
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_LSEEK, 4);
    command->value0 = htonl(fd);
    command->value1 = htonl(offset);
    command->value2 = htonl(whence);

    build_send_packet(sizeof(net_command_3int_t));
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int fstat(int fd, void *buf) {
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int command_len = sizeof(net_command_3int_t);

    memcpy(command->id, NET_SYSCALL_FSTAT, 4);
    command->value0 = htonl(fd);
    command->value1 = htonl((unsigned int)buf);
    command->value2 = htonl(sizeof(kosload_stat_t));

    /* Append KIR0 magic: tells a dedup-aware host to inline the out-buffer
     * into the RETVAL response instead of doing a separate SENDBINQ +
     * DONEBIN round-trip to the guest's buf address.  Legacy hosts ignore
     * the magic and still do the 2-trip path — wire-safe.  Halves the
     * packet count for fstat, which dominates KOS's file-open path. */
    if(host_supports_dedup()) {
        append_inline_ret_magic((unsigned char *)command + command_len);
        command_len += (int)NET_INLINE_RET_MAGIC_LEN;
    }
    build_send_packet(command_len);
    kos_syscall_wait_for_retval();

    /* If the host honored the KIR0 hint, the stat result is sitting in
     * syscall_data already; pull it out before returning to the guest. */
    if((int)syscall_retval >= 0 && host_supports_dedup())
        copy_inline_ret(buf, sizeof(kosload_stat_t));
    return syscall_retval;
}

int time(unsigned int *t) {
    net_command_int_t *command = (net_command_int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_TIME, 4);
    command->value0 = htonl((unsigned int)t);

    build_send_packet(sizeof(net_command_int_t));
    kos_syscall_wait_for_retval();

    if(t != 0)
        *t = syscall_retval;

    return syscall_retval;
}

int stat(const char *pathname, void *buf) {
    net_command_2int_string_t *command =
        (net_command_2int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(pathname);
    int command_len = sizeof(net_command_2int_string_t) + namelen + 1;

    memcpy(command->id, NET_SYSCALL_STAT, 4);
    memcpy(command->string, pathname, namelen + 1);
    command->value0 = htonl((unsigned int)buf);
    command->value1 = htonl(sizeof(kosload_stat_t));

    /* See fstat() for the KIR0 inline-return rationale. */
    if(host_supports_dedup()) {
        append_inline_ret_magic((unsigned char *)command + command_len);
        command_len += (int)NET_INLINE_RET_MAGIC_LEN;
    }
    build_send_packet(command_len);
    kos_syscall_wait_for_retval();

    if((int)syscall_retval >= 0 && host_supports_dedup())
        copy_inline_ret(buf, sizeof(kosload_stat_t));
    return syscall_retval;
}

int utime(const char *filename, void *buf) {
    net_command_3int_string_t *command =
        (net_command_3int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(filename);
    unsigned int *times = (unsigned int *)buf;

    memcpy(command->id, NET_SYSCALL_UTIME, 4);
    memcpy(command->string, filename, namelen + 1);
    command->value0 = htonl((unsigned int)buf);

    if(buf) {
        command->value1 = htonl(times[0]); /* actime */
        command->value2 = htonl(times[1]); /* modtime */
    }

    build_send_packet(sizeof(net_command_3int_string_t) + namelen + 1);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

unsigned int opendir(const char *name) {
    net_command_string_t *command = (net_command_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int namelen = my_strlen(name);

    memcpy(command->id, NET_SYSCALL_OPENDIR, 4);
    memcpy(command->string, name, namelen + 1);

    build_send_packet(sizeof(net_command_string_t) + namelen + 1);
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int closedir(unsigned int dir) {
    net_command_int_t *command = (net_command_int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_CLOSEDIR, 4);
    command->value0 = htonl(dir);

    build_send_packet(sizeof(net_command_int_t));
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

void *readdir(unsigned int dir) {
    net_command_3int_t *command = (net_command_3int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);
    int command_len = sizeof(net_command_3int_t);

    memcpy(command->id, NET_SYSCALL_READDIR, 4);
    command->value0 = htonl(dir);
    command->value1 = htonl((unsigned int)&our_dir);
    command->value2 = htonl(sizeof(dc_dirent_t));

    /* See fstat() for the KIR0 inline-return rationale. */
    if(host_supports_dedup()) {
        append_inline_ret_magic((unsigned char *)command + command_len);
        command_len += (int)NET_INLINE_RET_MAGIC_LEN;
    }
    build_send_packet(command_len);
    kos_syscall_wait_for_retval();

    if(syscall_retval) {
        if(host_supports_dedup())
            copy_inline_ret(&our_dir, sizeof(our_dir));
        return &our_dir;
    }
    return 0;
}

unsigned int gdbpacket(const char *in_buf, unsigned int size_pack, char *out_buf) {
    unsigned int in_size = size_pack >> 16;
    unsigned int out_size = size_pack & 0xffff;
    net_command_2int_string_t *command = (net_command_2int_string_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_GDBPACKET, 4);
    command->value0 = htonl(in_size);
    command->value1 = htonl(out_size);
    memcpy(command->string, in_buf, in_size);
    build_send_packet(sizeof(net_command_2int_string_t) + in_size);
    kos_syscall_wait_for_retval();

    if(syscall_retval <= out_size)
        memcpy(out_buf, syscall_data, syscall_retval);

    return syscall_retval;
}

int rewinddir(unsigned int dir) {
    net_command_int_t *command = (net_command_int_t *)(pkt_buf + ETHER_H_LEN + IP_H_LEN + UDP_H_LEN);

    memcpy(command->id, NET_SYSCALL_REWINDDIR, 4);
    command->value0 = htonl(dir);

    build_send_packet(sizeof(net_command_int_t));
    kos_syscall_wait_for_retval();

    return syscall_retval;
}

int gethostinfo(unsigned int *ip, unsigned int *port) {
    *ip = tool_ip;
    *port = tool_port;
    return our_ip;
}
