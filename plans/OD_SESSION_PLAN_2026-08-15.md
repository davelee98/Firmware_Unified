# Promote the BLE session subsystem to `shared/core/od_session.c`

## Context

Session auth + AES-CCM is the last large block of protocol logic reimplemented three times.
It is ~940 lines on `esp32-idf` (`src/encryption.cpp`) and ~1476 lines inside
`targets/nordic-zephyr/src/opendisplay_pipe.c`, with a third near-verbatim copy on Silabs. The
KDF, handshake shape, 30 s challenge window, 10-per-60 s lockout and ±32 replay window are
**byte-identical across all four repos and maintained by hand** — `DIVERGENCE_MATRIX.md` §6
confirms it, and `AUDIT_NORDIC_ZEPHYR_2026-08-14.md:259` records the flagship failure of that
arrangement: Nordic's port silently downgraded the auth-proof compare from constant-time to
`memcmp`, and nothing failed. **The same regression is still live on Silabs**
(`targets/efr32bg22-slc/opendisplay_pipe.c:646`) and appears in no document.

Promoting it collapses three copies to one, and closes six real defects that are currently
"deferred" precisely because fixing them three times was not worth it.

Scope confirmed with the user: od_session owns the handshake, KDF, nonce/replay **and** the CCM
envelope; the crypto HAL uses a prepared-key handle; session lands **before** `od_dispatch`
(which needs correct session state to gate on); and `esp32-idf` + `nordic-zephyr` are swapped in
one step, verified together. `efr32bg22-slc` is explicitly **not** in this step.

## Key findings that shape the design

**PSA's native CCM is available and simply unused.** `targets/nordic-zephyr/build/zephyr/zephyr/.config`
has `# CONFIG_PSA_WANT_ALG_CCM is not set`, which is why Nordic and Silabs hand-roll RFC 3610
over PSA AES-ECB with a `psa_import_key`/`psa_destroy_key` **per 16-byte block**. Both drivers
ship in this NCS tree — Oberon software (`oberon_aead.c` → `ocrypto_aes_ccm_*`, handles
`PSA_ALG_CCM`) and CRACEN hardware (`cracen_psa_aead.c`). Enabling one Kconfig replaces ~120
lines of hand-rolled crypto per target and gets hardware AES on the nRF54L.
**Consequence: `OD_CRYPTO_SOFT_CCM` is NOT needed in this step.** Both targets get native CCM
(mbedTLS on ESP32, PSA on Nordic). Revisit only if Silabs turns out to lack it.

**`od_span_t` is const-only** (`shared/core/od_span.h:41-44`). Every shared module so far only
reads. The CCM encrypt path must write, so this promotion forces a mutable-span decision —
see below. This is the one genuinely new primitive the promotion needs.

**ESP32's `replay_window_index` is a function-local `static`** (`src/encryption.cpp:194`), not a
struct field, so `clearEncryptionSession()` cannot reset it. Both other targets have `replay_idx`
in the struct. It must become state.

**Nordic advances the replay window before verifying the CCM tag** (`opendisplay_pipe.c:500` vs
`:505`), so a forged frame can move `last_seen_counter` and lock out legitimate reordered
frames. Silabs has this right, split into `nonce_replay_check()` (pure predicate, `:362`) and
`nonce_replay_advance()` (after tag verify, `:397`). `DIVERGENCE_MATRIX` §6 calls these
"identical" — it is wrong; take the Silabs split.

## Design

### 1. `shared/hal/od_hal_crypto.h` — the new third shared HAL

Prepared-key for the hot path, one-shot for the KDF. The split is not cosmetic: AEAD runs
per-frame under the **session** key (ESP32 caches `mbedtls_ccm_context`; PSA targets can import
once instead of per block), while CMAC/ECB run a handful of times per handshake under the
**master** key and must take the key explicitly.

