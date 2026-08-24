/* od_buzzer -- portable CMD_BUZZER melody runner. */

#ifndef OD_BUZZER_H
#define OD_BUZZER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OD_BUZZER_IDLE              0xFFFFFFFFu
#define OD_BUZZER_PAYLOAD_MAX       256u
#define OD_BUZZER_MAX_TOTAL_MS      30000u
#define OD_BUZZER_DURATION_UNIT_MS  5u
#define OD_BUZZER_INTER_PATTERN_MS  20u

struct od_buzzer_config {
    uint8_t drive_pin;
    uint8_t enable_pin;
    uint8_t flags;
    uint8_t duty_percent;
};

/* Authority frequency rule: 13.75 * 2^(idx/24) Hz, octave-folded into the safe index window
 * 117..234. Index 0 is silence. The integer result is centi-Hz. */
uint32_t od_buzzer_index_centihz(uint8_t index);

/* Validate and start one complete CMD_BUZZER body. Instance lookup and the drive-pin check stay
 * target-side; payload[0] is retained only as the opaque instance tag. Returns deployed errors:
 * 1 short, 4 zero patterns, 5 truncated/too large, 6 trailing bytes, or 0 accepted. */
int od_buzzer_activate(const struct od_buzzer_config *config,
                       const uint8_t *payload, uint16_t payload_len, uint32_t now_ms);

/* Stop immediately. Safe while idle. */
void od_buzzer_stop(void);

/* Advance at most one timed item and return a relative delay, or OD_BUZZER_IDLE. Early calls are
 * harmless. A late caller slips rather than fast-forwarding, matching both target schedulers. */
uint32_t od_buzzer_service(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* OD_BUZZER_H */
