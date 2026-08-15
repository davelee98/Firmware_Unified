# Promote the BLE session subsystem to `shared/core/od_session.c`

## Context

Session auth + AES-CCM is the last large block of protocol logic reimplemented three times:
~940 lines on `esp32-idf` (`src/encryption.cpp`) and ~1476 inside
`targets/nordic-zephyr/src/opendisplay_pipe.c`, with a third near-verbatim copy on Silabs. The
KDF, handshake shape, 30 s challenge window, 10-per-60 s lockout and ±32 replay window are
**byte-identical across all four repos and maintained by hand**. `AUDIT_NORDIC_ZEPHYR_2026-08-14.md:259`
records the flagship failure of that arrangement: Nordic's port silently downgraded the auth-proof
compare from constant-time to `memcmp`, and nothing failed. **The same regression is still live on
Silabs** (`targets/efr32bg22-slc/opendisplay_pipe.c:646`) and appears in no document.

One copy, and seven defects closed that were each deferred because fixing them three times wasn't
worth it.

**Decisions (settled):** od_session owns the handshake, KDF, nonce/replay **and** the CCM
envelope; the crypto HAL uses prepared keys; session lands **before** `od_dispatch`.
`efr32bg22-slc` is **not** in this step.

**Target sequencing, stated once so nothing below contradicts it.** This promotion covers *both*
`esp32-idf` and `nordic-zephyr` — a deliberate deviation from CLAUDE.md:171's "one target at a
time", taken by the user because the two are verified together on bench. Within it, **ESP32 swaps
first** (C5), Nordic second (C6), which *is* CLAUDE.md's order and keeps the authority target as
the reference. Each swap is still its own commit with its own hardware pass; "one step" means one
promotion, not one commit. C1 touches both targets together because it is a behaviour-preserving
repoint of existing crypto onto a new seam — if that proves uncomfortable, split it by target
before landing, not after.

## Relationship to the dispatch plan

`plans/PLAN_OD_DISPATCH_2026-08-14.md` has been revised and **already assumes session-first** —
its §7 opens with "C0 — `od_session` lands first, per its own plan" and §3 builds on
`od_session_seal()`. The global-state placeholder (`bool od_session_alive(void)`) that an earlier
draft declared is gone, so there is nothing to amend and no "§5-S3" to find. The two plans agree.

