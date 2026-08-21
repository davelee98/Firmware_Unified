#ifndef OD_FUZZ_PIPE_SUPPORT_H
#define OD_FUZZ_PIPE_SUPPORT_H

#include "od_cmd.h"

void fz_pipe_reset(void);
od_cmd_ctx_t fz_pipe_ctx(uint16_t wire_len, bool was_protected);
void fz_pipe_open(uint8_t flags, uint8_t window, uint8_t ack_every,
                  uint16_t client_max_frame);

#endif
