/* od_buzzer_app -- target tone generation for the shared buzzer runner. */

#ifndef OD_BUZZER_APP_H
#define OD_BUZZER_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start or retune a tone. Frequency is centi-Hz; duty 0 or >100 means the deployed 50% default. */
bool od_buzzer_app_tone_start(uint8_t drive_pin, uint32_t centihz, uint8_t duty_percent);

/* Stop tone generation and leave the encoded drive pin low. Must be idempotent. */
void od_buzzer_app_tone_stop(uint8_t drive_pin);

/* Write the optional enable pin. OD_PIN_UNUSED is filtered by the shared machine. */
void od_buzzer_app_enable_write(uint8_t enable_pin, bool level_high);

#ifdef __cplusplus
}
#endif

#endif /* OD_BUZZER_APP_H */
