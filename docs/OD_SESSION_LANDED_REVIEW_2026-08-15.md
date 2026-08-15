# `od_session` Landed-Code Review

**Date:** 2026-08-15  
**Branch reviewed:** `feat/od-session`  
**Reviewed tip:** `db54c5c` (`docs(session): record what the od_session promotion decided...`)  
**Plan:** [`plans/OD_SESSION_PLAN_2026-08-15.md`](../plans/OD_SESSION_PLAN_2026-08-15.md)  
**Disposition:** **Not ready for correctness sign-off**

## 1. Executive summary

The `od_session` promotion has landed in a coherent shape and most of the difficult shared-core
logic is sound. In particular, the KDF framing, replay bitmap, check-before-decrypt / commit-after-
authentication ordering, exact inner-length validation, absolute session timeout, bad-tag strike
policy, session-key slot ownership, and normal seal/open framing agree with the revised plan.

The landed implementation is nevertheless not fully correct against its own plan and public
contract. This review found:

- **one P1 target integration regression** that can abort an encrypted PIPE upload after a nonce
  replay/out-of-window rejection;
- **four P2 shared or integration defects** involving transactional authentication state,
  challenge consumption, unbounded nonce-failure logging, and unchecked integer narrowing;
- **three P3 hardening/diagnostic/resource defects** involving lost crypto status, the planned
  constant-time comparison form, and ignored PSA key-destruction failures;
- **material verification gaps:** neither swapped target is recorded as hardware-verified, the
  sole local gate does not build Nordic, CI no longer runs the gates, and the skipped C0 capture
  means there is no captured device-wire oracle.

The implementation should be corrected and the missing targeted tests added before hardware Gate
2. Hardware verification remains mandatory after those fixes because the host fake cannot prove
that the mbedTLS and PSA backends are configured correctly on real devices.

## 2. Scope and review method

The review covered:

- [`shared/core/od_session.c`](../shared/core/od_session.c) and
  [`shared/core/od_session.h`](../shared/core/od_session.h);
- [`shared/core/od_nonce_window.h`](../shared/core/od_nonce_window.h);
- [`shared/hal/od_hal_crypto.h`](../shared/hal/od_hal_crypto.h);
- ESP32 and Nordic crypto HAL implementations;
- ESP32 and Nordic session adapters and dispatch call sites;
- host unit tests, fake crypto HAL, fuzz targets, and `tools/check.sh`;
- the final session plan, relevant documentation, commit messages, and the authoritative sibling
  implementation under `../Firmware/src/`.

This was a read-only implementation review apart from adding this report. No production or test
code was changed.

### Commands and checks used

- Inspected the complete `main...feat/od-session` diff and commit sequence.
- Ran `tools/check.sh --fuzz-time 5`.
- Ran the exact `od_session_test` executable under Clang with LeakSanitizer disabled.
- Ran the exact `od_session_test` executable under ASan/UBSan with LeakSanitizer disabled.
- Ran all three libFuzzer targets with LeakSanitizer disabled.
- Ran `git diff --check`.
- Compared the target integration with the authoritative nonce-failure handling in
  `../Firmware/src/communication.cpp` and `../Firmware/src/encryption.cpp`.

LeakSanitizer cannot operate in the review environment because the process is under `ptrace`; that
is an environment limitation, not evidence of a code failure. The pinned Python wire-corpus run
also could not use its default `uv` cache because that cache is outside the writable workspace.

## 3. Overall correctness assessment

| Area | Assessment | Notes |
|---|---|---|
| Handshake wire shapes | Pass with defects around failure state | Step-1 reply is 23 bytes; step-2 reply is 19 bytes; normal success path is correct. |
| KDF and proof inputs | Pass | Host tests independently reconstruct the CMAC chain. |
| Challenge lifecycle | **Fail** | Several step-2 failure paths retain the challenge contrary to the plan. |
| Authentication rate limiting | Pass for normal full-capacity callers | Idle-gap behavior is preserved, but `NO_ROOM` mutates the rate state. |
| Replay window | Pass | Imported bitmap behavior and randomized oracle sweep look correct. |
| Replay commit ordering | Pass | Window commits only after tag verification and before exact inner-length rejection. |
| Integrity strikes | Pass | Only `BAD_TAG` counts; engine and nonce failures do not. |
| Envelope bounds | **Fail on public API edge case** | Normal target sizes are bounded, but `seal()` narrows a `size_t` before validating it. |
| Target response sealing | Pass for normal bounded responses | Nordic NFC cap is adjusted to the planned 218-byte tag-data ceiling. |
| PIPE nonce-loss policy | **Fail** | ESP32 sends the fatal NACK that the revised plan explicitly forbids. |
| Failure logging | **Fail** | Nonce failures are not rate-limited on either target. |
| Crypto error diagnostics | **Fail** | Authentication reports `OD_HAL_CRYPTO_OK` on engine failure. |
| Key-slot lifecycle | Mostly pass | Prepared session slot is handled carefully; Nordic one-shot key cleanup still ignores destroy failures. |
| Constant-time proof comparison | Partial | Fixed-iteration source loop exists, but the specified volatile accumulator is absent. |
| Host verification | Strong | Unit and fuzz coverage are substantial, with specific gaps described below. |
| Target/hardware verification | **Incomplete** | Builds are recorded, but neither swapped target is recorded as hardware-verified. |

