#ifndef OD_EPD_SPI_H
#define OD_EPD_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	OD_EPD_SPI_BACKEND_NONE = 0,
	OD_EPD_SPI_BACKEND_SPIM,
	OD_EPD_SPI_BACKEND_BITBANG,
} od_epd_spi_backend_t;

/* Acquire using the runtime-configured pins. Repeating the same acquisition is a no-op. */
bool od_epd_spi_init(uint8_t mosi_cfg, uint8_t sck_cfg);

/* Synchronous mode-0/MSB-first write. The source may be outside EasyDMA-accessible RAM. */
bool od_epd_spi_write(const uint8_t *src, size_t len);

void od_epd_spi_deinit(void); /* Safe before init and safe when repeated. */
bool od_epd_spi_faulted(void);

/* Only the display owner's abort/session-reset boundary may clear a fault, and only
 * after deinit. Ordinary acquire/release preserves it. */
bool od_epd_spi_fault_reset(void);
uint32_t od_epd_spi_hz(void);
od_epd_spi_backend_t od_epd_spi_backend(void);

#ifdef __cplusplus
}
#endif

#endif
