#ifndef OD_BOARD_H
#define OD_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

const char *od_board_name(void);
void od_board_early_init(void);
void od_board_prepare_epd_rail(void);
bool od_board_epd_requires_cold_cycle(void);
bool od_board_epd_pin_reserved(uint8_t port, uint8_t pin);
bool od_board_spim_pin_ok(uint8_t sck_port, uint8_t sck_pin,
			  uint8_t mosi_port, uint8_t mosi_pin);
/* Deep-power-down (0xB9) then park SPI lines. miso/wp/hold may be 0 or 0xFF to skip. */
void od_board_flash_powerdown(uint8_t mosi_cfg, uint8_t sck_cfg, uint8_t cs_cfg,
			      uint8_t miso_cfg, uint8_t wp_cfg, uint8_t hold_cfg);

#ifdef __cplusplus
}
#endif

#endif
