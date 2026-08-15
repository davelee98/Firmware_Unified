# `od_session` — the BLE session subsystem

Reference for `shared/core/od_session.c`. The code is the source of truth; this explains the parts
that are not obvious from reading it, and states exactly what is and is not verified.

## What it is

One implementation of the BLE session: the `0x0050` handshake, the KDF, the anti-replay window,
and the AES-CCM envelope in both directions. It replaced four hand-maintained copies — ~940 lines
on ESP32, ~1476 inside Nordic's `opendisplay_pipe.c`, a third on Silabs, and a legacy fourth.

The copies were byte-identical in their algorithms and drifted in their *policies*, which is the
failure mode the promotion exists to end: `AUDIT_NORDIC_ZEPHYR_2026-08-14.md:259` records Nordic
silently downgrading the auth-proof compare from constant-time to `memcmp`, and nothing failing.
That same regression is still live on Silabs.

**Callers:** `esp32-idf` and `nordic-zephyr`. `efr32bg22-slc` still open-codes its own and is
unaffected — see `FOLLOWUPS.md` § 5 for what it inherits when it swaps.

## What each target keeps

Everything that cannot be shared, and nothing else: the clock, the device identity (efuse MAC on
ESP32, `hwinfo` on Nordic), the logging, and the decision of *whether* to call in at all. The
origin gate is deliberately outside — a TLS-LAN frame is protected by the transport and carries no
device nonce, so it must never reach `od_session_open()`.

## The wire contract

```
sealed application value   [cmd:2][nonce:16][len:1][payload][tag:12]      <= 253
envelope (after cmd)              [nonce:16][ciphertext ][tag:12]         <= 251, >= 29
CCM plaintext                               [len:1][payload]             <= 223
payload                                            [payload]             <= 222
plain_frame handed to seal [cmd:2][payload]                              <= 224
```

Sealing adds exactly 29 bytes; an `OD_STATIC_ASSERT` in the header pins that. The CCM nonce is
envelope nonce bytes `[3..15]`; the AAD is the two big-endian command bytes. The envelope nonce is
`session_id(8) ‖ BE64(counter)`.

**256 and 253 are different numbers on purpose.** 256 is the RX slot — what the radio will take,
including the ATT opcode and handle. 253 is the largest thing this module may *emit* into it.
Collapsing them is how a 253-byte cap silently becomes 256.

### Derivations

All four verified byte-for-byte against `../Firmware/src/encryption.cpp` and
`../py-opendisplay/src/opendisplay/crypto.py`:

```
session_key  = AES-ECB(master, BE64(1) ‖ CMAC(master, "OpenDisplay session"‖0x00‖device_id‖
                                              client_nonce‖server_nonce‖0x00 0x80)[0..7])
session_id   = CMAC(session_key, client_nonce ‖ server_nonce)[0..7]
client proof = CMAC(master,      server_nonce ‖ client_nonce ‖ device_id)
server proof = CMAC(session_key, server_nonce ‖ client_nonce ‖ device_id)
```

**Both proofs are server-nonce-first.** Only the key differs. A comment in an earlier revision
claimed the device's proof reversed the order; it never did, and a review found that "correcting"
the code to match that comment passed the entire test suite while breaking every deployed host.
The reply bytes are now pinned so that cannot recur.

### Reply shapes

Step 1 is 23 bytes, step 2 is 19. The spec documents only a 3-byte step-2 reply; the 16-byte
mutual-auth proof is undocumented and universal, so the spec is what is wrong. `AUTH_STATUS_SUCCESS`
and `AUTH_STATUS_CHALLENGE` are **both `0x00`** — the enum and the reply *length* are the only
discriminators between the two steps.

## Design decisions worth knowing

**The session key is not in `struct od_session`.** It lives in an `od_hal_crypto` slot, so the
struct stays trivially zeroable and a memory dump yields no key material. Targets must never
`memset` a session: `od_session_clear()` is the only teardown, because it also releases the slot
and preserves the slot index.

