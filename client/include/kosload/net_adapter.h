/* client/include/kosload/net_adapter.h */
#ifndef KOSLOAD_NET_ADAPTER_H
#define KOSLOAD_NET_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Client network adapter interface.
 *
 * The network transport owns the host protocol. Console-specific adapter code
 * owns hardware detection, MAC setup, RX/TX, and polling for the selected
 * Ethernet device.
 */

/*
 * Raw receive buffer array size.
 *
 * 1514 bytes is not a multiple of 8. Ethernet header (14) + IP header (20) +
 * UDP header (8) + command struct (12) = 54 bytes before command->data. To
 * align command->data to 8 bytes, align the array to 8 bytes and offset the
 * start by 2 bytes. 1516 - 2 = 1514, and 54 bytes later (56 bytes from the
 * start of the array) is aligned to 8 bytes.
 *
 * The buffer is rounded up to 1536 so 32-byte cache operations cannot spill
 * onto adjacent data.
 */
#define RAW_RX_PKT_BUF_SIZE 1536

/* Receive buffer size: max Ethernet frame without CRC. */
#define RX_PKT_BUF_SIZE 1514

/*
 * Maximum Ethernet frame size on the wire: header + payload + CRC.
 *
 * Used to cap FIFO drain loops when discarding error/oversized packets. The
 * hardware cannot hold more than one frame, so draining beyond this on a
 * corrupted length field would read garbage from an empty FIFO.
 */
#define MAX_ETHERNET_FRAME_SIZE 1518

typedef struct {
    const char *name;
    unsigned char mac[6];

    /* Keep function pointers aligned to 4 bytes. */
    unsigned char pad[2];

    int  (*detect)(void);
    int  (*init)(void);
    void (*start)(void);
    void (*stop)(void);
    void (*loop)(bool is_main_loop);
    int  (*tx)(unsigned char *pkt, int len);

    /* True if this transport can silently drop datagrams, so the syscall
     * layer must use the bounded-wait retransmit (see
     * kos_syscall_wait_for_retval) instead of waiting forever for the
     * RETVAL.  Defaults to false (raw-NIC drivers on DC/GC/PS2 don't lose
     * packets); the Wii adapter sets it true for the internal Wi-Fi.
     * CONTRACT: any adapter that sets this true MUST honor
     * loop_deadline_ticks in its loop() — break (with escape_loop clear)
     * once it passes — so the retransmit can fire. */
    bool lossy;
} adapter_t;

/* Console-specific adapter.c selects and initializes the active driver. */
int adapter_detect(void);
const char *adapter_get_last_error(void);
const char *adapter_get_phase_status(void);
void adapter_start_static_ip(void);

/* The configured adapter, used by shared network code after init succeeds. */
extern adapter_t *bb;

/* Loop control variables shared by adapter drivers and DHCP code. */
extern volatile unsigned char escape_loop;
extern int timeout_loop;
extern int loop_secs_elapsed;

/* Per-syscall bounded-wait deadline (absolute, in target ticks).  Armed
 * only for a lossy adapter (adapter_t.lossy): kos_syscall_wait_for_retval()
 * sets it before bb->loop(0) and that adapter's loop breaks (with
 * escape_loop still clear) when reached, so the syscall layer can
 * retransmit a lost request/reply.  Today only the Wii's internal Wi-Fi
 * sets lossy; reliable transports (DC/GC/PS2 raw NICs, the Wii LAN Adapter)
 * leave it 0 and wait forever for the RETVAL without consulting it. */
extern volatile uint64_t loop_deadline_ticks;

/* All adapter drivers receive into this shared packet buffer. */
extern __attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE];
extern __attribute__((aligned(2))) unsigned char *current_pkt;

#endif /* KOSLOAD_NET_ADAPTER_H */
