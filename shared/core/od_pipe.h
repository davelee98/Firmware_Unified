/* od_pipe.h -- shared PIPE command machine and its target-selected window facts. */
#ifndef OD_PIPE_H
#define OD_PIPE_H

#include "od_caps.h"
#include "od_cmd.h"
#include "od_rxq.h"
#include "od_span.h"
#include "od_txq.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#ifndef OD_PIPE_MAX_W
#define OD_PIPE_MAX_W 32u
#endif

#ifndef OD_PIPE_MAX_N
#define OD_PIPE_MAX_N OD_PIPE_MAX_W
#endif

#define OD_PIPE_REORDER_SLOTS   (OD_PIPE_MAX_W + 1u)
#define OD_PIPE_REORDER_PAYLOAD (PIPE_MAX_FRAME - PIPE_FRAME_OVERHEAD)

#if OD_CAP_PIPE
OD_STATIC_ASSERT(OD_PIPE_MAX_W >= 1u && OD_PIPE_MAX_W <= PIPE_ACK_MASK_BITS,
                 "PIPE window must fit the SACK mask");
OD_STATIC_ASSERT(OD_PIPE_MAX_N >= 1u && OD_PIPE_MAX_N <= OD_PIPE_MAX_W,
                 "PIPE ACK cadence must fit the window");
OD_STATIC_ASSERT(OD_PIPE_REORDER_SLOTS == OD_PIPE_MAX_W + 1u,
                 "PIPE reorder ring needs W+1 collision-free slots");
OD_STATIC_ASSERT(OD_PIPE_REORDER_PAYLOAD == 241u,
                 "PIPE reorder payload follows the wire frame geometry");
OD_STATIC_ASSERT(OD_RXQ_SLOTS >= OD_PIPE_MAX_W + 2u,
                 "RX ring must hold a PIPE window plus END");
OD_STATIC_ASSERT(OD_TXQ_SLOTS >= OD_PIPE_MAX_W + 2u,
                 "TX ring must hold a PIPE window plus END");
#endif

#ifdef __cplusplus
extern "C" {
#endif

od_cmd_result_t od_pipe_start(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_pipe_data(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_pipe_end(const od_cmd_ctx_t *ctx, od_span_t body);
void od_pipe_reset_state(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_PIPE_H */
