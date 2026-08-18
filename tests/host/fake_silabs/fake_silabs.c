/* Host drivers for targets/efr32bg22-slc/od_cmd_silabs.c. No expected reply bytes live here. */

#include "fake_silabs.h"

#include "od_config_asm.h"
#include "od_core.h"
#include "od_session.h"
#include "od_session_app.h"
#include "od_txq.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_display.h"
#include "opendisplay_led.h"
#include "opendisplay_pipe.h"

#include <string.h>

bool fake_silabs_store_ok;
unsigned fake_silabs_store_attempts;
unsigned fake_silabs_store_saves;
unsigned fake_silabs_store_reloads;
unsigned fake_silabs_store_clears;
bool fake_silabs_save_saw_queue_empty;
bool fake_silabs_reload_saw_queued;
bool fake_silabs_reload_saw_authenticated;
uint8_t fake_silabs_store_blob[2048];
uint32_t fake_silabs_store_len;
bool fake_silabs_xfer_active;
bool fake_silabs_refresh_ok;
unsigned fake_silabs_refreshes;
unsigned fake_silabs_aborts;
unsigned fake_silabs_resets;
bool fake_silabs_nfc_read_ok;
uint16_t fake_silabs_nfc_read_len;

static struct od_config_asm s_assembler;

void fake_silabs_reset(void)
{
    fake_silabs_store_ok = true;
    fake_silabs_store_attempts = 0u;
    fake_silabs_store_saves = 0u;
    fake_silabs_store_reloads = 0u;
    fake_silabs_store_clears = 0u;
    fake_silabs_save_saw_queue_empty = false;
    fake_silabs_reload_saw_queued = false;
    fake_silabs_reload_saw_authenticated = false;
    fake_silabs_xfer_active = false;
    fake_silabs_refresh_ok = true;
    fake_silabs_refreshes = 0u;
    fake_silabs_aborts = 0u;
    fake_silabs_resets = 0u;
    fake_silabs_nfc_read_ok = true;
    fake_silabs_nfc_read_len = 4u;
    fake_silabs_store_len = 0u;
    memset(fake_silabs_store_blob, 0, sizeof fake_silabs_store_blob);
    od_config_asm_reset(&s_assembler);
}

struct od_config_asm *opendisplay_config_assembler(void) { return &s_assembler; }
bool initConfigStorage(void) { return fake_silabs_store_ok; }

bool saveConfig(uint8_t *data, uint32_t len)
{
    ++fake_silabs_store_attempts;
    fake_silabs_save_saw_queue_empty = od_txq_depth() == 0u;
    if (!fake_silabs_store_ok || data == NULL || len > sizeof fake_silabs_store_blob) return false;
    memcpy(fake_silabs_store_blob, data, len);
    fake_silabs_store_len = len;
    ++fake_silabs_store_saves;
    od_config_asm_reset(&s_assembler);
    return true;
}

bool loadConfig(uint8_t *data, uint32_t *len)
{
    if (!fake_silabs_store_ok || data == NULL || len == NULL || fake_silabs_store_len == 0u ||
        *len < fake_silabs_store_len) return false;
    memcpy(data, fake_silabs_store_blob, fake_silabs_store_len);
    *len = fake_silabs_store_len;
    return true;
}

bool clearStoredConfig(void)
{
    if (!fake_silabs_store_ok) return false;
    fake_silabs_store_len = 0u;
    ++fake_silabs_store_clears;
    return true;
}

uint32_t calculateConfigCRC(uint8_t *data, uint32_t len)
{ (void)data; (void)len; return 0u; }

const struct SecurityConfig *od_get_parsed_security(void) { return NULL; }

uint16_t opendisplay_ble_get_app_version(void) { return 0x0105u; }
uint8_t opendisplay_ble_get_app_version_patch(void) { return 3u; }
void opendisplay_ble_schedule_dfu(void) { }
void opendisplay_ble_schedule_deep_sleep(void) { }
void opendisplay_ble_reload_config_from_nvm(void)
{
    fake_silabs_reload_saw_queued = od_txq_depth() != 0u;
    fake_silabs_reload_saw_authenticated =
        od_session_authenticated(od_session_app_state());
    ++fake_silabs_store_reloads;
}
void opendisplay_ble_copy_msd_bytes(uint8_t out[16]) { memset(out, 0, 16u); }

bool opendisplay_ble_nfc_read(uint8_t *type, uint8_t *data, uint16_t *len, uint16_t max)
{
    const uint16_t n = fake_silabs_nfc_read_len;
    if (!fake_silabs_nfc_read_ok || n > max) return false;
    *type = 1u;
    memset(data, 0x5au, n);
    *len = n;
    return true;
}

bool opendisplay_ble_nfc_write(uint8_t type, const uint8_t *data, uint16_t len)
{ (void)type; (void)data; (void)len; return true; }

int opendisplay_display_direct_write_start(const uint8_t *data, uint16_t len)
{ (void)data; (void)len; fake_silabs_xfer_active = true; return 0; }

int opendisplay_display_direct_write_data(const uint8_t *data, uint16_t len)
{ (void)data; (void)len; return fake_silabs_xfer_active ? 0 : -1; }

int opendisplay_display_direct_write_end_prepare(const uint8_t *data, uint16_t len)
{ (void)data; (void)len; return fake_silabs_xfer_active ? 0 : -1; }

int opendisplay_display_direct_write_end_refresh(bool *ok)
{
    ++fake_silabs_refreshes;
    fake_silabs_xfer_active = false;
    if (ok != NULL) *ok = fake_silabs_refresh_ok;
    return 0;
}

int opendisplay_display_direct_write_end(const uint8_t *data, uint16_t len, bool *ok)
{
    int rc = opendisplay_display_direct_write_end_prepare(data, len);
    return rc == 0 ? opendisplay_display_direct_write_end_refresh(ok) : rc;
}

void opendisplay_display_abort(void)
{ ++fake_silabs_aborts; fake_silabs_xfer_active = false; }
void opendisplay_display_boot_apply(void) { }
void opendisplay_display_park_pins(void) { }
void opendisplay_display_power_off(void) { }

int opendisplay_led_activate(uint8_t index, const uint8_t *data, uint16_t len)
{ (void)index; (void)data; (void)len; return 0; }
int opendisplay_led_stop(uint8_t index, bool present)
{ (void)index; (void)present; return 0; }

#ifndef OD_TEST_REAL_SILABS_PIPE
bool opendisplay_pipe_wait_tx_idle(uint32_t tag, uint32_t deadline_ms)
{ (void)tag; (void)deadline_ms; return true; }
void opendisplay_pipe_close_tag(uint32_t tag) { (void)tag; }
void opendisplay_pipe_reset_transport(void) { ++fake_silabs_resets; od_core_reset(); }
void sl_bt_run(void) { }
#endif
void NVIC_SystemReset(void) { ++fake_silabs_resets; }
