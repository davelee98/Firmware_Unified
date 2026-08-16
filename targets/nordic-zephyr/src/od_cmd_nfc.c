/* od_cmd_nfc.c -- CMD_NFC_ENDPOINT (0x0083): tag read, single-shot write, and the chunked write
 * that carries a long MIME or vCard record across several frames.
 *
 * The chunk staging buffer and the response buffer are this file's, which is the point of the
 * split: they are the only NFC state that outlives a dispatch, and the reset they need is the one
 * thing the header exports.
 */

#include "od_cmd_nfc.h"

#include "od_cmd_app.h"
#include "od_cmd_reply.h"
#include "od_session.h"
#include "opendisplay_ble.h"
#include "opendisplay_constants.h"
#include "opendisplay_protocol.h"

#include <string.h>

static uint8_t s_nfc_rsp_buf[OD_PIPE_MAX_PAYLOAD];

typedef struct {
  bool active;
  uint8_t rec_type;
  uint16_t total_len;
  uint16_t received_len;
  /* Match OD_NFC write staging (long MIME / vCard). */
  uint8_t data[512];
} od_nfc_write_chunk_t;

static od_nfc_write_chunk_t s_nfc_write_chunk;

void od_cmd_nfc_reset(void)
{
  memset(&s_nfc_write_chunk, 0, sizeof(s_nfc_write_chunk));
}

static bool nfc_rec_type_valid(uint8_t rec_type)
{
  return rec_type == OD_NFC_REC_TEXT || rec_type == OD_NFC_REC_URI
         || rec_type == OD_NFC_REC_WELL_KNOWN_RAW || rec_type == OD_NFC_REC_MIME
         || rec_type == OD_NFC_REC_RAW_NDEF;
}

od_cmd_result_t od_cmd_app_nfc(const od_cmd_ctx_t *ctx, od_span_t body)
{
  const uint8_t *payload = body.p;
  const uint16_t payload_len = (uint16_t)body.n;
  uint16_t out_len;
  uint8_t rec_type;

  if (payload == NULL || payload_len < 1u) {
    uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x01u };
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }

  if (payload[0] == 0x00u) {
    /* 218, not OD_PIPE_MAX_PAYLOAD - 6 (238). The response is [status][cmd][4 B NFC metadata]
     * [tag data], and sealing caps the whole thing at OD_SESSION_PAYLOAD_MAX (222) after the two
     * response bytes -- so 238 bytes of tag data would seal to more than a BLE frame can carry and
     * be refused outright. Applied in BOTH modes on purpose: one discoverable bound beats a
     * response whose length depends on whether the session happens to be encrypted. */
    uint16_t max_out = (uint16_t)(OD_SESSION_PAYLOAD_MAX - 4u);
    out_len = max_out;
    if (!opendisplay_ble_nfc_read(&rec_type, &s_nfc_rsp_buf[6], &out_len, max_out)) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x02u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    s_nfc_rsp_buf[0] = 0x00u;
    s_nfc_rsp_buf[1] = RESP_NFC_ENDPOINT;
    s_nfc_rsp_buf[2] = 0x80u;
    s_nfc_rsp_buf[3] = rec_type;
    s_nfc_rsp_buf[4] = (uint8_t)((out_len >> 8) & 0xFFu);
    s_nfc_rsp_buf[5] = (uint8_t)(out_len & 0xFFu);
    (void)od_cmd_reply(ctx, s_nfc_rsp_buf, (uint16_t)(6u + out_len));
    return OD_CMD_OK;
  }

  if (payload[0] == 0x01u) {
    uint16_t text_len;
    if (payload_len < 4u) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x01u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    rec_type = payload[1];
    text_len = (uint16_t)(((uint16_t)payload[2] << 8) | payload[3]);
    if ((uint16_t)(4u + text_len) > payload_len) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x01u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    if (!nfc_rec_type_valid(rec_type)) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x05u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    if (!opendisplay_ble_nfc_write(rec_type, &payload[4], text_len)) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x03u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    {
      uint8_t ok[] = { 0x00u, RESP_NFC_ENDPOINT, 0x81u };
      (void)od_cmd_reply(ctx, ok, sizeof(ok));
    }
    return OD_CMD_OK;
  }

  if (payload[0] == 0x10u) {
    uint16_t total_len;
    if (payload_len < 4u) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x01u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    rec_type = payload[1];
    total_len = (uint16_t)(((uint16_t)payload[2] << 8) | payload[3]);
    if (!nfc_rec_type_valid(rec_type)) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x05u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    if (total_len == 0u || total_len > sizeof(s_nfc_write_chunk.data)) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x06u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    od_cmd_nfc_reset();
    s_nfc_write_chunk.active = true;
    s_nfc_write_chunk.rec_type = rec_type;
    s_nfc_write_chunk.total_len = total_len;
    {
      uint8_t ok[] = { 0x00u, RESP_NFC_ENDPOINT, 0x82u };
      (void)od_cmd_reply(ctx, ok, sizeof(ok));
    }
    return OD_CMD_OK;
  }

  if (payload[0] == 0x11u) {
    uint16_t chunk_len;
    if (!s_nfc_write_chunk.active) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x07u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    if (payload_len < 2u) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x01u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    chunk_len = (uint16_t)(payload_len - 1u);
    if ((uint16_t)(s_nfc_write_chunk.received_len + chunk_len) > s_nfc_write_chunk.total_len) {
      od_cmd_nfc_reset();
      {
        uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x08u };
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      }
      return OD_CMD_NACK;
    }
    memcpy(&s_nfc_write_chunk.data[s_nfc_write_chunk.received_len], &payload[1], chunk_len);
    s_nfc_write_chunk.received_len = (uint16_t)(s_nfc_write_chunk.received_len + chunk_len);
    {
      uint8_t ok[] = { 0x00u, RESP_NFC_ENDPOINT, 0x82u };
      (void)od_cmd_reply(ctx, ok, sizeof(ok));
    }
    return OD_CMD_OK;
  }

  if (payload[0] == 0x12u) {
    if (!s_nfc_write_chunk.active) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x07u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    if (s_nfc_write_chunk.received_len != s_nfc_write_chunk.total_len) {
      uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x09u };
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    if (!opendisplay_ble_nfc_write(s_nfc_write_chunk.rec_type, s_nfc_write_chunk.data,
                                   s_nfc_write_chunk.total_len)) {
      od_cmd_nfc_reset();
      {
        uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x03u };
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      }
      return OD_CMD_NACK;
    }
    od_cmd_nfc_reset();
    {
      uint8_t ok[] = { 0x00u, RESP_NFC_ENDPOINT, 0x81u };
      (void)od_cmd_reply(ctx, ok, sizeof(ok));
    }
    return OD_CMD_OK;
  }

  {
    /* Unknown NFC subcommand: a refusal, not an acceptance. */
    uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x04u };
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
  }
  return OD_CMD_NACK;
}
