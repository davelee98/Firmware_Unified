/* od_cmd_direct.c -- the non-PIPE image transfers: DIRECT_WRITE start/data/end and PARTIAL_WRITE
 * start.
 *
 * Adapters, not a state machine. The transfer state lives in opendisplay_display, so there is
 * nothing here for disconnect cleanup to reset -- opendisplay_display_abort() covers it.
 */

#include "od_cmd_app.h"

#include "od_cmd_reply.h"
#include "od_log.h"
#include "opendisplay_display.h"
#include "opendisplay_pipe_write.h"
#include "opendisplay_protocol.h"

#include <zephyr/kernel.h>

od_cmd_result_t od_cmd_app_partial_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { 0x00u, 0x76u };
  uint8_t err[] = { 0xFFu, 0x76u, OD_ERR_PARTIAL_STREAM, 0x00u };
  uint8_t err_code = OD_ERR_PARTIAL_STREAM;

  opendisplay_pipe_write_reset();
  if (opendisplay_display_partial_write_start(body.p, (uint16_t)body.n, &err_code) == 0) {
    (void)od_cmd_reply(ctx, ok, sizeof(ok));
  } else {
    err[2] = err_code;
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_direct_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { 0x00u, 0x70u };
  uint8_t err[] = { 0xFFu, 0x70u };

  od_log_info("pipe 0070 recv len=%u (epd init next)", (unsigned)body.n);
  opendisplay_pipe_write_reset();
  if (opendisplay_display_direct_write_start(body.p, (uint16_t)body.n) == 0) {
    (void)od_cmd_reply(ctx, ok, sizeof(ok));
  } else {
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_direct_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ack_data[] = { 0x00u, 0x71u };
  uint8_t err[] = { 0xFFu, 0x71u };
  uint8_t partial_err[] = { 0xFFu, 0x71u, OD_ERR_PARTIAL_STREAM, 0x00u };
  int rc;

  rc = opendisplay_display_direct_write_data(body.p, (uint16_t)body.n);
  if (rc == -4) {
    (void)od_cmd_reply_plain(ctx, partial_err, sizeof(partial_err));
    return OD_CMD_NACK;
  }
  if (rc != 0) {
    opendisplay_display_abort();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }

  (void)od_cmd_reply(ctx, ack_data, sizeof(ack_data));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_direct_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
  const uint8_t *payload = body.p;
  const uint16_t payload_len = (uint16_t)body.n;
  bool refresh_ok = false;
  uint8_t ack_end[] = { 0x00u, 0x72u };
  uint8_t ack_refresh_ok[] = { 0x00u, 0x73u };
  uint8_t ack_refresh_timeout[] = { 0x00u, 0x74u };
  uint8_t err[] = { 0xFFu, 0x72u };
  uint8_t partial_err[] = { 0xFFu, 0x72u, OD_ERR_PARTIAL_STREAM, 0x00u };
  int rc;

  rc = opendisplay_display_direct_write_end_prepare(payload, payload_len);
  if (rc == -4) {
    (void)od_cmd_reply_plain(ctx, partial_err, sizeof(partial_err));
    return OD_CMD_NACK;
  }
  if (rc != 0) {
    opendisplay_display_abort();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  /* Ack 0x72 before the blocking refresh, then report 0x73/0x74 afterwards
   * (same ordering as the nRF52840 Firmware).
   *
   * THE ACK DECIDES WHETHER THE REFRESH HAPPENS. od_reply() can substitute a plaintext hard NACK
   * for an END ack it could not seal, and it reports that rather than lying. Emitting the refresh
   * status afterwards would queue a success behind a rejection; refreshing at all would put
   * content on the panel that the host has just been told was refused. Neither the wire nor the
   * display may claim what the other denies, so both stop. */
  if (od_cmd_reply(ctx, ack_end, sizeof(ack_end)) != OD_TXQ_OK) {
    opendisplay_display_abort();
    return OD_CMD_NACK;
  }
  /* PUT THE ACK ON AIR BEFORE BLOCKING. od_cmd_reply only enqueues, and the pump cannot drain
   * until this handler returns -- which is on the far side of a refresh that can hold this thread
   * for a minute. Without this the host spends its tail-flush read, probes, and aborts a transfer
   * that completed. */
  od_cmd_flush_before_refresh();
  k_msleep(20);
  if (opendisplay_display_direct_write_end_refresh(payload, payload_len, &refresh_ok) != 0) {
    opendisplay_display_abort();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  /* Same rule as the END ack above: if od_reply had to substitute a hard NACK for this last
   * frame, that NACK is the only thing the host received for this command, and reporting
   * acceptance would report the opposite of what went out. The transfer is over either way. */
  if (od_cmd_reply(ctx, refresh_ok ? ack_refresh_ok : ack_refresh_timeout,
                   refresh_ok ? sizeof(ack_refresh_ok) : sizeof(ack_refresh_timeout))
      != OD_TXQ_OK) {
    return OD_CMD_NACK;
  }
  return OD_CMD_OK;
}
