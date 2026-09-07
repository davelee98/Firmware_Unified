#ifndef OD_NRF51_BLE_H
#define OD_NRF51_BLE_H

#include <stdbool.h>

void od_nrf51_ble_init(void);
bool od_nrf51_ble_process_one(void);
void od_nrf51_ble_flush(void);
bool od_nrf51_ble_healthy(void);
void od_nrf51_ble_note_progress(void);

#endif
