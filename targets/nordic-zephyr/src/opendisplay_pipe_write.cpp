#include "od_cmd_app.h"
#include "od_cmd_reply.h"
#include "od_rxq.h"
#include "opendisplay_pipe_write.h"
#include "opendisplay_display.h"
#include "opendisplay_protocol.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#define PIPE_REORDER_SLOTS 33
#define PIPE_MAX_W         32
/* The shared RX ring must hold a whole window plus its END, or a saturating client stalls: the
 * frame whose ACK would refund a slot is the one being dropped. shared/ cannot see PIPE_MAX_W, so
 * the relationship is asserted here, where both are visible -- the same check ESP32 makes in
 * structs.h. This target's default OD_RXQ_SLOTS (34) already covers W=32. */
OD_STATIC_ASSERT(OD_RXQ_SLOTS >= (PIPE_MAX_W + 2u),
                 "RX ring is too shallow for this target's PIPE window");
#define PIPE_MAX_N         32
#define PIPE_REORDER_SLOT_SIZE 248

#define RESP_ACK  0x00u
#define RESP_NACK 0xFFu

/* PipeStartRequest and PipePartialExt come from shared/protocol/opendisplay_structs.h -- the
 * canonical wire contract -- via od_rxq.h. This file carried hand-written copies of both, matching
 * field for field but under no assertion tying them to the header; a change upstream would have
 * left the shadow behind silently. That is precisely what OD_STATIC_ASSERT(sizeof(...)) exists to
 * catch there, and a local redefinition escapes it. */

struct PipeReorderSlot {
  bool occupied;
  uint8_t seq;
  uint16_t len;
  uint8_t data[PIPE_REORDER_SLOT_SIZE];
};

struct PipeWriteState {
  bool active;
  bool error;
  bool compressed;
  bool partial;
  bool gap_open;
  uint8_t window;
  uint8_t ack_every;
  uint16_t max_frame;
  uint8_t expected_seq;
  bool has_received;
  uint8_t highest_seen;
  uint32_t received_count;
  uint8_t frames_since_ack;
  uint8_t ooo_acks_since_gap;
  uint32_t total_size;
  uint16_t queued_count;
  uint16_t queue_high_water;
};

static PipeWriteState s_pipe;
static PipeReorderSlot s_reorder[PIPE_REORDER_SLOTS];

static inline uint8_t pipe_slot(uint8_t seq)
{
  return (uint8_t)(seq % PIPE_REORDER_SLOTS);
}

extern "C" void opendisplay_pipe_write_reset(void)
{
  memset(&s_pipe, 0, sizeof(s_pipe));
  for (int i = 0; i < PIPE_REORDER_SLOTS; ++i) {
    s_reorder[i].occupied = false;
  }
}

extern "C" bool opendisplay_pipe_write_active(void)
{
  return s_pipe.active;
}

static bool pipe_chunk_received(uint8_t c)
{
  uint8_t below = (uint8_t)(s_pipe.expected_seq - 1u - c);
  uint32_t accepted_depth = (s_pipe.received_count < PIPE_ACK_MASK_BITS) ? s_pipe.received_count
                                                                         : PIPE_ACK_MASK_BITS;
  if (below < accepted_depth) {
    return true;
  }
  return s_reorder[pipe_slot(c)].occupied && s_reorder[pipe_slot(c)].seq == c;
}

static void pipe_build_ack_payload(uint8_t *out)
{
  uint8_t hs = s_pipe.has_received ? s_pipe.highest_seen : (uint8_t)(s_pipe.expected_seq - 1u);
  uint32_t mask = 0;

  for (uint8_t i = 0; i < PIPE_ACK_MASK_BITS; ++i) {
    if (pipe_chunk_received((uint8_t)(hs - 1u - i))) {
      mask |= (1u << i);
    }
  }
  out[0] = hs;
  out[1] = (uint8_t)(mask & 0xFFu);
  out[2] = (uint8_t)((mask >> 8) & 0xFFu);
  out[3] = (uint8_t)((mask >> 16) & 0xFFu);
  out[4] = (uint8_t)((mask >> 24) & 0xFFu);
}

