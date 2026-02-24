/* client/network/entry.c */
/*
 * Network client entry point for kosload.
 * Defines global state and display helpers used by the network stack.
 *
 * Platform-independent: uses target_get_ops() instead of direct
 * target struct references.
 *
 * DHCP support ported from dcload-ip: dcload-ip/target-src/dcload/dcload.c
 */

#include <kosload/target.h>
#include <kosload/transport.h>
#include <kosload/video.h>
#include <kosload/info.h>

#include "dhcp.h"
#include "perfctr.h"
#include "dcload.h"

#include <kosload/screensaver.h>

#ifndef KOSLOAD_VERSION_STRING
#define KOSLOAD_VERSION_STRING "0.1.0"
#endif

/* Scale up the onscreen refresh interval */
#define ONSCREEN_REFRESH_SCALED ((unsigned long long int)ONSCREEN_DHCP_LEASE_TIME_REFRESH_INTERVAL * (unsigned long long int)PERFCOUNTER_SCALE)

extern void common_main(const target_ops_t *tgt, const client_transport_ops_t *xport);
extern const target_ops_t *common_get_target(void);

/* From video.c / video.h (platform-specific) */
extern void uint_to_string(unsigned int foo, unsigned char *bar);
extern void clear_lines(int y, int height, unsigned int color);

/* From commands.c */
extern volatile unsigned int our_ip;
extern unsigned int tool_ip;
extern unsigned short tool_port;

extern kosload_info_t kosload_info;

/* From adapter.c */
typedef struct {
    const char *name;
    unsigned char mac[6];
    unsigned char pad[2];
    int (*detect)(void);
    int (*init)(void);
    void (*start)(void);
    void (*stop)(void);
    void (*loop)(int is_main_loop);
    int (*tx)(unsigned char *pkt, int len);
} adapter_t;
extern adapter_t *bb;

/* ===== Global state ===== */

volatile unsigned char booted;
volatile unsigned char running;
volatile unsigned char receiving;
unsigned int global_bg_color;
volatile unsigned int installed_adapter;

/* ===== DHCP state ===== */

static volatile unsigned int current_counter_array[2] = {0};
static volatile unsigned int old_dhcp_lease_updater_array[2] = {0};
static volatile unsigned char dont_renew = 0;

/* Accumulated PMCR ticks from before program executions.
 * Persists across program return since .data isn't reinitialized. */
static volatile unsigned int pmcr_saved_array[2] = {0};

static char ip_disp_string[16] = "000.000.000.000";
static const char *waiting_string = "Waiting For IP...";
static const char *dhcp_mode_string = " (DHCP Mode)";
static const char *dhcp_timeout_string = " (DHCP Timed Out!)";
static const char *dhcp_lease_string = "DHCP Lease Time (sec): ";
static const char *dhcp_attempts_string = "DHCP Attempts: ";
static const char *dhcp_next_string = "Next Attempt In... ";
static char dhcp_lease_time_string[11] = {0};
static char dhcp_attempts_num[9] = {0};
static char dhcp_next_counter[9] = {0};

/* ===== Display helpers ===== */

static void ip_to_string(unsigned int ip, char *buf)
{
    /* ip is in host byte order; extract octets via shifts (endian-safe) */
    int pos = 0;
    int i;

    for (i = 3; i >= 0; i--) {
        unsigned char val = (ip >> (i * 8)) & 0xFF;
        if (val >= 100) {
            buf[pos++] = '0' + val / 100;
            val %= 100;
            buf[pos++] = '0' + val / 10;
            buf[pos++] = '0' + val % 10;
        } else if (val >= 10) {
            buf[pos++] = '0' + val / 10;
            buf[pos++] = '0' + val % 10;
        } else {
            buf[pos++] = '0' + val;
        }
        if (i > 0)
            buf[pos++] = '.';
    }
    buf[pos] = '\0';
}

static void mac_to_string(unsigned char *mac, char *buf)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 6; i++) {
        buf[i * 3] = hex[mac[i] >> 4];
        buf[i * 3 + 1] = hex[mac[i] & 0xf];
        buf[i * 3 + 2] = (i < 5) ? ':' : '\0';
    }
}

