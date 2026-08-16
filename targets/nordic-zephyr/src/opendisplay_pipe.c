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

#include "od_cmd_app.h"
#include "od_cmd_reply.h"
#include "od_config_read.h"
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
  /* Whether this transfer OPENED unauthenticated, under the key-loss exemption. Every continuation
   * must match, so a transfer cannot be started with a session and finished without one. */
  bool unauth;
  uint8_t buffer[MAX_CONFIG_SIZE];
  /* Vestigial on a single-connection target: always 0, so every guard against it is a tautology.
   * Kept rather than deleted here because removing a guard is a C11 shrink decision, not something
   * a reply-classification pass should smuggle in. */
  uint8_t connection;
} od_chunked_config_t;

static od_chunked_config_t s_cfg_chunk;
/* The uptime clock, defined below with the other Zephyr shims. */
static uint32_t od_now_ms(void)
{
  return k_uptime_get_32();
}

/* ============================================================================================
 * THE SESSION ADAPTER. The handshake, KDF, replay window and CCM envelope are
 * shared/core/od_session.c; what stays here is this target's half -- the clock and the nRF device
 * identity. The logging moved to od_session_app.c, and the handshake itself is od_gate's.
 * ============================================================================================ */

static struct od_session s_session;



static void clear_session(void)
{
  od_session_clear(&s_session);
}

/* The four device-id bytes that feed both the KDF and the auth proof: the low 32 bits of the
 * 64-bit hwinfo id, big-endian. Wire-visible identity -- a different packing is a different
 * device to the host. */
static void od_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN]);


void od_pipe_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
  od_device_id(out);
}

/* Bumped on every close. A frame carries the generation that produced it, so both queues can
 * discard work belonging to a connection that has gone. */
static atomic_t s_conn_gen;
/* Raised by the BT RX thread on disconnect; the cleanup it triggers runs on main, because tearing
 * down the display and the session from a stack callback is what starved ATT in the first place. */
static atomic_t s_close_pending;

struct od_session *od_pipe_session(void)
{
  return &s_session;
}

uint32_t od_pipe_conn_gen(void)
{
  return (uint32_t)atomic_get(&s_conn_gen);
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

static uint16_t s_long_write_len;
static uint8_t s_long_write_conn = 0xFFu;
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
   * (same ordering as the nRF52840 Firmware).
   *
   * THE ACK DECIDES WHETHER THE REFRESH HAPPENS. od_reply() can substitute a plaintext hard NACK
   * for an END ack it could not seal, and it reports that rather than lying. Emitting the refresh
   * status afterwards would queue a success behind a rejection; refreshing at all would put
   * content on the panel that the host has just been told was refused. Neither the wire nor the
   * display may claim what the other denies, so both stop. Inert under legacy routing, where the
   * adapter cannot fail. */
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
  (void)od_cmd_reply(ctx, refresh_ok ? ack_refresh_ok : ack_refresh_timeout,
            refresh_ok ? sizeof(ack_refresh_ok) : sizeof(ack_refresh_timeout));
  return OD_CMD_OK;
}

/* An unauthenticated rewrite has NO session to seal with -- that is the whole point of the
 * exemption -- so its ack MUST go plaintext. Sending it through od_cmd_reply() instead makes
 * od_reply substitute a plaintext hard NACK, and the host is told the write FAILED after the new
 * config has already been persisted: recovery appears broken while actually having worked.
 *
 * This is not the byte-2 inference returning. The choice is made from whether a session EXISTS,
 * which is a fact about what is possible, never from the response bytes.
 */
static od_txq_status_t config_ack(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  if (!od_session_authenticated(&s_session)) {
    return od_cmd_reply_plain(ctx, frame, len);
  }
  return od_cmd_reply(ctx, frame, len);
}

