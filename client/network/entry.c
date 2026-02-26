/* client/network/entry.c */
/*
 * Network client entry point for kosload.
 * Defines global state and display helpers used by the network stack.
 *
 * Platform-independent: uses target_get_ops() instead of direct
 * target struct references.
 *
 * DHCP support ported from dcload-ip: dcload-ip/target-src/dcload/dcload.c
 *
 * Lease tracking uses two mechanisms:
 *   1. Absolute RTC expiry (set at DHCP ACK, persists in .data across
 *      warm reboots) — used only at key moments: DHCP ACK, renewal,
 *      NAK, and post-reboot resync.
 *   2. Tick-based ~1Hz countdown for display updates — decrements a
 *      seconds counter via get_ticks() delta, no RTC reads needed.
 * After warm reboot the tick counter resyncs from the RTC expiry once.
 */

#include <kosload/target.h>
#include <kosload/transport.h>
#include <kosload/video.h>
#include <kosload/info.h>

#include "dhcp.h"
#include "dcload.h"

#include <kosload/screensaver.h>

#ifndef KOSLOAD_VERSION_STRING
#define KOSLOAD_VERSION_STRING "0.1.0"
#endif

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
    void (*loop)(bool is_main_loop);
    int (*tx)(unsigned char *pkt, int len);
} adapter_t;
extern adapter_t *bb;

/* ===== Global state ===== */

volatile bool booted;
volatile bool running;
volatile bool receiving;
unsigned int global_bg_color;
volatile unsigned int installed_adapter;

/* ===== DHCP lease tracking state ===== */

/* Absolute RTC time (Unix seconds) when the current lease expires.
 * Set at DHCP ACK; persists across warm reboots (.data, not .bss).
 * Only read at key moments — not in the ~1Hz display loop. */
static unsigned int lease_expiry_rtc = 0;

/* Tick-decremented countdown (seconds) for display and renewal checks.
 * Resynced from lease_expiry_rtc via RTC on warm reboot; decremented
 * via tick deltas otherwise — no RTC reads in the hot path. */
static unsigned int lease_display_secs = 0;

/* Tick counter snapshot for ~1Hz display throttle */
static uint64_t last_display_tick = 0;

static void lease_resync_from_rtc(void);

static volatile bool dont_renew = false;

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

    /* Reset lease display timer so the next ~1Hz update starts from "now"
     * rather than accumulating a bogus delta from TBR epoch (which could
     * over-subtract from the lease countdown). */
    last_display_tick = t->get_ticks();

    /* Resync lease countdown from RTC — accounts for time spent in
     * loaded programs or between warm reboots. */
    if (lease_expiry_rtc)
        lease_resync_from_rtc();

    booted = true;
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

/* Resync the tick-based countdown from the absolute RTC expiry.
 * Called once after warm reboot and after DHCP events — NOT in the hot path. */
static void lease_resync_from_rtc(void)
{
    if (!lease_expiry_rtc) {
        lease_display_secs = 0;
        return;
    }
    const target_ops_t *t = common_get_target();
    unsigned int now = t->get_rtc();
    lease_display_secs = (now < lease_expiry_rtc) ? (lease_expiry_rtc - now) : 0;
    last_display_tick = t->get_ticks();
}

/* ===== DHCP management ===== */
void dhcp_poll(void)
{
    const target_ops_t *t = common_get_target();

    if (__builtin_expect(!booted, 0)) {
        disp_info();
        disp_status("idle...");
    }

    /* --- Renewal check (50% of lease elapsed) --- */
    if (__builtin_expect(dhcp_lease_time && (!dont_renew) &&
                         (lease_display_secs <= dhcp_lease_time / 2), 0))
    {
        unsigned int saved_lease_time = dhcp_lease_time;
        dhcp_lease_time = 0;

        dhcp_waiting_mode_display();
        disp_status("DHCP renewing...");
        int renew_result = dhcp_renew((unsigned int *)&our_ip);

        if (renew_result == -2)
        {
            /* NAK: IP no longer valid, wait until 87.5% to do new discover.
             * Set expiry so the lease runs out at the 87.5% mark. */
            dont_renew = true;
            unsigned int nak_remaining = (saved_lease_time * 3 + 4) / 8;
            lease_expiry_rtc = t->get_rtc() + nak_remaining;
            lease_display_secs = nak_remaining;
            last_display_tick = t->get_ticks();
        }
        else if (renew_result == -1)
        {
            /* Error: ACK was invalid. Disable DHCP entirely. */
            our_ip = 0xffffffff;
            lease_expiry_rtc = 0;
            lease_display_secs = 0;
            update_ip_display(our_ip, dhcp_timeout_string);
            update_lease_time_display(0);
            return;
        }
        else
        {
            /* Success: set new expiry from fresh lease time */
            lease_expiry_rtc = t->get_rtc() + dhcp_lease_time;
            lease_display_secs = dhcp_lease_time;
            last_display_tick = t->get_ticks();
            update_ip_display(our_ip, dhcp_mode_string);
            update_lease_time_display(lease_display_secs);
        }

        disp_status("idle...");
    }
    /* --- ~1Hz lease countdown --- */
    else if (lease_display_secs > 0)
    {
        uint64_t now = t->get_ticks();
        uint64_t delta = now - last_display_tick;

        if (delta >= t->ticks_per_second) {
            last_display_tick = now;
            lease_display_secs--;
            if (!screensaver_is_active())
                update_lease_time_display(lease_display_secs);
        }
    }

    /* --- Discovery check (no IP, or NAK'd and lease fully expired) --- */
    if (__builtin_expect(((our_ip & 0xff000000) == 0) ||
                         (dont_renew && lease_display_secs == 0), 0))
    {
        dont_renew = false;
        dhcp_waiting_mode_display();

        disp_status("Acquiring new IP address via DHCP...");
        int dhcp_result = dhcp_go((unsigned int *)&our_ip);
        if (dhcp_result == -1 || dhcp_nest_counter_maxed)
        {
            /* Failed: set IP to 255.255.255.255 to disable DHCP */
            our_ip = 0xffffffff;
            lease_expiry_rtc = 0;
            lease_display_secs = 0;
            update_ip_display(our_ip, dhcp_timeout_string);
        }
        else
        {
            /* Got an address from DHCP */
            lease_expiry_rtc = t->get_rtc() + dhcp_lease_time;
            lease_display_secs = dhcp_lease_time;
            last_display_tick = t->get_ticks();
            update_ip_display(our_ip, dhcp_mode_string);
            update_lease_time_display(lease_display_secs);
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