## 4. Findings

### OD-S1 — P1: encrypted PIPE nonce loss is converted into a fatal NACK

**Locations**

- [`targets/esp32-idf/src/communication.cpp:842`](../targets/esp32-idf/src/communication.cpp#L842)
- [`targets/esp32-idf/src/communication.cpp:845`](../targets/esp32-idf/src/communication.cpp#L845)
- Plan requirement: [`plans/OD_SESSION_PLAN_2026-08-15.md:106`](../plans/OD_SESSION_PLAN_2026-08-15.md#L106)

**Observed behavior**

The ESP32 adapter calls `od_session_open(..., NULL)`, discarding `report.nonce_reason`. Every
non-OK result then enters the same branch and sends a three-byte plaintext NACK.

That loses a behavior already present in the authoritative Firmware implementation. For
`CMD_PIPE_WRITE_DATA`, `NONCE_REPLAY` and `NONCE_OUT_OF_WINDOW` mean ordinary duplicate/delayed
delivery. The correct response is silence: the missing sequence remains absent from the next SACK,
the host retransmits it under a fresh higher counter, and the transfer continues.

The PIPE client treats a `0x81` NACK as fatal. The landed code can therefore terminate an encrypted
PIPE upload on the first nonce-loss rejection.

**Why tests missed it**

The shared host suite tests `od_session_open()` result classification, but there is no target-
adapter test asserting the result-to-response policy for PIPE.

**Required correction**

1. Pass an `od_session_report` to `od_session_open()`.
2. For `CMD_PIPE_WRITE_DATA`, return without responding when `nonce_reason` is
   `NONCE_REPLAY` or `NONCE_OUT_OF_WINDOW`.
3. Preserve the NACK for `BAD_TAG` and other failures.
4. Keep the exception deliberately narrow; do not silently change legacy direct-write ACK policy.

**Required regression tests**

- PIPE DATA + replay nonce -> no response.
- PIPE DATA + out-of-window nonce -> no response.
- PIPE DATA + bad tag -> plaintext NACK.
- Non-PIPE command + replay nonce -> existing non-PIPE error behavior.

---

### OD-S2 — P2: authentication `NO_ROOM` is not transactional

**Locations**

- [`shared/core/od_session.c:276`](../shared/core/od_session.c#L276)
- [`shared/core/od_session.c:293`](../shared/core/od_session.c#L293)
- [`shared/core/od_session.c:303`](../shared/core/od_session.c#L303)
- [`shared/core/od_session.c:345`](../shared/core/od_session.c#L345)
- Contract: [`shared/core/od_session.h:281`](../shared/core/od_session.h#L281)
- Plan requirement: [`plans/OD_SESSION_PLAN_2026-08-15.md:513`](../plans/OD_SESSION_PLAN_2026-08-15.md#L513)

**Observed behavior**

The function applies the rate limiter before it knows which reply shape is required. It increments
`auth_attempts` and overwrites `last_auth_ms`, then checks the 23-byte step-1 or 19-byte step-2
capacity.

Consequently, a call returning `OD_SESSION_AUTH_NO_ROOM` can still:

- consume one authentication attempt;
- open or extend the 60-second idle-gap rate window;
- eventually rate-limit a peer even though every call was rejected before protocol processing.

This violates the documented rule that capacity is checked before **any** state mutation.

The current production adapters always provide `OD_SESSION_REPLY_MAX`, so this is primarily a
shared API correctness defect today. It becomes externally relevant as soon as another caller uses
a smaller response buffer.

**Why tests missed it**

[`tests/host/session_test.c:182`](../tests/host/session_test.c#L182) resets the session for each
step-1 capacity and checks only `challenge_pending` and key-set calls. It does not assert
`auth_attempts`, `last_auth_ms`, or the complete struct state. It also does not perform the plan's
required capacity sweep across step 2.

The auth fuzzer records only whether the session was authenticated before the call. It likewise
does not compare the full pre/post state on `NO_ROOM`.

**Required correction**

Classify the body sufficiently to determine the required reply size, perform the step-specific
capacity preflight, and only then enter the rate limiter or mutate session state.

**Required regression tests**

- Sweep `rsp_cap == 0..22` for step 1 and compare every session field before/after `NO_ROOM`.
- Mint a valid challenge, construct a valid step-2 proof, sweep `rsp_cap == 0..18`, and compare the
  complete session and fake-HAL call counters before/after.
- Assert that repeated `NO_ROOM` calls cannot cause `RATE_LIMITED`.
- Strengthen the fuzzer: on `NO_ROOM`, compare a snapshot of the complete session state rather than
  only `authenticated`.

---

### OD-S3 — P2: the step-2 challenge is retained on malformed and early crypto failures

**Locations**

- [`shared/core/od_session.c:359`](../shared/core/od_session.c#L359)
- [`shared/core/od_session.c:375`](../shared/core/od_session.c#L375)
- [`shared/core/od_session.c:381`](../shared/core/od_session.c#L381)
- [`shared/core/od_session.c:392`](../shared/core/od_session.c#L392)
- [`shared/core/od_session.c:441`](../shared/core/od_session.c#L441)
- Contract: [`shared/core/od_session.h:284`](../shared/core/od_session.h#L284)
- Plan requirement: [`plans/OD_SESSION_PLAN_2026-08-15.md:599`](../plans/OD_SESSION_PLAN_2026-08-15.md#L599)

**Observed behavior**

Challenge consumption is inconsistent:

- success consumes it;
- an expired challenge consumes it;
- a wrong proof consumes it;
- server-proof or key-set failures call `od_session_clear()` and consume it indirectly;
- expected-proof CMAC failure, session-key derivation failure, session-id derivation failure, and
  the all-zero session-id rejection leave it pending;
- a malformed request while a challenge is pending also leaves it pending.

The revised plan and public header say that every step-2 outcome reaching this stage consumes the
challenge, except `RATE_LIMITED`, `BAD_ARGUMENT`, and capacity-preflight `NO_ROOM`.

**Impact**

The implementation no longer enforces its stated single-use challenge model on all paths. A peer
can retry against the same server nonce after malformed input or some engine failures. The rate
limiter and 30-second expiry limit the exposure, but they do not make the state machine correct.

**Required correction**

Introduce one challenge-consumption helper that clears:

- `challenge_pending`;
- `pending_server_nonce`;
- `challenge_ms`.

Call it on every specified step-2 outcome. Keep the three explicitly non-consuming exits ahead of
that point.

**Required regression tests**

- Pending challenge + malformed body consumes the challenge.
- Forced CMAC/KDF failure consumes the challenge.
- Forced key-set failure consumes the challenge and leaves no session/key.
- `RATE_LIMITED`, `BAD_ARGUMENT`, and `NO_ROOM` preserve the pending challenge.

---

### OD-S4 — P2: nonce rejection logs are not rate-limited

**Locations**

- [`targets/esp32-idf/src/communication.cpp:842`](../targets/esp32-idf/src/communication.cpp#L842)
- [`targets/esp32-idf/src/communication.cpp:850`](../targets/esp32-idf/src/communication.cpp#L850)
- [`targets/nordic-zephyr/src/opendisplay_pipe.c:999`](../targets/nordic-zephyr/src/opendisplay_pipe.c#L999)
- [`targets/nordic-zephyr/src/opendisplay_pipe.c:1004`](../targets/nordic-zephyr/src/opendisplay_pipe.c#L1004)
- Contract: [`shared/core/od_session.h:323`](../shared/core/od_session.h#L323)
- Plan requirement: [`plans/OD_SESSION_PLAN_2026-08-15.md:113`](../plans/OD_SESSION_PLAN_2026-08-15.md#L113)

**Observed behavior**

Both target adapters discard the report and emit a log for every rejected envelope. The core
deliberately does not count wrong-session, replay, or out-of-window results as integrity strikes,
so those paths do not naturally terminate or throttle a noisy peer.

**Impact**

A stale or hostile connected peer can drive unbounded logs. On embedded targets this can consume
CPU, serial bandwidth, and logging buffers; depending on backend configuration it can also perturb
timing on a path that is expected to recover from packet loss.

**Required correction**

- Retain `od_session_report` at both call sites.
- Apply independent five-second budgets to wrong-session and replay/window logs, matching the
  authority implementation.
- Preserve enough report detail to distinguish routine loss from tag failure and engine failure.
- Combine this work with OD-S1 so one result classification drives both response and log policy.

**Required regression tests**

Use an injectable clock or a small target-policy helper to assert first-log, suppressed-log, and
post-five-second-log behavior independently for each nonce rejection site.

---

### OD-S5 — P2: `od_session_seal()` validates after narrowing `size_t`

**Locations**

- [`shared/core/od_session.c:599`](../shared/core/od_session.c#L599)
- [`shared/core/od_session.c:603`](../shared/core/od_session.c#L603)

**Observed behavior**

`plain_frame.n - 2` is cast to `uint16_t` before it is compared with
`OD_SESSION_PAYLOAD_MAX`. `sealed_len` is also derived through a narrowing cast.

For example, when `plain_frame.n == 65538`, the logical payload length is 65536 but the cast makes
`payload_len == 0`. The maximum check passes and the function seals an empty payload rather than
returning `OD_SESSION_SEAL_TOO_LONG`.

Normal target call sites currently pass small statically bounded frames, so this does not imply an
active overflow there. It is nevertheless a definite public API error and defeats the reason the
span length is a `size_t`.

**Required correction**

Before any narrowing or addition, check:

```c
if (plain_frame.n > 2u + OD_SESSION_PAYLOAD_MAX) {
    return OD_SESSION_SEAL_TOO_LONG;
}
```

Then derive the `uint16_t` lengths. The HAL-capacity cast should likewise use the already-computed,
bounded required capacity instead of casting arbitrary `out_cap` arithmetic.

**Required regression tests**

- `plain_frame.n == OD_SESSION_PLAIN_FRAME_MAX` succeeds.
- One byte above the maximum returns `TOO_LONG`.
- Representative wrap values such as 65538 and `SIZE_MAX` return `TOO_LONG` without calling the
  cipher or advancing `tx_counter`.

---

### OD-S6 — P3: authentication crypto status is reported as `OK` on failure

**Locations**

- [`shared/core/od_session.c:311`](../shared/core/od_session.c#L311)
- [`shared/core/od_session.c:359`](../shared/core/od_session.c#L359)
- [`shared/core/od_session.c:406`](../shared/core/od_session.c#L406)
- ESP32 log: [`targets/esp32-idf/src/encryption.cpp:151`](../targets/esp32-idf/src/encryption.cpp#L151)
- Nordic log: [`targets/nordic-zephyr/src/opendisplay_pipe.c:135`](../targets/nordic-zephyr/src/opendisplay_pipe.c#L135)

**Observed behavior**

`report_reset()` initializes `crypto_status` to enum value zero, `OD_HAL_CRYPTO_OK`. The
authentication helpers collapse HAL results to `bool`, and the direct RNG/key-set checks compare
the result without saving it. Authentication can therefore return `OD_SESSION_AUTH_CRYPTO_ERROR`
while both targets log `status 0`.

**Impact**

This does not weaken the rejection itself, but it makes first-hardware diagnosis misleading. The
plan specifically identifies PSA algorithm-policy mistakes as likely target failures; reporting
`OK` on that path obscures the evidence needed to distinguish configuration, unsupported
algorithm, and generic engine failure.

**Required correction**

Preserve `enum od_hal_crypto_status` through each helper or provide an explicit status output.
Populate `report.crypto_status` before every `OD_SESSION_AUTH_CRYPTO_ERROR` return.

**Required regression tests**

Force each relevant HAL operation to return `UNSUPPORTED` and `ERROR`; assert both the auth result
and the exact report value.

---

### OD-S7 — P3: proof comparison does not use the specified volatile accumulator

**Locations**

- [`shared/core/od_session.c:30`](../shared/core/od_session.c#L30)
- Plan requirement: [`plans/OD_SESSION_PLAN_2026-08-15.md:263`](../plans/OD_SESSION_PLAN_2026-08-15.md#L263)

**Observed behavior**

`od_ct_equal()` uses a fixed-iteration XOR/OR loop, but its accumulator is an ordinary `uint8_t`.
The plan explicitly required a `volatile` accumulator. No test directly pins this property.

The current source has data-independent loop bounds, and common embedded compilers may preserve a
constant-work implementation. That is not the same as satisfying the requested hardening: the C
optimizer is free to transform non-volatile operations in ways the source author did not intend.

**Required correction**

Use the specified volatile accumulator, retain the fixed-length loop, and inspect optimized target
assembly for the 8-byte session-id and 16-byte proof call sites. Document that this is best-effort
portable constant-time code, not a guarantee provided by ISO C.

---

### OD-S8 — P3: Nordic one-shot PSA operations ignore key-destruction failures

**Locations**

- CMAC: [`targets/nordic-zephyr/src/od_hal_crypto.c:165`](../targets/nordic-zephyr/src/od_hal_crypto.c#L165)
- ECB: [`targets/nordic-zephyr/src/od_hal_crypto.c:193`](../targets/nordic-zephyr/src/od_hal_crypto.c#L193)

**Observed behavior**

Both one-shot operations import a volatile PSA key, perform the operation, and discard the return
from `psa_destroy_key()`. If destruction fails, the function can return success while losing the
only handle to a live key.

**Impact**

The failure is expected to be rare, but repeated failures leak finite PSA key resources and can
eventually make authentication fail. This is the slow resource-exhaustion shape the slot design was
created to prevent for the prepared CCM key.

**Required correction**

Track operation status and destruction status separately. Always attempt destruction after a
successful import; if destruction fails, log it and return `OD_HAL_CRYPTO_ERROR`, without masking an
earlier operation error.

Hardware fault injection may not be practical, but the cleanup logic can be factored so a host
unit test verifies status precedence.

## 5. Test-suite gaps exposed by the findings

The landed suite is strong on shared algorithmic behavior but weak at the boundaries where most of
the findings live.

### 5.1 Transactionality is asserted too narrowly

The named capacity test does not satisfy the plan's complete requirement:

- it covers only step 1;
- it checks selected state rather than full state identity;
- it does not assert rate-limit timestamps/counters;
- it does not verify that no HAL operation occurred on a short-capacity step 2.

### 5.2 Challenge failure transitions are missing

There are tests for success, wrong proof, expiry, and rate-limited preservation, but not for:

- malformed body with a pending challenge;
- expected-proof CMAC failure;
- session-key derivation failure;
- session-id derivation or all-zero-ID failure;
- server-proof failure;
- key-set failure.

### 5.3 Target policy is not tested

The shared result enum is tested, but neither target has an adapter-level test for:

- response versus silence after each open result;
- plaintext versus sealed error response;
- per-site log throttling;
- use of `report.nonce_reason`;
- reporting crypto engine status.

OD-S1 is the consequence: the core produced enough information for correct behavior, but the
adapter discarded it and no test noticed.

### 5.4 Large `size_t` API values are absent

Fuzz inputs are naturally small because they correspond to allocated input buffers. Add explicit
unit cases for protocol APIs that accept `size_t` and narrow to wire-sized integers.

## 6. Verification results

### Confirmed locally

- Shared boundary checks passed.
- GCC host suite passed.
- `od_session_test` under Clang: **11,195 checks, 0 failures**.
- `od_session_test` under ASan/UBSan with LeakSanitizer disabled: **11,195 checks, 0 failures**.
- `session_open_raw`, `session_open_sealed`, and `session_auth` fuzz targets completed substantial
  bounded runs with no crash after disabling LeakSanitizer for the `ptrace` environment.
- `git diff --check` passed.

### Not established by this review

- A clean full `tools/check.sh` result in an unrestricted environment.
- The pinned py-opendisplay corpus run in this sandbox.
- Fresh ESP32 builds of every fragment.
- Fresh Nordic builds of all three boards.
- Authentication or encrypted traffic on physical ESP32/Nordic hardware.
- Real-device mbedTLS/PSA interoperability with py-opendisplay.
- PIPE recovery under real BLE reordering/loss.

Existing commit records and release artifacts state that all target variants built. That is useful
evidence, but it is not a substitute for re-running the current tree or for the plan's hardware
Gate 2.

## 7. Plan-compliance gaps

### 7.1 Hardware Gate 2 remains open

The plan states that hardware is the only verification that counts for the target swaps. Current
status documentation says `od_session` is hardware-verified on neither target:

- [`CLAUDE.md:54`](../CLAUDE.md#L54)
- [`CLAUDE.md:111`](../CLAUDE.md#L111)
- Plan gate: [`plans/OD_SESSION_PLAN_2026-08-15.md:882`](../plans/OD_SESSION_PLAN_2026-08-15.md#L882)

This is especially important on Nordic because the promotion replaces the shipped hand-rolled CCM
with native PSA CCM, and no host fake can prove the shortened-tag PSA policy on the device.

### 7.2 The sole gate omits Nordic target builds

[`tools/check.sh`](../tools/check.sh) calls itself “every gate this repo has” but its target-build
section contains only the optional ESP32 baseline. It never invokes
`targets/nordic-zephyr/build.sh --all`, despite the verification plan requiring all three Nordic
boards.

Because CI has been removed, this omission means no automatic or standard local gate protects the
Nordic crypto HAL from build regressions.

### 7.3 CI requirement and landed process disagree

The plan's C4 section says CI wiring is part of the commit, including a fuzz job. The landed history
later deletes all workflows and makes `tools/check.sh` manual-only. That may be a deliberate project
decision, but it is still a divergence from the reviewed plan and weakens enforcement:

- nothing runs on push;
- the latest-py-opendisplay drift check is no longer scheduled;
- opt-in target builds are easy to omit;
- Nordic is not represented even when the script is run.

### 7.4 C0 capture was deliberately skipped

The plan records this decision honestly. Its consequence remains: the host crypto oracle proves
agreement with a transcription of readable source, not with bytes captured from a deployed device.
Once both targets have swapped, there is no untouched implementation in this repository from which
to take the original capture.

## 8. Known residual protocol risk

The bidirectional nonce-reuse flaw is not counted as a promotion regression because every shipping
implementation already has it and fixing it unilaterally would break the wire protocol. It remains
a high-severity production risk:

- inbound and outbound use the same session key;
- both use the same session ID;
- both counters start at zero;
- therefore the same CCM nonce is used in both directions under one key.

The issue and protocol-revision options are documented in
[`docs/FOLLOWUPS.md:360`](FOLLOWUPS.md#L360). Directional key separation remains the recommended
fix, with a nonce-domain bit as the lower-resource alternative.

## 9. Recommended repair order

1. **Fix OD-S1 first:** restore PIPE silence for replay/out-of-window results and retain the report
   at the target adapter.
2. **Fix OD-S2 and OD-S3 together:** restructure authentication preflight and centralize challenge
   consumption, then add complete state-transition tests.
3. **Fix OD-S4:** use the same retained report for classified, rate-limited logging on both targets.
4. **Fix OD-S5:** validate `size_t` lengths before narrowing and add wrap-value unit tests.
5. **Fix OD-S6 through OD-S8:** preserve crypto status, harden the comparison, and check PSA
   destruction.
6. Add Nordic `--all` to the standard gate, or define a clearly named opt-in target-build mode that
   includes both target families and is required before merge/release.
7. Re-run the complete host, sanitizer, fuzz, wire-corpus, ESP32, and Nordic gates.
8. Perform hardware Gate 2 independently on ESP32 and `xiao_nrf52840`, including encrypted PIPE
   reorder/retransmission behavior.

## 10. Proposed sign-off criteria

`od_session` is ready for correctness sign-off when all of the following are true:

- OD-S1 through OD-S8 are fixed or explicitly dispositioned with rationale.
- The expanded transactional and challenge-lifecycle unit tests pass under GCC and Clang.
- ASan/UBSan and all three fuzz targets pass in an unrestricted runner.
- The pinned py-opendisplay corpus passes.
- All ESP32 fragments and all three Nordic boards build from the final tree.
- ESP32 hardware passes authentication, encrypted config write/read, encrypted image upload,
  timeout, replay refusal, TLS bypass, and PIPE reorder/retransmission.
- Nordic hardware passes authentication, encrypted config write/read, encrypted image upload,
  timeout, replay refusal, and the target-appropriate encrypted transfer path.
- Failure logs show meaningful non-zero crypto status on injected/observed engine failures.
- Nonce-loss logging is demonstrably throttled and PIPE nonce loss does not emit a fatal NACK.

Until those conditions are met, the best description is: **algorithmically promising and strongly
host-tested, but not yet correct against the final plan and not yet proven on target hardware.**
