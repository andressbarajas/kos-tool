/* client/gamecube/net/adapter.h */
/*
 * Network adapter interface for GameCube.
 * Same adapter_t struct as DC for shared code compatibility.
 * GC only has the Broadband Adapter (no LAN Adapter equivalent).
 */

#ifndef __ADAPTER_H__
#define __ADAPTER_H__

/* Raw receive buffer size: 1536 = nearest multiple of 32 >= 1514 */
#define RAW_RX_PKT_BUF_SIZE 1536

/* Receive buffer size (max Ethernet frame without CRC) */
#define RX_PKT_BUF_SIZE 1514

/* Network adapter driver interface */
typedef struct {
	const char *name;
	unsigned char mac[6];
	unsigned char pad[2];
	int  (*detect)(void);
	int  (*init)(void);
	void (*start)(void);
	void (*stop)(void);
	void (*loop)(int is_main_loop);
	int  (*tx)(unsigned char *pkt, int len);
} adapter_t;

/* Detect which adapter we are using and init our structs */
int adapter_detect(void);

/* The configured adapter */
extern adapter_t *bb;
extern adapter_t adapter_bba;
extern adapter_t adapter_enc28j60;

/* Loop control variables */
extern volatile unsigned char escape_loop;
extern int timeout_loop;
extern int loop_secs_elapsed;

/* Shared receive buffer */
extern __attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE];
extern __attribute__((aligned(2))) unsigned char *current_pkt;

#endif /* __ADAPTER_H__ */
