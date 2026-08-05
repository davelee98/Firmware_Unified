#ifndef BOARD_NRF54_H
#define BOARD_NRF54_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void board_nrf54_early_init(void);
void board_nrf54_prepare_epd_rail(void);
/* Deep-power-down (0xB9) then park SPI lines. miso/wp/hold may be 0 or 0xFF to skip. */
void board_nrf54_flash_powerdown(uint8_t mosi_cfg, uint8_t sck_cfg, uint8_t cs_cfg,
				 uint8_t miso_cfg, uint8_t wp_cfg, uint8_t hold_cfg);

#ifdef __cplusplus
}
#endif

#endif