/* The teardown half of a fatal PIPE refusal, WITHOUT the NACK: od_reply has already substituted
 * one for the ack it could not seal, and every 0x81 NACK is fatal (pipe-write-protocol.md 5.1), so
 * a second would add nothing and cost a slot. Stopping the replies is not enough on its own -- a
 * transfer owns the panel, and leaving it half-alive keeps the display session open for a transfer
 * the host has abandoned. */
static void pipe_abort_no_reply(void)
{
  opendisplay_display_abort();
  opendisplay_pipe_write_reset();
}

/* RETURNS ITS RESULT, and callers that go on to emit more -- or to touch the panel -- must honour
 * it. The cadence counters reset either way: they describe what this device decided to send, not
 * what the transport managed to do with it. */
static od_txq_status_t send_pipe_ack(const od_cmd_ctx_t *ctx)
{
  uint8_t r[7] = { RESP_ACK, 0x81u, 0, 0, 0, 0, 0 };
  od_txq_status_t rc;

  pipe_build_ack_payload(r + 2);
  rc = od_cmd_reply(ctx, r, sizeof(r));
  s_pipe.frames_since_ack = 0;
  s_pipe.ooo_acks_since_gap = 0;
  return rc;
}

/* Send a SACK, and stop the transfer if od_reply had to substitute a hard NACK for it.
 *
 * EVERY 0x81 NACK IS FATAL (pipe-write-protocol.md 5.1), whatever frame it replaced. So a
 * substituted SACK is not "an ack the host missed" -- it is the end of the upload, and the device
 * must not carry on holding the panel and consuming DATA for a transfer the host has abandoned.
 * Returns false when that happened; the caller reports OD_CMD_NACK and does no more. */
static bool sack_or_abort(const od_cmd_ctx_t *ctx)
{
  if (send_pipe_ack(ctx) == OD_TXQ_OK) {
    return true;
  }
  pipe_abort_no_reply();
  return false;
}

static void send_pipe_nack(const od_cmd_ctx_t *ctx, uint8_t err)
{
  uint8_t r[8] = { RESP_NACK, 0x81u, err, 0, 0, 0, 0, 0 };

  pipe_build_ack_payload(r + 3);
  (void)od_cmd_reply_plain(ctx, r, sizeof(r));
  s_pipe.error = true;
  if (s_pipe.partial) {
    opendisplay_display_clear_etag();
  }
  opendisplay_display_abort();
}

static void send_pipe_start_nack(const od_cmd_ctx_t *ctx, uint8_t err)
{
  uint8_t r[4] = { RESP_NACK, 0x80u, err, 0x00u };

  (void)od_cmd_reply_plain(ctx, r, sizeof(r));
}

static void pipe_update_highest_seen(uint8_t seq)
{
  if (!s_pipe.has_received) {
    s_pipe.has_received = true;
    s_pipe.highest_seen = seq;
    return;
  }
  uint8_t fwd = (uint8_t)(seq - s_pipe.highest_seen);
  if (fwd != 0u && fwd <= PIPE_ACK_MASK_BITS) {
    s_pipe.highest_seen = seq;
  }
}

static bool pipe_consume_payload(uint8_t *data, uint16_t len)
{
  if (len == 0u) {
    return true;
  }
  return opendisplay_display_direct_write_data(data, len) == 0;
}