static od_cmd_result_t handle_config_clear(const od_cmd_ctx_t *ctx)
{
  uint8_t ok[] = { 0x00u, RESP_CONFIG_CLEAR, 0x00u, 0x00u };
  uint8_t err[] = { 0xFFu, RESP_CONFIG_CLEAR, 0x00u, 0x00u };

  if (!clearStoredConfig()) {
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  /* SEALED BEFORE THE RELOAD, same rule as the config-write acks. Reloading an erased config
   * turns security OFF, after which od_reply takes its plaintext shortcut -- so replying second
   * silently downgrades an ack that the session which sent the command could still protect. */
  (void)config_ack(ctx, ok, sizeof(ok));
  opendisplay_ble_reload_config_from_nvm();
  return OD_CMD_OK;
}

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


static od_cmd_result_t handle_config_read(const od_cmd_ctx_t *ctx)
{
  /* File-static: the producer reads from this across many pump passes, which is exactly why
   * od_dispatch DEFERS every config-mutating opcode while a read is active -- a write reloading
   * through the same buffer mid-read would splice two configs into one CRC-valid read-back. */
  static uint8_t config_data[MAX_CONFIG_SIZE];
  uint32_t config_len = MAX_CONFIG_SIZE;

  if (!initConfigStorage()) {
    uint8_t err[] = { 0xFFu, RESP_CONFIG_READ, 0x00u, 0x00u };
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }

  if (!loadConfig(config_data, &config_len)) {
    /* NO CONFIG STORED answers the 4-byte ERROR frame, not a zero-length ACK, and the difference
     * is one the host acts on. py-opendisplay tests for {FF,40,..} explicitly and raises "Device
     * has no stored configuration"; its comment names the exact failure the test prevents --
     * without it, the {00,00} of an error frame reads as a zero-length config "instead of no
     * config". This target used to answer a valid read of nothing, so an UNPROVISIONED device
     * reported as provisioned-with-nothing, and a corrupt config did too.
     *
     * The canonical header's prose does not settle this and should not be read as if it did: it
     * describes an EMPTY config (stored, length zero) and reserves 0xFF for storage-init failure,
     * and neither sentence covers "nothing stored" -- which is the case that actually occurs.
     * Firmware and the host agree on the answer; FOLLOWUPS.md carries the header wording.
     *
     * Passing NULL rather than hand-building the frame: od_config_read_start() emits exactly this
     * shape for a NULL blob, so both targets now get it from one place. */
    const od_txq_status_t rc = od_config_read_start(&ctx->rp, ctx->r, NULL, 0u);
    (void)rc;
    return OD_CMD_NACK;
  }

  /* STARTS a read; it does not perform one. Chunk 0 goes out here and od_config_read_pump() emits
   * the rest, one slot per pass. The synchronous loop this replaces queued up to 44 frames in a
   * single call against a one-slot reservation -- at the cutover chunk 0 (which declares the total
   * length to the host) would have gone out and every later chunk failed silently on an exhausted
   * token, leaving the host waiting for a config it had been promised.
   *
   * The reservation is TRANSFERRED; od_dispatch's release afterwards is a no-op because the token
   * is already spent. */
  {
    const od_txq_status_t rc =
      od_config_read_start(&ctx->rp, ctx->r, config_data, config_len);
    return (rc == OD_TXQ_OK) ? OD_CMD_OK : OD_CMD_NACK;
  }
}



/* The erase that pays for the key-loss exemption, and the half od_dispatch deliberately does not
 * own -- what "erase" means is storage policy.
 *
 * Runs only when this write actually arrived unauthenticated, so an ordinary authenticated write
 * never touches it. Dropping the stored config drops the OLD KEY with it, which is what stops the
 * exemption being used to layer a new configuration over a key the peer never proved it had. The
 * reference erases on the single-shot path and again on the first chunk of a chunked one, for the
 * same reason: a chunked write starts as CONFIG_WRITE and continues as CONFIG_CHUNK, and a session
 * that expires mid-transfer must not land a new config over a retained key. */
static void erase_before_unauthenticated_rewrite(uint16_t cmd)
{
  if (!od_cmd_allow_unauthenticated(cmd)) {
    return;
  }
  if (od_session_authenticated(&s_session)) {
    return;                       /* an authenticated write: nothing to pay for */
  }
  (void)clearStoredConfig();
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

  /* AFTER the length checks, never before: erasing and then rejecting a malformed frame destroys
   * a working configuration on a frame the device does not even accept. */
  erase_before_unauthenticated_rewrite(CMD_CONFIG_WRITE);

  if (len > CONFIG_CHUNK_SIZE) {
    cfg_chunk_reset();
    s_cfg_chunk.active = true;
    s_cfg_chunk.unauth = !od_session_authenticated(&s_session);
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
        (void)config_ack(ctx, ack, sizeof(ack));
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

    (void)config_ack(ctx, ack, sizeof(ack));
    return OD_CMD_OK;
  }

  if (saveConfig((uint8_t *)(void *)data, len)) {
        /* THE ACK IS SEALED AND QUEUED BEFORE THE RELOAD, and the order is load-bearing.
         * clear_session() drops the session -- the new config may carry a new key -- so a reply
         * attempted after it finds none, and od_reply substitutes a plaintext hard NACK: the host
         * is told a write FAILED that has already been persisted, and cannot tell that from a real
         * failure. od_reply seals at commit, so queueing first makes the bytes final while the
         * session that sent the write is still the one answering it. */
    (void)config_ack(ctx, ack, sizeof(ack));
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
  /* A TRANSFER MAY NOT CHANGE AUTHENTICATION STATE MID-FLIGHT. The opening CONFIG_WRITE recorded
   * whether it arrived unauthenticated, and every continuation must match: otherwise a transfer
   * begun under a live session could be COMPLETED unauthenticated after that session expired,
   * finishing without the erase the exemption is paid for -- landing a new config while the old
   * key survived. Refused rather than erased here, because erasing mid-stream would discard the
   * chunks already collected and turn a downgrade attempt into data loss. */
  if (s_cfg_chunk.unauth != !od_session_authenticated(&s_session)) {
    cfg_chunk_reset();
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
      (void)config_ack(ctx, ack, sizeof(ack));
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
  (void)config_ack(ctx, ack, sizeof(ack));
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


/* ---------------------------------------------------------------------------------------------
 * shared/core/od_cmd_app.h -- one function per canonical opcode. The opcode MAP is od_dispatch.c's;
 * what is here is what THIS target does about each command. A capability this target lacks still
 * gets a definition, because a missing one is a link error -- which is how an opcode cannot be
 * added to the shared map without every target stating its answer.
 * ------------------------------------------------------------------------------------------ */

static const uint8_t *pl(od_span_t body)
{
  return body.p;
}

static uint16_t pl_len(od_span_t body)
{
  return (uint16_t)body.n;
}

od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body)
{
  (void)ctx;
  (void)body;
  od_log_info("reboot");
  for (volatile uint32_t i = 0; i < 800000u; i++) {
  }
  NVIC_SystemReset();
  return OD_CMD_OK;                   /* not reached: the reset does not return */
}

od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body)
{
  (void)body;
  return reply_firmware_version(ctx);
}

od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body)
{
  (void)body;
  return reply_read_msd(ctx);
}

od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { 0x00u, RESP_ENTER_DFU };

  (void)body;
  (void)od_cmd_reply(ctx, ok, sizeof(ok));
  opendisplay_ble_schedule_dfu();
  return OD_CMD_OK;
}

