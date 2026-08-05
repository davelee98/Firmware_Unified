#ifndef OPENDISPLAY_DISPLAY_H
#define OPENDISPLAY_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int opendisplay_display_direct_write_start(const uint8_t *payload, uint16_t payload_len);
int opendisplay_display_direct_write_data(const uint8_t *payload, uint16_t payload_len);
int opendisplay_display_direct_write_end_prepare(const uint8_t *payload, uint16_t payload_len);
int opendisplay_display_direct_write_end_refresh(const uint8_t *payload, uint16_t payload_len,
                                                 bool *refresh_ok);
int opendisplay_display_partial_write_start(const uint8_t *payload, uint16_t payload_len,
                                            uint8_t *err_code_out);
bool opendisplay_display_partial_active(void);
bool opendisplay_display_dw_active(void);
uint32_t opendisplay_display_bytes_written(void);
uint32_t opendisplay_display_total_bytes(void);
uint32_t opendisplay_display_expected_dw_bytes(void);
uint32_t opendisplay_display_displayed_etag(void);
void opendisplay_display_clear_etag(void);
void opendisplay_display_set_partial_new_etag(uint32_t new_etag);
uint32_t opendisplay_display_partial_bytes_written(void);
uint32_t opendisplay_display_partial_expected(void);
bool opendisplay_display_partial_compressed(void);
uint32_t opendisplay_display_calc_plane_bytes(uint16_t width, uint16_t height);
int opendisplay_display_pipe_full_start(bool compressed, uint32_t total_size);
int opendisplay_display_pipe_partial_arm(uint8_t flags, uint32_t old_etag, uint16_t x, uint16_t y,
                                         uint16_t w, uint16_t h, uint32_t total_size,
                                         uint8_t *err_code_out);
int opendisplay_display_pipe_partial_prepare(void);
void opendisplay_display_abort(void);
void opendisplay_display_boot_apply(void);
void opendisplay_display_park_pins(void);
void opendisplay_display_power_off(void);

#ifdef __cplusplus
}
#endif

#endif
