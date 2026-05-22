/* host/src/dedup.c
 *
 * Single-slot duplicate-request cache.  See dedup.h for the rationale
 * and wire format. */

#include <kostool/dedup.h>
#include <kosload/protocol.h>

#include <string.h>

typedef struct {
    uint8_t data[DEDUP_MAX_PKT_BYTES];
    size_t  len;
} dedup_packet_t;

static struct {
    uint32_t        seq;
    uint64_t        captured_at_us;
    dedup_packet_t  packets[DEDUP_MAX_PACKETS];
    size_t          packet_count;
    int             valid;        /* 1 once at least one capture has been committed */
    int             capturing;    /* 1 between begin and end */
    uint32_t        capture_seq;  /* seq id being captured right now */
    size_t          capture_count;
} state;

int dedup_extract_seq(const uint8_t *buf, size_t len, uint32_t *out_seq)
{
    if (!buf || len < NET_SEQ_TRAILER_LEN)
        return 0;
    const uint8_t *trailer = buf + len - NET_SEQ_TRAILER_LEN;
    if (memcmp(trailer, NET_SEQ_MAGIC, NET_SEQ_MAGIC_LEN) != 0)
        return 0;
    if (out_seq) {
        *out_seq = ((uint32_t)trailer[NET_SEQ_MAGIC_LEN + 0] << 24) |
                   ((uint32_t)trailer[NET_SEQ_MAGIC_LEN + 1] << 16) |
                   ((uint32_t)trailer[NET_SEQ_MAGIC_LEN + 2] << 8)  |
                   ((uint32_t)trailer[NET_SEQ_MAGIC_LEN + 3]);
    }
    return 1;
}

int dedup_try_replay(uint32_t seq, uint64_t now_us,
                     const platform_socket_ops_t *socket_ops, int sock_fd)
{
    if (!state.valid || seq != state.seq || !socket_ops)
        return 0;
    if (now_us - state.captured_at_us > DEDUP_WINDOW_USEC)
        return 0;

    /* Replay every captured packet in original order.  Each one already
     * carries its own outbound trailer (we captured the wire bytes), so
     * the client sees byte-identical responses to the byte-identical
     * request — same as if we'd re-run the syscall, only without doing
     * the work or mutating any host-side state. */
    for (size_t i = 0; i < state.packet_count; i++)
        socket_ops->send(sock_fd, state.packets[i].data, state.packets[i].len);
    return 1;
}

int dedup_is_stale(uint32_t seq)
{
    if (!state.valid)
        return 0;
    /* Signed delta handles the (in-practice unreachable) 32-bit wrap too:
     * seq "at or before" the last committed seq is a superseded duplicate. */
    return (int32_t)(seq - state.seq) <= 0;
}

void dedup_begin_capture(uint32_t seq, uint64_t now_us)
{
    state.capturing = 1;
    state.capture_seq = seq;
    state.capture_count = 0;
    state.captured_at_us = now_us;
    /* Don't touch state.seq / state.valid yet — only commit on
     * dedup_end_capture, so an aborted dispatch (e.g. socket error
     * mid-syscall) leaves the previous cache intact. */
}

void dedup_capture_pkt(const uint8_t *buf, size_t len)
{
    if (!state.capturing || !buf || len == 0)
        return;
    if (state.capture_count >= DEDUP_MAX_PACKETS)
        return;   /* drop overflow silently; replay will be incomplete,
                     which is no worse than the no-dedup baseline. */
    if (len > DEDUP_MAX_PKT_BYTES)
        return;

    dedup_packet_t *slot = &state.packets[state.capture_count++];
    memcpy(slot->data, buf, len);
    slot->len = len;
}

void dedup_end_capture(void)
{
    if (!state.capturing)
        return;

    state.seq = state.capture_seq;
    state.packet_count = state.capture_count;
    state.valid = 1;
    state.capturing = 0;
}

uint32_t dedup_current_seq(void)
{
    return state.capture_seq;
}

int dedup_is_capturing(void)
{
    return state.capturing;
}
