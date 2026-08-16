#include "opendisplay_pipe.h"
#include "od_log.h"
#include "opendisplay_ble.h"
#include "opendisplay_display.h"
#include "opendisplay_led.h"
#include "opendisplay_buzzer.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_constants.h"
#include "opendisplay_protocol.h"
#include "opendisplay_config_parser.h"
#include "od_runtime_types.h"
#include "od_session.h"
#include "od_span.h"
#include "opendisplay_pipe_write.h"
#include "od_hal_crypto.h"
#include <stdio.h>
#include <string.h>

#include "od_cmd_reply.h"
#include "od_dispatch.h"
#include "od_rxq.h"
#include "opendisplay_pipe_internal.h"
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>

#ifndef SHA
#define SHA ""
#endif
#define OD_STRINGIFY(x) #x
#define OD_XSTRINGIFY(x) OD_STRINGIFY(x)
#define SHA_STRING OD_XSTRINGIFY(SHA)

#define FIRMWARE_SHA_HEX_BYTES 40
static const char kFirmwareShaPlaceholder[FIRMWARE_SHA_HEX_BYTES + 1] =
    "0000000000000000000000000000000000000000";

/* SHA may be -DSHA=abc or CMake SHA=\"abc\"; XSTRINGIFY covers both and may
 * leave surrounding quotes. Empty / missing SHA → 40 zero hex chars (Firmware). */
static const char *fw_sha_string(void)
{
  static char sha_buf[FIRMWARE_SHA_HEX_BYTES + 1];
  const char *sha = SHA_STRING;
  size_t len;

  if (sha[0] == '"') {
    sha++;
  }
  len = strlen(sha);
  if (len > 0u && sha[len - 1u] == '"') {
    len--;
  }
  if (len == 0u) {
    return kFirmwareShaPlaceholder;
  }
  if (len > FIRMWARE_SHA_HEX_BYTES) {
    len = FIRMWARE_SHA_HEX_BYTES;
  }
  memcpy(sha_buf, sha, len);
  sha_buf[len] = '\0';
  return sha_buf;
}

typedef struct {
  bool active;
  uint32_t total_size;
  uint16_t expected_chunks;
  uint16_t received_chunks;
  uint32_t received_size;
  uint8_t buffer[MAX_CONFIG_SIZE];
  /* Vestigial on a single-connection target: always 0, so every guard against it is a tautology.
   * Kept rather than deleted here because removing a guard is a C11 shrink decision, not something
   * a reply-classification pass should smuggle in. */
  uint8_t connection;
} od_chunked_config_t;

static bool s_notify;
static od_chunked_config_t s_cfg_chunk;
static uint8_t s_cfg_read_buf[MAX_RESPONSE_DATA_SIZE];
/* The uptime clock, defined below with the other Zephyr shims. */
static uint32_t od_now_ms(void);

/* ============================================================================================
 * THE SESSION ADAPTER. The handshake, KDF, replay window and CCM envelope are
 * shared/core/od_session.c; what stays here is this target's half -- the clock, the nRF device
 * identity, and the Zephyr logging. od_session sends nothing itself, which is why
 * authenticate_handle still returns its reply for dispatch() to send.
 * ============================================================================================ */

static struct od_session s_session;

/* Budgets for the nonce-rejection logs. Nonce failures deliberately do not count toward
 * integrity_failures, so nothing else throttles a peer that drives these lines, and
 * out-of-window fires routinely on a lossy link.
 *
 * One budget PER SITE, not one shared: a stale client spamming session-id mismatches must not
 * be able to silence the out-of-window line, which is the one that reports real transfer loss. */
static uint32_t s_nonce_log_window_ms;   /* replay / out-of-window */
static uint32_t s_nonce_log_other_ms;    /* wrong session, bad tag, malformed, engine fault */

static bool nonce_log_allowed(uint32_t *last_ms)
{
  const uint32_t now = od_now_ms();

  if (*last_ms != 0u && (uint32_t)(now - *last_ms) < 5000u) {
    return false;
  }
  *last_ms = now;
  return true;
}

static void clear_session(void)
{
  od_session_clear(&s_session);
}

/* Mutating by design: an expired session is torn down by the act of asking, which is what every
 * call site here already relied on. */
static bool session_alive(void)
{
  return od_session_alive(&s_session, od_now_ms(), NULL);
}

/* The four device-id bytes that feed both the KDF and the auth proof: the low 32 bits of the
 * 64-bit hwinfo id, big-endian. Wire-visible identity -- a different packing is a different
 * device to the host. */
static void od_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN]);
static void pipe_send(const od_cmd_ctx_t *ctx, const uint8_t *data, uint16_t len);

/* Bumped on every close. A frame carries the generation that produced it, so both queues can
 * discard work belonging to a connection that has gone. */
static atomic_t s_conn_gen;

struct od_session *od_pipe_session(void)
{
  return &s_session;
}

uint32_t od_pipe_conn_gen(void)
{
  return (uint32_t)atomic_get(&s_conn_gen);
}

void od_pipe_legacy_send(const uint8_t *data, uint16_t len)
{
  /* NULL context, deliberately and safely: pipe_send() and pipe_send_raw() both (void) it -- the
   * legacy sender routes by inference, not by context. Spelled NULL rather than 0 so it cannot be
   * mistaken for the connection number this parameter used to be, which is how a null context
   * reached on_pipe_write() unnoticed and would have silenced every reply. */
  pipe_send(NULL, data, len);
}

void od_pipe_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
  od_device_id(out);
}

static void od_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
  uint8_t id[8];
  uint64_t uid = 0;
  unsigned i;

  (void)hwinfo_get_device_id(id, sizeof(id));
  for (i = 0; i < sizeof(id); i++) {
    uid = (uid << 8) | id[i];
  }
  out[0] = (uint8_t)((uid >> 24) & 0xFFu);
  out[1] = (uint8_t)((uid >> 16) & 0xFFu);
  out[2] = (uint8_t)((uid >> 8) & 0xFFu);
  out[3] = (uint8_t)(uid & 0xFFu);
}

