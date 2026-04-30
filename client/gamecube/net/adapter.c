/* client/gamecube/net/adapter.c */
/*
 * Network adapter detection for GameCube.
 * Supports BBA (Broadband Adapter), ENC28J60 (GCNet/ETH2GC), and
 * W5500 (SPI-over-EXI).
 *
 * Detection priority: ENC28J60 > W5500 > BBA
 * (SPI adapters are tried first since they can coexist in different slots)
 */

#include "adapter.h"
#include "bba.h"
#include "enc28j60.h"
#include "w5500_spi_gc.h"

/* Loop escape flag, used by all drivers */
volatile unsigned char escape_loop = 0;
int timeout_loop = 0;
int loop_secs_elapsed = 0;

/* The currently configured driver */
adapter_t *bb;

/* Packet receive buffer */
__attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE];
/* Offset by 2 for command->data alignment (same trick as DC) */
__attribute__((aligned(2))) unsigned char *current_pkt = &(raw_current_pkt[2]);

/*
 * Generate a MAC address for W5500 on GameCube.
 * Uses the ECID hardware registers (same approach as ENC28J60,
 * but with a different salt for uniqueness).
 */
static inline unsigned int mfspr_ecid0(void)
{
    unsigned int val;
    __asm__ volatile("mfspr %0, 0x39C" : "=r"(val));
    return val;
}

static inline unsigned int mfspr_ecid1(void)
{
    unsigned int val;
    __asm__ volatile("mfspr %0, 0x39D" : "=r"(val));
    return val;
}

static inline unsigned int mfspr_ecid2(void)
{
    unsigned int val;
    __asm__ volatile("mfspr %0, 0x39E" : "=r"(val));
    return val;
}

static inline unsigned int mfspr_ecid3(void)
{
    unsigned int val;
    __asm__ volatile("mfspr %0, 0x39F" : "=r"(val));
    return val;
}

static void w5500_gc_generate_mac(unsigned char *mac)
{
    union {
        unsigned int cid[4];
        unsigned char data[19];
    } ecid = {0};
    unsigned int sum;
    int i;

    ecid.cid[0] = mfspr_ecid0();
    ecid.cid[1] = mfspr_ecid1();
    ecid.cid[2] = mfspr_ecid2();
    ecid.cid[3] = mfspr_ecid3();

    /* Mix in a salt to differentiate from ENC28J60 MAC */
    ecid.data[15] ^= 0x55;
    ecid.data[16] = 0x00;
    ecid.data[17] = 0xA5;

    /* Hash ECID into 3 bytes */
    sum = 0x57;  /* Seed: 'W' for W5500 */
    for (i = 0; i < 18; i += 3) {
        unsigned int word;
        word  = (unsigned int)ecid.data[i]     << 16;
        word |= (unsigned int)ecid.data[i + 1] << 8;
        word |= (unsigned int)ecid.data[i + 2];
        sum += word;
        sum = (sum & 0x00FFFFFF) + (sum >> 24);
    }

    /* Locally administered, unicast */
    mac[0] = 0x02;
    mac[1] = 0x57;  /* 'W' for W5500 */
    mac[2] = 0x55;  /* 'U' */
    mac[3] = (sum >> 16) & 0xFF;
    mac[4] = (sum >> 8) & 0xFF;
    mac[5] = sum & 0xFF;
}

int adapter_detect(void)
{
    /* Try ENC28J60 first (most common SPI adapter) */
    if (adapter_enc28j60.detect() >= 0) {
        bb = &adapter_enc28j60;
    } else {
        /* Try W5500 on all EXI locations */
        if (w5500_probe_exi_all()) {
            w5500_set_spi_ops(&gc_w5500_spi_ops);
            w5500_gc_generate_mac(adapter_w5500.mac);

            if (adapter_w5500.detect() >= 0) {
                bb = &adapter_w5500;
                goto detected;
            }
        }

        /* Try BBA last */
        if (adapter_bba.detect() >= 0) {
            bb = &adapter_bba;
        } else {
            return -1;
        }
    }

detected:
    if (bb->init() < 0)
        return -1;

    escape_loop = 0;

    return 0;
}

void adapter_start_static_ip(void)
{
    if (bb == &adapter_enc28j60 || bb == &adapter_bba)
        bb->start();
}