static od_cmd_result_t finish_and_refresh(const od_cmd_ctx_t *ctx, const uint8_t *payload,
                                          uint16_t payload_len, uint8_t end_opcode)
{
  bool refresh_ok = false;
  uint8_t ack_end[2] = { RESP_ACK, end_opcode };
  uint8_t ack_ok[2] = { RESP_ACK, 0x73u };
  uint8_t ack_to[2] = { RESP_ACK, 0x74u };
  uint8_t nack[2] = { RESP_NACK, end_opcode };
  const uint8_t *prep = payload;
  uint16_t prep_len = payload_len;
  int rc;

  if (s_pipe.partial) {
    if (payload != nullptr && payload_len >= 5u) {
      uint32_t new_etag = ((uint32_t)payload[1] << 24) | ((uint32_t)payload[2] << 16)
                          | ((uint32_t)payload[3] << 8) | (uint32_t)payload[4];
      opendisplay_display_set_partial_new_etag(new_etag);
    }
    /* Legacy 0x72/0x76 END prepare only accepts refresh selector (len <= 1). */
    prep_len = (payload != nullptr && payload_len >= 1u) ? 1u : 0u;
    prep = (prep_len != 0u) ? payload : nullptr;
  }

  rc = opendisplay_display_direct_write_end_prepare(prep, prep_len);
  if (rc != 0) {
    (void)od_cmd_reply_plain(ctx, nack, sizeof(nack));
    if (s_pipe.partial) {
      opendisplay_display_clear_etag();
    }
    opendisplay_display_abort();
    opendisplay_pipe_write_reset();
    return OD_CMD_NACK;
  }

  /* THE ACK GATES THE REFRESH -- see the same rule on the direct-write END path. A hard NACK
   * substituted for this ack is fatal to the transfer, so neither the refresh status nor the panel
   * work may follow it. Inert under legacy routing. */
  if (od_cmd_reply(ctx, ack_end, sizeof(ack_end)) != OD_TXQ_OK) {
    opendisplay_display_abort();
    opendisplay_pipe_write_reset();
    return OD_CMD_NACK;          /* od_reply substituted a hard NACK: the wire says refused */
  }
  /* On air before blocking -- see the same barrier on the direct-write END path. */
  od_cmd_flush_before_refresh();
  k_msleep(20);
  if (opendisplay_display_direct_write_end_refresh(prep, prep_len, &refresh_ok) != 0) {
    (void)od_cmd_reply_plain(ctx, nack, sizeof(nack));
    opendisplay_pipe_write_reset();
    return OD_CMD_NACK;
  }
  /* OK on BOTH refresh outcomes. 0x74 reports a refresh that timed out, not a refused command:
   * the transfer completed and the panel was driven. The distinction the verdict carries is
   * accepted-vs-refused, and this frame was accepted.
   *
   * Unless this last reply was itself substituted -- then the only thing the host received for
   * this frame was a hard NACK, and reporting acceptance would be reporting the opposite of what
   * went out. The transfer is over either way, so only the verdict differs. */
  {
    const od_txq_status_t rc = od_cmd_reply(ctx, refresh_ok ? ack_ok : ack_to, 2u);
    opendisplay_pipe_write_reset();
    return (rc == OD_TXQ_OK) ? OD_CMD_OK : OD_CMD_NACK;
  }
}

