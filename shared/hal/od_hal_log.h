/* od_hal_log.h -- complete-record application logging transport seam. */

#ifndef OD_HAL_LOG_H
#define OD_HAL_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the selected application-log transport. Idempotent. */
void od_hal_log_open(void);

/* True only after the selected transport has opened successfully. */
bool od_hal_log_is_open(void);

/* Consume or copy record[0..len) before returning. record[len] is NUL for text transports.
 * The implementation must not mutate or retain record, and must serialize concurrent callers. */
void od_hal_log_write(char *record, size_t len);

/* Drain toward the host using the target's bounded policy, then retain its settlement delay. */
void od_hal_log_flush(void);

/* Persistent boot/wake cycle number used in the application record prefix. */
uint32_t od_hal_log_cycle_count(void);

#ifdef __cplusplus
}
#endif

#endif
