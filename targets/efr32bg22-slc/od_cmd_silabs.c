/* BG22 implementations of every shared command hook. */

#include "od_cmd_app.h"

#include "od_config_read.h"
#include "od_dispatch.h"
#include "od_reply.h"
#include "od_session.h"
#include "od_session_app.h"
#include "od_txq.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_constants.h"
#include "opendisplay_led.h"
#include "opendisplay_protocol.h"

#include "em_device.h"

#include <stdio.h>
#include <string.h>

#ifndef OPENDISPLAY_BUILD_ID
#define OPENDISPLAY_BUILD_ID "bg22-dev"
#endif

static bool authenticated(void)
{
  return od_session_authenticated(od_session_app_state());
}

static od_txq_status_t reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  return od_reply(ctx->r, &ctx->rp, frame, len);
}

static od_txq_status_t reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  return od_reply_plain(ctx->r, &ctx->rp, frame, len);
}

static od_txq_status_t config_ack(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  return authenticated() ? reply(ctx, frame, len) : reply_plain(ctx, frame, len);
}

static od_cmd_result_t persist_config(const od_cmd_ctx_t *ctx, uint8_t response,
                                      const uint8_t *data, uint32_t len)
{
  uint8_t ok[] = { RESP_ACK, response, 0u, 0u };
  uint8_t err[] = { RESP_NACK, response, 0u, 0u };
  od_txq_status_t qrc;

  /* Persistence is the commit point. A success response must never get ahead of it. */
  if (!saveConfig((uint8_t *)(uintptr_t)data, len)) {
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  /* Seal while the old key/session is still live, then load the new config and retire it. */
  qrc = config_ack(ctx, ok, sizeof(ok));
  opendisplay_ble_reload_config_from_nvm();
  od_session_clear(od_session_app_state());
  return qrc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t rsp[46];
  const char *build = OPENDISPLAY_BUILD_ID;
  uint8_t n = (uint8_t)strlen(build);
  uint16_t version = opendisplay_ble_get_app_version();

  (void)body;
  if (n > 40u) n = 40u;
  rsp[0] = RESP_ACK;
  rsp[1] = RESP_FIRMWARE_VERSION;
  rsp[2] = (uint8_t)(version >> 8);
  rsp[3] = (uint8_t)version;
  rsp[4] = n;
  memcpy(&rsp[5], build, n);
  rsp[5u + n] = opendisplay_ble_get_app_version_patch();
  (void)reply_plain(ctx, rsp, (uint16_t)(6u + n));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t rsp[18] = { RESP_ACK, RESP_MSD_READ };
  (void)body;
  opendisplay_ble_copy_msd_bytes(&rsp[2]);
  (void)reply(ctx, rsp, sizeof(rsp));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body)
{
  (void)ctx; (void)body;
  NVIC_SystemReset();
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_ENTER_DFU };
  (void)body;
  (void)reply(ctx, ok, sizeof(ok));
  opendisplay_ble_schedule_dfu();
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t err[] = { RESP_NACK, RESP_POWER_OFF, OD_ERR_POWER_OFF_UNSUPPORTED, 0u };
  (void)body;
  (void)reply_plain(ctx, err, sizeof(err));
  return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_DEEP_SLEEP };
  (void)body;
  (void)reply(ctx, ok, sizeof(ok));
  opendisplay_ble_schedule_deep_sleep();
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t *buf = opendisplay_config_assembler()->buffer;
  uint32_t len = MAX_CONFIG_SIZE;
  od_txq_status_t rc;

  (void)body;
  if (opendisplay_config_assembler()->active) {
    (void)od_config_read_start(&ctx->rp, ctx->r, NULL, 0u);
    return OD_CMD_NACK;
  }
  if (!initConfigStorage() || !loadConfig(buf, &len)) {
    (void)od_config_read_start(&ctx->rp, ctx->r, NULL, 0u);
    return OD_CMD_NACK;
  }
  rc = od_config_read_start(&ctx->rp, ctx->r, buf, len);
  return rc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
  struct od_config_asm *s = opendisplay_config_assembler();
  enum od_config_asm_result result = od_config_asm_start(s, body);
  uint8_t ack[] = { RESP_ACK, RESP_CONFIG_WRITE, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_CONFIG_WRITE, 0u, 0u };

  switch (result) {
  case OD_CONFIG_ASM_SINGLE:
    return persist_config(ctx, RESP_CONFIG_WRITE, body.p, (uint32_t)body.n);
  case OD_CONFIG_ASM_ACCEPTED:
    (void)config_ack(ctx, ack, sizeof(ack));
    return OD_CMD_OK;
  case OD_CONFIG_ASM_COMPLETE:
    return persist_config(ctx, RESP_CONFIG_WRITE, s->buffer, s->received);
  case OD_CONFIG_ASM_REJECTED:
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body)
{
  struct od_config_asm *s = opendisplay_config_assembler();
  enum od_config_asm_result result = od_config_asm_chunk(s, body);
  uint8_t ack[] = { RESP_ACK, RESP_CONFIG_CHUNK, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_CONFIG_CHUNK, 0u, 0u };

  if (result == OD_CONFIG_ASM_ACCEPTED) {
    (void)config_ack(ctx, ack, sizeof(ack));
    return OD_CMD_OK;
  }
  if (result == OD_CONFIG_ASM_COMPLETE) {
    return persist_config(ctx, RESP_CONFIG_CHUNK, s->buffer, s->received);
  }
  (void)reply_plain(ctx, err, sizeof(err));
  return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_CONFIG_CLEAR, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_CONFIG_CLEAR, 0u, 0u };
  od_txq_status_t qrc;

  (void)body;
  if (!clearStoredConfig()) {
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  od_config_asm_reset(opendisplay_config_assembler());
  qrc = config_ack(ctx, ok, sizeof(ok));
  opendisplay_ble_reload_config_from_nvm();
  od_session_clear(od_session_app_state());
  return qrc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

bool od_cmd_mutates_config(uint16_t cmd)
{
  return cmd == CMD_CONFIG_WRITE || cmd == CMD_CONFIG_CHUNK || cmd == CMD_CONFIG_CLEAR;
}

bool od_cmd_allow_unauthenticated(uint16_t cmd)
{
  /* BG22 has never shipped the Nordic physical/key-loss rewrite policy. Do not silently create
   * an unauthenticated storage mutation merely because the wire config contains the same flag. */
  (void)cmd;
  return false;
}

od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_LED_ACTIVATE_ACK, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_LED_ACTIVATE_ACK, 2u, 0u };
  if (body.n < 1u || opendisplay_led_activate(body.p[0], body.p + 1u,
                                               (uint16_t)(body.n - 1u)) != 0) {
    if (body.n < 1u) err[2] = 1u;
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  (void)reply(ctx, ok, sizeof(ok));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_LED_STOP_ACK, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_LED_STOP_ACK, 2u, 0u };
  int rc = body.n ? opendisplay_led_stop(body.p[0], true) : opendisplay_led_stop(0u, false);
  if (rc != 0) {
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  (void)reply(ctx, ok, sizeof(ok));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t err[] = { RESP_NACK, RESP_BUZZER_ACK, OD_ERR_PARTIAL_UNSUPPORTED, 0u };
  (void)body;
  (void)reply_plain(ctx, err, sizeof(err));
  return OD_CMD_NACK;
}

