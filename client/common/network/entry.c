/* client/common/network/entry.c */
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
#include <kosload/net_adapter.h>
#include <kosload/net_stack.h>

#include <kosload/dhcp.h>
#include "kosload.h"

#include <kosload/screensaver.h>
#include <kosload/divutil.h>

#ifndef KOSLOAD_VERSION_STRING
#define KOSLOAD_VERSION_STRING "0.1.0"
#endif

/* Git revision sub-version stamp — on-screen DISPLAY ONLY, not the VERS wire
 * string (see mk/version.mk). */
#ifndef KOSLOAD_GIT_REV
#define KOSLOAD_GIT_REV "unknown"
#endif

#ifndef NETWORK_DISPLAY_X
#define NETWORK_DISPLAY_X 30
#endif

#ifndef NETWORK_DHCP_ATTEMPTS_Y
#define NETWORK_DHCP_ATTEMPTS_Y 402
#endif

#ifndef NETWORK_DHCP_LEASE_Y
#define NETWORK_DHCP_LEASE_Y 426
#endif

extern void common_main(const target_ops_t *tgt, const client_transport_ops_t *xport);
extern const target_ops_t *common_get_target(void);

/* From video.c / video.h (platform-specific) */
extern void uint_to_string(unsigned int foo, unsigned char *bar);
extern void clear_lines(int y, int height, unsigned int color);

/* From commands.c */
extern unsigned int tool_ip;
extern unsigned short tool_port;

extern kosload_info_t kosload_info;

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

/* Renewal cadence.  dhcp_poll() attempts a RENEW once lease_display_secs
 * drops to/below this.  Set to T/2 on every fresh lease; on a lost/late
 * renew ACK it is backed off (~half the remaining lease, floored) so the
 * fast poll loop retries the renew without storming the wire — instead
 * of the old behaviour of disabling DHCP entirely on a single miss. */
static unsigned int renew_threshold = 0;
#define DHCP_RENEW_RETRY_FLOOR_SECS 4

static char ip_disp_string[16] = "000.000.000.000";
static const char *waiting_string = "Waiting For IP...";
static const char *dhcp_mode_string = " (DHCP Mode)";
static const char *dhcp_lease_string = "DHCP Lease Time (sec): ";
static const char *dhcp_attempts_string = "DHCP Attempts: ";
static const char *dhcp_next_string = "Next Attempt In... ";
static char dhcp_lease_time_string[11] = {0};
static char dhcp_attempts_num[9] = {0};
static char dhcp_next_counter[9] = {0};

static int runtime_status_y(void)
{
    const char *phase_status = adapter_get_phase_status();

    if (phase_status != 0 && phase_status[0] != '\0')
        return 174;

    return 150;
}

/* ===== Display helpers ===== */

