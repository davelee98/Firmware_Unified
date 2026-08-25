#ifndef OPENDISPLAY_BLE_H
#define OPENDISPLAY_BLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct od_config;

void opendisplay_ble_init(void);
void opendisplay_ble_process(void);
void opendisplay_ble_advertising_tick(void);
bool opendisplay_ble_is_connected(void);
bool opendisplay_ble_advertising_active(void);

const struct od_config *opendisplay_get_global_config(void);
void opendisplay_ble_reload_config_from_nvm(void);

uint16_t opendisplay_ble_get_app_version(void);
uint8_t opendisplay_ble_get_app_version_patch(void);
void opendisplay_ble_copy_msd_bytes(uint8_t out[16]);
void opendisplay_ble_update_msd(bool refresh_advertising);
void opendisplay_ble_set_dynamic_byte(uint8_t index, uint8_t value);
float opendisplay_ble_get_chip_temperature(void);

/* Sets MSD status bit2 (connectionRequested). Mirrors the reference nRF52840
 * flag (main.h:127, display_service.cpp:1296): a device-side request for the
 * host to connect. No producer exists yet on this branch (nor in the reference,
 * where it is reserved-for-future and stays 0); this is the hook a future
 * feature / sibling branch wires. */
void opendisplay_ble_set_connection_requested(bool requested);

bool opendisplay_ble_pipe_notify(const uint8_t *data, uint16_t len);
bool opendisplay_ble_pipe_notify_enabled(void);
void opendisplay_ble_pipe_on_write(const uint8_t *data, uint16_t len, bool write_cmd);
void opendisplay_ble_pipe_on_connection_closed(void);

/* Low 24 bits of the advertised identity -- the "xxxxxx" in OD<xxxxxx>. Boards differ in
 * which FICR word this comes from, so every consumer must call this rather than re-derive it.
 * NOT the session-auth device id, which is a different word by contract. */
uint32_t opendisplay_ble_chip_id_last24(void);

void opendisplay_ble_schedule_dfu(void);
void opendisplay_ble_schedule_deep_sleep(void);
/* Tag I/O is shared/core/od_nfc_app.h's, implemented in opendisplay_nfc.c. */

#ifdef __cplusplus
}
#endif

#endif
