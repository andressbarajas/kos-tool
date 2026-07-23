/* client/dreamcast/hardware.h */
/*
 * Dreamcast system-type detection.
 *
 * The Holly system-mode register reports whether the SH4 is running in a
 * retail Dreamcast or a NAOMI/System SP arcade board.  This mirrors KOS's
 * hardware_sys_mode() (kernel/arch/dreamcast/hardware/hardware.c): a single
 * read of 0xa05f74b0, with bits [7:4] holding the hardware type and bits
 * [3:0] the region.
 *
 * Shared by the common video path (cable/VGA handling) and the IP network
 * adapter code, so it lives here rather than being duplicated per module.
 */

#ifndef KOSLOAD_DC_HARDWARE_H
#define KOSLOAD_DC_HARDWARE_H

#define DC_SYSMODE_REG     0xa05f74b0   /* Holly system-mode register */
#define DC_HW_TYPE_RETAIL  0x0          /* retail Dreamcast */
#define DC_HW_TYPE_SET5    0x9          /* A Set5.xx devkit. */
#define DC_HW_TYPE_NAOMI   0xa          /* NAOMI / System SP arcade */

/* Returns the hardware type: DC_HW_TYPE_RETAIL, DC_HW_TYPE_NAOMI, ... */
int dc_hardware_type(void);

#endif /* KOSLOAD_DC_HARDWARE_H */