static void ip_to_string(unsigned int ip, char *buf)
{
    /* ip is in host byte order; extract octets via shifts (endian-safe) */
    int pos = 0;
    int i;

    for (i = 3; i >= 0; i--) {
        unsigned char val = (ip >> (i * 8)) & 0xFF;
        if (val >= 100) {
            buf[pos++] = '0' + UDIV_CONST(val, 100);
            val = UMOD_CONST(val, 100);
            buf[pos++] = '0' + UDIV_CONST(val, 10);
            buf[pos++] = '0' + UMOD_CONST(val, 10);
        } else if (val >= 10) {
            buf[pos++] = '0' + UDIV_CONST(val, 10);
            buf[pos++] = '0' + UMOD_CONST(val, 10);
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
        unsigned int q = UDIV_CONST(foo, 10);
        tmp[i++] = '0' + (foo - q * 10);
        foo = q;
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
    t->draw_string(NETWORK_DISPLAY_X, 126, ip_disp_string, STR_COLOR);
    t->draw_string(NETWORK_DISPLAY_X + 180, 126, mode_string, STR_COLOR);
}

static void dhcp_waiting_mode_display(void)
{
    const target_ops_t *t = common_get_target();
    clear_lines(126, 24, global_bg_color);
    t->draw_string(NETWORK_DISPLAY_X, 126, waiting_string, STR_COLOR);
    t->draw_string(NETWORK_DISPLAY_X + 204, 126, dhcp_mode_string, STR_COLOR);
}

void update_lease_time_display(unsigned int new_time)
{
    const target_ops_t *t = common_get_target();
    uint_to_string_dec(new_time, dhcp_lease_time_string);
    clear_lines(NETWORK_DHCP_ATTEMPTS_Y, 48, global_bg_color); /* Clear 48 lines to also clear attempts/retry text */
    t->draw_string(NETWORK_DISPLAY_X, NETWORK_DHCP_LEASE_Y, dhcp_lease_string, STR_COLOR);
    t->draw_string(NETWORK_DISPLAY_X + 276, NETWORK_DHCP_LEASE_Y, dhcp_lease_time_string, STR_COLOR);
}

/* ===== Public display functions ===== */

void disp_info(void)
{
    const target_ops_t *t = common_get_target();
    char ip_str[16];
    char mac_str[18];
    const char *phase_status = adapter_get_phase_status();

    t->setup_video(0, 0);
    t->clear_screen(global_bg_color);
    t->draw_string(NETWORK_DISPLAY_X, 54, LOADER_NAME " " KOSLOAD_VERSION_STRING "  " KOSLOAD_GIT_REV, 0xffff);

    if (bb) {
        t->draw_string(NETWORK_DISPLAY_X, 78, bb->name, 0xffff);
        mac_to_string(bb->mac, mac_str);
        t->draw_string(NETWORK_DISPLAY_X, 102, mac_str, 0xffff);
    }

    /* our_ip == 0xffffffff is the "DHCP failed / disabled" sentinel;
     * don't draw "255.255.255.255" in that case — the row stays blank. */
    if (our_ip && our_ip != 0xffffffff) {
        ip_to_string(our_ip, ip_str);
        t->draw_string(NETWORK_DISPLAY_X, 126, ip_str, 0xffff);
        /* Re-show the "(DHCP Mode)" suffix on this redraw. */
        if (kosload_info.capabilities & KOSLOAD_CAP_DHCP)
            t->draw_string(NETWORK_DISPLAY_X + 180, 126, dhcp_mode_string, 0xffff);
    } else if (our_ip == 0) {
        /* DHCP mode, no lease yet — pre-show the "Waiting For IP..." line
         * so the IP row isn't blank during link/PHY/settle bring-up. */
        dhcp_waiting_mode_display();
    }

    if (phase_status != 0 && phase_status[0] != '\0')
        t->draw_string(NETWORK_DISPLAY_X, 150, phase_status, 0xffff);

    /* Update info block with current network state */
    kosload_info.console_ip = our_ip;
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
    int y = runtime_status_y();
    clear_lines(y, 24, global_bg_color);
    t->draw_string(NETWORK_DISPLAY_X, y, status, 0xffff);
}

void disp_dhcp_attempts_count(void)
{
    const target_ops_t *t = common_get_target();
    clear_lines(NETWORK_DHCP_ATTEMPTS_Y, 24, global_bg_color);
    t->draw_string(NETWORK_DISPLAY_X, NETWORK_DHCP_ATTEMPTS_Y, dhcp_attempts_string, STR_COLOR);
    uint_to_string_dec(dhcp_attempts, dhcp_attempts_num);
    t->draw_string(NETWORK_DISPLAY_X + 180, NETWORK_DHCP_ATTEMPTS_Y, dhcp_attempts_num, STR_COLOR);
}

void disp_dhcp_next_attempt(unsigned int time_left)
{
    const target_ops_t *t = common_get_target();
    clear_lines(NETWORK_DHCP_LEASE_Y, 24, global_bg_color);
    t->draw_string(NETWORK_DISPLAY_X, NETWORK_DHCP_LEASE_Y, dhcp_next_string, STR_COLOR);
    uint_to_string_dec(time_left, dhcp_next_counter);
    t->draw_string(NETWORK_DISPLAY_X + 228, NETWORK_DHCP_LEASE_Y, dhcp_next_counter, STR_COLOR);
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

/* Lease-countdown tick.  Pure timer work — never sends packets, never
 * touches DMA — so it's safe to call every loop iteration regardless of
 * link state.  Keeps the displayed "DHCP Lease Time" decrementing while
 * the cable is unplugged, which dhcp_poll() can't because the bb->loop
 * caller gates the full poll on link-up. */
void dhcp_tick(void)
{
    const target_ops_t *t = common_get_target();
    uint64_t now;
    uint64_t delta;

    if (lease_display_secs == 0)
        return;

    now = t->get_ticks();
    delta = now - last_display_tick;
    if (delta < t->ticks_per_second)
        return;

    last_display_tick = now;
    lease_display_secs--;
    if (!screensaver_is_active())
        update_lease_time_display(lease_display_secs);
}

void dhcp_poll(void)
{
    const target_ops_t *t = common_get_target();

    if (__builtin_expect(!booted, 0)) {
        disp_info();
    }

    /* --- Renewal check (50% of lease elapsed) --- */
    if (__builtin_expect(dhcp_lease_time && (!dont_renew) &&
                         (lease_display_secs <= renew_threshold), 0))
    {
        unsigned int saved_lease_time = dhcp_lease_time;
        dhcp_lease_time = 0;

        dhcp_waiting_mode_display();
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
            /* Lost/late renew ACK.  Renewal happens at T/2, so the
             * current lease is still valid — a single missed ACK must
             * NOT disable DHCP (the old code latched our_ip=0xffffffff
             * and blanked the IP forever, with no recovery path).  Keep
             * the IP, keep the existing lease counting down, and retry
             * RENEW after ~half the remaining lease (floored so the fast
             * poll loop can't storm the wire).  Only when the lease has
             * fully expired with every renew having failed do we fall
             * back to a fresh DISCOVER. */
            if (lease_display_secs == 0)
            {
                /* Lease expired, all renews failed → return to INIT;
                 * the discovery branch below re-acquires immediately. */
                our_ip = 0;
                dhcp_lease_time = 0;
                lease_expiry_rtc = 0;
                renew_threshold = 0;
            }
            else
            {
                unsigned int half = lease_display_secs / 2;

                dhcp_lease_time = saved_lease_time;   /* keep tracking */
                renew_threshold = (half > DHCP_RENEW_RETRY_FLOOR_SECS)
                                  ? half : DHCP_RENEW_RETRY_FLOOR_SECS;
                if (renew_threshold >= lease_display_secs)
                    renew_threshold = lease_display_secs - 1;
                last_display_tick = t->get_ticks();
                update_ip_display(our_ip, dhcp_mode_string);
                update_lease_time_display(lease_display_secs);
            }
        }
        else
        {
            /* Success: set new expiry from fresh lease time */
            lease_expiry_rtc = t->get_rtc() + dhcp_lease_time;
            lease_display_secs = dhcp_lease_time;
            renew_threshold = dhcp_lease_time / 2;
            last_display_tick = t->get_ticks();
            update_ip_display(our_ip, dhcp_mode_string);
            update_lease_time_display(lease_display_secs);
        }
    }

    /* --- Discovery check (no IP, or NAK'd and lease fully expired) --- */
    if (__builtin_expect(((our_ip & 0xff000000) == 0) ||
                         (dont_renew && lease_display_secs == 0), 0))
    {
        dont_renew = false;
        dhcp_waiting_mode_display();

        int dhcp_result = dhcp_go((unsigned int *)&our_ip);
        if (dhcp_result == -1 || dhcp_nest_counter_maxed)
        {
            /* Transient failure (cold-start IRX RX not live yet, OFFER
             * lost, ...).  Stay in the "no IP yet" state so the next
             * dhcp_poll() re-runs the full DISCOVER sequence — never
             * permanently disable DHCP on a timeout. */
            our_ip = 0;
            dhcp_nest_counter_maxed = 0;
            lease_expiry_rtc = 0;
            lease_display_secs = 0;
            dhcp_waiting_mode_display();
        }
        else
        {
            /* Got an address from DHCP */
            lease_expiry_rtc = t->get_rtc() + dhcp_lease_time;
            lease_display_secs = dhcp_lease_time;
            renew_threshold = dhcp_lease_time / 2;
            last_display_tick = t->get_ticks();
            update_ip_display(our_ip, dhcp_mode_string);
            update_lease_time_display(lease_display_secs);
        }
    }
}

/* ===== Entry point ===== */

int main(void)
{
    const target_ops_t *t = target_get_ops();
    common_main(t, &client_network_transport_ops);

    return 0;
}