static bool authenticate_handle(const uint8_t *payload, uint16_t payload_len,
                                uint8_t *rsp, uint16_t *rsp_len)
{
  uint8_t device_id[OD_SESSION_DEVICE_ID_LEN];
  struct od_session_report report;
  enum od_session_auth r;

  od_device_id(device_id);
  r = od_session_authenticate(&s_session, od_get_parsed_security(), device_id,
                              od_span_make(payload, payload_len), od_now_ms(),
                              rsp, OD_SESSION_REPLY_MAX, rsp_len, &report);
  switch (r) {
  case OD_SESSION_AUTH_CHALLENGE:
    od_log_info("auth: challenge sent");
    break;
  case OD_SESSION_AUTH_ESTABLISHED:
    od_log_info("auth: session established");
    break;
  case OD_SESSION_AUTH_RATE_LIMITED:
    od_log_warn("auth: rate limited (%u attempts in window)", (unsigned)report.attempts);
    break;
  case OD_SESSION_AUTH_CRYPTO_ERROR:
    od_log_error("auth: crypto engine failure (status %d)", (int)report.crypto_status);
    break;
  default:
    od_log_warn("auth: refused (rc=%d, status 0x%02X)", (int)r, (unsigned)report.status_byte);
    break;
  }
  return r == OD_SESSION_AUTH_ESTABLISHED;
}
static uint8_t s_long_write_buf[OD_PIPE_MAX_PAYLOAD];
static uint16_t s_long_write_len;
static uint8_t s_long_write_conn = 0xFFu;
static uint8_t s_plain_buf[512];
static uint8_t s_pipe_enc_buf[544];
static uint8_t s_nfc_rsp_buf[OD_PIPE_MAX_PAYLOAD];
typedef struct {
  bool active;
  /* Vestigial on a single-connection target: always 0, so every guard against it is a tautology.
   * Kept rather than deleted here because removing a guard is a C11 shrink decision, not something
   * a reply-classification pass should smuggle in. */
  uint8_t connection;
  uint8_t rec_type;
  uint16_t total_len;
  uint16_t received_len;
  /* Match OD_NFC write staging (long MIME / vCard). */
  uint8_t data[512];
} od_nfc_write_chunk_t;
static od_nfc_write_chunk_t s_nfc_write_chunk;

/*
 * GATT writes arrive on the BT RX thread. Commands (EPD init/refresh waits,
 * CCM crypto) must not run there: they starve ATT and break service discovery
 * on reconnect. Writes are queued here and drained on the main thread.
 */
/* Inbound frames live in shared/core/od_rxq.c now -- one ring, one SPSC contract, one arrival log
 * for both targets. The 509-byte slots this replaces were sized for an ATT MTU of 512 that nothing
 * used: the dispatcher refuses anything over OD_PIPE_MAX_PAYLOAD (244), so 40 x 513 B held ~20 KB
 * to buffer frames it would then reject. */
static atomic_t s_close_pending;

#ifndef OD_ALLOW_PLAINTEXT_WITH_SECURITY
#define OD_ALLOW_PLAINTEXT_WITH_SECURITY 0
#endif

static void pipe_send(const od_cmd_ctx_t *ctx, const uint8_t *data, uint16_t len);

static void cfg_chunk_reset(void)
{
  memset(&s_cfg_chunk, 0, sizeof(s_cfg_chunk));
}

static void nfc_write_chunk_reset(void)
{
  memset(&s_nfc_write_chunk, 0, sizeof(s_nfc_write_chunk));
  s_nfc_write_chunk.connection = 0xFFu;
}

static bool nfc_rec_type_valid(uint8_t rec_type)
{
  return rec_type == OD_NFC_REC_TEXT || rec_type == OD_NFC_REC_URI || rec_type == OD_NFC_REC_WELL_KNOWN_RAW
         || rec_type == OD_NFC_REC_MIME || rec_type == OD_NFC_REC_RAW_NDEF;
}

static uint32_t od_now_ms(void)
{
  return k_uptime_get_32();
}

#define OD_CRYPTO_SESSION_SLOT ((od_hal_crypto_slot_t)0u)


/*
 * Count a replay/decrypt failure and tear the session down after 3 strikes.
 * Mirrors the nRF52840 Firmware (encryption.cpp:640-646, 678-683): every
 * failed nonce-replay check or CCM tag verification increments the counter,
 * and reaching 3 clears the session so a forced re-auth is required.
 */

static bool sec_enabled(void)
{
  return od_session_security_enabled(od_get_parsed_security());
}




/*
 * Constant-time equality, for comparing a MAC against an attacker-supplied value.
 * memcmp() returns as soon as two bytes differ, so its runtime reveals how many leading
 * bytes matched -- enough to forge a 16-byte tag one byte at a time. This always reads all
 * len bytes and folds them into a single accumulator. Matches the reference's
 * constantTimeCompare() (Firmware/src/encryption.cpp).
 */

static void send_auth_required_response(const od_cmd_ctx_t *ctx, uint8_t resp_byte)
{
  uint8_t err[] = { 0x00u, resp_byte, RESP_AUTH_REQUIRED };
  (void)od_cmd_reply_plain(ctx, err, sizeof(err));
}




static void pipe_send_raw(const od_cmd_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
  (void)ctx;
  if (!s_notify || len == 0u) {
    return;
  }
  /*
   * bt_gatt_notify() returns -ENOMEM (notify → false) when the TX buffer pool is
   * momentarily exhausted; it never blocks. A single response almost always
   * succeeds on the first try. A multi-chunk config read (now up to
   * (MAX_CONFIG_SIZE+93)/94 = 44 notifications back-to-back) can outrun the pool,
   * so retry with a short yield while the link is still up — buffers free as the
   * BT RX thread processes completions. This keeps the pool small (prj.conf) yet
   * lets large reads through without dropping chunks. Bail if notifications are
   * no longer enabled (disconnected), so a dead link cannot spin here.
   */
  for (int attempt = 0; attempt < 200; attempt++) {
    if (opendisplay_ble_pipe_notify(data, len)) {
      return;
    }
    if (!opendisplay_ble_pipe_notify_enabled()) {
      break;
    }
    k_msleep(1);
  }
  od_log_info("pipe notify failed len=%u", (unsigned)len);
}