extern "C" od_cmd_result_t opendisplay_pipe_write_start(const od_cmd_ctx_t *ctx,
                                                       const uint8_t *payload,
                                                       uint16_t payload_len)
{
  struct PipeStartRequest req;
  uint8_t ver;
  uint8_t flags;
  uint8_t req_w;
  uint8_t req_n;
  uint16_t client_max_frame;
  uint32_t total_size;
  bool compressed;
  bool partial;
  uint8_t w_eff;
  uint8_t n_eff;
  uint16_t frame_eff;
  uint8_t resp[8];

  if (opendisplay_display_partial_active() || opendisplay_display_dw_active()) {
    opendisplay_display_abort();
  }
  opendisplay_pipe_write_reset();

  if (payload == nullptr || payload_len < sizeof(req)) {
    send_pipe_start_nack(ctx, OD_ERR_PIPE_START_BAD_HEADER);
    return OD_CMD_NACK;
  }
  memcpy(&req, payload, sizeof(req));
  ver = req.version;
  flags = req.flags;
  req_w = req.req_window;
  req_n = req.req_ack_every;
  client_max_frame = req.client_max_frame;
  total_size = req.total_size;

  if (ver != PIPE_VERSION) {
    send_pipe_start_nack(ctx, OD_ERR_PIPE_START_BAD_HEADER);
    return OD_CMD_NACK;
  }
  if ((flags & ~(PIPE_FLAG_COMPRESSED | PIPE_FLAG_PARTIAL)) != 0u) {
    send_pipe_start_nack(ctx, OD_ERR_PIPE_START_UNKNOWN_FLAG);
    return OD_CMD_NACK;
  }

  compressed = (flags & PIPE_FLAG_COMPRESSED) != 0u;
  partial = (flags & PIPE_FLAG_PARTIAL) != 0u;

  if (partial) {
    struct PipePartialExt ext;
    uint8_t err = OD_ERR_PIPE_START_BAD_HEADER;

    if (payload_len < sizeof(req) + sizeof(ext)) {
      send_pipe_start_nack(ctx, OD_ERR_PIPE_START_BAD_HEADER);
      return OD_CMD_NACK;
    }
    memcpy(&ext, payload + sizeof(req), sizeof(ext));
    if (opendisplay_display_pipe_partial_arm(flags, ext.old_etag, ext.x, ext.y, ext.w, ext.h,
                                             total_size, &err)
        != 0) {
      send_pipe_start_nack(ctx, err);
      return OD_CMD_NACK;
    }
  } else {
    uint32_t expected = opendisplay_display_expected_dw_bytes();

    if (expected == 0u || total_size != expected) {
      send_pipe_start_nack(ctx, OD_ERR_PIPE_START_SIZE_MISMATCH);
      return OD_CMD_NACK;
    }
  }

  w_eff = (req_w > PIPE_MAX_W) ? PIPE_MAX_W : req_w;
  if (w_eff == 0u) {
    w_eff = 1u;
  }
  n_eff = (req_n > PIPE_MAX_N) ? PIPE_MAX_N : req_n;
  if (n_eff == 0u) {
    n_eff = 1u;
  }
  if (n_eff > w_eff) {
    n_eff = w_eff;
  }
  frame_eff = (client_max_frame < PIPE_MAX_FRAME) ? client_max_frame : PIPE_MAX_FRAME;

  s_pipe.active = true;
  s_pipe.error = false;
  s_pipe.compressed = compressed;
  s_pipe.partial = partial;
  s_pipe.gap_open = false;
  s_pipe.window = w_eff;
  s_pipe.ack_every = n_eff;
  s_pipe.max_frame = frame_eff;
  s_pipe.expected_seq = 0;
  s_pipe.has_received = false;
  s_pipe.highest_seen = 0;
  s_pipe.received_count = 0;
  s_pipe.frames_since_ack = 0;
  s_pipe.ooo_acks_since_gap = 0;
  s_pipe.total_size = total_size;
  s_pipe.queued_count = 0;
  s_pipe.queue_high_water = 0;

  resp[0] = RESP_ACK;
  resp[1] = 0x80u;
  resp[2] = PIPE_VERSION;
  resp[3] = PIPE_MAX_W;
  resp[4] = PIPE_MAX_N;
  resp[5] = (uint8_t)(PIPE_MAX_FRAME & 0xFFu);
  resp[6] = (uint8_t)((PIPE_MAX_FRAME >> 8) & 0xFFu);
  resp[7] = (uint8_t)(0x01u | (partial ? PIPE_FLAG_PARTIAL : 0u));
  /* THE STATE IS ALREADY ARMED ABOVE, so a substituted hard NACK has to unwind it. The host was
   * told the transfer was refused and will start no upload; leaving s_pipe.active set would leave
   * a transfer nobody is driving -- and, worse, one whose display session was never started,
   * because the setup below is on the far side of this return. A later DATA frame would then
   * stream into a panel that was never opened. */
  if (od_cmd_reply(ctx, resp, sizeof(resp)) != OD_TXQ_OK) {
    if (partial) {
      opendisplay_display_clear_etag();
    }
    pipe_abort_no_reply();
    return OD_CMD_NACK;
  }

  /* PAST THE ACK. The host has been told the transfer is open, so these are failures of the
   * DEVICE's own setup rather than refusals of this frame: the error flag makes every later DATA
   * frame refuse, and the host learns of it there. The frame itself was accepted and answered. */
  if (partial) {
    if (opendisplay_display_pipe_partial_prepare() != 0) {
      s_pipe.error = true;
      opendisplay_display_clear_etag();
      opendisplay_display_abort();
    }
    return OD_CMD_OK;
  }

  if (opendisplay_display_pipe_full_start(compressed, total_size) != 0) {
    s_pipe.error = true;
    opendisplay_display_abort();
  }
  return OD_CMD_OK;
}

