#ifndef OPENDISPLAY_BUZZER_H
#define OPENDISPLAY_BUZZER_H

#include <stdbool.h>
#include <stdint.h>

/* Configure the drive/enable GPIOs for every passive_buzzer (0x29) instance in
 * the loaded config. Safe to call when no buzzer is configured. */
void opendisplay_buzzer_init(void);

/*
 * Handle a 0x0077 buzzer-activate payload (bytes after the 2-byte command).
 * Validation and wire semantics mirror the Arduino reference handleBuzzerActivate.
 * Return value is the reference error code:
 *   0 = accepted (playback started in the background)
 *   1 = payload too short (len < 3)
 *   2 = instance out of range
 *   3 = instance has no drive pin
 *   4 = pattern_count == 0
 *   5 = truncated pattern/step data
 *   6 = trailing bytes after declared patterns
 * The pipe layer turns 0 into {0x00,0x77,0x00,0x00} and n into {0xFF,0x77,n,0x00}.
 */
int opendisplay_buzzer_activate(const uint8_t *payload, uint16_t payload_len);

/* Advance the non-blocking playback state machine. Call from the main loop. */
void opendisplay_buzzer_process(void);

/* Stop any playback immediately and silence the buzzer. */
void opendisplay_buzzer_stop(void);

#endif