static uint8_t s_nfc_data[512];
static uint16_t s_nfc_total;
static uint16_t s_nfc_received;
static uint8_t s_nfc_type;

static bool nfc_type_valid(uint8_t type)
{
  return type == OD_NFC_REC_TEXT || type == OD_NFC_REC_URI ||
         type == OD_NFC_REC_WELL_KNOWN_RAW || type == OD_NFC_REC_MIME ||
         type == OD_NFC_REC_RAW_NDEF;
}

void od_cmd_silabs_reset(void)
{
  od_config_asm_reset(opendisplay_config_assembler());
  s_nfc_total = 0u;
  s_nfc_received = 0u;
}

od_cmd_result_t od_cmd_app_nfc(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t rsp[OD_SESSION_PAYLOAD_MAX + 2u];
  uint8_t err[] = { RESP_NACK, RESP_NFC_ENDPOINT, 0xFFu, NFC_ERR_MALFORMED };
  const uint8_t *p = body.p;
  uint16_t n = (uint16_t)body.n;

  if (n < 1u) goto fail;
  if (p[0] == NFC_SUB_READ) {
    uint16_t out_len = OD_SESSION_PAYLOAD_MAX - 4u;
    uint8_t type;
    if (!opendisplay_ble_nfc_read(&type, &rsp[6], &out_len, out_len)) {
      err[3] = NFC_ERR_READ_FAILED; goto fail;
    }
    rsp[0] = RESP_ACK; rsp[1] = RESP_NFC_ENDPOINT; rsp[2] = NFC_STATUS_READ_DATA;
    rsp[3] = type; rsp[4] = (uint8_t)(out_len >> 8); rsp[5] = (uint8_t)out_len;
    (void)reply(ctx, rsp, (uint16_t)(6u + out_len)); return OD_CMD_OK;
  }
  if (p[0] == NFC_SUB_WRITE && n >= 4u) {
    uint16_t len = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    if (!nfc_type_valid(p[1])) {
      err[3] = NFC_ERR_INVALID_REC_TYPE; goto fail;
    }
    if ((uint32_t)len + 4u > n || !opendisplay_ble_nfc_write(p[1], &p[4], len)) {
      err[3] = NFC_ERR_TAG_WRITE_FAILED; goto fail;
    }
    { uint8_t ok[] = { RESP_ACK, RESP_NFC_ENDPOINT, NFC_STATUS_WRITE_COMMITTED };
      (void)reply(ctx, ok, sizeof(ok)); return OD_CMD_OK; }
  }
  if (p[0] == NFC_SUB_WRITE_START && n >= 4u) {
    uint16_t total = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    if (!nfc_type_valid(p[1])) {
      err[3] = NFC_ERR_INVALID_REC_TYPE; goto fail;
    }
    if (total == 0u || total > sizeof(s_nfc_data)) {
      err[3] = NFC_ERR_BAD_TOTAL_LEN; goto fail;
    }
    s_nfc_type = p[1]; s_nfc_total = total; s_nfc_received = 0u;
    { uint8_t ok[] = { RESP_ACK, RESP_NFC_ENDPOINT, NFC_STATUS_CHUNK_ACCEPTED };
      (void)reply(ctx, ok, sizeof(ok)); return OD_CMD_OK; }
  }
  if (p[0] == NFC_SUB_WRITE_DATA && s_nfc_total == 0u) {
    err[3] = NFC_ERR_CHUNK_NO_START; goto fail;
  }
  if (p[0] == NFC_SUB_WRITE_DATA && n >= 2u &&
      (uint32_t)s_nfc_received + n - 1u > s_nfc_total) {
    s_nfc_total = 0u; s_nfc_received = 0u;
    err[3] = NFC_ERR_CHUNK_OVERFLOW; goto fail;
  }
  if (p[0] == NFC_SUB_WRITE_DATA && n >= 2u) {
    memcpy(&s_nfc_data[s_nfc_received], &p[1], n - 1u);
    s_nfc_received = (uint16_t)(s_nfc_received + n - 1u);
    { uint8_t ok[] = { RESP_ACK, RESP_NFC_ENDPOINT, NFC_STATUS_CHUNK_ACCEPTED };
      (void)reply(ctx, ok, sizeof(ok)); return OD_CMD_OK; }
  }
  if (p[0] == NFC_SUB_WRITE_END && s_nfc_total == 0u) {
    err[3] = NFC_ERR_CHUNK_NO_START; goto fail;
  }
  if (p[0] == NFC_SUB_WRITE_END && s_nfc_received != s_nfc_total) {
    err[3] = NFC_ERR_END_LEN_MISMATCH; goto fail;
  }
  if (p[0] == NFC_SUB_WRITE_END &&
      opendisplay_ble_nfc_write(s_nfc_type, s_nfc_data, s_nfc_total)) {
    s_nfc_total = 0u;
    { uint8_t ok[] = { RESP_ACK, RESP_NFC_ENDPOINT, NFC_STATUS_WRITE_COMMITTED };
      (void)reply(ctx, ok, sizeof(ok)); return OD_CMD_OK; }
  }
  if (p[0] == NFC_SUB_WRITE_END) {
    s_nfc_total = 0u; s_nfc_received = 0u;
    err[3] = NFC_ERR_TAG_WRITE_FAILED;
  } else if (p[0] != NFC_SUB_READ && p[0] != NFC_SUB_WRITE &&
             p[0] != NFC_SUB_WRITE_START && p[0] != NFC_SUB_WRITE_DATA) {
    err[3] = NFC_ERR_UNKNOWN_SUBCMD;
  }
fail:
  (void)reply_plain(ctx, err, sizeof(err));
  return OD_CMD_NACK;
}
