#include "opendisplay_pipe_write.h"
#include "opendisplay_display.h"
#include "opendisplay_protocol.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

#define PIPE_REORDER_SLOTS 33
#define PIPE_MAX_W         32
#define PIPE_MAX_N         32
#define PIPE_REORDER_SLOT_SIZE 248

#define RESP_ACK  0x00u
#define RESP_NACK 0xFFu

struct PipeStartRequest {
  uint8_t version;
  uint8_t flags;
  uint8_t req_window;
  uint8_t req_ack_every;
  uint16_t client_max_frame;
  uint32_t total_size;
} __attribute__((packed));

struct PipePartialExt {
  uint32_t old_etag;
  uint16_t x;
  uint16_t y;
  uint16_t w;
  uint16_t h;
} __attribute__((packed));

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

static void send_pipe_ack(uint8_t connection, opendisplay_pipe_reply_fn reply)
{
  uint8_t r[7] = { RESP_ACK, 0x81u, 0, 0, 0, 0, 0 };

  pipe_build_ack_payload(r + 2);
  reply(connection, r, sizeof(r));
  s_pipe.frames_since_ack = 0;
  s_pipe.ooo_acks_since_gap = 0;
}

static void send_pipe_nack(uint8_t connection, uint8_t err, opendisplay_pipe_reply_fn reply)
{
  uint8_t r[8] = { RESP_NACK, 0x81u, err, 0, 0, 0, 0, 0 };

  pipe_build_ack_payload(r + 3);
  reply(connection, r, sizeof(r));
  s_pipe.error = true;
  if (s_pipe.partial) {
    opendisplay_display_clear_etag();
  }
  opendisplay_display_abort();
}

static void send_pipe_start_nack(uint8_t connection, uint8_t err, opendisplay_pipe_reply_fn reply)
{
  uint8_t r[4] = { RESP_NACK, 0x80u, err, 0x00u };

  reply(connection, r, sizeof(r));
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

static void finish_and_refresh(uint8_t connection, const uint8_t *payload, uint16_t payload_len,
                               uint8_t end_opcode, opendisplay_pipe_reply_fn reply)
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
    reply(connection, nack, sizeof(nack));
    if (s_pipe.partial) {
      opendisplay_display_clear_etag();
    }
    opendisplay_display_abort();
    opendisplay_pipe_write_reset();
    return;
  }

  reply(connection, ack_end, sizeof(ack_end));
  k_msleep(20);
  if (opendisplay_display_direct_write_end_refresh(prep, prep_len, &refresh_ok) != 0) {
    reply(connection, nack, sizeof(nack));
    opendisplay_pipe_write_reset();
    return;
  }
  reply(connection, refresh_ok ? ack_ok : ack_to, 2u);
  opendisplay_pipe_write_reset();
}

