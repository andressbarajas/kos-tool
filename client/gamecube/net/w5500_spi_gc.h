/* client/gamecube/net/w5500_spi_gc.h */
#ifndef __W5500_SPI_GC_H__
#define __W5500_SPI_GC_H__

#include "../../common/drivers/w5500.h"

extern const w5500_spi_ops_t gc_w5500_spi_ops;

/* Probe all EXI locations for a W5500. Returns 1 if found, 0 if not. */
int w5500_probe_exi_all(void);

#endif /* __W5500_SPI_GC_H__ */
