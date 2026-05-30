/* client/playstation2/iop_dev9.c
 *
 * EE-side front end for IOP DEV9 initialization.
 *
 * This file is intentionally freestanding and project-owned.  It keeps the
 * public API and last-diagnostics state local to the PS2 networking stack,
 * while delegating the IOP driver-load mechanics to the bootstrap layer.
 */

#include <string.h>
#include "iop_dev9.h"
#include "bootstrap/iop_bootstrap.h"

static iop_dev9_diag_t last_diag;

int iop_dev9_init(void) {
    int result;

    memset(&last_diag, 0, sizeof(last_diag));
    result = ps2_bootstrap_iop_dev9_init(&last_diag);
    last_diag.result = result;

    return result;
}

const iop_dev9_diag_t *iop_dev9_last_diag(void) {
    return &last_diag;
}

const char *iop_dev9_phase1_status(void) {
    return ps2_bootstrap_phase1_status();
}
