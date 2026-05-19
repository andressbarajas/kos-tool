/* client/include/kosload/target.h */
#ifndef KOSLOAD_TARGET_H
#define KOSLOAD_TARGET_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Target platform interface.
 *
 * This is the console boundary for client firmware. Shared client code should
 * use this table for hardware-facing work instead of branching on Dreamcast,
 * GameCube, or future console names. A new console port should provide one
 * implementation of this table, wire it into its serial/network entrypoints,
 * and keep console-specific video, timer, cache, reboot, execute, and storage
 * behavior behind these callbacks.
 *
 * The common entrypoint calls init() before transport initialization. Transport
 * implementations may use the timer and video callbacks after common_main()
 * stores this table, so implementations should leave those callbacks usable
 * for the rest of the loader lifetime.
 */
typedef struct target_ops {
    const char *name;               /* "Dreamcast" or "GameCube" */
    uint32_t    default_load;       /* DC: 0x8c010000, GC: 0x80003100 */

    int         (*init)(void);
    void        (*draw_string)(int x, int y, const char *str, uint32_t color);
    void        (*clear_screen)(uint32_t color);
    void        (*setup_video)(uint32_t mode, uint32_t bg_color);
    void        (*execute)(uint32_t address);
    void        (*disable_cache)(void);
    void        (*reboot)(void);
    void        (*cdfs_redir_save)(void);
    void        (*cdfs_redir_enable)(void);
    void        (*cdfs_redir_disable)(void);
    void        (*set_console_enabled)(bool enabled);
    void        (*set_rtc)(uint32_t unix_timestamp);
    uint32_t    (*get_rtc)(void);       /* Read RTC as Unix timestamp (seconds since 1970) */

    /* Screensaver support */
    uint64_t    (*get_ticks)(void);     /* Monotonic count-up tick counter */
    uint32_t    ticks_per_second;       /* Tick rate for get_ticks */
    void        (*fill_rect)(int x, int y, int w, int h, uint32_t color);
    void        (*draw_bitmap)(int x, int y, int w, int h,
                               const uint32_t *bits, uint32_t color);
    void        (*restart_timer)(void); /* Restart hardware timer after program return */

    /* Memory detection */
    uint32_t    (*detect_ram_size)(void); /* Detect total usable RAM in bytes */
} target_ops_t;

/* Target implementations */
extern const target_ops_t dreamcast_target_ops;
extern const target_ops_t gamecube_target_ops;
extern const target_ops_t playstation2_target_ops;

/* Gets the target_ops for the current build */
const target_ops_t *target_get_ops(void);

#endif /* KOSLOAD_TARGET_H */
