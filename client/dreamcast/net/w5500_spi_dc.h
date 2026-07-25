/* client/dreamcast/net/w5500_spi_dc.h */
#ifndef __W5500_SPI_DC_H__
#define __W5500_SPI_DC_H__

#include "../../common/drivers/w5500.h"

/*
 * Dreamcast SPI backends for the W5500 (one per serial port wiring):
 *   - SCIF: bit-bang SPI on the SCIF pins (w5500_spi_dc_scif.c)
 *   - SCI:  hardware synchronous-mode SPI on the SCI pins (w5500_spi_dc_sci.c)
 */
extern const w5500_spi_ops_t dc_w5500_scif_spi_ops;
extern const w5500_spi_ops_t dc_w5500_sci_spi_ops;

#endif /* __W5500_SPI_DC_H__ */