Its one substantive decision carries into this one: **the handshake returns its response bytes
rather than sending them** (Nordic's shape), because a handler that sends behind the dispatcher's
back can't be routed by origin or counted for auth abuse.

### The interlock — what both plans must keep saying the same thing about

**Agreed and verified (do not change one without the other):**

| | Value | Where |
|---|---:|---|
| Response/session payload | **222** | dispatch §2 table; `OD_SESSION_PAYLOAD_MAX` |
| Complete plain frame `[cmd:2][payload]` | **224** | dispatch §2; `od_session_seal` input |
| CCM plaintext `[len:1][payload]` | **223** | dispatch D-C; `OD_SESSION_PLAIN_MAX` |
| Envelope after the cmd bytes | **251** | dispatch D-C; `OD_SESSION_ENVELOPE_MAX` |
| Sealed application value | **253** | dispatch §2 / D-C2; `OD_SESSION_SEALED_MAX` |
| RX slot width / admission | **256** | dispatch D-F; ARCHITECTURE.md § target state |

Dispatch's §8 already tests these by name — "225 is rejected as `OD_SESSION_SEAL_TOO_LONG` before
sealing (so no nonce is burned)" and "a 252-byte envelope is rejected before the cipher is
touched" — which is exactly this plan's preflight and reject-before-CCM rules.

**`OD_TX_FRAME_MAX` (256) and `OD_SESSION_SEALED_MAX` (253) are deliberately different numbers.**
The first is the TX *slot*, sized to what the radio will take; the second is the largest thing this
module may *emit* into it. Do not unify them — that is the admission-vs-generation distinction, and
collapsing it is how a 253-byte cap silently becomes 256.

**The API surface dispatch depends on**, so both docs use one spelling: the gate calls
`od_session_security_enabled()` and `od_session_alive()`; the `0x50` handler calls
`od_session_authenticate()` and sends the bytes it returns; the decrypt path calls
`od_session_open()`; the seal predicate calls `od_session_seal()`; every teardown route calls
`od_session_clear()`. **Dispatch's "activity stamp" IS `od_session_touch()`** — one mechanism, with
the origin scoping (stamp has no origin test, the abuse counter is BLE-only, dispatch §5) staying
on the dispatcher side.

**Scratch ownership changes hands mid-stream, and that is fine if it is stated.** D-C makes
dispatch the owner of the shared 223-byte decrypt / 253-byte encrypt scratch — but the session
swaps here land *before* dispatch's C1–C4, so in the interim each target passes its own buffers to
`od_session_open`/`seal`. The function signatures do not change at that handover; only who owns the
memory does.

**Numbering is one continuous sequence across both plans:** this plan is **C0–C7**, the dispatch
plan continues at **C8–C12** (renumbered 2026-08-15 from its original C1–C5, which collided with
these). A bare commit label therefore means exactly one thing in either document. The one
exception is the "Corrections to earlier drafts" table in the dispatch plan, whose left column
cites *rev-4's* labels historically and is deliberately left alone.

## Findings that shaped the design

### THE REFERENCE IS `../Firmware/src/`, NOT THIS REPO'S IMPORT

**Read this before writing a line of C4.** `targets/esp32-idf/src/encryption.cpp` is an *older
import*. Upstream `Firmware` — which CLAUDE.md names the authority — has moved on, and every
mistake found in this plan so far came from treating the import as the shipped algorithm. A full
diff of the subsystem (upstream vs the import at `80cc028`, taken 2026-08-15) found **seven items
of genuine drift**, three of which had already been designed wrongly here:

1. **`nonce_window.h` (206 lines, new).** The replay window extracted as a zero-dependency state
   machine with an 816-line host test at `../Firmware/tools/test_nonce_window.cpp`. 512 B ring →
   32 B bitmap. **C4 ports it as `shared/core/od_nonce_window.h` with no logic change**, and ports
   its test as the differential oracle. Do not write a replacement — see the replay section for
   the two ways the one designed here was worse.
2. **`NonceResult` is four-valued:** `OK`, `REPLAY`, `OUT_OF_WINDOW`, `BAD_SESSION`. The session-id
   mismatch became a nonce result rather than a separate branch.
3. **Nonce failures no longer count toward `integrity_failures`.** Only the CCM tag is a tamper
   oracle; nonce failures are evidence of a lossy link. **This plan had it backwards** — `REPLAY`
   and `WRONG_SESSION` counted, which lets ordinary packet loss tear a live session down after
   three frames. Same self-DoS shape this plan flags elsewhere, built straight in.
4. **`BAD_SESSION` routes to that same non-counting path**, deliberately: a session-id mismatch is
   what a stale client sends after the device re-authenticated.
5. **`decryptCommand` gained a `NonceResult *reason_out`** so the caller can separate a
   nonce-reason rejection — ordinary loss, which must **not** draw a fatal NACK on the pipe path —
   from a tag failure. This plan collapsed both into one result and asserted the caller could not
   act differently. It can, and must; `report->nonce_reason` carries it.
6. **`resetNonceState()`** resets the four nonce fields together and zeroes the device's **own
   outbound** counter on re-auth: carrying it across while the client restarts at 0 reproduces
   keystream reuse against itself.
7. **Nonce logging is rate-limited per site** (5 s each), because once these stop counting toward
   the strike limit nothing else throttles a peer driving them, and out-of-window fires routinely
   on a lossy link.

**Not drift — correct adaptations, leave them:** `securityConfig` as a reference (the `od_config`
promotion), `getChipIdHex()` in char-buffer form rather than Arduino `String`, `<Arduino.h>`
dropped, and `session_guard.cpp` on `od_hal_time`. `session_guard.h` is byte-identical.

**Unchanged upstream, so these findings stand:** the wrap-unsafe session timeout, and the
idle-gap rate limiter.

**The rule for C4:** when this plan and the import disagree, check `../Firmware/src/` before
assuming either. The import is a snapshot; the authority is upstream.

**PSA's native CCM is available and simply switched off.** `prj.conf:99-106` enables only CMAC and
ECB; the generated `build-*/zephyr/generated/library_nrf_security_psa/nrf-psa-crypto-config.h` shows
`/* #undef PSA_WANT_ALG_CCM */`. The symbol is live (Kconfig prints "not set" only for known
symbols) and the Oberon driver behind it implements AES-CCM (`oberon_aead.c` → `ocrypto_aes_ccm_*`);
CRACEN provides hardware CCM on nRF54L. So Nordic/Silabs hand-roll RFC 3610 over PSA ECB — with a
`psa_import_key`/`psa_destroy_key` **per 16-byte block** — because of one missing `prj.conf` line,
not a missing capability. *Check the generated header, never the source, to answer this.*

**Use `OD_STATIC_ASSERT`, not `_Static_assert`.** `opendisplay_structs.h:258-267` defines it and
picks the right spelling per language level, because the host gate is `-std=c99` with
`CMAKE_C_EXTENSIONS OFF` (`tests/host/CMakeLists.txt:23-27`). Raw `_Static_assert` is C11.

**ESP32's session timeout is not wrap-safe** — a new defect, in no doc.
`encryption.cpp:266-267` divides *both* timestamps by 1000 and then subtracts:
```c
uint32_t currentTime = od_now_ms() / 1000;
uint32_t sessionAge  = currentTime - (encryptionSession.session_start_time / 1000);
```
Unsigned subtraction is wrap-safe only in the domain the wrap happens in. Across the 49.7-day
`uint32_t` ms rollover `currentTime` restarts near 0 while the divided start stays ~4.29e6, so
`sessionAge` wraps huge and the session is torn down. Nordic's ms-domain
`(now_ms - session_start_ms) > timeout_ms` (`opendisplay_pipe.c:186`) is correct — promote that.

**`od_span_t` is const-only** (`od_span.h:41-44`); every shared module so far only reads. The CCM
path writes, so out-buffers are `(ptr, cap)` pairs with the cap checked before any write.

**The `diff == 0` fix is host-compatible — checked, not assumed.** The risk was real: today a
resend of identical sealed bytes is *accepted* (`diff == 0` skips the seen-scan on all three
targets); after the fix it is a REPLAY, and three of them tear the session down
(`encryption.cpp:778-783`), so a retry layer that re-sent rather than re-sealed would self-DoS into
forced re-auth mid-transfer. **py-opendisplay does not do that.** `_encrypt_frame()` advances the
nonce counter on *every* transmission and says so
(`py-opendisplay/src/opendisplay/device.py:758-770`); PIPE retransmissions re-enter
`_write_pipe_frame()` and therefore re-seal; the transport issues one GATT write and never retries
a previously sealed buffer (`transport/connection.py:364`). It is pinned by an existing test —
`tests/unit/test_pipe_write_sender.py:532`, `test_encrypted_frames_and_fresh_nonce_on_retransmit`,
which asserts 3 sends + 1 retransmit = 4 encryptions and that the retransmit carries the higher
nonce. Verified against v7.14.0 (the version `manifest.json` pins) and current `main`. No host
release is required and this does not gate C5.

## Design

### 1. `shared/hal/od_hal_crypto.h` — the third shared HAL

**Four-valued enum return, not `int`.** CCM decrypt has two failure modes that mean opposite
things: a tag mismatch is an attack and *must* count toward the 3-strike teardown; an engine
failure (PSA slot exhaustion, alloc failure) is local and counting it converts a transient OOM
into forced re-authentication — a self-inflicted DoS. All three targets collapse both into
`false` today, so the distinction has never existed; a shared strike counter makes it matter.
`int` carrying vendor status would couple `shared/` to mbedTLS's and PSA's numbering.

```c
enum od_hal_crypto_status {
    OD_HAL_CRYPTO_OK = 0,
    OD_HAL_CRYPTO_AUTH_FAILED,   /* decrypt only: tag did not verify. NOT an engine fault. */
    OD_HAL_CRYPTO_UNSUPPORTED,
    OD_HAL_CRYPTO_ERROR          /* engine/resource failure; the target has already logged it */
};
```
Precedent: `enum od_hal_wdt_arm_result` (`od_hal_wdt.h:94-100`) exists for the same reason —
preserving a distinction a bool would erase. Vendor codes stay debuggable because HAL
implementations live in `targets/` and may log the raw `psa_status_t`/mbedTLS `ret` at the point
of failure, exactly as `encryption.cpp:425-431` does now.

**The prepared key is a slot, and the key never enters `struct od_session`.**
**Every entry point self-initialises; there is no init ordering to get wrong.** Step 1 needs
`random` and `cmac` and touches **no slot**, so a HAL that initialised the engine inside `key_set`
would fail the very first challenge forever — Nordic's `od_random` returns false when
`!s_crypto_ready` (`opendisplay_pipe.c:303-309`), and `crypto_init_once()` is called from
`authenticate_handle` (`:689`) precisely to beat it. State the contract in the header: *every*
function calls the equivalent of `crypto_init_once()` first, idempotently. Host case: the first
call made against a fresh fake is `random`, and it must succeed.

```c
/* Tag length is a property of the SLOT, not of the call. On PSA the key policy pins
 * PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 12) at import, so a call with any other tag_len
 * fails NOT_PERMITTED on Nordic while succeeding on mbedTLS — one HAL, two behaviours, found
 * only on hardware. Fix the length at the contract instead. */
#define OD_HAL_CRYPTO_TAG_LEN 12u
#ifndef OD_HAL_CRYPTO_KEY_SLOTS
#define OD_HAL_CRYPTO_KEY_SLOTS 1u
#endif
typedef uint8_t od_hal_crypto_slot_t;

/* IDEMPOTENT AND SELF-REPAIRING: MUST release whatever the slot already held. That is what
 * makes a lost clear survivable instead of fatal. */
enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot, const uint8_t key[16]);
void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot);
```
An embedded `psa_key_id_t` would make `memset(&session, 0, sizeof session)` — *literally* how both
Nordic and Silabs clear the session today (`opendisplay_pipe.c:143-146`,
`efr32bg22-slc/opendisplay_pipe.c:92-95`) — silently drop a live PSA handle. PSA slots are a finite pool, so a few hundred re-authentications would exhaust
them and present months later as "auth stops working after a while". The slot form makes that
impossible to hold: `shared/` stores one `uint8_t`, the struct stays trivially zeroable, and the
vendor context stays entirely in `targets/`.

**AEAD takes ONE combined `ciphertext || tag` buffer, not two pointers.** PSA consumes and emits
them contiguously; separate `cipher`/`tag` pointers would make adjacency an unwritten precondition
the signature cannot express or enforce, and the HAL would need a bounce buffer on every Nordic
call. A combined buffer states the layout in the type. mbedTLS takes separate pointers and is
happy with `tag = buf + cipher_len`. Note this makes the C1 migration **not** purely mechanical
on Nordic: `encrypt_response_payload()` currently encrypts into the output while collecting the
tag in a separate stack `uint8_t tag[12]` (`opendisplay_pipe.c:529`), so C1 must change that
envelope caller, not just the crypto forwarder.

```c
/* `ct` receives ciphertext || tag and must have room for plain_len + OD_HAL_CRYPTO_TAG_LEN.
 * *ct_len is set to plain_len + OD_HAL_CRYPTO_TAG_LEN on success.
 * NO tag_len PARAMETER — it is the fixed constant above. A per-call length would re-admit exactly
 * the cross-target disagreement the constant exists to prevent. */
enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len,
        uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len);

/* `ct` is ciphertext || tag of ct_len bytes (ct_len > OD_HAL_CRYPTO_TAG_LEN). Returns
 * AUTH_FAILED — never ERROR — when the tag fails.
 * ON ANY NON-OK RETURN `plain` IS UNDEFINED AND MUST NOT BE READ: CCM is decrypt-then-verify. */
enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len,
        uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len);

/* One-shot, key-per-call: CMAC runs ≤3 times per handshake under TWO different keys (master for
 * the challenge, session for the id and proof), so a prepared handle would buy only a lifetime
 * to get wrong. */
enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16]);
enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16]);   /* KDF finalisation only */
/* MUST be cryptographically secure — it fills the server nonce. An implementation that cannot
 * meet that returns ERROR; it does NOT fall back to a weak source, which encryption.cpp:590-600
 * does on the nRF arm and which silently downgrades the whole handshake. */
enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len);
```

**No constant-time compare in the HAL** — four lines of portable C with a `volatile` accumulator,
kept in `od_session.c` so it is written once and the host test can assert it directly.

Header conventions per `od_hal_wdt.h`: division-of-labour paragraph, "no ISR context", and the
`extern "C"` guard with its rationale.

**Implementations.**
*ESP32* `hal/od_hal_crypto.c` (new; add to the explicit list at `main/CMakeLists.txt:48-55`):
file-static `mbedtls_ccm_context s_slots[]`; `key_set` = free-if-ready + `ccm_init` +
`mbedtls_ccm_setkey(..., 128)` lifted from `encryption.cpp:292-304`. **Map
`MBEDTLS_ERR_CCM_AUTH_FAILED` → `AUTH_FAILED`, everything else → `ERROR`** — the mapping today's
code throws away at `:425`. `cmac`/`aes_ecb`/`random` from `:313-360`, `:436-438`. The
`#ifdef TARGET_NRF` CC310 arm (`:440-600`) is **not ported** — it leaves with migration step 4.

*Nordic* `src/od_hal_crypto.c` (new) + `CONFIG_PSA_WANT_ALG_CCM=y` in `prj.conf`. Native
`psa_aead_*`. **Import with `PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 12)`** — the wire tag is
12 bytes and PSA's key policy pins the algorithm *including* tag length, so plain `PSA_ALG_CCM`
(16-byte tag) fails every operation with `PSA_ERROR_NOT_PERMITTED`. **This is the single most
likely first-flash failure.** Map `PSA_ERROR_INVALID_SIGNATURE` → `AUTH_FAILED`. `crypto_init_once`
(`opendisplay_pipe.c:132-140`) moves here; `cmac`/`ecb`/`random` (`:193-235`, `:303-309`) move
verbatim; the hand-rolled `od_ccm_*` (`:317-441`, ~125 lines) is deleted — **but only after** the
host differential test has transcribed it (§6) and hardware has passed.

`psa_aead_encrypt` writes `ciphertext||tag` contiguously. Rather than a bounce buffer, have
`od_session.c` lay the envelope out so they are already adjacent (`[cmd:2][nonce:16][ct][tag:12]`
— they *are* adjacent on the wire) and pass `tag = cipher + cipher_len`. mbedTLS takes separate
pointers and doesn't care that they abut.

**Two different failures, two different answers — they are not the same policy.**
*PSA CCM does not work* (wrong results, key-policy rejection, a driver bug on a board): revert to
soft CCM **inside the Nordic HAL file**, `od_hal_crypto.h` unchanged, and the plan proceeds. That
is a local contingency and it is why the HAL shape is worth having regardless.
*PSA CCM works but costs unacceptable flash*: that is the `OD_CRYPTO_SOFT_CCM` trigger in §2 —
a **shared** soft path every target links, not a per-target `#ifdef`. Measure in C1 and say which
one you are in.

### 2. `OD_CRYPTO_SOFT_CCM` — defer, with the trigger written down

Both targets here get native CCM; Silabs is out of scope and its PSA has CCM too. Writing
`shared/core/od_ccm_soft.c` now would add a shared source **no consumer compiles** — the exact
shape `sources.cmake:12-14` exists to prevent — and ship untested crypto. Pay the design cost now:
a soft CCM needs a *prepared ECB* or it reintroduces the per-block import this whole change
removes, so `od_hal_crypto.h` names that future call (`od_hal_crypto_ecb_prepared`) and the macro
in a comment, and adds nothing else. **Trigger:** a target with no native CCM, or an unacceptable
flash delta from `PSA_WANT_ALG_CCM` (measure in C1).

### 3. `struct od_session` and the replay bitmap

```c
struct od_session {
    bool     authenticated;
    uint8_t  session_id[8];
    od_hal_crypto_slot_t slot;
    bool     key_loaded;
    uint64_t tx_counter;            /* device→host, monotonic */
    uint64_t rx_last;               /* highest ACCEPTED inbound counter */
    uint64_t rx_seen;               /* the replay bitmap */
    uint32_t session_start_ms;      /* ABSOLUTE lifetime basis, never last-activity */
    uint32_t last_activity_ms;      /* diagnostics + the target's idle policy; NOT the timeout */
    uint32_t timeout_ms;            /* snapshot of session_timeout_seconds*1000 at auth */
    uint32_t challenge_ms;
    bool     challenge_pending;     /* NOT challenge_ms == 0: a challenge issued at uptime 0 is
                                     * a real challenge, and a zero sentinel calls it absent */
    uint32_t last_auth_ms;
    uint8_t  auth_attempts;         /* 0 also means "no rate window open" — no separate flag */
    uint8_t  integrity_failures;
    uint8_t  pending_server_nonce[16];
};
OD_STATIC_ASSERT(sizeof(struct od_session) <= 96, "session state is a BG22 RAM ratchet");
```
Removed versus the three shipped structs: **`session_key[16]`** (lives in the HAL slot — `shared/`
holds no key material after establish, a real reduction in what a memory dump yields);
`client_nonce`/`server_nonce` (handshake-local); **`replay_window[64]` + `replay_idx`, 513 B → 8 B**
(`MEMORY_CONSTRAINTS.md:124-128` item 6). `timeout_ms` is snapshotted so the hot path needs no
`SecurityConfig` — safe *only because* a config save clears the session, which the header must
state so nobody removes that call.

**THE REPLAY WINDOW IS PORTED, NOT DESIGNED. Corrected 2026-08-15.** Upstream `Firmware` — the
authority repo — has already replaced the 64-entry ring with a bitmap: `../Firmware/src/
nonce_window.h`, zero-dependency, with an 816-line host test at `../Firmware/tools/
test_nonce_window.cpp`. This repo's `targets/esp32-idf/src/encryption.cpp` is an *older import*
and does not reflect it. C4 lands that header as `shared/core/od_nonce_window.h` with the `od_`
prefix and **no logic change**, per CLAUDE.md's "Firmware is the authority" and "import working
drivers as-is".

**Earlier revisions of this plan specified a bitmap I designed, and it was worse in two
security-relevant ways.** Recorded because the reasoning is the point:

- **I bounded the forward direction at ±32. Upstream leaves it unbounded** — any counter above
  `last_seen` is accepted, and only the *backward* window is bounded (256 bits). Upstream's
  "Reversal of Decision A" shows no forward cap can be sized safely: once a gap exceeds it,
  nothing commits, `last_seen` never advances, and every later frame is rejected further out than
  the last. That strands the session rather than rejecting a frame.
- **I used signed difference arithmetic. Upstream is plain unsigned, deliberately.** The counter
  is parsed off the wire *before* the tag is verified, so an attacker controls both operands.
  Signed differences mean converting a `uint64_t >= 2^63` to `int64_t` (implementation-defined
  before C++20), signed overflow, and negating `INT64_MIN` — three routes to UB on
  attacker-chosen input. Upstream uses only unsigned comparison and one subtraction guarded by
  its own branch.

Both of my "two intended divergences" (`diff == 0`, and counter 0 as a backfill) are divergences
from the **stale import**, not from current Firmware: upstream's `od_nonce_check` tests bit 0 like
any other bit, and its all-zero bitmap accepts counter 0 exactly once with no sentinel. So the
differential reference is **`tools/test_nonce_window.cpp`, ported**, not a transcription of the
ring — which is a strictly better oracle than anything C0's capture would have given for this
subsystem.

Sizes change with it: the backward window is **256 bits = 32 B**, not 8 B, so the saving against
the 512 B ring is **480 B** rather than 504, and `sizeof(struct od_session)` is 112 against a
128-byte ratchet. Its width is **not** a replay-security parameter and is deliberately not derived
from the PIPE window — upstream says so explicitly, which retires the coupling assert below.

**The PIPE-window coupling assert is RETIRED.** `SHARED_API_DESIGN.md:711` wanted
`OD_PIPE_MAX_W <= OD_REPLAY_WINDOW_HALF`, which made sense against a ±32 window. Upstream's
`nonce_window.h` states the opposite outright: the width is only reordering tolerance, "narrowing
or widening it cannot create a replay hole", and there is "deliberately no attempt to derive it
from the client's pipe window". A backward rejection is self-healing, because the client
re-encrypts every retransmission under a higher counter. Delete the assert from the design doc
rather than carrying it into shared/.

### 4. `od_session.h` — the API

Constants (`OD_SESSION_STEP1_REPLY_LEN` 23, `OD_SESSION_STEP2_REPLY_LEN` 19, tag/nonce lengths)
each pinned with `OD_STATIC_ASSERT` against the protocol header, including
`AUTH_STATUS_SUCCESS == AUTH_STATUS_CHALLENGE` (both 0x00 — `opendisplay_protocol.h:747`).

**Header order is C99, so the three result enums and `struct od_session_report` are defined ABOVE
every prototype**; the listing below is grouped for reading, not for transcription.

**CALLER CONTEXT for the whole module: single-flow, never an ISR or stack callback. No locks, no
atomics** — the `od_adv_control`/`od_watchdog` rule. This HOLDS today (ESP32 touches the session
only on the loop task, LAN frames included; Nordic's BT RX thread only enqueues, and the session
is touched solely from `opendisplay_pipe_process()` on main), but it is a precondition, not an
accident: `od_watchdog_app.c` needed a spinlock the moment its callers spanned contexts. State it
in the header so a future work-queue refactor cannot break it silently.

```c
void od_session_init(struct od_session *s, od_hal_crypto_slot_t slot);

/* THE ONLY TEARDOWN. Releases the HAL slot, then zeroes — but PRESERVES s->slot, which is
 * configuration, not session state. Zeroing it would silently move every session after the first
 * timeout/disconnect/failed-auth onto slot 0:
 *     slot = s->slot;
 *     if (s->key_loaded) od_hal_crypto_key_clear(slot);
 *     memset(s, 0, sizeof *s);
 *     s->slot = slot;
 * A target MUST NOT memset a struct od_session itself: that drops the slot without releasing it.
 * Idempotent, NULL-safe. od_session_init() is called EXACTLY ONCE, on zero-initialised storage. */
void od_session_clear(struct od_session *s);

bool od_session_security_enabled(const struct SecurityConfig *sec);  /* the zero-key rule, once */
bool od_session_authenticated(const struct od_session *s);           /* pure */

/* MUTATING BY DESIGN: an expired session is CLEARED here, matching what every call site already
 * relies on (encryption.cpp:263-274, opendisplay_pipe.c:168-192). Timeout is ABSOLUTE from
 * session_start_ms and computed as an unsigned ms-domain subtraction — wrap-safe, unlike ESP32's
 * divide-then-subtract. timeout_ms == 0 means no expiry.
 *
 * BOUNDARY: expire on `elapsed >= timeout_ms`. THREE bases ship today: ESP32
 * `sessionAge >= timeout` in SECONDS from session start (encryption.cpp:266-268), Nordic
 * `elapsed > timeout_ms` in ms from session start (opendisplay_pipe.c:186), and Silabs from
 * LAST ACTIVITY (efr32bg22-slc/opendisplay_pipe.c:117) — an idle basis, so a busy Silabs session
 * never expires at all. Firmware is the authority, so `>=` wins; taking Nordic's ms DOMAIN is not
 * the same as taking its boundary. This shifts Nordic by 1 ms and ESP32 by up to ~999 ms (its
 * seconds-domain floor can expire early), and it will change Silabs' semantics outright — idle to
 * absolute — when that target swaps. Record the idle-vs-absolute divergence in DIVERGENCE_MATRIX
 * §6 NOW, not at the Silabs swap. Tested at exactly timeout_ms and at timeout_ms - 1. */
bool od_session_alive(struct od_session *s, uint32_t now_ms, struct od_session_report *report);

/* The whole 0x0050 machine, both steps. device_id is a PARAMETER (different silicon per target,
 * and it is wire-visible identity feeding both KDF and proof). RETURNS the reply, never sends it;
 * the reply is always well-formed [00][50][status] even on rejection. */
enum od_session_auth od_session_authenticate(struct od_session *s,
        const struct SecurityConfig *sec, const uint8_t device_id[4],
        od_span_t body, uint32_t now_ms,
        uint8_t *rsp, size_t rsp_cap, uint16_t *rsp_len, struct od_session_report *report);

/* Decrypt [nonce:16][ciphertext][tag:12]. ORDERING IS THE SECURITY PROPERTY: the window is
 * CHECKED before the tag and ADVANCED only after it verifies, so a forged frame cannot move
 * rx_last and lock out legitimate lower-counter frames. BOTH targets in this step get that
 * behaviour change -- Nordic advances early at opendisplay_pipe.c:500 (decrypt at :505) and so
 * does ESP32 at encryption.cpp:740 (decrypt at :763); only Silabs already splits it.
 * MINIMUM ENVELOPE IS 29 BYTES (nonce16 + one encrypted length byte + tag12). envelope.n < 29 is
 * OD_SESSION_OPEN_SHORT, checked BEFORE any subtraction or CCM call — the subtraction is where an
 * under-length frame becomes a huge unsigned length.
 *
 * THE INNER LENGTH IS EXACT: after a successful decrypt, require
 *     decrypted[0] == decrypted_len - 1
 * and return OD_SESSION_OPEN_BAD_LENGTH otherwise. Both shipped targets use a permissive `<=`
 * (encryption.cpp:767, opendisplay_pipe.c:509), which accepts authenticated trailing bytes the
 * caller never sees — harmless today because the sender never emits them, but it is slack in a
 * length field on the pre-auth-adjacent path and tightening it costs nothing. Record it as a
 * deliberate behaviour change; test declared lengths one below and one above the real payload.
 *
 * ON ANY NON-OK RETURN `out` IS UNDEFINED. Strikes: REPLAY/WRONG_SESSION/BAD_TAG count;
 * CRYPTO_ERROR does not. */
enum od_session_open od_session_open(struct od_session *s, uint16_t cmd, od_span_t envelope,
        uint8_t *out, size_t out_cap, uint16_t *out_len, uint32_t now_ms,
        struct od_session_report *report);

/* `plain_frame` is [cmd:2][payload] — NOT payload alone. The two leading bytes are both echoed
 * into the output and used as the AAD, which is why they must arrive with the payload rather than
 * as a separate uint16_t: it is the shape definitive Firmware's encryptResponse() already takes
 * (`../Firmware/src/encryption.cpp:810-840`). plain_frame.n < 2 has no cmd to echo and returns
 * OD_SESSION_SEAL_TOO_SHORT.
 *
 * CAPACITY IS CHECKED BEFORE ANYTHING IS SPENT:
 *     payload_len = plain_frame.n - 2;
 *     sealed_len  = plain_frame.n + 29;      // nonce16 + len1 + tag12
 * If plain_frame.n > OD_SESSION_PLAIN_FRAME_MAX, return OD_SESSION_SEAL_TOO_LONG before the
 * capacity check. The one-byte length field could represent more, but no supported producer or
 * response transport can carry it.
 * If out_cap < sealed_len, return OD_SESSION_SEAL_NO_ROOM *before* drawing a nonce, advancing
 * tx_counter or calling CCM — a short output buffer must not burn a counter value. out[0..1] and
 * the AAD are exactly plain_frame.p[0..1]. On any non-OK result `out` is undefined.
 *
 * Build [cmd:2][nonce:16][len:1][payload][tag:12]. The TX counter advances on every call that
 * reaches the cipher, INCLUDING one that then fails, so the TX counter never repeats — except at
 * the top: spending UINT64_MAX would wrap to 0 and reuse a nonce under the same key, which is
 * total CCM failure. On reaching UINT64_MAX the session refuses to seal and must be re-authed.
 *
 * THIS IS TX-SIDE NON-REUSE ONLY, and the distinction is not pedantry: inbound and outbound share
 * one session_id and both counters start at 0, so the same session_id||counter nonce is used in
 * both directions under one key. That is a shipped wire flaw od_session cannot fix without a
 * protocol change; do not let this comment grow into a claim that nonces are globally unique. */
enum od_session_seal od_session_seal(struct od_session *s, od_span_t plain_frame,
        uint8_t *out, size_t out_cap, uint16_t *out_len, uint32_t now_ms,
        struct od_session_report *report);

void od_session_touch(struct od_session *s, uint32_t now_ms);   /* idle policy only */
```
All three result enums, in full — no "etc.", because a missing member is a missing branch:
```c
enum od_session_auth { OD_SESSION_AUTH_CHALLENGE = 0, OD_SESSION_AUTH_ESTABLISHED,
    OD_SESSION_AUTH_REJECTED, OD_SESSION_AUTH_RATE_LIMITED, OD_SESSION_AUTH_NOT_CONFIGURED,
    OD_SESSION_AUTH_MALFORMED, OD_SESSION_AUTH_EXPIRED, OD_SESSION_AUTH_CRYPTO_ERROR,
    OD_SESSION_AUTH_NO_ROOM,        /* rsp_cap < the reply this outcome needs */
    OD_SESSION_AUTH_BAD_ARGUMENT }; /* s or rsp NULL — no reply can be produced at all */
enum od_session_open { OD_SESSION_OPEN_OK = 0, OD_SESSION_OPEN_NO_SESSION, OD_SESSION_OPEN_SHORT,
    OD_SESSION_OPEN_WRONG_SESSION, OD_SESSION_OPEN_REPLAY, OD_SESSION_OPEN_BAD_TAG,
    OD_SESSION_OPEN_BAD_LENGTH, OD_SESSION_OPEN_NO_ROOM, OD_SESSION_OPEN_CRYPTO_ERROR };
enum od_session_seal { OD_SESSION_SEAL_OK = 0, OD_SESSION_SEAL_NO_SESSION,
    OD_SESSION_SEAL_TOO_LONG,            /* payload > OD_SESSION_PAYLOAD_MAX (222) */
    OD_SESSION_SEAL_NO_ROOM, OD_SESSION_SEAL_CRYPTO_ERROR,
    OD_SESSION_SEAL_TOO_SHORT,           /* plain_frame.n < 2: no cmd bytes to echo or use as AAD */
    OD_SESSION_SEAL_COUNTER_EXHAUSTED }; /* tx_counter hit UINT64_MAX; re-auth required */
```
So "the reply is always well-formed" holds **only** once `rsp != NULL && rsp_cap >= 3`; outside
that the function cannot write a reply and says so with `BAD_ARGUMENT` / `NO_ROOM`.

**Capacity is checked BEFORE any state mutation — the whole call is transactional.** A step-1 with
`rsp_cap == 3` must not mint a challenge, and a valid step-2 with `rsp_cap < 19` must not derive a
key, load a slot or mark the session authenticated, only to discover it cannot return the proof.
Otherwise a caller with a short buffer leaves the device holding a session the client can never
learn about — an unauthenticated-but-open session, reachable without knowing the key. Preflight
the required reply length for the step being handled, return `NO_ROOM` first, touch nothing.
Test **every** `rsp_cap` from 0 through 22 across both steps.

**The NULL contract, completely:** `s`, `rsp`, `rsp_len` or `device_id` NULL →
`OD_SESSION_AUTH_BAD_ARGUMENT`. **`sec == NULL` is different — it is
`OD_SESSION_AUTH_NOT_CONFIGURED`**, because "no security configuration" is a legitimate protocol
state with a defined reply (`AUTH_STATUS_NOT_CONFIG`), not a programming error; Silabs already
answers exactly that (`efr32bg22-slc/opendisplay_pipe.c:599`), and the enum carries the value. For `open`/`seal`, a NULL `s`, `out` or `out_len`, or an invalid
`od_span_t`, maps to `OD_SESSION_OPEN_SHORT` / `OD_SESSION_SEAL_NO_ROOM` respectively — those
enums already carry "cannot proceed, nothing written", and adding a second spelling would give
callers two branches for one condition. State it in the header rather than leaving it inferred.

**The definitive `Firmware` implementation owns the envelope shape; the supported producers and
transport own its maximum.** `../Firmware/src/encryption.cpp:810-840` computes
`payload_len = plaintext_len - 2` and emits
`[cmd:2][nonce:16][len:1][payload][tag:12]`, so sealing adds exactly 29 bytes to `plain_frame`.
Its `payload_len > 255` check is a necessary guard against wrapping the one-byte length field, not
evidence that a 255-byte response payload or 286-byte sealed response is supported. No producer
can emit that much.

The largest planned producer is Nordic's NFC response after the dispatch plan caps its tag data at
218 bytes: 218 tag-data bytes + 4 NFC metadata bytes = 222 session payload bytes; adding the two
response bytes makes a 224-byte `plain_frame`; sealing makes exactly 253 bytes. Session and
dispatch therefore use one contract:

| Quantity | Contents | Maximum |
|---|---|---:|
| Session payload | bytes after the two command/response bytes | **222** |
| Complete `plain_frame` | `[cmd:2][payload]` | **224** |
| CCM plaintext | `[len:1][payload]` | **223** |
| Envelope passed to `od_session_open` | `[nonce][ciphertext][tag]` | **251** |
| Complete sealed application value | `[cmd:2][envelope]` | **253** |

Encrypted LAN is TLS-PSK and bypasses the firmware envelope. Plaintext LAN plus an enabled CCM
session is only a defensive path in the shipped configuration and does not widen these bounds.
Unsealed LAN frames up to 4094 bytes dispatch in place and never transit the session scratch.

```c
/* Supported envelope limits. The inner length byte can represent more, but no producer or BLE
 * response path supports a larger frame. */
#define OD_SESSION_SEALED_MAX       (OD_BLE_MAX_FRAME - 3u)          /* 253, including cmd */
#define OD_SESSION_ENVELOPE_MAX     (OD_SESSION_SEALED_MAX - 2u)     /* 251, after cmd */
#define OD_SESSION_PLAIN_MAX        (OD_SESSION_ENVELOPE_MAX - ENCRYPTION_NONCE_SIZE - \
                                     OD_HAL_CRYPTO_TAG_LEN)           /* [len][payload] = 223 */
#define OD_SESSION_PAYLOAD_MAX      (OD_SESSION_PLAIN_MAX - 1u)      /* 222 */
#define OD_SESSION_PLAIN_FRAME_MAX  (2u + OD_SESSION_PAYLOAD_MAX)    /* 224 */
OD_STATIC_ASSERT(OD_SESSION_PAYLOAD_MAX <= 255u, "inner length prefix is one byte");
```
`od_session_open` **rejects an envelope over `OD_SESSION_ENVELOPE_MAX` up front, before touching
the cipher**, and `od_session_seal` rejects a payload over `OD_SESSION_PAYLOAD_MAX` — a length
error is the honest answer for an over-long frame; `NO_ROOM` is not. The dispatch plan uses the
same 223-byte decrypt scratch and 253-byte encrypt scratch.

**`out_cap` for `od_session_open` must cover the DECRYPTED FRAME, not the payload:**
```
out_cap >= ct_len - OD_HAL_CRYPTO_TAG_LEN        /* one byte MORE than *out_len will be */
```
CCM emits `[len:1][payload]` — the length byte is plaintext the HAL writes, so sizing `out_cap` to
the payload alone (`- 1`) overflows the caller's buffer by exactly one byte on every frame. Decrypt
the whole thing into `out`, validate the inner length against `ct_len - TAG_LEN - 1`, then
`memmove` the payload down one byte and set `*out_len`. The ≤222-byte move per frame (BLE) is cheaper
than the alternative — a scratch buffer — which `shared/` cannot have as a file-static and should
not demand from every caller.

**`od_session_open` decrypts into the caller's buffer and the two never alias.** No in-place
decrypt across the HAL: `psa_aead_decrypt` does not guarantee overlap support, and an aliasing
contract that happens to work on mbedTLS and corrupts on PSA is the worst kind of portability bug.
(The `memmove` above is inside `out` *after* the HAL has returned, which is not aliasing.)
`struct od_session_report` carries what the three targets logged: `status_byte`, `attempts`,
`integrity_failures`, `torn_down`, `rx_counter`, `rx_last`, `rx_diff`, `age_ms`, `crypto_status`.

**`od_session` takes no `origin`.** The TLS-PSK bypass (SECTION 9 rule 4) is a *dispatcher*
decision about whether to call `od_session_open` at all (`communication.cpp:806`), not something
the session should know. This is load-bearing rather than tidy: an encrypted-LAN frame is
protected by TLS and carries **no** device nonce, so if it ever reached `od_session_open` it would
be rejected as malformed — and if the session were somehow applied to it, the counter space would
advance on traffic the replay window never authenticated. Keeping the decision at the dispatcher
means od_session only ever sees frames that genuinely carry its envelope.

### The handshake state machine, written down rather than inferred

`challenge_pending` is the whole of the step-1/step-2 interlock, so every transition must be
named or an implementer will invent one:

- **Step 1 during a live session clears it and issues a fresh challenge.** Current firmware does
  exactly this (`encryption.cpp:625-628`) — it does *not* answer `AUTH_STATUS_ALREADY`, which is
  defined in the protocol and never sent. Preserve that; a client re-authenticating mid-session is
  the normal recovery path.
- **`challenge_pending` is set only AFTER `od_hal_crypto_random()` succeeds**, together with
  `challenge_ms` and `pending_server_nonce`. A failed RNG leaves no challenge outstanding and
  returns `CRYPTO_ERROR` — never a challenge the device cannot honour.
- **Step 2 requires `challenge_pending`**; without it the reply is `MALFORMED`
  (`AUTH_STATUS_ERROR`), matching a step 2 that arrives with no challenge behind it.
- **It is consumed — cleared, along with `pending_server_nonce` — on every step-2 outcome that
  reaches the proof check:** success, wrong proof, expiry, malformed body, and crypto failure.
  **`RATE_LIMITED` does NOT consume it**, and neither do `BAD_ARGUMENT` or the capacity preflight
  `NO_ROOM`: all three return before the request shape is even examined, which is the order both
  shipped targets already use (`encryption.cpp:609-623`, `opendisplay_pipe.c:678`). A client that
  is throttled must still be able to answer the challenge it already holds once the window clears. One challenge answers one
  step 2; there is no retry against a spent nonce, which is what stops an attacker grinding proofs
  against a fixed server nonce.
- **The expiry boundary is `elapsed > 30000`, not `>=`.** Both shipped targets accept a step 2 at
  *exactly* 30 000 ms (`encryption.cpp:646`, `opendisplay_pipe.c:718`), so test **30 000
  (accepted)**, 29 999 (accepted) and 30 001 (expired) — the exact-boundary case is the one that
  distinguishes the two conventions and it must be in the suite, not just in this paragraph. Note
  this is the opposite boundary convention from the session timeout above, which uses `>=` — the
  two are genuinely different in the shipped code and neither should be "harmonised" silently.

### Two security semantics that must be decided, not inherited

**The rate limiter is not a fixed window, and the promotion keeps it that way.** Read
`encryption.cpp:608-623`: `last_auth_time` is rewritten on *every* attempt, so the counter resets
only after a 60 s **idle gap** — not 60 s from the first attempt. An attacker pacing one attempt
every 59 s is throttled forever after ten; ten attempts spread over nine minutes also lock out.
That is stricter than a fixed window and it is what the fleet ships, so **preserve it** and say so
in the header, because "10 attempts / 60 s" reads as a fixed window to everyone who has not read
the code. (`if (last_auth_time > 0)` also means the very first attempt skips the check entirely —
another zero sentinel; `auth_attempts == 0` as the window-closed state removes it.) Test at
exactly 60 000 ms either side.

**`encryption_enabled`: take Nordic's `!= 0`, not Firmware's `== 1`, and record the deviation.**
ESP32 gates on `== 1` (`encryption.cpp:232`), Nordic on `!= 0` (`opendisplay_pipe.c:165`); the
wire contract defines only 0 and 1 (`opendisplay_structs.h:914`). CLAUDE.md makes Firmware the
default authority, but that is "a default, not a licence to skip the write-up", and here the two
readings fail in opposite directions: under `== 1` a single corrupted byte silently turns
encryption **off** on a device whose config says it is on; under `!= 0` a corrupt byte can only
fail closed, and the client still authenticates normally because it holds the key. **Fail-safe
wins on a security gate.** This is the one place the plan overrides the authority target, so it
belongs in `DIVERGENCE_MATRIX` §6 with the reasoning. Test 0, 1 and 2. Implement via the existing
`od_config_security_key_set()` (`od_config.h:192`) rather than a second zero-key scan.

### Key material on the stack must be wiped

The claim that `shared/` holds no key material is true of the *struct* and false of the
*handshake*: `od_session_authenticate` necessarily holds the derived session key and the KDF
intermediate in locals before handing the key to the HAL slot. A plain `memset` at the end is
removable by the optimiser as a dead store. Add a small `od_secure_zero()` in `od_session.c`
(a `volatile unsigned char *` write loop, or `memset_s`/`explicit_bzero` where available) and call
it on **every** exit path — including the `goto cleanup` failures — for the session key, the KDF
intermediate and the CMAC scratch. A host case cannot prove the wipe survives optimisation, so
also assert it in review and keep the function in one place where it can be inspected.

**Stay in C — the CLAUDE.md decision-1 revisit, answered.** CLAUDE.md names `od_session.c` as a
revisit point for "nested resource lifetimes with several failure exits". The slot HAL removes
that shape: after this design `od_session.c` holds *nothing* across a fallible step — the one
resource with a lifetime lives in `targets/`, acquired by one call, released by one, idempotent
on both ends. The handshake's five fallible steps sit in one function with one cleanup action
reached by `goto cleanup`, which is exactly what rule 3 prescribes. RAII would buy one `goto`
label, against a doubled host gate and an `-fno-exceptions` contract across three build systems.
Record this in the commit message so the decision is re-taken, not inherited.

### 5. What stays in each target

`device_id` derivation (different silicon, passed in as a parameter); `origin` plumbing; **all
logging**, via a per-target `od_session_app.{c,h}` owner modelled on
`nordic-zephyr/src/od_watchdog_app.h`; link teardown / `session_guard` / auth-abuse drop (that is
CONNECTION_POLICY, not session crypto — `resetAuthAbuseCounter()` moves from inside the handshake
at `encryption.cpp:703` to the caller, driven off `OD_SESSION_AUTH_ESTABLISHED`);
`secureEraseConfig()`, `checkResetPin()`, `getChipIdHex()`; `deriveTlsPsk()` (LAN-only, one
target — repoints onto the new HAL in C1). **`isEncryptionEnabled()` moves to shared** as
`od_session_security_enabled()`, since it is the zero-key rule and
`od_config_security_key_set()` already exists. **Clearing the session after a config save stays a
target call** but becomes contractual — the header states that `timeout_ms` being a snapshot
depends on it.

### 6. Build wiring

New tier `OD_SHARED_SOURCES_HAL_CRYPTO` in `shared/sources.cmake`, composed into the aggregate.
`esp32-idf/main/CMakeLists.txt:64` takes the aggregate so the *shared source* needs no edit — but
the *HAL implementation* does, since each `hal/*.c` is listed explicitly at `:48-55`.
`nordic-zephyr/zephyr/CMakeLists.txt:253` names tiers and must add the new one. Silabs takes PURE
only and is untouched. While in `sources.cmake`: reorder the arrival list so `od_session.c`
precedes `od_dispatch.c` with the reason, and delete the **stale duplicate `core/od_advert.c` line**.

## Commit sequence

**C0 — Capture. SKIPPED by decision, 2026-08-15.** The hardware capture is not being taken, so
`tests/vectors/session.json`, `tools/session_vectors_gen.py` and `session_vectors.inc` drop out of
this plan and **C1 is no longer blocked**.

*What that costs, recorded so it is a decision and not an oversight.* The differential test loses
its third and strongest layer. Layers 1 and 2 survive — an independent transcription of ESP32's
KDF, and the transcribed soft CCM pinning the envelope framing — and they still prove the promoted
code agrees with the shipped algorithms. But they prove it against **a transcription of the source
I can read**, not against bytes a real device actually emitted, so a misreading of the shipped code
is invisible: both sides are then wrong in the same way. Capture is the only layer that could have
caught that, and `TEST_OWNERSHIP.md:269-275` is explicit that once C5/C6 land there is no untouched
reference left to take it from. Gate 1's "shared wire vectors passing against both the C core and
py-opendisplay" is therefore **not met for this subsystem**.

*Cheap partial substitutes, if wanted later.* `replay_vectors.py` already carries a 0x0050 builder
for both h2d handshake shapes, so **authored** (not captured) h2d vectors would still exercise the
host encoder against the C core without any bench time. And the 19-byte step-2 proof — the one
undocumented thing on this wire — stays pinned by `OD_SESSION_STEP2_REPLY_LEN` and its
`OD_STATIC_ASSERT` regardless. Hardware verification at C5/C6 remains the real gate and is
unchanged.

**C1 — the crypto seam, proven in isolation.** `od_hal_crypto.h` + both implementations +
`CONFIG_PSA_WANT_ALG_CCM=y`. Repoint the *existing* target code onto it: ESP32's `aes_*` become
thin forwarders; Nordic's `od_ccm_*` are **replaced** by the HAL, key prepared at
`authenticate_handle`'s success point and released in `clear_session`. Not purely mechanical on
Nordic — `encrypt_response_payload()` (`opendisplay_pipe.c:520-545`) collects the tag in a
separate stack `tag[12]` and must be reworked for the combined `ct||tag` buffer (the *inbound*
path already has them adjacent, `:492-495`). Behaviour byte-identical on the wire. One thing ESP32
loses: `aes_ccm_*` today fall back to a per-call `setkey` when `!is_ccm_ready`
(`encryption.cpp:366-378`) and a slot-only forwarder drops that — edge case, since `setkey` does
not fail in practice, but state it rather than discover it.
**Copy Nordic's soft CCM (`opendisplay_pipe.c:317-441`) into `tests/host/` as the differential
fixture, and delete the production copy — both in this commit, after C1's hardware pass.** C1 is
what orphans it and `-Werror` rejects an unused static, so there is no version of this where the
deletion waits for C7. The differential test cannot run until C4 registers it; that is fine,
because the fixture copy is what the test needs and it exists from C1 onward. **C7 does not
delete anything.**
**Hardware: authenticate + encrypted upload on both.** This is where PSA CCM either works or does
not, isolated from every other change. **Measure the flash delta.**

**C2 — docs for the seam.** `SHARED_API_DESIGN.md` § `od_hal_crypto` rewritten (enum returns,
prepared slots, soft-CCM deferred with its trigger), `DIVERGENCE_MATRIX` §6.3, `CLAUDE.md`.
Doc-only, so C1 stays revertable without a doc rollback.

**C3 — `od_session.h` + `session_test.c` as an UNREGISTERED source.** Header written first and the
test written against it (the `od_adv_control` precedent). The test file is added but **not** added
to `tests/host/CMakeLists.txt` — registering a test with no implementation to link is a red CI,
not a discipline. Registration happens in C4. Second deliverable: the vendored host AES-128 the
fake depends on. No dispatch-plan amendment is needed — that plan already assumes session-first
(see above). (Merging C3 into C4 is an acceptable alternative; what is not acceptable is a commit
that does not build.)

**C4 — `od_session.c`, test registered, host suite green, no target calls it.** Compiles and links
dead on ESP32 (aggregate) and in the host build. **Gate 1 closes here — including the CI wiring,
which is part of this commit and not a loose intention:** the `fuzz-short` job lands in
`.github/workflows/host-tests.yml` here. There is no `session_vectors_gen.py --check` job and no
`session.json` — C0 is skipped, so `replay_vectors.py` is untouched by this plan.

**First move of C4: port, don't write.** `../Firmware/src/nonce_window.h` →
`shared/core/od_nonce_window.h`, `od_` prefix and no logic change, and
`../Firmware/tools/test_nonce_window.cpp` → the host suite as its differential oracle. That is
~206 lines of already-tested, already-argued state machine this plan would otherwise re-derive
worse. Everything else in `od_session.c` is written against `../Firmware/src/encryption.cpp` — the
upstream file, not the import — with the seven drift items above applied.

**C5 — ESP32 swaps** (first, per CLAUDE.md:171 — the authority target is the reference). ~350
lines out of `encryption.cpp`; `struct EncryptionSession` deleted from `encryption_state.h`.
`handleAuthenticate`'s call site (`communication.cpp:793`) becomes
call-then-send-then-`resetAuthAbuseCounter()`-on-ESTABLISHED. **Hardware:** authenticate,
encrypted config write, encrypted image upload, forced session timeout, replayed frame refused,
a LAN-TLS frame still bypassing the envelope, and **PIPE under reorder**. Re-run
`compat/ratchet.sh` and `tools/sdkconfig_baseline.sh`.

**C6 — Nordic swaps.** ~400 lines out of `opendisplay_pipe.c`; `struct EncryptionSession` deleted
from `od_runtime_types.h`. **Hardware on `xiao_nrf52840`:** the same list — including **encrypted
PIPE under reorder**, because Nordic ships PIPE with `PIPE_MAX_W 32` and that is the only test
that exercises the ±32 replay window against real reordering on this target.

**C7 — document, and file the one thing this promotion cannot fix.** Open a security follow-up in
`FOLLOWUPS.md` (and a `DIVERGENCE_MATRIX` §6 row) for the **bidirectional nonce reuse**: inbound
and outbound share one `session_id` and both counters start at 0, so the same
`session_id||counter` nonce is used in both directions under one key. That is a protocol-level
flaw, not a promotion defect — it needs directional key separation or a nonce-domain bit in the
next protocol revision, and it must not stay buried in an API comment where the next reader
mistakes it for a note. (The soft CCM is already gone, in C1.) Update `CLAUDE.md` § Status,
`sources.cmake`, `DIVERGENCE_MATRIX` §6.1/6.2, `MEMORY_CONSTRAINTS` item 6 (closed, with the
measured RAM delta), `FOLLOWUPS.md` for the Silabs carry-forward.

## Testing

`tests/host/session_test.c`, registered the standard three lines, using the existing hand-rolled
`CHECK`/`CASE` macros from `watchdog_test.c:20-34`.

**The HAL fake** is defined at file scope and linked in as a target's would be
(`watchdog_test.c:37-40`). It must be a *real* AES-128 — a stub cannot exercise the KDF or CCM —
and **there is no AES on the host today**: `tests/host/` links nothing but `od_shared`, and the
soft CCM being transcribed is only a *mode* whose primitive (`aes_ecb_encrypt_16`,
`opendisplay_pipe.c:215-235`) is PSA and cannot come with it. **Vendoring a C99, `-Werror`-clean
AES-128 encrypt-only core into `tests/host/` is a named C3 deliverable** (tiny-AES or equivalent,
~150 lines, public domain, provenance recorded). Without it C3/C4 cannot build the fake, the KDF
differential, or the CCM differential at all.
**Transcribe Nordic's soft CCM (`opendisplay_pipe.c:317-441`) into the test fixture in C1, the
commit that deletes the production copy**:
the implementation being removed is exactly the reference the test needs. Deterministic
counter-seeded `random` so handshakes reproduce, plus a knob forcing `OD_HAL_CRYPTO_ERROR` (the
path that must *not* count a strike), plus set/clear counters to assert **no slot leak across 1000
re-authentications**.

**Differential reference, three layers:** an independent transcription of ESP32's KDF
(`encryption.cpp:107-154`) swept over nonces; the transcribed soft CCM pinning the envelope
framing. **The third layer — the C0 hardware capture — is skipped (see C0), so nothing here proves
agreement with bytes a real device emitted; the two transcriptions are the whole reference.**

**How the capture actually gets replayed, because "a C runner" is not a design.** There is no C
JSON parser in this repo and there will not be one: `replay_vectors.py` is per-frame and cannot
establish a multi-frame session, so it stays as-is for the h2d halves it already handles.
**With C0 skipped there is no `session.json`, no generator and no C corpus runner in this plan.**
The vector machinery described in earlier revisions (`tools/session_vectors_gen.py` emitting a
committed `session_vectors.inc`, a scripted RNG stream carrying a captured server nonce, and the
`AUTH_REQUEST` / `H2D_ENCRYPTED` / `D2H_PLAINTEXT` event types) all existed to replay captured
device traffic. None of it is needed for authored tests, which drive the API directly and use the
deterministic counter-seeded fake. If a capture is taken later, that machinery is the shape to
build — see the git history of this file rather than re-deriving it.

**The replay assertion still needs two halves, capture or not.** A replay case must assert both
that the promoted core returns `OD_SESSION_OPEN_REPLAY` **and** that the transcribed legacy oracle
*accepted* that same frame — the shipped code skips the seen-scan when `diff == 0`. Either
assertion alone proves nothing: the first could pass against a core that rejects everything, the
second is just a description of the old bug. With C0 skipped the frame is authored rather than
captured, which does not weaken this particular pairing — both halves run against code, not bytes.

**Cases that carry the weight:** `diff == 0` refused *and* asserted to disagree with the
transcribed ring-scan (that is what makes it a fix rather than a claim); window-advance ordering
(corrupt tag at `rx_last+30` → `rx_last` unchanged and `rx_last+1` still decrypts — the Nordic bug,
invisible without asserting on `rx_last`); a 10 000-frame bitmap sweep against a brute-force model;
boundary exactness (+32 accepted, +33 refused, −32 once, −33 refused); rate limit and its 60 s
reset; challenge expiry at 29 999, **exactly 30 000 (accepted — the `>` boundary)** and 30 001,
against the session timeout's `>=` at exactly `timeout_ms`; **absolute timeout across the `uint32_t` rollover**
(establish at `0xFFFFF000` — this case fails against today's ESP32 code, which is the point);
strikes (3×`BAD_TAG` tears down, 3×`CRYPTO_ERROR` does not, success resets); constant-time compare
asserted structurally plus in review, not pretending a host test can measure timing; slot hygiene;
NULL-tolerance everywhere.

Added by review, each guarding a specific hole:
- **`now_ms == 0`** — a challenge issued at uptime zero must be a challenge, and the rate window
  must not read as already-open. Run the whole handshake suite once with the clock starting at 0.
- **`od_session_clear()` preserves the slot** — init on slot 1, force a timeout, re-authenticate,
  assert the key still lands in slot 1 and not slot 0. **Needs `OD_HAL_CRYPTO_KEY_SLOTS=2`**
  compiled into *both* `od_shared` and the fake for this test binary; at the default of 1, slot 0
  is the only legal value and the case cannot express the bug it exists to catch.
- **TX counter exhaustion** — force `tx_counter` to `UINT64_MAX - 1`, seal twice, assert the
  second returns `COUNTER_EXHAUSTED` and that no subsequent call ever emits nonce counter 0.
- **Timeout boundary** — exactly `timeout_ms` expires, `timeout_ms - 1` does not.
- **`rsp == NULL` / `rsp_cap < 3`** — `BAD_ARGUMENT` / `NO_ROOM`, no write, no crash.

**No C corpus runner here.** With C0 skipped this plan lands none, so the dispatch plan's C12
runner for `dispatch.json` is the first — it sets the pattern rather than reusing one.

**Stand up `tests/fuzz/` now, minimally — and wire it, or it does not close Gate 1.** The
handshake *is* the pre-auth surface and `MIGRATION.md:286` requires coverage for it.
`fuzz_session_auth.c` over `od_session_authenticate` and `fuzz_session_open.c` over the envelope
(the inner length byte at `encryption.cpp:767-771` is exactly the shape that goes wrong), both
against the same fake HAL, asserting no crash and that `*rsp_len` is always a valid reply length.

**`fuzz_session_open` must open a real session in its initializer** — a scripted handshake against
the deterministic fake, once, before the first input. Without it every input returns
`NO_SESSION` at the first branch and the envelope parser — the entire point of that target — is
never reached, while the fuzzer reports healthy coverage of a rejection path.

Concretely, since "add fuzzing" is where this silently becomes a no-op:
- `tests/fuzz/CMakeLists.txt`, clang-only, guarded by `if(CMAKE_C_COMPILER_ID MATCHES "Clang")`,
  `-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g`.
- Corpus at `tests/fuzz/corpus/<target>/`, seeded from authored handshake and envelope frames
  (C0 is skipped, so there are no captured seeds) plus authored malformed ones;
  every crash reproducer checked in **and also** replayed as an ordinary case in `session_test.c`,
  so a regression fails the normal gcc+clang run and not only a fuzz run nobody triggers.
- A `fuzz-short` job in `.github/workflows/host-tests.yml`: `-max_total_time=60 -runs=200000` per
  target on PR, so it is a gate rather than a folder. Longer soaks stay manual.

## Verification

- Host: `cmake -S tests/host -B <dir> && ctest --test-dir <dir>` under **gcc and clang**, `-Werror`;
  plus `python tests/host/replay_vectors.py tests/vectors`.
- `shared-boundary.yml` grep: `od_session.c` pulls in no vendor header.
- Builds: `targets/esp32-idf/build.sh` (10 fragments), `targets/nordic-zephyr/build.sh --all`
  (3 boards), `compat/ratchet.sh`, `tools/sdkconfig_baseline.sh`.
- **RAM/flash delta measured and stated** on both targets: the bitmap should return ~505 B;
  `PSA_WANT_ALG_CCM` costs flash that must be quantified, not assumed.
- **Hardware (Gate 2) is the only thing that counts** — the per-commit lists in C1/C5/C6.

## Risks, most likely first

1. **PSA 12-byte tag policy.** Importing with plain `PSA_ALG_CCM` fails everything with
   `NOT_PERMITTED` and will *look* like "PSA CCM doesn't work". C1 exists to isolate this, with the
   soft-CCM revert one `git revert` away.
2. **PSA key-slot leak** — degrades slowly, presents months later. Mitigated by the slot API, the
   idempotent `key_set` mandate, and the 1000-re-auth assertion.
3. **`od_session_alive()` mutates.** A `bool` function that tears down a session violates every
   reader's expectation, and all three targets already rely on it. If it is quietly made pure
   during implementation, sessions become immortal. Name, doc comment, non-const argument and a
   host case all defend it.
4. **No capture means a transcription error is invisible.** C0 is skipped by decision, so the
   differential reference is my reading of the shipped code rather than device bytes; if I
   misread it, both sides agree and the test passes. Mitigated only by hardware verification at
   C5/C6, which is now the sole check that the wire did not move. If anything looks wrong on a
   board, suspect the transcription before the promoted code.
5. **The 19-byte reply gets "corrected" to the spec's 3 bytes.** Every shipping host expects 19.
   Pinned by constant + assert + capture vector; the spec correction is filed upstream.
6. **`AUTH_STATUS_SUCCESS == AUTH_STATUS_CHALLENGE == 0x00`** — a refactor keying on the status
   byte alone cannot tell step 1 from step 2. The enum and the reply *length* are the discriminators.
7. **Unverified plaintext on `BAD_TAG`.** A caller that logs or dispatches before checking the
   return is decrypting for the attacker. Stated at both functions and asserted by poisoning.
8. **Drift between the two swaps.** C5 and C6 land days apart; both gated by the same suite and
   capture, and neither may change `od_session.c` — if a target swap needs a core change, that is
   its own commit re-verified on the target that already swapped.
9. **Nordic flash from `PSA_WANT_ALG_CCM`.** Measure in C1; a material delta is the
   `OD_CRYPTO_SOFT_CCM` trigger firing early, and the answer is the shared soft path, not a
   per-target `#ifdef`.
10. `MAX_CONFIG_SIZE` is still 2048 on Silabs vs 4096 elsewhere — out of scope, but it means the
    RAM pressure making the bitmap mandatory for BG22 has not actually landed yet.