extern "C" od_cmd_result_t opendisplay_pipe_write_data(const od_cmd_ctx_t *ctx,
                                                      const uint8_t *payload,
                                                      uint16_t payload_len)
{
  uint8_t seq;
  uint8_t *data;
  uint16_t plen;
  uint8_t fwd;
  uint8_t back;
  const uint8_t W = s_pipe.window;

  /* SILENT, AND STILL A REFUSAL. A DATA frame for a transfer that is not open -- or that has
   * already failed -- is discarded: answering it would send a 0x81 NACK, which is fatal to a
   * client's upload loop, for a frame that changes nothing. Nothing was accepted, so the verdict
   * is NACK and the frame does not stamp the session's activity clock. */
  if (!s_pipe.active || s_pipe.error || payload == nullptr || payload_len < 1u) {
    return OD_CMD_NACK;
  }

  seq = payload[0];
  data = (uint8_t *)(void *)(payload + 1);
  plen = (uint16_t)(payload_len - 1u);
  if (plen > PIPE_REORDER_SLOT_SIZE) {
    send_pipe_nack(ctx, 0x03u);
    return OD_CMD_NACK;
  }

  fwd = (uint8_t)(seq - s_pipe.expected_seq);
  back = (uint8_t)(s_pipe.expected_seq - seq);

  if (fwd == 0u) {
    if (!pipe_consume_payload(data, plen)) {
      send_pipe_nack(ctx, s_pipe.compressed ? 0x02u : 0x03u);
      return OD_CMD_NACK;
    }
    s_pipe.expected_seq++;
    s_pipe.received_count++;
    s_pipe.frames_since_ack++;
    pipe_update_highest_seen(seq);

    while (s_reorder[pipe_slot(s_pipe.expected_seq)].occupied
           && s_reorder[pipe_slot(s_pipe.expected_seq)].seq == s_pipe.expected_seq) {
      PipeReorderSlot &slot = s_reorder[pipe_slot(s_pipe.expected_seq)];
      if (!pipe_consume_payload(slot.data, slot.len)) {
        send_pipe_nack(ctx, s_pipe.compressed ? 0x02u : 0x03u);
        return OD_CMD_NACK;
      }
      slot.occupied = false;
      if (s_pipe.queued_count > 0u) {
        s_pipe.queued_count--;
      }
      s_pipe.expected_seq++;
      s_pipe.received_count++;
      if (s_pipe.frames_since_ack < 0xFFu) {
        s_pipe.frames_since_ack++;
      }
    }
    if (s_pipe.queued_count == 0u) {
      s_pipe.gap_open = false;
    }

    if (!s_pipe.partial && !s_pipe.compressed
        && opendisplay_display_bytes_written() >= opendisplay_display_total_bytes()) {
      if (!sack_or_abort(ctx)) {
        return OD_CMD_NACK;
      }
      /* AUTO-COMPLETE: the byte count met the total, so this DATA frame carries the END. Its
       * verdict is the END's. */
      return finish_and_refresh(ctx, nullptr, 0u, 0x82u);
    }
    /* No later reply follows a cadence ACK -- but the TRANSFER does, which is why the result is
     * not simply discarded: a substituted NACK is fatal, so carrying on would accept DATA for a
     * transfer the host has already stopped. */
    if (s_pipe.frames_since_ack >= s_pipe.ack_every && !sack_or_abort(ctx)) {
      return OD_CMD_NACK;
    }
    return OD_CMD_OK;
  }

  if (fwd < W) {
    PipeReorderSlot &slot = s_reorder[pipe_slot(seq)];
    bool duplicate = (slot.occupied && slot.seq == seq);

    slot.occupied = true;
    slot.seq = seq;
    slot.len = plen;
    memcpy(slot.data, data, plen);
    if (!duplicate) {
      s_pipe.queued_count++;
      if (s_pipe.queued_count > s_pipe.queue_high_water) {
        s_pipe.queue_high_water = s_pipe.queued_count;
      }
    }
    if (s_pipe.queued_count >= PIPE_REORDER_SLOTS) {
      send_pipe_nack(ctx, 0x03u);
      return OD_CMD_NACK;
    }
    pipe_update_highest_seen(seq);
    /* A GAP SACK, not a refusal: the frame was accepted into the reorder window. But if the ack
     * could not be sealed the host got a fatal NACK instead, and the queued frame is then part of
     * an upload that is over -- so the transfer stops rather than holding the panel and the slot. */
    if (!s_pipe.gap_open) {
      s_pipe.gap_open = true;
      if (!sack_or_abort(ctx)) {
        return OD_CMD_NACK;
      }
    } else if (++s_pipe.ooo_acks_since_gap >= s_pipe.ack_every) {
      if (!sack_or_abort(ctx)) {
        return OD_CMD_NACK;
      }
    }
    return OD_CMD_OK;
  }

  /* A DUPLICATE inside the window: already consumed, so nothing is stored, but it is answered
   * with the current SACK so a client that lost an ack can resynchronise. Accepted. */
  if (back <= W) {
    if (!s_pipe.gap_open) {
      if (!sack_or_abort(ctx)) {
        return OD_CMD_NACK;
      }
    } else if (++s_pipe.ooo_acks_since_gap >= s_pipe.ack_every) {
      if (!sack_or_abort(ctx)) {
        return OD_CMD_NACK;
      }
    }
    return OD_CMD_OK;
  }

  send_pipe_nack(ctx, 0x04u);      /* outside the window in both directions: fatal */
  return OD_CMD_NACK;
}

