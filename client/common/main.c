/* client/common/main.c */
/*
 * Common client main entry point.
 * Initializes the target platform and transport, then enters the main loop.
 */

#include <stdint.h>
#include <kosload/target.h>
#include <kosload/transport.h>
#include <kosload/protocol.h>

/* From target-specific target.c */
extern void exception_init(void);
#include <kosload/video.h>

/* Version string - configured by CMake, fallback for direct builds */
#ifndef KOSLOAD_VERSION_STRING
#define KOSLOAD_VERSION_STRING "0.1.0"
#endif

static const target_ops_t *target;
static const client_transport_ops_t *transport;

void common_main(const target_ops_t *tgt, const client_transport_ops_t *xport) {
    target = tgt;
    transport = xport;

    /* Initialize target hardware (serial port, etc.) */
    target->init();

    /* Install exception handlers */
    exception_init();

    /* Initialize transport BEFORE video — on Dreamcast, the BBA/GAPS
     * bridge must be initialized before video init touches the G2 bus.
     * This matches the original dcload-ip initialization order. */
    if (transport->init() != 0) {
        /* Transport failed — show red error screen and keep retrying.
         * 0x2000 is red in RGB555 (matches legacy ERROR_BG_COLOR).
         * The user can insert the BBA/LAN adapter without power cycling. */
        target->setup_video(0, 0x2000);
        target->clear_screen(0x2000);
        target->draw_string(30, 54, LOADER_NAME " " KOSLOAD_VERSION_STRING, 0xffff);
        if (transport->init_error_msg)
            target->draw_string(30, 78, transport->init_error_msg, 0xffff);

        while (transport->init() != 0) {
            /* Wait ~1 second before trying again */
            uint64_t start = target->get_ticks();
            while ((target->get_ticks() - start) < target->ticks_per_second)
                ;
        }
    }

    /* Enter main event loop — each transport draws its own boot screen */
    transport->loop(true);
}

const target_ops_t *common_get_target(void) {
    return target;
}

const client_transport_ops_t *common_get_transport(void) {
    return transport;
}