static void pipe_send(const od_cmd_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
  uint8_t err[3];
  uint16_t enc_len = 0;
  uint8_t status;
  uint8_t cmd;
  bool force_plain;

  if (len == 0u) {
    return;
  }
  if (len < 2u) {
    pipe_send_raw(ctx, data, len);
    return;
  }
  cmd = data[1];
  /* THE STATUS IS BYTE 2, not byte 0. Every frame this file builds leads with 0x00 for an ACK or
   * 0xFF for a hard error, and puts the FE/FF outcome in byte 2 -- {0x00, cmd, 0xFF} for a
   * rejected decrypt, {0x00, cmd, 0xFE} for auth-required. Reading byte 0 meant neither test
   * could ever fire for those two, so a rejected command was SEALED, and py-opendisplay decrypts
   * any frame of 31 bytes or more and returns immediately (device.py:830), never reaching its
   * raw[2] == 0xFF guard. The 2-byte echo then validates as an ACK and the host reports success
   * for a command the device refused. Error frames are plaintext by contract
   * (py-opendisplay protocol/responses.py) -- this is the same rule esp32-idf applies at
   * communication.cpp:412.
   *
   * The 7-byte PIPE data ACK {0x00,0x81,highest_seen,mask:4} carries a rolling seq at byte 2, so
   * a highest_seen of 0xFE/0xFF on any image of 255+ chunks must not be mistaken for a status;
   * pipe ACKs encrypt normally. Other pipe shapes cannot collide: 0x80's byte 2 is a version,
   * a pipe NACK's is an error code 0x01-0x04, and 0x82 acks are two bytes. */
  {
    const bool pipe_data_ack = (len == 7u && data[0] == 0x00u && data[1] == 0x81u);
    status = (len >= 3u && !pipe_data_ack) ? data[2] : 0x00u;
  }
  force_plain = (status == RESP_AUTH_REQUIRED || status == RESP_NACK
                 || data[0] == 0xFFu           /* 4-byte hard-error frames lead with 0xFF */
                 || cmd == RESP_AUTHENTICATE || cmd == RESP_FIRMWARE_VERSION);
  if (force_plain || !session_alive()) {
    pipe_send_raw(ctx, data, len);
    return;
  }
  /* od_session_seal takes the complete [status][cmd][payload] frame: those two leading bytes are
   * the AAD as well as the echoed prefix, so they travel with the payload rather than separately. */
  if (od_session_seal(&s_session, od_span_make(data, len), s_pipe_enc_buf,
                      sizeof(s_pipe_enc_buf), &enc_len, od_now_ms(), NULL)
      == OD_SESSION_SEAL_OK) {
    pipe_send_raw(ctx, s_pipe_enc_buf, enc_len);
    return;
  }
  err[0] = 0x00u;
  err[1] = cmd;
  err[2] = 0xFFu;
  pipe_send_raw(ctx, err, sizeof(err));
}

static od_cmd_result_t reply_firmware_version(const od_cmd_ctx_t *ctx)
{
  /* [ACK][0x43][major][minor][shaLen][sha…][patch] — patch trails so old
   * hosts that stop after SHA keep working. */
  uint8_t rsp[2 + 1 + 1 + 1 + 40 + 1];
  uint16_t ver = opendisplay_ble_get_app_version();
  uint8_t major = (uint8_t)((ver >> 8) & 0xFFu);
  uint8_t minor = (uint8_t)(ver & 0xFFu);
  uint8_t patch = opendisplay_ble_get_app_version_patch();
  const char *sha = fw_sha_string();
  uint8_t sha_len = (uint8_t)strlen(sha);
  uint16_t o = 0;

  if (sha_len > 40u) {
    sha_len = 40u;
  }
  rsp[o++] = 0x00u;
  rsp[o++] = RESP_FIRMWARE_VERSION;
  rsp[o++] = major;
  rsp[o++] = minor;
  rsp[o++] = sha_len;
  memcpy(&rsp[o], sha, sha_len);
  o += sha_len;
  rsp[o++] = patch;
  (void)od_cmd_reply_plain(ctx, rsp, o);
  return OD_CMD_OK;
}

static od_cmd_result_t reply_read_msd(const od_cmd_ctx_t *ctx)
{
  uint8_t rsp[2 + 16];

  rsp[0] = 0x00u;
  rsp[1] = RESP_MSD_READ;
  opendisplay_ble_copy_msd_bytes(&rsp[2]);
  (void)od_cmd_reply(ctx, rsp, sizeof(rsp));
  return OD_CMD_OK;
}