```c
enum od_hal_crypto_result {
    OD_HAL_CRYPTO_OK = 0,
    OD_HAL_CRYPTO_AUTH_FAILED,   /* tag mismatch — the caller MUST branch on this alone */
    OD_HAL_CRYPTO_NO_KEY,        /* aead used before _key_set */
    OD_HAL_CRYPTO_UNSUPPORTED,   /* backend lacks the primitive (cf. OD_HAL_WDT_ARM_UNSUPPORTED) */
    OD_HAL_CRYPTO_ERROR
};

enum od_hal_crypto_result od_hal_crypto_init(void);                       /* psa_crypto_init etc. */
enum od_hal_crypto_result od_hal_crypto_aead_key_set(const uint8_t key[16]);
void                      od_hal_crypto_aead_key_clear(void);
enum od_hal_crypto_result od_hal_crypto_aead_encrypt(od_span_t nonce13, od_span_t aad,
                                                     od_span_t plain,
                                                     od_mut_span_t cipher_out,
                                                     uint8_t tag_out[12]);
enum od_hal_crypto_result od_hal_crypto_aead_decrypt(od_span_t nonce13, od_span_t aad,
                                                     od_span_t cipher, const uint8_t tag[12],
                                                     od_mut_span_t plain_out);
enum od_hal_crypto_result od_hal_crypto_cmac(const uint8_t key[16], od_span_t msg,
                                             uint8_t out[16]);
enum od_hal_crypto_result od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16]);          /* KDF only */
enum od_hal_crypto_result od_hal_crypto_random(od_mut_span_t out);
```

Enum returns rather than `int`: both existing shared HALs use enums, and `AUTH_FAILED` must be
distinguishable from `ERROR` because only the former feeds the 3-strike teardown.

This **supersedes** the one-shot signatures in `docs/SHARED_API_DESIGN.md:363-372`. That doc
already flags `od_hal_crypto` as unverified (`:153`), and CLAUDE.md decision 14 says headers
beat design docs — so update the doc as part of the work.

Add to `shared/core/od_span.h`:
```c
typedef struct od_mut_span { uint8_t *p; size_t n; } od_mut_span_t;
static inline od_mut_span_t od_mut_span_make(uint8_t *p, size_t n);
static inline od_span_t     od_mut_span_ro(od_mut_span_t s);   /* narrow to read-only */
```

Implementations: `targets/esp32-idf/hal/od_hal_crypto.c` (mbedTLS, keeps the cached
`mbedtls_ccm_context` behind `_key_set`/`_key_clear`), `targets/nordic-zephyr/src/od_hal_crypto.c`
(PSA; `CONFIG_PSA_WANT_ALG_CCM=y` added to `prj.conf`, key imported once in `_key_set`).

### 2. `shared/core/od_session.h` / `.c`

Caller-owned state, no singletons, `now_ms` passed in, NULL-tolerant everywhere — the
`od_watchdog` / `od_config` pattern exactly.

```c
#define OD_SESSION_KEY_LEN 16
#define OD_SESSION_ID_LEN   8
#define OD_REPLAY_WINDOW_HALF 32u          /* the ±32 accept window */

struct od_session {
    bool     authenticated;
    uint8_t  session_key[16];
    uint8_t  session_id[8];
    uint64_t tx_counter;          /* outbound */
    uint64_t rx_high_water;       /* inbound high-water */
    uint64_t rx_bitmap;           /* 64 counters at and below rx_high_water */
    uint32_t session_start_ms;
    uint8_t  integrity_failures;
    uint8_t  auth_attempts;
    uint32_t last_auth_ms;        /* survives a clear, by design — lockout must persist */
    uint8_t  client_nonce[16], server_nonce[16], pending_server_nonce[16];
    uint32_t server_nonce_ms;
};
```

**The replay bitmap replaces `uint64_t replay_window[64]` (512 B → 8 B)**, which
`MEMORY_CONSTRAINTS.md:124-128` requires and ties to closing the `diff == 0` hole in the same
change. Bit *i* means "counter `rx_high_water - i` has been seen", bit 0 = `rx_high_water`:

- `check(c)`: reject if `c > rx_high_water + OD_REPLAY_WINDOW_HALF`; if `c <= rx_high_water`,
  let `d = rx_high_water - c`, reject if `d >= 64`, **reject if bit `d` is set** — this is the
  `diff == 0` fix, because `d == 0` is now an ordinary set-bit test rather than a skipped case.
- `advance(c)`: on `c > rx_high_water`, shift the bitmap left by `c - rx_high_water` (≥64 ⇒ 0),
  set bit 0, update `rx_high_water`; else set bit `rx_high_water - c`.

Split into two functions so the target calls `check` before decrypt and `advance` **only after**
the tag verifies — Silabs' ordering, not Nordic's.

`SHARED_API_DESIGN.md:711` wants `_Static_assert(OD_PIPE_MAX_W <= OD_REPLAY_WINDOW_HALF)` in
shared/, but **`OD_PIPE_MAX_W` is not a shared symbol yet** — it is target-local
(`targets/esp32-idf/src/structs.h:51,55` = 16 or 32; `targets/nordic-zephyr/src/opendisplay_pipe_write.cpp:9`
= 32). So `od_session.h` defines `OD_REPLAY_WINDOW_HALF` now, and the assert goes in **each
target** beside its `PIPE_MAX_W` (both values are ≤ 32 today, so it passes), moving into
`od_pipe.c` when that lands. Writing it in `od_session.h` now would not compile.

