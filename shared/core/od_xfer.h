/* od_xfer.h -- shared legacy direct/partial transfer command policy. */
#ifndef OD_XFER_H
#define OD_XFER_H

#include "od_cmd.h"
#include "od_span.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

od_cmd_result_t od_xfer_direct_start(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_xfer_data(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_xfer_end(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_xfer_partial_start(const od_cmd_ctx_t *ctx, od_span_t body);

void od_xfer_reset(void);
bool od_xfer_active(void);
bool od_xfer_owns_hardware(void);
bool od_xfer_frames_may_arrive(void);
bool od_xfer_owner(od_reply_t *out);
bool od_xfer_started_ms(uint32_t *out);

/* True once an active transfer has begun consuming DATA frames. Safe to call from a radio
 * callback while the loop task owns the transfer state. */
bool od_xfer_log_quiet(uint16_t opcode);

#ifdef __cplusplus
}
#endif

#endif /* OD_XFER_H */
