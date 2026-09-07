#ifndef OD_NRF51_SLIM_H
#define OD_NRF51_SLIM_H

#include <stdbool.h>
#include <stdint.h>

void od_nrf51_slim_init(void);
void od_nrf51_slim_reset(void);
bool od_nrf51_slim_admission_free(void);
bool od_nrf51_slim_receive(const uint8_t *frame, uint16_t length);
void od_nrf51_slim_tick(void);

bool od_nrf51_slim_tx_peek(const uint8_t **frame, uint8_t *length);
void od_nrf51_slim_tx_accepted(void);

/* Target link seams. Host tests provide fakes. */
void od_nrf51_copy_msd(uint8_t output[16]);
uint32_t od_nrf51_now_ms(void);
bool od_nrf51_panel_begin(void);
bool od_nrf51_panel_write(const uint8_t *data, uint16_t length);
bool od_nrf51_panel_refresh_start(void);
int od_nrf51_panel_poll(void);
void od_nrf51_panel_abort(void);

#endif