Public API:
```c
void od_session_reset(struct od_session *s);                       /* clears key material */
bool od_session_is_live(const struct od_session *s, uint32_t now_ms, uint16_t timeout_s);
enum od_session_auth od_session_handle_authenticate(struct od_session *s,
        od_span_t body, uint32_t now_ms, const uint8_t master_key[16],
        const uint8_t device_id[4], od_mut_span_t reply_out, size_t *reply_len,
        struct od_session_report *report);
enum od_session_result od_session_open(struct od_session *s, od_span_t frame, od_origin_t origin,
        od_mut_span_t plain_out, size_t *plain_len, uint32_t now_ms);   /* decrypt one inbound */
enum od_session_result od_session_seal(struct od_session *s, od_span_t plain, od_origin_t origin,
        od_mut_span_t frame_out, size_t *frame_len);                    /* encrypt one outbound */
bool od_session_derive_tls_psk(const uint8_t master_key[16], uint8_t psk_out[16]);
```

`od_origin_t { OD_ORIGIN_BLE, OD_ORIGIN_LAN_PLAIN, OD_ORIGIN_LAN_TLS }` is a **parameter**, never
a global: `OD_ORIGIN_LAN_TLS` bypasses the envelope entirely (protocol SECTION 9 rule 4).

`struct od_session_report` carries what the targets currently log: `auth_status`, `rate_limited`,
`challenge_expired`, `proof_mismatch`, `replay_rejected`, `tag_failed`, `integrity_strikes`,
`session_cleared_reason`. shared/ never logs.

**Stay in C.** CLAUDE.md decision 1 rule 3 names `od_session.c` as the C-vs-C++ revisit point
because of "nested resource lifetimes with several failure exits". The prepared-key HAL removes
exactly that: every crypto resource (`mbedtls_ccm_context`, `psa_key_id_t`) now lives behind
`_key_set`/`_key_clear` **inside the target**, so od_session.c holds no resource across a
fallible step and needs no cleanup ladder. Record this in the header — it retires the open
question rather than deferring it again.

### 3. What stays in each target

Device identity (`esp_efuse_mac_get_default` vs `hwinfo_get_device_id`) — passed in as
`device_id[4]`, already a parameter on Nordic. The clock. All logging, via a per-target
`od_session_app.{c,h}` owner modelled on `targets/nordic-zephyr/src/od_watchdog_app.h`. The
plaintext-exemption gate and origin plumbing (they move later, with `od_dispatch`). Link
teardown (`session_guard.cpp`, `abortToKnownState`) — that is CONNECTION_POLICY, not session
crypto, and must not be dragged in. `secureEraseConfig`, `checkResetPin`, `isEncryptionEnabled`.

### 4. Build wiring

New tier in `shared/sources.cmake`:
```cmake
set(OD_SHARED_SOURCES_HAL_CRYPTO "${CMAKE_CURRENT_LIST_DIR}/core/od_session.c")
```
composed into `OD_SHARED_SOURCES`. `targets/esp32-idf/main/CMakeLists.txt:64` takes the aggregate,
so the shared source reaches it with **no edit** — but the HAL implementation does not: each
`hal/*.c` is listed explicitly at `:48-55`, so `od_hal_crypto.c` needs a line there.
`targets/nordic-zephyr/zephyr/CMakeLists.txt:253` names tiers explicitly and must add
`${OD_SHARED_SOURCES_HAL_CRYPTO}`. Silabs takes PURE only and is unaffected — which is what
keeps it out of this step even though its `od_advert` is already linked.
While in `sources.cmake`, delete the **stale duplicate `core/od_advert.c` line** in the arrival-
order comment (it was promoted early; the original line was never removed).

## Commit sequence

1. **`test(session): capture auth vectors from hardware`** — **must come first.**
   `TEST_OWNERSHIP.md` warns that once shared/core replaces a target's logic there is no
   untouched reference left. Capture a full handshake + several encrypted frames from a flashed
   `xiao_nrf52840` and an ESP32-S3 into `tests/vectors/session.json`. **Use a throwaway master
   key on a device that will be re-provisioned, and scrub addresses — this is a public repo and a
   captured session key is a real leak.** Note `replay_vectors.py` already has a 0x0050 builder
   for both h2d shapes and can check those immediately; d2h replies (including the 19-byte
   server proof) are structurally uncheckable host-side today and get `expect.note`.