/* NO POWER LATCH ON THIS TARGET, and it says so rather than staying silent. Before C11 the opcode
 * fell through to the unknown arm, so a host could not distinguish "this device has no rail cut"
 * from "this firmware is older than the command" -- and 0x0052 is exactly the opcode where that
 * ambiguity is expensive, because the client's alternative is to keep retrying. The canonical
 * header defines OD_ERR_POWER_OFF_UNSUPPORTED for this answer.
 *
 * NACK, not UNKNOWN: the frame WAS recognised. It still must not stamp activity, and it does not
 * -- od_frame_policy gives HANDLER_NACK no stamp either. */
od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t err[] = { 0xFFu, RESP_POWER_OFF, OD_ERR_POWER_OFF_UNSUPPORTED, 0x00u };

  (void)body;
  (void)od_cmd_reply_plain(ctx, err, sizeof(err));
  return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body)
{
  /* RECOGNISED AND SILENT, matching the reference nRF52840 build
   * (device_control.cpp:691-705): the command is acted on but NO response is sent, so clients do
   * not treat deep sleep as supported on this target.
   *
   * OPCODE CHANGED 0x0052 -> 0x0053 when this target adopted the canonical protocol header. The
   * subset header it used to carry still had the value from before the split that made 0x0052
   * CMD_POWER_OFF -- a hard rail-cut -- and left deep sleep on 0x0053, which is what Firmware and
   * Firmware_Silabs already answer.
   *
   * py-opendisplay still sends 0x0052 (protocol/commands.py DEEP_SLEEP), so deep sleep from that
   * client stops working against this target until the host library is updated. That is the known
   * cost of the alignment, and 0x0052 now answers the unsupported NACK above rather than falling
   * silent -- which tells such a host something, where silence told it nothing. It MUST be fixed
   * before anything implements a real CMD_POWER_OFF here: at that point an un-updated host asking
   * for deep sleep would cut the rail. */
  (void)ctx;
  (void)body;
  opendisplay_ble_schedule_deep_sleep();
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body)
{
  (void)body;
  return handle_config_read(ctx);
}

