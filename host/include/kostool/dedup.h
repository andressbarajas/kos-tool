/* host/include/kostool/dedup.h
 *
 * Per-target duplicate-request cache for the network console syscall path.
 *
 * The wire format is the KSQ0 trailer defined in <kosload/protocol.h>:
 * every modern client (kosload >= 3.0.0) appends an 8-byte trailer
 * (4-byte magic + BE 32-bit seq id) at the end of each syscall request
 * UDP payload.  On RETVAL and any other outbound packet the host emits
 * during that dispatch, the host echoes the same trailer.
 *
 * Why we need it: the legacy syscall protocol has no retransmits, so a
 * lost packet (request or response) used to wedge both sides.  The Wii
 * client now retransmits its request after a bounded timeout
 * (kos_syscall_wait_for_retval in client/common/network/network_syscalls.c —
 * wired consoles still wait forever and never retransmit).  If the host has
 * already serviced the original request, re-running it would corrupt
 * non-idempotent syscalls (read, lseek, readdir).  This cache holds the
 * captured response packets for the most recent seq id within
 * DEDUP_WINDOW_USEC; a re-arrived request with a matching seq id replays the
 * captured bytes verbatim instead.  Same algorithm as NFS's Duplicate
 * Request Cache (RFC 1813 §4.5).
 *
 * Single-target scope: the host only talks to one console per
 * invocation, so a single cache slot is enough.  A re-arriving request
 * with a non-matching seq id (e.g. session moved to a different
 * console) naturally evicts the cache via dedup_begin_capture overwrite.
 *
 * Transparent to non-dedup clients: a request without the KSQ0 trailer
 * skips the whole cache (dedup_extract_seq returns 0), preserving
 * byte-for-byte the legacy dc-tool 2.0.1 behavior. */
#ifndef KOSTOOL_DEDUP_H
#define KOSTOOL_DEDUP_H

#include <stddef.h>
#include <stdint.h>

#include <kostool/platform.h>

/* DRC must outlast the client's full retry budget plus jitter, or the
 * last retry can race past the window and re-execute a stale request.
 * Client today: 250 ms * 10 retries = 2.5 s.  Set 4 s for headroom; the
 * cache is one slot, so this is essentially free RAM. */
#define DEDUP_WINDOW_USEC       3000000

/* Bounds for one captured dispatch.  A read() with a 1440-byte body
 * generates ~1 LOADBIN + ~30 PARTBINs + 1 DONEBIN + 1 RETVAL ≈ 35
 * packets, well under DEDUP_MAX_PACKETS.  Bytes-per-packet matches the
 * UDP wire-size cap in net_send_cmd / send_cmd. */
#define DEDUP_MAX_PACKETS       64
#define DEDUP_MAX_PKT_BYTES     2048

/* True if `buf` ends with the KSQ0 magic.  When true and out_seq is
 * non-NULL, writes the host-endian seq id; len must reflect just the
 * UDP payload (no Ethernet/IP/UDP headers). */
int dedup_extract_seq(const uint8_t *buf, size_t len, uint32_t *out_seq);

/* If the cache currently holds a complete capture for `seq` and we're
 * still within DEDUP_WINDOW_USEC, replay every captured outbound packet
 * verbatim on `sock_fd` via `socket_ops` and return 1.  Returns 0 (no
 * action taken) on any miss — non-matching seq, expired window, or
 * cache never primed. */
int dedup_try_replay(uint32_t seq, uint64_t now_us,
                     const platform_socket_ops_t *socket_ops, int sock_fd);

/* Is this an old request we already handled and should ignore?
 *
 * The client numbers its requests 1, 2, 3, ... and only moves to the next
 * number once it gets an answer.  So if a request shows up with a number we
 * already finished (<= the last one we did), it's just a late copy of an old
 * request the client has long since moved past — a leftover from the network
 * re-sending it.
 *
 * We must NOT run it again: re-running read/lseek/readdir would change state
 * on our side (e.g. advance the file position), corrupting the transfer the
 * client is actually doing now.  So return true here and the caller drops it.
 *
 * Call this only after dedup_try_replay() has already said "not a packet I
 * can resend."  Returns false until we've handled at least one request
 * (state.valid == 0), so the very first request always counts as new. */
int dedup_is_stale(uint32_t seq);

/* Open a capture for `seq`.  The cache remains armed (replay still
 * possible for the previous seq) until dedup_end_capture commits the
 * new capture.  Calls to dedup_capture_pkt between begin and end append
 * outbound packets to the in-flight capture. */
void dedup_begin_capture(uint32_t seq, uint64_t now_us);
void dedup_capture_pkt(const uint8_t *buf, size_t len);
void dedup_end_capture(void);

/* Helpers used by send paths that need to know whether a capture is
 * actively open (to decide whether to echo the trailer on the wire). */
uint32_t dedup_current_seq(void);
int dedup_is_capturing(void);

#endif /* KOSTOOL_DEDUP_H */