extern "C" od_cmd_result_t opendisplay_pipe_write_end(const od_cmd_ctx_t *ctx,
                                                     const uint8_t *payload,
                                                     uint16_t payload_len)
{
  uint8_t nack[2] = { RESP_NACK, 0x82u };
  bool incomplete;

  if (!s_pipe.active) {
    (void)od_cmd_reply_plain(ctx, nack, sizeof(nack));
    return OD_CMD_NACK;
  }
  if (s_pipe.error) {
    (void)od_cmd_reply_plain(ctx, nack, sizeof(nack));
    opendisplay_display_abort();
    opendisplay_pipe_write_reset();
    return OD_CMD_NACK;
  }

  if (!sack_or_abort(ctx)) {
    return OD_CMD_NACK;
  }

  incomplete = (s_pipe.queued_count > 0u);
  if (s_pipe.partial) {
    if (!s_pipe.compressed
        && opendisplay_display_partial_bytes_written() != opendisplay_display_partial_expected()) {
      incomplete = true;
    }
  } else if (!s_pipe.compressed
             && opendisplay_display_bytes_written() < opendisplay_display_total_bytes()) {
    incomplete = true;
  }

  if (incomplete) {
    (void)od_cmd_reply_plain(ctx, nack, sizeof(nack));
    if (s_pipe.partial) {
      opendisplay_display_clear_etag();
    }
    opendisplay_display_abort();
    opendisplay_pipe_write_reset();
    return OD_CMD_NACK;
  }

  return finish_and_refresh(ctx, payload, payload_len, 0x82u);
}

/* ---------------------------------------------------------------------------------------------
 * shared/core/od_cmd_app.h -- the three PIPE opcodes.
 *
 * THE MODULE'S VERDICT IS THE COMMAND'S. It answers the wire itself, so it is the only thing that
 * knows whether a frame was accepted or refused; an OK tail here would report a PIPE NACK as an
 * accepted command and stamp the session clock on traffic the device rejected.
 * ------------------------------------------------------------------------------------------ */

extern "C" od_cmd_result_t od_cmd_app_pipe_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return opendisplay_pipe_write_start(ctx, body.p, (uint16_t)body.n);
}

extern "C" od_cmd_result_t od_cmd_app_pipe_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return opendisplay_pipe_write_data(ctx, body.p, (uint16_t)body.n);
}

extern "C" od_cmd_result_t od_cmd_app_pipe_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
  return opendisplay_pipe_write_end(ctx, body.p, (uint16_t)body.n);
}