od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return handle_config_write(ctx, pl(body), pl_len(body));
}

od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return handle_config_chunk(ctx, pl(body), pl_len(body));
}

od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body)
{
  (void)body;
  return handle_config_clear(ctx);
}

od_cmd_result_t od_cmd_app_direct_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return handle_direct_write_start(ctx, pl(body), pl_len(body));
}

od_cmd_result_t od_cmd_app_direct_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return handle_direct_write_data(ctx, pl(body), pl_len(body));
}

od_cmd_result_t od_cmd_app_direct_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return handle_direct_write_end(ctx, pl(body), pl_len(body));
}

od_cmd_result_t od_cmd_app_partial_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return handle_partial_write_start(ctx, pl(body), pl_len(body));
}

od_cmd_result_t od_cmd_app_pipe_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
  opendisplay_pipe_write_start(ctx, pl(body), pl_len(body));
  /* The PIPE-write module answers for itself and does not yet report a verdict; C11.3 gives it
   * one. Until then a PIPE NACK is followed by an unconditional OK here, which is wrong and is
   * the next commit's subject. */
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_pipe_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
  opendisplay_pipe_write_data(ctx, pl(body), pl_len(body));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_pipe_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
  opendisplay_pipe_write_end(ctx, pl(body), pl_len(body));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { 0x00u, RESP_LED_ACTIVATE_ACK, 0x00u, 0x00u };
  uint8_t e1[] = { 0xFFu, RESP_LED_ACTIVATE_ACK, 0x01u, 0x00u };
  uint8_t e2[] = { 0xFFu, RESP_LED_ACTIVATE_ACK, 0x02u, 0x00u };
  const uint8_t *payload = pl(body);
  const uint16_t payload_len = pl_len(body);

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

od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { 0x00u, RESP_LED_STOP_ACK, 0x00u, 0x00u };
  uint8_t e2[] = { 0xFFu, RESP_LED_STOP_ACK, 0x02u, 0x00u };
  const uint8_t *payload = pl(body);
  const uint16_t payload_len = pl_len(body);
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

