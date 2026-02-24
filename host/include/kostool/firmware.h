/* host/include/kostool/firmware.h */
#ifndef KOSTOOL_FIRMWARE_H
#define KOSTOOL_FIRMWARE_H

#include <stdint.h>
#include "context.h"

/* Embedded firmware data (provided by BinToC-generated .c files,
 * or zero-length stubs when no firmware is embedded) */
extern const uint8_t firmware_dc_serial_data[];
extern const uint32_t firmware_dc_serial_size;
extern const uint8_t firmware_dc_ip_data[];
extern const uint32_t firmware_dc_ip_size;
extern const uint8_t firmware_gc_serial_data[];
extern const uint32_t firmware_gc_serial_size;
extern const uint8_t firmware_gc_ip_data[];
extern const uint32_t firmware_gc_ip_size;

/*
 * Check remote loader version and auto-update if needed.
 *
 * Returns:
 *   1  — firmware was updated, transport reconnected
 *   0  — no update needed
 *  -1  — update failed
 */
int auto_update_firmware(kostool_context_t *ctx);

#endif /* KOSTOOL_FIRMWARE_H */