void uint_to_string_dec(unsigned int foo, char *bar)
{
    int i = 0;
    char tmp[12];

    if (foo == 0) {
        bar[0] = '0';
        bar[1] = '\0';
        return;
    }

    while (foo > 0) {
        tmp[i++] = '0' + (foo % 10);
        foo /= 10;
    }

    int j;
    for (j = 0; j < i; j++)
        bar[j] = tmp[i - 1 - j];
    bar[i] = '\0';
}

static void update_ip_display(unsigned int new_ip, const char *mode_string)
{
    const target_ops_t *t = common_get_target();
    clear_lines(126, 24, global_bg_color);
    ip_to_string(new_ip, ip_disp_string);
    t->draw_string(30, 126, ip_disp_string, STR_COLOR);
    t->draw_string(210, 126, mode_string, STR_COLOR);
}

static void dhcp_waiting_mode_display(void)
{
    const target_ops_t *t = common_get_target();
    clear_lines(126, 24, global_bg_color);
    t->draw_string(30, 126, waiting_string, STR_COLOR);
    t->draw_string(234, 126, dhcp_mode_string, STR_COLOR);
}

static void update_lease_time_display(unsigned int new_time)
{
    const target_ops_t *t = common_get_target();
    uint_to_string_dec(new_time, dhcp_lease_time_string);
    clear_lines(402, 48, global_bg_color); /* Clear 48 lines to also clear attempts/retry text */
    t->draw_string(30, 426, dhcp_lease_string, STR_COLOR);
    t->draw_string(306, 426, dhcp_lease_time_string, STR_COLOR);
}

/* ===== Public display functions ===== */

void disp_info(void)
{
    const target_ops_t *t = common_get_target();
    char ip_str[16];
    char mac_str[18];

    t->setup_video(0, 0);
    t->clear_screen(global_bg_color);
    t->draw_string(30, 54, LOADER_NAME " " KOSLOAD_VERSION_STRING, 0xffff);

    if (bb) {
        t->draw_string(30, 78, bb->name, 0xffff);
        mac_to_string(bb->mac, mac_str);
        t->draw_string(30, 102, mac_str, 0xffff);
    }

    if (our_ip) {
        ip_to_string(our_ip, ip_str);
        t->draw_string(30, 126, ip_str, 0xffff);
    }

    /* Update info block with current network state */
    kosload_info.dc_ip = our_ip;
    kosload_info.host_ip = tool_ip;
    kosload_info.host_port = tool_port;

    /* Reset lease display timer so it updates promptly after screen redraw */
    old_dhcp_lease_updater_array[0] = 0;
    old_dhcp_lease_updater_array[1] = 0;

    booted = 1;
}

void disp_status(const char *status)
{
    const target_ops_t *t = common_get_target();
    clear_lines(150, 24, global_bg_color);
    t->draw_string(30, 150, status, 0xffff);
}

void disp_dhcp_attempts_count(void)
{
    const target_ops_t *t = common_get_target();
    clear_lines(402, 24, global_bg_color);
    t->draw_string(30, 402, dhcp_attempts_string, STR_COLOR);
    uint_to_string_dec(dhcp_attempts, dhcp_attempts_num);
    t->draw_string(210, 402, dhcp_attempts_num, STR_COLOR);
}

void disp_dhcp_next_attempt(unsigned int time_left)
{
    const target_ops_t *t = common_get_target();
    clear_lines(426, 24, global_bg_color);
    t->draw_string(30, 426, dhcp_next_string, STR_COLOR);
    uint_to_string_dec(time_left, dhcp_next_counter);
    t->draw_string(258, 426, dhcp_next_counter, STR_COLOR);
}

/* ===== PMCR elapsed time save/restore ===== */

void save_pmcr_elapsed(void)
{
    volatile unsigned int tmp[2];
    PMCR_Read(DCLOAD_PMCR, tmp);
    *(unsigned long long int *)pmcr_saved_array += *(unsigned long long int *)tmp;
}

/* ===== DHCP management ===== */