od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body)
{
  int rc = opendisplay_buzzer_activate(pl(body), pl_len(body));

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

od_cmd_result_t od_cmd_app_nfc(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return handle_nfc_endpoint(ctx, pl(body), pl_len(body));
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
  od_log_info("pipe notifications %s", enabled ? "on" : "off");
}

/* BT RX thread: mark only; cleanup (EPD abort etc.) runs on the main thread. */
void opendisplay_pipe_on_connection_closed(void)
{
  atomic_inc(&s_conn_gen);
  atomic_set(&s_close_pending, 1);
}

/* Main thread: run deferred cleanup and process queued writes. */
static bool rx_tag_is_live(uint32_t tag, void *context)
{
  (void)context;
  return tag == (uint32_t)atomic_get(&s_conn_gen);
}

/* IMPLEMENTED FOR shared/core/od_cmd.h. Applies od_frame_policy() with this target's scoping.
 *
 * Nordic has NO exclusive-link idle clock and NO auth-abuse drop -- those are ESP32's
 * CONNECTION_POLICY mechanisms and there is nothing here to stamp or to count. What IS applicable
 * is the session's own activity clock: od_session_open() touches it on the encrypted path, but an
 * accepted PLAINTEXT command (security off, or FIRMWARE_VERSION) never reaches od_session_open, so
 * without this the clock would stop for exactly the traffic keeping the link busy. */
void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome)
{
  const od_frame_policy_t p = od_frame_policy(outcome);

  if (rp == NULL || !p.stamp_activity) {
    return;
  }
  od_session_touch(&s_session, od_now_ms());
}

/* The main-thread pump. The ORDER is the design, and each step is here because leaving it out is
 * silent rather than loud:
 *
 *   1. deferred disconnect cleanup      -- before anything reads session or transfer state
 *   2. TX progress                      -- free capacity BEFORE dispatch needs to reserve it
 *   3. config-read producer             -- one chunk per available slot; without it a read stops
 *                                          after chunk 0 and stays active forever, deferring every
 *                                          later config write behind it
 *   4. stale discard + identity recheck -- a frame must not run on an identity that has gone
 *   5. shared dispatch                  -- validate, gate, decrypt, handler
 *   6. outcome policy
 *   7. RX consumption, EXCEPT DEFERRED  -- which stays at the head, byte-identical
 *   8. TX progress again                -- so a reply queued by this pass leaves in this pass
 *
 * 2 before 5 is the one that is not obvious: dispatch reserves capacity before it decrypts, so
 * draining first is what stops a frame being decrypted -- advancing the replay window -- and only
 * then discovering there is no room to answer it. A frame deferred after decrypt is a REPLAY when
 * it is re-offered, and the window refuses it the second time. */