extern "C" void opendisplay_pipe_write_start(uint8_t connection, const uint8_t *payload,
                                             uint16_t payload_len, opendisplay_pipe_reply_fn reply)
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
    send_pipe_start_nack(connection, OD_ERR_PIPE_START_BAD_HEADER, reply);
    return;
  }
  memcpy(&req, payload, sizeof(req));
  ver = req.version;
  flags = req.flags;
  req_w = req.req_window;
  req_n = req.req_ack_every;
  client_max_frame = req.client_max_frame;
  total_size = req.total_size;

  if (ver != PIPE_VERSION) {
    send_pipe_start_nack(connection, OD_ERR_PIPE_START_BAD_HEADER, reply);
    return;
  }
  if ((flags & ~(PIPE_FLAG_COMPRESSED | PIPE_FLAG_PARTIAL)) != 0u) {
    send_pipe_start_nack(connection, OD_ERR_PIPE_START_UNKNOWN_FLAG, reply);
    return;
  }

  compressed = (flags & PIPE_FLAG_COMPRESSED) != 0u;
  partial = (flags & PIPE_FLAG_PARTIAL) != 0u;

  if (partial) {
    struct PipePartialExt ext;
    uint8_t err = OD_ERR_PIPE_START_BAD_HEADER;

    if (payload_len < sizeof(req) + sizeof(ext)) {
      send_pipe_start_nack(connection, OD_ERR_PIPE_START_BAD_HEADER, reply);
      return;
    }
    memcpy(&ext, payload + sizeof(req), sizeof(ext));
    if (opendisplay_display_pipe_partial_arm(flags, ext.old_etag, ext.x, ext.y, ext.w, ext.h,
                                             total_size, &err)
        != 0) {
      send_pipe_start_nack(connection, err, reply);
      return;
    }
  } else {
    uint32_t expected = opendisplay_display_expected_dw_bytes();

    if (expected == 0u || total_size != expected) {
      send_pipe_start_nack(connection, OD_ERR_PIPE_START_SIZE_MISMATCH, reply);
      return;
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
  reply(connection, resp, sizeof(resp));

  if (partial) {
    if (opendisplay_display_pipe_partial_prepare() != 0) {
      s_pipe.error = true;
      opendisplay_display_clear_etag();
      opendisplay_display_abort();
      return;
    }
    return;
  }

  if (opendisplay_display_pipe_full_start(compressed, total_size) != 0) {
    s_pipe.error = true;
    opendisplay_display_abort();
  }
}

extern "C" void opendisplay_pipe_write_data(uint8_t connection, const uint8_t *payload,
                                            uint16_t payload_len, opendisplay_pipe_reply_fn reply)
{
  uint8_t seq;
  uint8_t *data;
  uint16_t plen;
  uint8_t fwd;
  uint8_t back;
  const uint8_t W = s_pipe.window;

  if (!s_pipe.active || s_pipe.error || payload == nullptr || payload_len < 1u) {
    return;
  }

  seq = payload[0];
  data = (uint8_t *)(void *)(payload + 1);
  plen = (uint16_t)(payload_len - 1u);
  if (plen > PIPE_REORDER_SLOT_SIZE) {
    send_pipe_nack(connection, 0x03u, reply);
    return;
  }

  fwd = (uint8_t)(seq - s_pipe.expected_seq);
  back = (uint8_t)(s_pipe.expected_seq - seq);

  if (fwd == 0u) {
    if (!pipe_consume_payload(data, plen)) {
      send_pipe_nack(connection, s_pipe.compressed ? 0x02u : 0x03u, reply);
      return;
    }
    s_pipe.expected_seq++;
    s_pipe.received_count++;
    s_pipe.frames_since_ack++;
    pipe_update_highest_seen(seq);

    while (s_reorder[pipe_slot(s_pipe.expected_seq)].occupied
           && s_reorder[pipe_slot(s_pipe.expected_seq)].seq == s_pipe.expected_seq) {
      PipeReorderSlot &slot = s_reorder[pipe_slot(s_pipe.expected_seq)];
      if (!pipe_consume_payload(slot.data, slot.len)) {
        send_pipe_nack(connection, s_pipe.compressed ? 0x02u : 0x03u, reply);
        return;
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
      send_pipe_ack(connection, reply);
      finish_and_refresh(connection, nullptr, 0u, 0x82u, reply);
      return;
    }
    if (s_pipe.frames_since_ack >= s_pipe.ack_every) {
      send_pipe_ack(connection, reply);
    }
    return;
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
      send_pipe_nack(connection, 0x03u, reply);
      return;
    }
    pipe_update_highest_seen(seq);
    if (!s_pipe.gap_open) {
      s_pipe.gap_open = true;
      send_pipe_ack(connection, reply);
    } else if (++s_pipe.ooo_acks_since_gap >= s_pipe.ack_every) {
      send_pipe_ack(connection, reply);
    }
    return;
  }

  if (back <= W) {
    if (!s_pipe.gap_open) {
      send_pipe_ack(connection, reply);
    } else if (++s_pipe.ooo_acks_since_gap >= s_pipe.ack_every) {
      send_pipe_ack(connection, reply);
    }
    return;
  }

  send_pipe_nack(connection, 0x04u, reply);
}

extern "C" void opendisplay_pipe_write_end(uint8_t connection, const uint8_t *payload,
                                           uint16_t payload_len, opendisplay_pipe_reply_fn reply)
{
  uint8_t nack[2] = { RESP_NACK, 0x82u };
  bool incomplete;

  if (!s_pipe.active) {
    reply(connection, nack, sizeof(nack));
    return;
  }
  if (s_pipe.error) {
    reply(connection, nack, sizeof(nack));
    opendisplay_display_abort();
    opendisplay_pipe_write_reset();
    return;
  }

  send_pipe_ack(connection, reply);

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
    reply(connection, nack, sizeof(nack));
    if (s_pipe.partial) {
      opendisplay_display_clear_etag();
    }
    opendisplay_display_abort();
    opendisplay_pipe_write_reset();
    return;
  }

  finish_and_refresh(connection, payload, payload_len, 0x82u, reply);
}