void set_ip_dhcp(void)
{
    if (__builtin_expect(!booted, 0)) {
        disp_info();
        disp_status("idle...");
    }

    /* Check renewal condition. Only matters if dhcp_lease_time has been set. */
    unsigned long long int long_dhcp_lease_time = (unsigned long long int)dhcp_lease_time * (unsigned long long int)(PERFCOUNTER_SCALE);
    PMCR_Read(DCLOAD_PMCR, current_counter_array);
    /* Add accumulated time from before program executions */
    *(unsigned long long int *)current_counter_array += *(unsigned long long int *)pmcr_saved_array;

    unsigned long long int *current_counter = (unsigned long long int *)current_counter_array;
    unsigned long long int *old_dhcp_lease_updater = (unsigned long long int *)old_dhcp_lease_updater_array;

    /* Check if lease is still active, renewal threshold is at 50% lease time */
    if (__builtin_expect(dhcp_lease_time && (!dont_renew) && ((long_dhcp_lease_time >> 1) < (*current_counter)), 0))
    {
        dhcp_lease_time = 0;
        old_dhcp_lease_updater_array[0] = 0;
        old_dhcp_lease_updater_array[1] = 0;

        dhcp_waiting_mode_display();
        disp_status("DHCP renewing...");
        int renew_result = dhcp_renew((unsigned int *)&our_ip);

        if (renew_result == -2)
        {
            /* NAK: IP no longer valid, wait until 87.5% to do new discover */
            dont_renew = 1;
        }
        else if (renew_result == -1)
        {
            /* Error: ACK was invalid. Disable DHCP entirely. */
            our_ip = 0xffffffff;
            update_ip_display(our_ip, dhcp_timeout_string);
            update_lease_time_display(dhcp_lease_time);
            PMCR_Disable(DCLOAD_PMCR);
            return;
        }
        else
        {
            /* PMCR was restarted in handle_dhcp_reply; clear saved
             * accumulator so it doesn't double-count after program return */
            pmcr_saved_array[0] = 0;
            pmcr_saved_array[1] = 0;
            update_ip_display(our_ip, dhcp_mode_string);
            update_lease_time_display(dhcp_lease_time);
        }

        disp_status("idle...");
    }
    /* Update lease time display at ~1Hz (skip during screensaver) */
    else if (
        !screensaver_is_active() &&
        (long_dhcp_lease_time >= (*current_counter)) && ((*current_counter) > ((*old_dhcp_lease_updater) + ONSCREEN_REFRESH_SCALED - 1ULL)))
    {
        old_dhcp_lease_updater_array[0] = current_counter_array[0];
        old_dhcp_lease_updater_array[1] = current_counter_array[1];

        unsigned long long int difference = long_dhcp_lease_time - (*current_counter);
        unsigned int remaining_lease = PMCR_TicksToSeconds(difference);
        update_lease_time_display(remaining_lease);
    }

    /* 87.5% threshold for re-discovery after NAK */
    unsigned long long int eighty_seven_point_five = (long_dhcp_lease_time >> 1) + (long_dhcp_lease_time >> 2) + (long_dhcp_lease_time >> 3);

    /* Check if we need DHCP discovery: IP in 0.0.0.0/8 range, or renewal NAK'd past 87.5% */
    if (__builtin_expect(((our_ip & 0xff000000) == 0) || (dont_renew && (eighty_seven_point_five < (*current_counter))), 0))
    {
        dont_renew = 0;
        dhcp_waiting_mode_display();

        disp_status("Acquiring new IP address via DHCP...");
        int dhcp_result = dhcp_go((unsigned int *)&our_ip);
        if ((dhcp_result == -1) || dhcp_nest_counter_maxed)
        {
            /* Failed: set IP to 255.255.255.255 to disable DHCP */
            our_ip = 0xffffffff;
            update_ip_display(our_ip, dhcp_timeout_string);
            PMCR_Disable(DCLOAD_PMCR);
        }
        else
        {
            /* Got an address from DHCP — clear saved accumulator */
            pmcr_saved_array[0] = 0;
            pmcr_saved_array[1] = 0;
            update_ip_display(our_ip, dhcp_mode_string);
            update_lease_time_display(dhcp_lease_time);
        }

        disp_status("idle...");
    }
}

/* ===== Entry point ===== */

int main(void)
{
    const target_ops_t *t = target_get_ops();
    common_main(t, &client_network_transport_ops);
    return 0;
}
