#ifndef OD_EPD_SPI_BITBANG_H
#define OD_EPD_SPI_BITBANG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool od_epd_bitbang_init(uint8_t mosi_port, uint8_t mosi_pin,
			 uint8_t sck_port, uint8_t sck_pin);
bool od_epd_bitbang_write(const uint8_t *src, size_t len);
void od_epd_bitbang_deinit(void);
uint32_t od_epd_bitbang_hz(void);

#endif
