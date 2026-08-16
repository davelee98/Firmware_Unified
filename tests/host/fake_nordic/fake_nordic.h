/* fake_nordic.h -- the target seams the Nordic command translation units reach through, faked so
 * they can be compiled and run on a workstation.
 *
 * THE POINT IS THAT THE COMMAND CODE IS REAL. od_cmd_{device,config,direct,nfc}.c and
 * opendisplay_pipe_write.cpp are linked as production sources; what is faked is everything BELOW
 * them -- the panel, NVM, the BLE stack, the kernel. A transcription of a handler would prove only
 * that the copy agrees with itself, which is the rule tests/host has followed since C11.
 *
 * KNOBS, NEVER ANSWERS. Nothing here may be given an expected wire frame. Each fake exposes the
 * inputs a real driver would have -- a return code, a stored blob, a version number -- and the
 * reply bytes are assembled by the production code above it. See the C12 plan, section 4.2.
 */

#ifndef OD_TEST_FAKE_NORDIC_H
#define OD_TEST_FAKE_NORDIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Clear every fake back to a benign, working device. Called before each vector so one vector's
 * leftovers cannot make the next one pass. */
void fake_nordic_reset(void);

/* --- panel ---------------------------------------------------------------------------------- */

extern int      fake_disp_data_rc;        /* what direct_write_data returns */
extern int      fake_disp_prepare_rc;
extern int      fake_disp_refresh_rc;
extern bool     fake_disp_refresh_ok;     /* the refresh completed rather than timing out */
extern int      fake_disp_start_rc;
extern int      fake_disp_partial_start_rc;
extern uint8_t  fake_disp_partial_err;
extern uint32_t fake_disp_total_bytes;    /* the transfer's declared size */
extern uint32_t fake_disp_written;
extern bool     fake_disp_dw_active;
extern unsigned fake_disp_aborts;
extern unsigned fake_disp_refreshes;

/* --- config storage ------------------------------------------------------------------------- */

extern bool     fake_store_init_ok;
extern bool     fake_store_save_ok;
extern bool     fake_store_load_ok;
extern bool     fake_store_clear_ok;
extern unsigned fake_store_saves;
extern unsigned fake_store_reloads;       /* opendisplay_ble_reload_config_from_nvm() calls */
#define FAKE_STORE_MAX 4096u
extern uint8_t  fake_store_blob[FAKE_STORE_MAX];
extern uint32_t fake_store_len;

/* --- BLE, LED, buzzer, NFC -------------------------------------------------------------------- */

extern uint16_t fake_ble_version;         /* [major:8][minor:8] */
extern uint8_t  fake_ble_patch;
extern uint8_t  fake_ble_msd[16];
extern unsigned fake_ble_dfu;
extern unsigned fake_ble_deep_sleep;
extern int      fake_led_activate_rc;
extern int      fake_led_stop_rc;
extern bool     fake_led_stop_had_index;
extern int      fake_buzzer_rc;
extern bool     fake_nfc_read_ok;
extern bool     fake_nfc_write_ok;
extern uint8_t  fake_nfc_rec_type;
extern uint16_t fake_nfc_read_len;        /* how many tag bytes a read returns */

/* --- kernel ----------------------------------------------------------------------------------- */

extern uint32_t fake_k_uptime_ms;
extern uint32_t fake_k_slept_ms;
extern unsigned fake_nvic_resets;

#endif /* OD_TEST_FAKE_NORDIC_H */
