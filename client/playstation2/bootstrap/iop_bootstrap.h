/* client/playstation2/bootstrap/iop_bootstrap.h
 *
 * EE-side entry points for loading the PS2 IOP Ethernet drivers.
 *
 * The network stack calls these helpers without knowing the details of how
 * DEV9 and SMAP are loaded into the IOP.
 */

#ifndef PS2_IOP_BOOTSTRAP_H
#define PS2_IOP_BOOTSTRAP_H

#include <stdint.h>

#include "iop_dev9.h"

int ps2_bootstrap_iop_dev9_init(iop_dev9_diag_t *diag);
const char *ps2_bootstrap_phase1_status(void);

/* Optional diagnostic: list IOP libraries found in RAM.  Only runs when
 * explicitly built/called for debugging; not part of normal loader startup. */
void ps2_bootstrap_dump_iop_modules(void);

/* Connect the EE side to smap.irx after ps2_bootstrap_iop_dev9_init()
 * has loaded it.  Returns 0 on success. */
int ps2_bootstrap_iop_smap_init(void);

/* DMA bytes from EE memory into IOP RAM.  Destination is an IOP physical
 * address, not the EE 0xBC mirror. */
int ps2_bootstrap_iop_dma_write(uint32_t iop_dest, const void *src,
                                uint32_t size);

#endif /* PS2_IOP_BOOTSTRAP_H */