2. **`feat(shared): od_hal_crypto -- the crypto seam`** — header + `od_mut_span_t`. No consumer.
3. **`feat(shared): od_session -- auth, KDF, nonce/replay, CCM envelope`** — the module plus
   `tests/host/session_test.c`. Nothing calls it yet; mirrors `ea5bf7d` / `3f778a9`.
4. **`feat(esp32,nordic): implement od_hal_crypto`** — mbedTLS and PSA backends,
   `CONFIG_PSA_WANT_ALG_CCM=y`. Verify Nordic still builds and measure the flash delta.
5. **`feat(esp32,nordic): promote od_session`** — both targets swapped; `encryption.cpp` reduced
   to identity/storage helpers, Nordic's ~600 session lines deleted from `opendisplay_pipe.c`.
6. **`docs: od_session promoted`** — CLAUDE.md status, `SHARED_API_DESIGN.md` HAL signatures,
   the three `DIVERGENCE_MATRIX` rows this closes, and the stale line numbers agents found.

## Testing

`tests/host/session_test.c`, registered the standard three lines in `tests/host/CMakeLists.txt`,
using the existing hand-rolled `CHECK`/`CASE` macros. The crypto HAL is faked by **defining its
symbols at file scope** exactly as `watchdog_test.c` does for `od_hal_wdt` — a deterministic
fake (counter-based "random", identity-ish CMAC) plus a failure-injecting mode to force
`AUTH_FAILED` and prove the 3-strike teardown and that a failed tag does **not** advance the
replay window.

Differential reference: transcribe the **shipped** KDF and replay check into the test (the
`advert_test.c` pattern) and sweep both against the promoted code — a vector table derived from
the new code proves only self-consistency.

Must-have cases: the bitmap replay (in-window, out-of-window, exact-counter `d == 0` **now
rejected**, 64-boundary, wrap past 64); challenge expiry at 30 s; the 10/60 s lockout including
that it survives `od_session_reset`; envelope round-trip and every truncation of it; origin
gating (`OD_ORIGIN_LAN_TLS` must not encrypt); NULL-safety on every entry point.

**Stand up `tests/fuzz/` in this step.** It does not exist, and `TEST_OWNERSHIP.md` requires
fuzz coverage for anything reachable pre-auth — the handshake *is* the pre-auth surface. One
libFuzzer target over `od_session_handle_authenticate` and one over `od_session_open`, ASan +
UBSan, seed corpus checked in.

## Verification

- Host: `cmake -S tests/host -B <dir> && ctest --test-dir <dir>` (gcc **and** clang, `-Werror`),
  plus `python tests/host/replay_vectors.py tests/vectors`.
- Boundary: the `shared-boundary.yml` grep — `od_session.c` must pull in no vendor header.
- Builds: `targets/esp32-idf/build.sh` (10 fragments), `targets/nordic-zephyr/build.sh --all`
  (3 boards), and `compat/ratchet.sh` + `tools/sdkconfig_baseline.sh`.
- **RAM/flash delta measured on both targets**, and stated: the bitmap should return ~504 B per
  target; `CONFIG_PSA_WANT_ALG_CCM` costs flash that must be quantified, not assumed.
- **Hardware (Gate 2), the only thing that counts**: on a flashed `xiao_nrf52840` and an
  ESP32-S3 — authenticate, complete an encrypted image upload, do an encrypted config write,
  confirm a wrong key is rejected, confirm the session survives a reorder burst under PIPE
  (this is what the ±32 window exists for), and confirm re-auth after a session timeout.

## Risks

- **The 19-byte STEP-2 server proof is undocumented** (spec says 3 bytes). It is real and shipped;
  the host depends on it. Preserve it byte-for-byte and fix the spec — do not "correct" the code.
- **Replay-window resize is security-sensitive.** The bitmap and the `diff == 0` fix must land
  together; a half-done version is worse than either.
- **`_key_set` lifetime.** ESP32 caches a CCM context and Nordic will cache a PSA key id. Both
  must be cleared on every teardown path or a key outlives its session. Enumerate the paths
  (agent found 8 on ESP32) and route them all through one call.
- **Nordic's replay-ordering change alters behaviour**, tightening it. A client that relied on the
  looser ordering will now see rejections — expected, but watch for it during hardware test.
- **Both targets swapped in one commit** (the user's choice) means a hardware regression is
  harder to attribute. Mitigate by keeping steps 4 and 5 separate and building between them.
- `MAX_CONFIG_SIZE` is still 2048 on Silabs vs 4096 elsewhere — out of scope here, but it means
  the RAM pressure that makes the bitmap mandatory for BG22 has not actually landed yet.