static od_cmd_result_t handle_partial_write_start(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len)
{
  uint8_t ok[] = { 0x00u, 0x76u };
  uint8_t err[] = { 0xFFu, 0x76u, OD_ERR_PARTIAL_STREAM, 0x00u };
  uint8_t err_code = OD_ERR_PARTIAL_STREAM;

  opendisplay_pipe_write_reset();
  if (opendisplay_display_partial_write_start(payload, payload_len, &err_code) == 0) {
    (void)od_cmd_reply(ctx, ok, sizeof(ok));
  } else {
    err[2] = err_code;
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  return OD_CMD_OK;
}

static od_cmd_result_t handle_direct_write_start(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len)
{
  uint8_t ok[] = { 0x00u, 0x70u };
  uint8_t err[] = { 0xFFu, 0x70u };
  od_log_info("pipe 0070 recv len=%u (epd init next)", (unsigned)payload_len);
  opendisplay_pipe_write_reset();
  if (opendisplay_display_direct_write_start(payload, payload_len) == 0) {
    (void)od_cmd_reply(ctx, ok, sizeof(ok));
  } else {
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  return OD_CMD_OK;
}

static od_cmd_result_t handle_direct_write_data(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len)
{
  uint8_t ack_data[] = { 0x00u, 0x71u };
  uint8_t err[] = { 0xFFu, 0x71u };
  uint8_t partial_err[] = { 0xFFu, 0x71u, OD_ERR_PARTIAL_STREAM, 0x00u };
  int rc;

  rc = opendisplay_display_direct_write_data(payload, payload_len);
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

static od_cmd_result_t handle_direct_write_end(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len)
{
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
    if (rc != -4) {
      opendisplay_display_abort();
    }
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  /* Ack 0x72 before the blocking refresh, then report 0x73/0x74 afterwards
   * (same ordering as the nRF52840 Firmware). */
  (void)od_cmd_reply(ctx, ack_end, sizeof(ack_end));
  k_msleep(20);
  if (opendisplay_display_direct_write_end_refresh(payload, payload_len, &refresh_ok) != 0) {
    opendisplay_display_abort();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  (void)od_cmd_reply(ctx, refresh_ok ? ack_refresh_ok : ack_refresh_timeout,
            refresh_ok ? sizeof(ack_refresh_ok) : sizeof(ack_refresh_timeout));
  return OD_CMD_OK;
}

static od_cmd_result_t handle_config_clear(const od_cmd_ctx_t *ctx)
{
  uint8_t ok[] = { 0x00u, RESP_CONFIG_CLEAR, 0x00u, 0x00u };
  uint8_t err[] = { 0xFFu, RESP_CONFIG_CLEAR, 0x00u, 0x00u };

  if (!clearStoredConfig()) {
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  opendisplay_ble_reload_config_from_nvm();
  (void)od_cmd_reply(ctx, ok, sizeof(ok));
  return OD_CMD_OK;
}

static od_cmd_result_t handle_config_read(const od_cmd_ctx_t *ctx)
{
  static uint8_t config_data[MAX_CONFIG_SIZE];
  uint32_t config_len = MAX_CONFIG_SIZE;
  /* Derive the chunk cap from MAX_CONFIG_SIZE like the reference firmware
   * (communication.cpp: (MAX_CONFIG_SIZE + 93) / 94). Each chunk carries at
   * least 94 config bytes (chunk 0: 100-byte response - 2 status - 2 chunk# - 2
   * total-len; later chunks carry 96), so 94 is the conservative per-chunk rate.
   * The old hardcoded 10 truncated any read past ~940 bytes. Flow control in
   * pipe_send_raw keeps the larger burst from dropping chunks on a full TX pool. */
  const uint16_t max_chunks = (uint16_t)((MAX_CONFIG_SIZE + 93) / 94);

  if (!initConfigStorage()) {
    uint8_t err[] = { 0xFFu, RESP_CONFIG_READ, 0x00u, 0x00u };
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }

  if (!loadConfig(config_data, &config_len)) {
    uint8_t empty[] = {
      0x00u, RESP_CONFIG_READ, 0x00u, 0x00u, 0x00u, 0x00u,
    };
    (void)od_cmd_reply(ctx, empty, sizeof(empty));
    return OD_CMD_OK;
  }

  uint32_t remaining = config_len;
  uint32_t offset = 0;
  uint16_t chunk_number = 0;

  while (remaining > 0 && chunk_number < max_chunks) {
    uint16_t response_len = 0;
    uint16_t chunk_size;

    s_cfg_read_buf[response_len++] = 0x00u;
    s_cfg_read_buf[response_len++] = RESP_CONFIG_READ;
    s_cfg_read_buf[response_len++] = (uint8_t)(chunk_number & 0xFFu);
    s_cfg_read_buf[response_len++] = (uint8_t)((chunk_number >> 8) & 0xFFu);

    if (chunk_number == 0u) {
      s_cfg_read_buf[response_len++] = (uint8_t)(config_len & 0xFFu);
      s_cfg_read_buf[response_len++] = (uint8_t)((config_len >> 8) & 0xFFu);
    }

    {
      uint16_t max_data = (uint16_t)(MAX_RESPONSE_DATA_SIZE - response_len);
      chunk_size = (remaining < max_data) ? (uint16_t)remaining : max_data;
    }

    if (chunk_size == 0u) {
      break;
    }

    memcpy(s_cfg_read_buf + response_len, config_data + offset, chunk_size);
    response_len += chunk_size;

    if (response_len > MAX_RESPONSE_DATA_SIZE) {
      break;
    }

    (void)od_cmd_reply(ctx, s_cfg_read_buf, response_len);
    offset += chunk_size;
    remaining -= chunk_size;
    chunk_number++;
  }
  return OD_CMD_OK;
}

static od_cmd_result_t handle_config_write(const od_cmd_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
  od_cmd_result_t rc = OD_CMD_NACK;
  uint8_t ack[] = { 0x00u, RESP_CONFIG_WRITE, 0x00u, 0x00u };
  uint8_t err[] = { 0xFFu, RESP_CONFIG_WRITE, 0x00u, 0x00u };

  if (len == 0u) {
    /* Answers NOTHING, which is the shipped behaviour and is preserved here. It is still a
     * REFUSAL: the frame was malformed and no config was written, so it must not be reported as
     * an accepted command. */
    return OD_CMD_NACK;
  }

  if (len > CONFIG_CHUNK_SIZE) {
    cfg_chunk_reset();
    s_cfg_chunk.active = true;
    s_cfg_chunk.connection = 0u;
    s_cfg_chunk.received_chunks = 1;

    if (len >= CONFIG_CHUNK_SIZE_WITH_PREFIX) {
      s_cfg_chunk.total_size = (uint32_t)data[0] | ((uint32_t)data[1] << 8);
      if (s_cfg_chunk.total_size > MAX_CONFIG_SIZE || s_cfg_chunk.total_size == 0u) {
        cfg_chunk_reset();
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
        return OD_CMD_NACK;
      }
      {
        uint16_t chunk_data_size = (uint16_t)(len - 2u);
        if (chunk_data_size > CONFIG_CHUNK_SIZE) {
          chunk_data_size = CONFIG_CHUNK_SIZE;
        }
        if ((uint32_t)chunk_data_size > s_cfg_chunk.total_size) {
          chunk_data_size = (uint16_t)s_cfg_chunk.total_size;
        }
        memcpy(s_cfg_chunk.buffer, data + 2, chunk_data_size);
        s_cfg_chunk.received_size = chunk_data_size;
      }
    } else {
      s_cfg_chunk.total_size = len;
      if (s_cfg_chunk.total_size > MAX_CONFIG_SIZE) {
        cfg_chunk_reset();
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
        return OD_CMD_NACK;
      }
      {
        uint16_t chunk_size = (len < CONFIG_CHUNK_SIZE) ? len : CONFIG_CHUNK_SIZE;
        memcpy(s_cfg_chunk.buffer, data, chunk_size);
        s_cfg_chunk.received_size = chunk_size;
      }
    }

    if (s_cfg_chunk.received_size >= s_cfg_chunk.total_size) {
      if (saveConfig(s_cfg_chunk.buffer, s_cfg_chunk.received_size)) {
        /* THE ACK IS SEALED AND QUEUED BEFORE THE RELOAD, and the order is load-bearing.
         * clear_session() drops the session -- the new config may carry a new key -- so a reply
         * attempted after it finds none, and od_reply substitutes a plaintext hard NACK: the host
         * is told a write FAILED that has already been persisted, and cannot tell that from a real
         * failure. od_reply seals at commit, so queueing first makes the bytes final while the
         * session that sent the write is still the one answering it. */
        (void)od_cmd_reply(ctx, ack, sizeof(ack));
        opendisplay_ble_reload_config_from_nvm();
        clear_session();
        rc = OD_CMD_OK;
      } else {
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
        rc = OD_CMD_NACK;
      }
      cfg_chunk_reset();
      /* The verdict is CARRIED, not re-derived: both branches answer, and which one ran is the
       * only thing that distinguishes an accepted write from a refused one by the time control
       * reaches here. */
      return rc;
    }

    {
      uint32_t rem = s_cfg_chunk.total_size - s_cfg_chunk.received_size;
      s_cfg_chunk.expected_chunks =
        (uint16_t)(1u + (rem + CONFIG_CHUNK_SIZE - 1u) / CONFIG_CHUNK_SIZE);
    }

    (void)od_cmd_reply(ctx, ack, sizeof(ack));
    return OD_CMD_OK;
  }

  if (saveConfig((uint8_t *)(void *)data, len)) {
        /* THE ACK IS SEALED AND QUEUED BEFORE THE RELOAD, and the order is load-bearing.
         * clear_session() drops the session -- the new config may carry a new key -- so a reply
         * attempted after it finds none, and od_reply substitutes a plaintext hard NACK: the host
         * is told a write FAILED that has already been persisted, and cannot tell that from a real
         * failure. od_reply seals at commit, so queueing first makes the bytes final while the
         * session that sent the write is still the one answering it. */
    (void)od_cmd_reply(ctx, ack, sizeof(ack));
    opendisplay_ble_reload_config_from_nvm();
    clear_session();
  } else {
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  return OD_CMD_OK;
}

static od_cmd_result_t handle_config_chunk(const od_cmd_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
  od_cmd_result_t rc = OD_CMD_NACK;
  uint8_t ack[] = { 0x00u, RESP_CONFIG_CHUNK, 0x00u, 0x00u };
  uint8_t err[] = { 0xFFu, RESP_CONFIG_CHUNK, 0x00u, 0x00u };

  if (!s_cfg_chunk.active || s_cfg_chunk.connection != 0u) {
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  if (len == 0u) {
    /* Silent, as shipped -- but a refusal, not an acceptance. */
    return OD_CMD_NACK;
  }
  if (len > CONFIG_CHUNK_SIZE) {
    cfg_chunk_reset();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  if (s_cfg_chunk.received_size + len > s_cfg_chunk.total_size) {
    cfg_chunk_reset();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  if (s_cfg_chunk.received_size + len > MAX_CONFIG_SIZE) {
    cfg_chunk_reset();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  if (s_cfg_chunk.received_chunks >= MAX_CONFIG_CHUNKS) {
    cfg_chunk_reset();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }

  memcpy(s_cfg_chunk.buffer + s_cfg_chunk.received_size, data, len);
  s_cfg_chunk.received_size += len;
  s_cfg_chunk.received_chunks++;

  if (s_cfg_chunk.received_chunks >= s_cfg_chunk.expected_chunks) {
    if (s_cfg_chunk.received_size != s_cfg_chunk.total_size) {
      cfg_chunk_reset();
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    if (saveConfig(s_cfg_chunk.buffer, s_cfg_chunk.received_size)) {
        /* THE ACK IS SEALED AND QUEUED BEFORE THE RELOAD, and the order is load-bearing.
         * clear_session() drops the session -- the new config may carry a new key -- so a reply
         * attempted after it finds none, and od_reply substitutes a plaintext hard NACK: the host
         * is told a write FAILED that has already been persisted, and cannot tell that from a real
         * failure. od_reply seals at commit, so queueing first makes the bytes final while the
         * session that sent the write is still the one answering it. */
      (void)od_cmd_reply(ctx, ack, sizeof(ack));
      opendisplay_ble_reload_config_from_nvm();
      clear_session();
      rc = OD_CMD_OK;
    } else {
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      rc = OD_CMD_NACK;
    }
    cfg_chunk_reset();
    return rc;
  }
  /* An intermediate chunk: accepted, and the transfer continues. */
  (void)od_cmd_reply(ctx, ack, sizeof(ack));
  return OD_CMD_OK;
}

static od_cmd_result_t handle_nfc_endpoint(const od_cmd_ctx_t *ctx, const uint8_t *payload, uint16_t payload_len)
{
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
    nfc_write_chunk_reset();
    s_nfc_write_chunk.active = true;
    s_nfc_write_chunk.connection = 0u;
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
    if (!s_nfc_write_chunk.active || s_nfc_write_chunk.connection != 0u) {
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
      nfc_write_chunk_reset();
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
    if (!s_nfc_write_chunk.active || s_nfc_write_chunk.connection != 0u) {
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
      nfc_write_chunk_reset();
      {
        uint8_t err[] = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, 0x03u };
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      }
      return OD_CMD_NACK;
    }
    nfc_write_chunk_reset();
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

static od_cmd_result_t dispatch(const od_cmd_ctx_t *ctx, uint16_t cmd,
                                const uint8_t *payload, uint16_t payload_len)
{
  uint8_t auth_rsp[32];
  uint16_t auth_rsp_len = 0;
  if (cmd == CMD_AUTHENTICATE) {
    (void)authenticate_handle(payload, payload_len, auth_rsp, &auth_rsp_len);
    (void)od_cmd_reply_plain(ctx, auth_rsp, auth_rsp_len);
    /* The handshake is answered, whatever its outcome -- the reply carries the status byte. At the
     * cutover od_gate_authenticate() owns this and reports AUTH_CONTROL vs AUTH_ESTABLISHED, which
     * the abuse policy needs and this gate cannot express. */
    return OD_CMD_OK;
  }
  if (sec_enabled() && !session_alive()) {
#if !OD_ALLOW_PLAINTEXT_WITH_SECURITY
    /* Same policy as the nRF52840 Firmware: firmware version is always
     * readable; config write/chunk pass through unauthenticated only when the
     * rewrite flag (SecurityConfig flags bit0) is set, after erasing the
     * stored config so the old key cannot be recovered. */
    const struct SecurityConfig *sec = od_get_parsed_security();
    bool rewrite_allowed = (sec != NULL) && ((sec->flags & 0x01u) != 0u);
    bool config_rewrite = rewrite_allowed
                          && (cmd == CMD_CONFIG_WRITE || cmd == CMD_CONFIG_CHUNK);
    if (cmd != CMD_FIRMWARE_VERSION && !config_rewrite) {
      send_auth_required_response(ctx, (uint8_t)(cmd & 0xFFu));
      return OD_CMD_AUTH_REJECTED;
    }
    /* Erase the stored config (and thus the old key) before accepting an
     * unauthenticated rewrite, on BOTH the single-shot and chunked paths.
     * The reference secure-erases in handleWriteConfig (communication.cpp:372)
     * and again on the first chunk of handleWriteConfigChunk (:435). A chunked
     * write starts as CMD_CONFIG_WRITE (erased here) and continues as
     * CMD_CONFIG_CHUNK; erase on that first chunk too so a session that
     * expires mid-transfer cannot land a new config over a retained key. */
    if (cmd == CMD_CONFIG_WRITE) {
      (void)clearStoredConfig();
    } else if (cmd == CMD_CONFIG_CHUNK && s_cfg_chunk.active &&
               s_cfg_chunk.received_chunks == 1u) {
      (void)clearStoredConfig();
    }
#endif
  }
  return od_cmd_dispatch(ctx, cmd, od_span_make(payload, payload_len));
}

/* IMPLEMENTED FOR shared/core/od_dispatch.h. One seam rather than one function per opcode: the
 * dispatch plan names a per-opcode split as C11's shrink, and doing it here would rewrite every
 * handler signature at the same moment the ordering changes underneath them.
 *
 * The opcode-to-handler mapping and its comments are the shipped ones. What changes is that the
 * caller now asks for a VERDICT instead of inferring acceptance from a flag set behind it. */
od_cmd_result_t od_cmd_dispatch(const od_cmd_ctx_t *ctx, uint16_t cmd, od_span_t body)
{
  const uint8_t *payload = body.p;
  const uint16_t payload_len = (uint16_t)body.n;

  switch (cmd) {
    case CMD_FIRMWARE_VERSION:
      return reply_firmware_version(ctx);
    case CMD_READ_MSD:
      return reply_read_msd(ctx);
    case CMD_CONFIG_READ:
      return handle_config_read(ctx);
    case CMD_CONFIG_CLEAR:
      return handle_config_clear(ctx);
    case CMD_CONFIG_WRITE:
      return handle_config_write(ctx, payload, payload_len);
    case CMD_CONFIG_CHUNK:
      return handle_config_chunk(ctx, payload, payload_len);
    case CMD_REBOOT:
      od_log_info("reboot");
      for (volatile uint32_t i = 0; i < 800000u; i++) {
      }
      NVIC_SystemReset();
      return OD_CMD_OK;                 /* not reached: the reset does not return */
    case CMD_ENTER_DFU:
      {
        uint8_t ok[] = { 0x00u, RESP_ENTER_DFU };
        (void)od_cmd_reply(ctx, ok, sizeof(ok));
      }
      opendisplay_ble_schedule_dfu();
      return OD_CMD_OK;
    case CMD_DEEP_SLEEP:
      /* Match the reference nRF52840 build (device_control.cpp:691-705): the
       * command is recognized and logged but NO response is sent, so clients do
       * not treat deep sleep as supported on this target. (Composes with the
       * separate DFU-honesty question for 0x0051, handled elsewhere.)
       *
       * OPCODE CHANGED 0x0052 -> 0x0053 when this target adopted the canonical
       * protocol header. The subset header it used to carry still had the value
       * from before the split that made 0x0052 CMD_POWER_OFF -- a hard rail-cut --
       * and left deep sleep on 0x0053, which is what Firmware and Firmware_Silabs
       * already answer. 0x0052 now falls through to the unknown-opcode path here,
       * which is the safe direction: a host that has not moved gets no deep sleep
       * rather than an unintended power-off.
       *
       * py-opendisplay still sends 0x0052 (protocol/commands.py DEEP_SLEEP), so
       * deep sleep from that client stops working against this target until the
       * host library is updated. That is the known cost of the alignment, and it
       * MUST be fixed before anything implements CMD_POWER_OFF on 0x0052 here --
       * at that point an un-updated host asking for deep sleep would cut the rail. */
      opendisplay_ble_schedule_deep_sleep();
      return OD_CMD_OK;
    case CMD_LED_ACTIVATE: {
      uint8_t ok[] = { 0x00u, RESP_LED_ACTIVATE_ACK, 0x00u, 0x00u };
      uint8_t e1[] = { 0xFFu, RESP_LED_ACTIVATE_ACK, 0x01u, 0x00u };
      uint8_t e2[] = { 0xFFu, RESP_LED_ACTIVATE_ACK, 0x02u, 0x00u };

      if (payload_len < 1u) {
        (void)od_cmd_reply_plain(ctx, e1, sizeof(e1));
        return OD_CMD_NACK;
      }
      if (opendisplay_led_activate(payload[0], payload + 1u,
                                  (uint16_t)(payload_len - 1u)) != 0) {
        (void)od_cmd_reply_plain(ctx, e2, sizeof(e2));
        return OD_CMD_NACK;
      }
      (void)od_cmd_reply(ctx, ok, sizeof(ok));
      return OD_CMD_OK;
    }
    case CMD_LED_STOP: {
      uint8_t ok[] = { 0x00u, RESP_LED_STOP_ACK, 0x00u, 0x00u };
      uint8_t e2[] = { 0xFFu, RESP_LED_STOP_ACK, 0x02u, 0x00u };
      int rc;

      if (payload_len >= 1u) {
        rc = opendisplay_led_stop(payload[0], true);
      } else {
        rc = opendisplay_led_stop(0, false);
      }
      if (rc != 0) {
        (void)od_cmd_reply_plain(ctx, e2, sizeof(e2));
        return OD_CMD_NACK;
      }
      (void)od_cmd_reply(ctx, ok, sizeof(ok));
      return OD_CMD_OK;
    }
    case CMD_BUZZER: {
      int rc = opendisplay_buzzer_activate(payload, payload_len);

      if (rc == 0) {
        uint8_t ok[] = { 0x00u, RESP_BUZZER_ACK, 0x00u, 0x00u };
        (void)od_cmd_reply(ctx, ok, sizeof(ok));
        return OD_CMD_OK;
      }
      {
        uint8_t err[] = { 0xFFu, RESP_BUZZER_ACK, (uint8_t)rc, 0x00u };
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      }
      return OD_CMD_NACK;
    }
    case CMD_NFC_ENDPOINT:
      return handle_nfc_endpoint(ctx, payload, payload_len);
    case CMD_DIRECT_WRITE_START:
      return handle_direct_write_start(ctx, payload, payload_len);
    case CMD_DIRECT_WRITE_DATA:
      return handle_direct_write_data(ctx, payload, payload_len);
    case CMD_DIRECT_WRITE_END:
      return handle_direct_write_end(ctx, payload, payload_len);
    case CMD_PARTIAL_WRITE_START:
      return handle_partial_write_start(ctx, payload, payload_len);
    case CMD_PIPE_WRITE_START:
      opendisplay_pipe_write_start(ctx, payload, payload_len);
      /* The PIPE-write module answers for itself and does not yet
       * report a verdict; C11 gives it one with its own header. */
      return OD_CMD_OK;
    case CMD_PIPE_WRITE_DATA:
      opendisplay_pipe_write_data(ctx, payload, payload_len);
      /* The PIPE-write module answers for itself and does not yet
       * report a verdict; C11 gives it one with its own header. */
      return OD_CMD_OK;
    case CMD_PIPE_WRITE_END:
      opendisplay_pipe_write_end(ctx, payload, payload_len);
      /* The PIPE-write module answers for itself and does not yet
       * report a verdict; C11 gives it one with its own header. */
      return OD_CMD_OK;
    default:
      /* NOT a NACK. An unrecognised opcode must not stamp activity or reset the abuse run, or
       * unknown-command traffic keeps the exclusive link alive -- od_frame_policy gives
       * UNKNOWN_OPCODE neither. The log line is the whole of the response, as shipped. */
      od_log_info("unknown cmd 0x%04X", (unsigned)cmd);
      return OD_CMD_UNKNOWN;
  }
}

static void on_pipe_write(const od_cmd_ctx_t *ctx, const uint8_t *data, uint16_t len, bool write_cmd)
{
  uint16_t cmd;
  const uint8_t *frame = data;
  uint16_t frame_len = len;
  uint16_t plain_len = 0;

  (void)write_cmd;
  if (frame_len < 2u) {
    return;
  }
  if (frame_len > OD_PIPE_MAX_PAYLOAD) {
    uint8_t err[] = { 0xFFu, frame[1], 0xFEu };
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return;
  }

  cmd = (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
  if (cmd != CMD_DIRECT_WRITE_DATA && cmd != CMD_PIPE_WRITE_DATA) {
    od_log_info("rx cmd=0x%04X len=%u sec=%d sess=%d", (unsigned)cmd,
           (unsigned)frame_len, (int)sec_enabled(), (int)session_alive());
  }
  if (sec_enabled() && cmd != CMD_AUTHENTICATE) {
    if (frame_len >= 31u) {
      if (!session_alive()) {
        send_auth_required_response(ctx, frame[1]);
        return;
      }
      /* The envelope [nonce:16][ciphertext][tag:12] is contiguous behind the two command bytes,
       * so it is one span. s_plain_buf must hold the decrypted [len:1][payload] frame -- one byte
       * more than plain_len ends up being. */
      struct od_session_report report;
      enum od_session_open opened =
        od_session_open(&s_session, cmd, od_span_make(&frame[2], (size_t)(frame_len - 2u)),
                        s_plain_buf, sizeof(s_plain_buf), &plain_len, od_now_ms(), &report);
      if (opened != OD_SESSION_OPEN_OK) {
        uint8_t err[] = { 0x00u, frame[1], 0xFFu };
        /* A PIPE DATA frame refused for a NONCE reason is ordinary packet loss, and the answer
         * is SILENCE. pipe-write-protocol.md 5.1 makes a 0x81 NACK unconditionally fatal and 5.2
         * reserves NACKs for unrecoverable conditions, so answering here kills the upload on the
         * first dropped frame. Saying nothing leaves the seq absent from the next SACK; the host
         * retransmits under a fresh higher counter, which the window accepts unconditionally.
         * This target ships PIPE_MAX_W 32, so reordering reaches the window in normal use.
         *
         * Deliberately narrow: a TAG failure keeps the NACK -- it is tamper evidence, not loss. */
        const bool nonce_loss = (report.nonce_reason == (uint8_t)NONCE_OUT_OF_WINDOW ||
                                 report.nonce_reason == (uint8_t)NONCE_REPLAY);
        /* LOG BEFORE THE SILENT RETURN -- returning first would hide replay and out-of-window
         * events on the one path that produces them routinely, which is the condition the
         * throttle exists to let you observe. */
        if (nonce_log_allowed(nonce_loss ? &s_nonce_log_window_ms : &s_nonce_log_other_ms)) {
          od_log_warn("decrypt failed: cmd=0x%04X rc=%d nonce_reason=%u envelope=%u B",
                      (unsigned)cmd, (int)opened, (unsigned)report.nonce_reason,
                      (unsigned)(frame_len - 2u));
        }
        if (nonce_loss && cmd == CMD_PIPE_WRITE_DATA) {
          return;   /* silence is the wire answer; the line above is the record of it */
        }
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
        return;
      }
      dispatch(ctx, cmd, s_plain_buf, plain_len);
      return;
    }
    /*
     * Sub-31-byte frame while security is enabled. If a session is live, an
     * unencrypted command must be rejected (matches the reference's
     * "Unencrypted command received when encryption is enabled",
     * communication.cpp:502-507) so a plaintext reboot/DFU/etc. cannot slip
     * past the auth+replay checks mid-session. Firmware-version stays always
     * plaintext-readable (exempted at communication.cpp:488); authenticate is
     * already excluded above. When no session is live, fall through to
     * dispatch()'s auth gate, preserving today's behaviour exactly
     * (fw-version + the config-rewrite path stay reachable).
     */
    if (session_alive() && cmd != CMD_FIRMWARE_VERSION) {
      send_auth_required_response(ctx, frame[1]);
      return;
    }
  }
  dispatch(ctx, cmd, &frame[2], (uint16_t)(frame_len - 2u));
}

/* BT RX thread: copy into the queue only, no command processing here. */
void opendisplay_pipe_on_write(const uint8_t *data, uint16_t len, bool write_cmd)
{
  /* write_cmd is not carried into the ring: on_pipe_write() has always (void)'d it, so a per-frame
   * field for it would be storage for a value nothing reads. Write-command vs write-request is a
   * transport distinction the protocol does not act on. */
  (void)write_cmd;
  /* The connection generation IS the frame's identity, and it travels with the frame rather than
   * being re-read at dispatch: a frame queued by a closed connection must not run against whoever
   * inherited the link. Same role as ESP32's packed owner word. */
  (void)od_rxq_push(data, len, (uint32_t)atomic_get(&s_conn_gen));
}

void opendisplay_pipe_on_notify_changed(bool enabled)
{
  s_notify = enabled;
  od_log_info("pipe notifications %s", enabled ? "on" : "off");
}

/* BT RX thread: mark only; cleanup (EPD abort etc.) runs on the main thread. */
void opendisplay_pipe_on_connection_closed(void)
{
  s_notify = false;
  atomic_inc(&s_conn_gen);
  atomic_set(&s_close_pending, 1);
}

/* Main thread: run deferred cleanup and process queued writes. */
static bool rx_tag_is_live(uint32_t tag, void *context)
{
  (void)context;
  return tag == (uint32_t)atomic_get(&s_conn_gen);
}

void opendisplay_pipe_process(void)
{
  if (atomic_cas(&s_close_pending, 1, 0)) {
    clear_session();
    s_long_write_len = 0;
    s_long_write_conn = 0xFFu;
    cfg_chunk_reset();
    nfc_write_chunk_reset();
    opendisplay_pipe_write_reset();
    opendisplay_display_abort();
  }
  for (;;) {
    (void)od_rxq_discard_stale(rx_tag_is_live, NULL);
    od_rxq_item_t *item = od_rxq_peek();
    if (item == NULL) {
      break;
    }
    /* Close can advance the generation after the shared helper accepts this head. Recheck at the
     * handler boundary; the next iteration consumes it using the new generation. */
    if (!rx_tag_is_live(item->tag, NULL)) {
      continue;
    }
    {
      /* THE CONTEXT IS BUILT HERE, and it must be a real one. Passing 0 for it compiles -- it is a
       * valid null pointer constant -- and od_cmd_reply() then returns INVARIANT for every reply,
       * so the device would accept commands and answer NONE. Nothing in a build catches that.
       *
       * The tag is the frame's own generation, taken from the slot rather than re-read from
       * s_conn_gen: a frame must be answered on the identity that sent it, not on whatever the
       * link has become since. The reservation is empty under legacy routing, where od_cmd_reply
       * hands frames straight to pipe_send(); the dispatcher supplies a real one at the cutover. */
      od_tx_reservation_t r = { 0u, 0u };
      const od_cmd_ctx_t ctx = { { OD_ORIGIN_BLE, item->tag }, &r };
      on_pipe_write(&ctx, item->data, item->len, false);
    }
    /* Stale frames were consumed above rather than retried. Peek/consume avoids a copying get, so
     * the dispatcher decrypts in the slot instead of on a 256-byte stack frame in main. */
    od_rxq_consume();
  }
}

/* IMPLEMENTED FOR shared/core/od_dispatch.h. The opcodes a live CONFIG_READ must exclude, because
 * the producer reads the same stored blob a write reloads through: letting one land between two
 * chunks splices two configs into one CRC-valid read-back.
 *
 * Target-side because the set is a statement about THIS firmware's handlers, not about the wire --
 * Nordic has NFC, Silabs has no PIPE. CONFIG_READ itself is absent deliberately: the dispatcher
 * tests for it separately, since a second read is excluded for a different reason (it would
 * restart a producer that has already promised the host a chunk count). */
bool od_cmd_mutates_config(uint16_t cmd)
{
  switch (cmd) {
  case CMD_CONFIG_WRITE:   /* saveConfig + reload */
  case CMD_CONFIG_CHUNK:   /* same, on completion */
  case CMD_CONFIG_CLEAR:   /* clearStoredConfig */
    return true;
  default:
    return false;
  }
}