**The replay window is upstream's, ported verbatim** (`od_nonce_window.h` ← `Firmware/src/
nonce_window.h`): a 256-bit backward bitmap with **no forward bound**. A forward cap strands a
session permanently once a gap exceeds it and bounds nothing an attacker cares about. 640 B → 112 B
for the whole session object.

**Check is pure; commit happens only after the tag verifies** — so a forged frame at a high counter
cannot move `rx_last` and lock out legitimate in-flight frames. Commit deliberately precedes the
inner-length check: that frame carried a valid tag, so it is authentic and leaving it uncommitted
would leave it replayable.

**Only a CCM tag failure spends an integrity strike.** Replay, out-of-window and wrong-session do
not — counting them would let ordinary packet loss tear down a live session, and an engine fault
would turn a transient OOM into forced re-authentication.

**Authentication is transactional.** Capacity is preflighted before the rate limiter, so a caller
with a short reply buffer cannot consume an attempt, open the rate window, or leave a session open
that the client can never learn about.

**The rate limiter drains while a peer knocks.** A *throttled* attempt returns before updating
`last_auth_ms`, so at 59 s pacing the 11th is refused and the 12th accepted. This matches upstream
and is deliberate: making a refusal extend the window would let any unauthenticated peer lock the
real client out permanently.

## The five deliberate behaviour changes

Recorded in full in `DIVERGENCE_MATRIX.md` § 6.5–6.9.

| | Change | Risk |
|---|---|---|
| 6.5 | bidirectional nonce reuse — **not fixed, not fixable here** | see below |
| 6.6 | a `diff == 0` resend is now `REPLAY` | can refuse a frame the old code accepted |
| 6.7 | the window advances only after the tag verifies | — |
| 6.8 | the inner length byte is exact, not permissive | can refuse a frame the old code accepted |
| 6.9 | `encryption_enabled` read as `!= 0`, overriding the authority target's `== 1` | fail-safe |

6.6 and 6.8 are the two that could reject traffic the fleet previously accepted. 6.6 is
host-compatible — py-opendisplay re-seals on every transmission including PIPE retransmits, pinned
by its own test.

## The one flaw this could not fix

Inbound and outbound share one `session_id` and both counters start at 0, so **the same CCM nonce
is used in both directions under one key**. That is the catastrophic failure mode of a counter-mode
AEAD. `od_session` reproduces it faithfully because changing it changes the wire; it needs
directional key separation or a nonce-domain bit in a protocol revision. Filed with three costed
options in `FOLLOWUPS.md` § 5.

## Verification

**`tools/check.sh --targets` is the gate. There is no CI.** 12 checks: three boundary greps, the
host suite under gcc and clang, the same suite under ASan/UBSan, three fuzz targets, the
py-opendisplay wire corpus, the shim ratchet, all 10 ESP32 fragments with the sdkconfig baseline,
and all 3 Nordic boards. A skip is counted, reprinted, and exits 2.

**Host suite: 11,756 checks.** Its credibility rests on mutation testing rather than on passing —
~40 deliberate defects were injected and the suite's ability to catch each was measured. Six
survivors were found and closed; the surviving-mutation list is in
`OD_SESSION_LANDED_REVIEW_2026-08-15.md`.

**One mutation survives permanently:** `od_ct_equal` → `memcmp`. No host test can measure
constant-time behaviour, so it is enforced structurally — `check.sh` fails if `memcmp` appears in
`od_session.c` at all.

### Hardware

| | status |
|---|---|
| `nordic-zephyr` / `xiao_nrf52840` | **Gate 2 passed 2026-08-15.** Native PSA CCM with the 12-byte shortened-tag policy authenticates real traffic — the risk both the plan and the review ranked first. |
| `esp32-idf` | **No hardware result.** C5's swap and C1's mbedTLS arm are unproven. The authority target is the one trailing. |
| `xiao_nrf54l15`, `xiao_nrf54lm20a` | build clean, never flashed |

**Not exercised on any board:** the PIPE silence path (a nonce-rejected `0x81` frame must draw no
response, or the client's upload dies on the first dropped frame) — it needs deliberately induced
loss or reordering, and no host test can produce the condition. Nordic's 218-byte NFC read cap is
likewise unexercised.

## Open items

- Flash an ESP32-S3 and close C5.
- Force reorder on the nRF52840 to exercise the PIPE silence path.
- `FOLLOWUPS.md` § 5 — bidirectional nonce reuse, needs a protocol revision.
- Test gaps left open, from the mutation audit: the CCM reference is used for both encrypt and
  decrypt so it cancels as an oracle (needs RFC 3610 vectors); `OD_SESSION_OPEN_REPLAY` is
  unreachable in both open fuzzers; the fake's error injection is all-or-nothing, so four of five
  step-2 crypto-failure paths never run; one of nine `od_session_report` fields is asserted.
- **Nordic's `slot_release()` latches the prepared key slot** — a single `psa_destroy_key` failure
  leaves `s_slot_ready` set, `key_set()` then fails forever, and the device is UNAUTHENTICATABLE
  until reboot. Same latch class removed from the one-shot path in `c0b3206`; the fix is the same
  decision (clear the tracking, accept the leak). Deliberately deferred to `od_dispatch` C11, which
  rewrites that file — see `plans/PLAN_OD_DISPATCH_2026-08-14.md` § 7.1. Needs a PSA fault to
  trigger, which is why it is scheduled rather than hotfixed.
- Smaller, same commit: ESP32's RNG wrapper cannot report failure (so the "never offer a challenge
  the device cannot honour" branch is unreachable there), and `od_session_seal()` dropped upstream's
  activity stamp.