void opendisplay_pipe_process(void)
{
  uint8_t drained;

  if (atomic_cas(&s_close_pending, 1, 0)) {
    clear_session();
    s_long_write_len = 0;
    s_long_write_conn = 0xFFu;
    cfg_chunk_reset();
    nfc_write_chunk_reset();
    opendisplay_pipe_write_reset();
    opendisplay_display_abort();
    /* The producer holds the config scratch, and od_dispatch DEFERS every config-mutating opcode
     * while a read is active -- so a client that vanishes mid-read would otherwise defer every
     * later config write for the life of the boot. Egress goes with it: its queued frames belong
     * to a connection that is gone. */
    od_config_read_cancel();
    od_txq_reset();
  }

  /* BOUNDED, and the bound is not defensive tidiness. A central issuing write-without-response can
   * refill the SPSC ring as fast as this drains it, and an unbounded loop then never returns to the
   * LED, buzzer, input and watchdog work the main thread also owns -- a peer could hold the thread
   * indefinitely with valid traffic. One ring's worth per pass is enough for a full PIPE window
   * plus its END, which is the deepest legitimate burst; anything beyond that is the next pass's. */
  for (drained = 0u; drained < OD_RXQ_SLOTS; ++drained) {
    od_rxq_item_t *item;
    od_reply_t rp;
    od_frame_outcome_t outcome;

    (void)od_txq_process();
    (void)od_config_read_pump();

    (void)od_rxq_discard_stale(rx_tag_is_live, NULL);
    item = od_rxq_peek();
    if (item == NULL) {
      break;
    }
    /* Close can advance the generation after the shared helper accepted this head. Recheck at the
     * handler boundary; the next iteration consumes it under the new generation. */
    if (!rx_tag_is_live(item->tag, NULL)) {
      /* Consumed here rather than left for the next iteration's discard: `continue` alone would
       * re-peek the same head, and if the generation moved again between the two calls the loop
       * would spin without progress. The frame is dead either way. */
      od_rxq_consume();
      continue;
    }

    /* The tag is the frame's OWN generation, from its slot rather than a re-read of s_conn_gen: a
     * frame must be answered on the identity that sent it, not on whatever the link has become. */
    rp.origin = OD_ORIGIN_BLE;
    rp.tag = item->tag;
    outcome = od_dispatch_frame(&rp, od_span_make(item->data, item->len));
    od_core_frame_done(&rp, outcome);
    /* The unknown-opcode line, kept where both the bytes and the verdict are visible. The opcode
     * map is shared now and shared/ cannot log; this is the one place that has the frame and the
     * dispatcher's conclusion at the same time. */
    if (outcome == OD_FRAME_UNKNOWN_OPCODE && item->len >= 2u) {
      od_log_info("unknown cmd 0x%04X",
                  (unsigned)(((uint16_t)item->data[0] << 8) | item->data[1]));
    }

    if (!od_frame_policy(outcome).consume_rx) {
      /* DEFERRED: not consumed, re-offered unchanged. Stop the drain too -- the head has not moved,
       * so another pass this tick would reach the same answer. What cleared it is TX capacity or a
       * finishing config read, and both advance at the top of the next pass. */
      break;
    }
    od_rxq_consume();
  }

  (void)od_txq_process();
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

/* IMPLEMENTED FOR shared/core/od_dispatch.h -- KEY-LOSS RECOVERY, and the only exemption this
 * target grants.
 *
 * A device provisioned with a session key whose host has lost that key is otherwise bricked for
 * configuration: every command answers AUTH_REQUIRED and the only route back is physical. When
 * SecurityConfig flags bit 0 is set, the owner has said -- in the device's own stored
 * configuration -- that an unauthenticated config REWRITE is acceptable, so the gate is skipped for
 * the two write opcodes, and only when no session is live, which od_dispatch establishes before
 * asking.
 *
 * READS ARE NEVER EXEMPT. CONFIG_READ is absent deliberately: this path exists to replace a
 * configuration, not to disclose one, and exempting the read would hand the stored config -- and
 * the key inside it -- to any unauthenticated peer. The handlers erase the stored config before
 * accepting such a write, so the old key cannot survive into the new one either.
 *
 * The local gate this replaces had the same intent and was almost entirely unreachable: it sat
 * behind a `frame_len >= 31` test that rejected any plaintext frame large enough to carry a real
 * config, so only a config of 28 bytes or fewer could reach it. That shadow is gone, and the
 * capability now works for the config sizes it was written for. */
bool od_cmd_allow_unauthenticated(uint16_t cmd)
{
  const struct SecurityConfig *sec = od_get_parsed_security();

  /* SECURITY MUST BE ON. With no key configured there is nothing to recover from and no gate to
   * be exempt from -- but the flag byte can still be set, and reading it alone made every ordinary
   * CONFIG_WRITE on an unsecured device erase its own storage first. The flag is documented as
   * applying "while encryption is enabled"; this is where that is enforced. */
  if (!od_session_security_enabled(sec)) {
    return false;
  }
  if ((sec->flags & 0x01u) == 0u) {
    return false;
  }
  return cmd == CMD_CONFIG_WRITE || cmd == CMD_CONFIG_CHUNK;
}
