# Converge debug logging onto shared/core, ESP32 wording as default

## 0. Status — Stages 0a-4 landed on main 2026-08-31

Merged as PR #76 (`7d22fa8`). Everything below this section is the plan as written; the stage
list in § 7 marks what is done, and Q1, Q2 and Q5 in § 9 are answered by what shipped.

| Stage | Commit | What landed |
|---|---|---|
| 0a (L0a) | `286cc85` | Nordic emits one terminal `CR LF` per record |
| 0 (L0) | `7b399b3` | `od_select_log_profile()`; no target consumes it yet, by design |
| 3 (L2) | `54e0663` | Session auth/decrypt/seal wording in `od_session.c` |
| 2 (L1) | `6cb49cb` | `od_rxq.c` logs its own arrivals and drops; `od_rxq_app_report()` gone |
| 4 (L3) | `6776bbd` | One dropped-response line in `od_txq.c`, at INFO |

Four defects found reviewing those commits landed with them: the host log stub sat inside the
fake archives where archive-extraction order never pulled it, so a *fresh* configure failed to
link five suites and the clang fuzz targets (`5a4677e`); the session throttle read a zero
timestamp as "never logged" and kept its state at `OD_CAP_LOG=0` (`ad5d45d`); Nordic's terminator
rewrite was skipped above its copy buffer, restoring the doubled CR (`709f0b6`); and the
selector's duplicate guard missed the `-D` and bare valueless spellings, the latter defining the
macro as `1` — `OD_LOG_WARN` — so it builds clean at a level nobody chose (`ba33c55`).

Verified at merge: `tools/check.sh` 42 passed / 0 failed, and all three target families green
(`--esp32` 10 fragments, `--nordic` three boards, `--silabs` BG22 headless). **The Nordic
hardware capture in § 8 is still open** — L0a is proven by the modeled host adapter test only, so
no board has yet shown the CDC ACM and RTT bytes.

**Remaining: Stages 5-9** — transfer (L4), config (L5), NFC (L6, needs Q3), dispatch/gate/reply/
cmd (L7), and the independent ESP32 `ESP_LOGx` cleanup. Q3 and Q4 are still open.

## 1. Objective and authority

Maximize the fraction of debug/diagnostic logging that lives in `shared/core/*.c` (one
implementation, both targets get it for free) and minimize what remains in `targets/esp32-idf/`
and `targets/nordic-zephyr/`. What's left in `targets/` after this pass should be genuinely
platform-specific: HAL driver internals, radio/BLE/Wi-Fi stack lifecycle, deep-sleep/RTC,
watchdog, panel/display driver timing — content with no shared subsystem behind it.

**Authority for this pass is `targets/esp32-idf/` as it exists in this repo today** — not
`../Firmware`. This is a deliberate, scoped exception to the general sibling-authority rule in
`CLAUDE.md` ("THE AUTHORITY IS `../Firmware/`"), which governs *protocol/algorithm* correctness.
Log text is not wire-visible and carries no correctness content, so that rule doesn't bind here;
the user has directed ESP32's in-repo wording/levels as the default instead. Where Nordic diverges
from that default, it is evaluated case by case in § 5 — adopt-ESP32, adopt-Nordic-instead, or
flagged as a question in § 9. **BG22 (`targets/efr32bg22-slc/`) is untouched this round** — see § 10.

## 2. Current state

`shared/core/od_log.c` + `shared/hal/od_hal_log.h` already unify the record *format*
(`[%04lu.%03lu|Cn] %c: text`) — every target already goes through it. `OD_CAP_LOG` and `HAL_LOG`
are in every target's tier list (`shared/sources.cmake` — ESP32 takes the aggregate; Nordic and
BG22 both list `HAL_LOG` explicitly), so `od_log.h` is universally linkable. That part isn't broken
and this plan doesn't touch it.

What's actually diverged is *where the log call sites live*. Today:

| Shared subsystem (`shared/core/`) | `od_log_*` calls inside it | Where the logging about it actually happens |
|---|---|---|
| `od_dispatch.c`, `od_gate.c`, `od_reply.c`, `od_cmd.c`, `od_core.c`, `od_txq.c`, `od_rxq.c`, `od_config.c`, `od_config_read.c`, `od_config_asm.c`, `od_config_tlv.c`, `od_xfer.c`, `od_xfer_direct.c`, `od_xfer_partial.c`, `od_nfc.c` | **0** | Scattered in target app code, or via two "report" seams (below) |
| `od_pipe.c` | 1 | mostly target code |
| `od_touch_gt911.c` | 13 | **nowhere else — logs directly, no seam** |
| `od_sensor_bq27220.c`, `od_sensor_sht40.c` | 3, 2 | nowhere else |

Call-site counts, target app code (`targets/*/src`, excludes HAL):

| Area | ESP32 (`od_log_*` calls) | Nordic (`od_log_*` calls) |
|---|---|---|
| Total | 567 (248 at debug level) | 175 (10 at debug level) |
| Transfer (`display_service.cpp` / `opendisplay_display.cpp`+`opendisplay_pipe.c`) | 151 | 27 + 2 |
| Config (`config_parser.cpp` / `opendisplay_config_parser.c`) | 180 | 18 |
| Session/auth (`od_session_app.cpp` / `od_session_app.c`) | 10 | 7 |
| RXQ report (`od_rxq_app.cpp` / `od_rxq_app.c`) | 5 | 4 |

**The unlock for this whole plan**: two seams exist *specifically because someone believed
`shared/` could not call `od_log.h`.` `shared/core/od_rxq.h`:135 says outright: *"THIS MODULE DOES
NOT LOG. od_log.h is target-local and shared/ may not include it, so arrivals and drops are
reported through `od_rxq_app_report()`."* `shared/core/od_session_app.h`'s implementers repeat the
same claim (`od_session_app.c`:8, both targets, near-identical wording). **That premise is false**
— `od_log.h` lives in `shared/core/`, is a PURE-tier header (no vendor includes), and
`od_touch_gt911.c`/`od_pipe.c`/`od_sensor_*.c` already call it directly with zero seam. Once this
is corrected, both report seams lose their stated reason to exist.

There is also one hardware-proven transport defect at the Nordic HAL boundary. A raw-mode capture
from the `xiao_nrf52840` USB CDC ACM device contained 28 `CR CR LF` terminators and zero ordinary
`CR LF` terminators. `od_log.c` supplies the first `CR LF`; Zephyr's deferred text formatter adds
the second `CR` before the CDC ACM driver, whose `poll_out` implementation is byte-transparent.
The ESPHome web console removes only one carriage return and treats the remaining one as an
overwrite command, so lines render on top of one another. The relevant `LOG_RAW` formatter path is
unchanged between the repo's pinned NCS 3.3.1 (`docs/TOOLCHAINS.md`) and the NCS 3.4.0 migration
target; the SDK migration therefore does not retire this defect. L0a below makes the correction
part of this refactor rather than a separate temporary patch.

## 3. Scope boundary

**In scope (converge into `shared/core/`):** dispatch/gate routing and refusal decisions, session
handshake/auth/decrypt/seal events, RXQ admission/drop, TXQ drop, transfer (`od_xfer*`/`od_pipe`)
progress and state events, config parse/validate/store outcomes, NFC assembly events.

**Out of scope (stays in `targets/`, untouched by this plan):** HAL implementation internals
(`od_hal_crypto.c`, `od_hal_nvs.c`, `od_hal_adc.c`, `od_hal_radio.c`, `od_hal_gpio.c`,
`od_hal_wdt.c` — these differ by construction, one target owns each `.c`), BLE/Wi-Fi stack
lifecycle and TLS, deep-sleep/RTC, watchdog subsystem specifics, panel/display driver timing
(FastEPD/bb_epaper), individual `od_cmd_app_*` command-handler *bodies* (the header says outright:
"a target supplies the BEHAVIOUR of a command" — logging about a handler's own decision, e.g.
device_control.cpp's `CMD_DEEP_SLEEP` rejections, is part of that behavior and stays put).
**One deliberate exception:** `targets/nordic-zephyr/src/od_hal_log.c` is otherwise the same kind
of target-owned HAL file, but its terminal-newline correction (L0a) is in scope — this plan is
what hardware-verifies converged log output on Nordic, and doing that against a transport known to
corrupt every line would defeat the verification. No other HAL file gets this exception.

**Adjacent, not part of this plan:** ESP32's HAL layer still has 22 raw `ESP_LOGx` call sites
that bypass `od_log` entirely (`od_hal_crypto.c`, `od_hal_nvs.c`, `od_hal_adc.c`,
`ble_transport_esp32.cpp:328`). Converting those to `od_log_error/warn` is a separate, small,
independent cleanup (unifies the *transport*, not the *ownership*) — listed as Stage 9 so it isn't
lost, but it doesn't block or depend on anything below.

## 4. The pattern to replicate

`od_touch_gt911.c` is the existing, working template: the shared driver calls `od_log_warn`/
`od_log_info` directly at the point it detects the event, using its own text. Its seam header
(`od_touch_app.h`) keeps only what's genuinely per-target (GPIO pins, reset delays, IRQ mask) —
nothing about logging. `od_session_app.h`'s other three accessors (`_state`, `_security`,
`_device_id`) are the same kind of genuinely-per-target fact and should **stay** seams. The two
*report* functions get different treatment, not one blanket "remove the anomaly": `od_rxq_app_report()`
(L1) has no BG22 consumer and is removed outright. `od_session_app_report()` (L2) does have a live
BG22 consumer — its text/level logic moves into `od_session.c` like the others, but the callback
itself stays, as a BG22-only compatibility seam with trivial ESP32/Nordic stubs (see L2's "Land
as, and where" and BG22 note).

## 5. Conversion candidates

### L0a — Nordic terminal newline contract

Make the Nordic HAL own the adaptation between shared complete records and Zephyr's terminal
newline policy. `od_log.c` continues to produce complete records ending in `CR LF`, preserving the
shared and ESP32 contract. `shared/hal/od_hal_log.h:21` is explicit that `od_hal_log_write()`
"must not mutate or retain record" — so `targets/nordic-zephyr/src/od_hal_log.c` must **not**
edit the caller's buffer in place. Instead, when the record ends in a terminal `CR LF`, copy it
into a bounded local buffer (sized from the `len` argument `od_hal_log_write()` already receives,
capped at `OD_LOG_TEXT_MAX`/`OD_LOG_RAW_TEXT_MAX` — the same 253/255-byte constants
`zephyr/CMakeLists.txt` already defines for this target) with that trailing `CR` dropped, and
submit the copy through `LOG_PRINTK`, whose explicit
contract owns `LF`-to-`CR LF` conversion. The original `record` passed in is read-only throughout.
This is an exact two-way split, not a choice left to the implementation: a record ending in a
terminal `CR LF` takes the bounded-copy-then-`LOG_PRINTK` path above; everything else — including
every `od_log_raw()` call, which is unterminated by contract — keeps going through `LOG_RAW`
unchanged, exactly as today. Do not route unterminated data through `LOG_PRINTK`: Zephyr's public
logging header documents `LOG_RAW` as emitting the supplied string without appending characters,
while `LOG_PRINTK` carries `printk`-style semantics — mixing the two paths for the "everything
else" case would reintroduce exactly the kind of behavior-depends-on-which-macro-was-used
ambiguity this section exists to eliminate, and would make the host test's byte-exact assertions
non-deterministic.

```text
shared complete record:  CR LF
Nordic HAL adaptation:      LF
Zephyr terminal output:  CR LF
```

This avoids relying on `LOG_RAW` behaving differently in a later SDK and avoids duplicating
Zephyr's backend selection, deferred copying and serialization with direct CDC/UART writes. Keep
it as a distinct commit within the logging refactor so the behavior remains independently
reviewable and revertible.

Extend the existing Nordic production-adapter host test so its fake logger distinguishes
`LOG_PRINTK` from `LOG_RAW` and models the documented newline conversion. It must prove that a
complete record reaches the modeled transport with exactly one terminal `CR LF`, that no
`CR CR LF` occurs, that unterminated raw data remains unterminated, and that clobbering the
caller's mutable stack record after submission does not alter queued bytes.

Hardware qualification is required on both Nordic backend profiles: capture raw CDC ACM bytes on
`xiao_nrf52840`, and raw RTT bytes on one nRF54 board. The online ESPHome console must also show
successive Nordic records on separate lines without overwrite. Preserve the captures or exact byte
counts in the implementation evidence.

### L0 — Shared compile-time log-level selector contract

The call sites cannot converge meaningfully while the two targets select their compiled-in level
through different mechanisms. ESP32's two `*-debug.cmake` board fragments inject
`OD_LOG_LEVEL=OD_LOG_DEBUG` directly into `OD_BOARD_DEFINES`, while ordinary ESP32 boards rely on
`od_log.h`'s implicit INFO fallback. Nordic instead turns `PROFILE=debug` into `OD_DEBUG_BUILD` and
then explicitly defines either `OD_LOG_LEVEL=OD_LOG_DEBUG` or `OD_LOG_LEVEL=OD_LOG_INFO` in
`zephyr/CMakeLists.txt`. The resulting levels happen to agree today, but the directives and their
precedence can drift independently.

Create one target-neutral CMake selector in `shared/profiles.cmake`:

```cmake
# Input: OD_LOG_PROFILE is exactly "info" or "debug".
# Output: OD_LOG_COMPILE_DEFINITION is exactly one OD_LOG_LEVEL=... definition.
function(od_select_log_profile profile out_var)
  # Validate the profile and return OD_LOG_LEVEL=OD_LOG_INFO or OD_LOG_LEVEL=OD_LOG_DEBUG.
endfunction()
```

The selector accepts exactly `info` or `debug` and returns exactly one definition:
`OD_LOG_LEVEL=OD_LOG_INFO` or `OD_LOG_LEVEL=OD_LOG_DEBUG`. Reject an unknown input, an empty
selection, or a destination definition list that already contains an `OD_LOG_LEVEL` entry. Test
those cases with an isolated CMake configure fixture so this contract is proven without assigning
ownership of either target's front-door wiring here.

See `PLAN_LOG_PROFILE_BUILD_CONVERGENCE_2026-08-30.md` §§ 3–5 for how each front door supplies
`OD_LOG_PROFILE`, including ESP32 debug pseudo-board removal, Nordic transport/log-profile
composition and legacy mapping. That plan also owns emitted firmware definitions, `--all` profile
expansion, release artifacts and manifest compatibility. None of it is a prerequisite for L1/L2.

### L1 — `od_rxq_app_report` → inline into `od_rxq.c`

Per-event comparison (ESP32 is `targets/esp32-idf/src/od_rxq_app.cpp`, Nordic is
`targets/nordic-zephyr/src/od_rxq_app.c`):

| Event | ESP32 (default) | Nordic | Disposition |
|---|---|---|---|
| `OD_RXQ_DROP_EMPTY` | `warn`, "WARNING: Empty BLE frame received, dropping" | `info`, "rx dropped: empty frame" | Adopt ESP32 text + level |
| `OD_RXQ_DROP_TOO_LARGE` | `warn`, includes byte counts | `info`, includes byte counts; Nordic's comment notes this is now unreachable there because GATT already refuses over-length writes at ATT with `0x0D` | Adopt ESP32 text + level. Note (not in scope): ESP32 has no equivalent pre-filter, so this line *is* reachable there — a possible follow-up, not a logging question |
| `OD_RXQ_DROP_FULL` | `error` | `info` | Adopt ESP32's `error` — a full ring is more severe than a malformed frame |
| `OD_RXQ_ARRIVED` | `debug`, full hex dump (32 B) + `[BLE][Q:n] ERX/URX 0x%04X (n B):` label + per-frame encrypted/plaintext token | **`info`** (not `debug` — corrected from an earlier draft of this table), one-line `rx cmd=0x%04X len=%u q=%u`, no hex, no encrypted token | See § 9 Q1 |
| Quiet-frame suppression | `imageWriteLogQuietFrame()` — checks a live streaming-image state machine (`imgLogChunks`, `imageWriteFramesMayStillArrive()`) to silence *only while a stream is actually mid-flight* | hardcoded: always silent for `CMD_DIRECT_WRITE_DATA`/`CMD_PIPE_WRITE_DATA`, regardless of state | Adopt ESP32's state-aware version — see L4, this becomes the first concrete item there |

Land as: `od_rxq.c` calls `od_log_*` directly for all four cases using ESP32's text/levels. The
`ERX`/`URX` token computation uses `CMD_AUTHENTICATE`, `CMD_FIRMWARE_VERSION`,
`BLE_CMD_HEADER_SIZE`, `ENCRYPTION_NONCE_SIZE` and `ENCRYPTION_TAG_SIZE`, which are portable, but
ESP32's `isEncryptionEnabled()` is not: it is a target-local function and Nordic has no equivalent
symbol. Add one narrow cross-target seam for that fact:

```c
/* shared/core/od_rxq_app.h */
bool od_rxq_app_encryption_enabled(void);
```

- ESP32 implements it by returning `isEncryptionEnabled()` so the existing log classification is
  unchanged.
- Nordic implements it by returning
  `od_session_security_enabled(od_session_app_security())`, which applies the same canonical
  configured-key rule used by shared dispatch/reply instead of inventing a second interpretation
  of its parsed security config.
- Host RXQ fixtures implement it as a controlled test value and add explicit ERX/URX cases.
- BG22 does not consume `APP_RXQ`, so it needs no implementation and gains no new dependency.

Keep this as an RXQ app seam rather than making `od_rxq.c` include `od_session_app.h`: `APP_RXQ`
is currently an independently selectable source tier, and a direct call to the session seam would
silently make it depend on `APP_SESSION`. Ratchet both ESP32 and Nordic implementations into their
target source lists and add a link fixture that fails if an `APP_RXQ` consumer omits the new
function. If a later source-tier consolidation makes `APP_RXQ` depend formally on `APP_SESSION`,
the two target implementations may be replaced by one shared inline helper, but that dependency
change is not part of this logging pass.

The quiet-frame predicate depends on transfer state that only `od_xfer.c` should own after L4
lands; until then, keep it as the second minimal function in the same seam
(`bool od_rxq_app_quiet(uint16_t cmd)`) rather than the full report function it's currently bundled
into.

### L2 — `od_session_app_report` → inline into `od_session.c`

Per-case comparison (ESP32: `targets/esp32-idf/src/od_session_app.cpp`, Nordic:
`targets/nordic-zephyr/src/od_session_app.c`):

| `enum od_session_auth` case | ESP32 | Nordic | Disposition |
|---|---|---|---|
| `CHALLENGE` | `info`, "Authentication challenge sent" | `info`, "auth: challenge sent" | Adopt ESP32 wording |
| `ESTABLISHED` | `info`, "Authentication successful, session established" | `info`, "auth: session established" | Adopt ESP32 wording |
| `REJECTED` | `error`, "ERROR: Authentication failed (wrong key)" | falls into Nordic's `default:` | Adopt ESP32 — this is a real coverage gap on Nordic, not a style choice |
| `RATE_LIMITED` | `warn`, includes attempt count | `warn`, includes attempt count | Adopt ESP32 wording (functionally identical already) |
| `NOT_CONFIGURED` | `error`, "ERROR: Authentication requested but encryption is not configured" | **absent** — falls into `default:` as a generic "refused" | Adopt ESP32 — coverage gap |
| `EXPIRED` | `error`, "ERROR: Server nonce expired" | **absent** — falls into `default:` | Adopt ESP32 — coverage gap |
| `CRYPTO_ERROR` | `error`, includes `crypto_status` | `error`, includes `crypto_status` | Adopt ESP32 wording |
| default/invalid | `error`, "ERROR: Invalid authentication request (rc=%d)" | `warn`, "auth: refused (rc=%d, status 0x%02X)" | Adopt ESP32's `error` + wording; Nordic's extra `status_byte` field is folded in as a coverage improvement, not dropped |

`OD_SESSION_APP_OPEN` (decrypt failed) and `OD_SESSION_APP_SEAL` are functionally identical
already — text differs only in capitalization/prefix; adopt ESP32's. The rate-limit throttle
(`budgetAllows`/`budget_allows`) is **duplicated near-verbatim** in both files — hoist it into
`od_session.c` as one static implementation instead of two. It is two independently-throttled
buckets, not one per specific nonce reason: `s_logWindowMs`/`s_log_window_ms` covers replay and
out-of-window rejections, `s_logOtherMs`/`s_log_other_ms` covers every other rejection (wrong
session, bad tag, malformed, engine fault), each at a 5 s minimum interval.

**Land as, and where — corrected; an earlier draft of this section wrongly located the existing
call inside `od_session.c`.** `od_session_app_report()` is not called from `od_session.c` today —
`shared/core/od_gate.c:37` and `:96` call it for the `AUTH` and `OPEN` cases, and
`shared/core/od_reply.c:61` calls it for `SEAL`. "Inline into `od_session.c`" means the new direct
`od_log_*` text/level logic moves into `od_session.c`'s own functions (`od_session_authenticate()`,
`od_session_open()`, `od_session_seal()` or equivalent), at the point each one determines its
result code — using ESP32's text/levels, plus Nordic's two missing cases restored for both targets
since there's now only one implementation. The **existing** calls to `od_session_app_report()` in
`od_gate.c` and `od_reply.c` are left exactly as they are today — not moved into `od_session.c`,
and not duplicated by a second call from inside it. The four accessor functions (`_state`,
`_security`, `_now_ms`, `_device_id`) also stay exactly where they are — they're genuinely
per-target facts, not logging.

**BG22 note — corrected; the earlier draft of this plan wrongly called this a harmless no-op.**
`targets/efr32bg22-slc/od_session_app.c` implements `od_session_app_report()` with direct
`printf()` calls (`"[OD] auth session established\r\n"`, `"[OD] auth refused rc=%d..."`,
`"[OD] decrypt failed cmd=0x%04X..."`) — this is BG22's *only* auth/decrypt diagnostic output, and
it bypasses `od_log`/`OD_CAP_LOG` entirely, so it is not compiled out. Had the existing calls in
`od_gate.c`/`od_reply.c` been removed (the original plan), those `printf` lines would have stopped
firing — not "become unreferenced dead code," a real loss of BG22's only diagnostic for this path.
Leaving `od_gate.c`/`od_reply.c` untouched (see "Land as, and where" above) avoids that exactly
once per event, with no duplication: BG22's implementation and its output are untouched, per the
original instruction that BG22 stays untouched this round. The cost is that ESP32's and Nordic's
`od_session_app_report()` implementations become trivial (empty) stubs once their text moves into
`od_session.c` directly, rather than being deleted outright — an acceptable one-seam-call cost to
avoid a BG22 behavior change.

### L3 — `od_txq_app_dropped` (queued response dropped, TX)

| | ESP32 (`od_session_app.cpp:131-136`) | Nordic (`od_hal_radio.c:68-72`) |
|---|---|---|
| Level | `warn` | `info` (explicit rationale in-comment: fires once per queued frame on every normal disconnect mid-upload, so `warn`/`error` would spam) |
| Content | `origin`, `tag` (hex), `len`, `reason` | `len`, `tag` (dec), `reason`, plus `live gen` (Nordic's connection-generation counter — a concept ESP32's origin/tag model doesn't have) |
| Where implemented | Alongside the session report, in the session seam file | Inside the radio HAL, separate file entirely |

This is the one case where ESP32's default is **not** obviously right: Nordic's `info` choice has
a stated, specific reason (normal-disconnect frequency), and its extra `live gen` field reports
something ESP32 structurally can't (no generation counter). Do not default here — see § 9 Q2.
Regardless of the level decision, the message itself should converge into one place: `od_txq.h`
already declares this as a seam (`od_txq.h:135`) parallel to `od_rxq_app_report`, so it should
follow the same L1/L2 pattern once § 9 Q2 is answered.

**Same BG22 caveat as L2.** `targets/efr32bg22-slc/od_hal_radio.c:56-60` implements
`od_txq_app_dropped()` with its own direct `printf("[OD] TX dropped len=%u tag=%lu reason=%d\r\n",
...)`, BG22's only diagnostic for this event. Whatever L3 converges into `od_txq.c`, the call to
`od_txq_app_dropped()` must remain unconditional (same reasoning as L2's "Land as, and where") so BG22's
`printf` keeps firing unchanged; only ESP32's and Nordic's implementations of the seam become
trivial once their text moves into `od_txq.c` directly.

### L4 — Transfer (`od_xfer.c`, `od_xfer_direct.c`, `od_xfer_partial.c`, `od_pipe.c`)

Zero logging inside these files today (`od_pipe.c` has one line). This is the largest and highest-
value target: `targets/esp32-idf/src/display_service.cpp` alone carries 151 `od_log_*` calls, a
meaningful fraction of which are a self-contained "transfer progress" mini-subsystem —
`imageWriteLogProgress()`/`imageWriteLogFinish()`/`imageWriteLogQuietCmd()` (lines ~1690-1725) —
tracking chunk count, percent-complete milestones (every 5%), and a completion summary with
throughput (KB/s). Nordic has no equivalent: `opendisplay_display.cpp` + `opendisplay_pipe.c`
together carry 29 calls, none of them a percent/throughput summary.

This subsystem is too large to enumerate line-by-line in this plan. **Triage method for Stage 5**:
for each of the 151 (ESP32) and 29 (Nordic) call sites, ask "does this reference state
`od_xfer*.c`/`od_pipe.c` already track internally (stream offset, total length, mode, refresh
trigger, completion, abort reason)?" — if yes, it's a convergence candidate; if it names a
target-specific driver/timing detail (FastEPD, panel busy pin, SPI), it stays. The
`imageWriteLog*` family above is the first concrete item: it depends only on byte counts and a
timestamp, both of which `od_xfer.c` already has or can trivially track, and its output (percent
milestones + KB/s summary) is exactly the kind of diagnostic Nordic is currently missing entirely
— recommend porting it to `od_xfer.c` essentially as-is, gated by `OD_LOG_LEVEL >= OD_LOG_DEBUG`
same as today.

### L5 — Config (`od_config.c`, `od_config_read.c`, `od_config_asm.c`, `od_config_tlv.c`,
`od_config_store.c`)

Same shape as L4: 0 logging in shared config files, 180 calls in `config_parser.cpp` (ESP32) vs.
18 in `opendisplay_config_parser.c` (Nordic). Apply the same triage method in Stage 6: parse/
validate/store outcomes (a field rejected, a bound violated, a record written/reloaded) belong in
`od_config*.c`; anything naming NVS/filesystem mechanics stays in the target.

### L6 — NFC (`od_nfc.c`)

ESP32 builds `OD_CAP_NFC=0` — **there is no ESP32 default to defer to for this subsystem.**
Nordic's `opendisplay_nfc.c` is the only existing NFC logging in the tree. § 9 Q3 asks how to
handle this: adopt Nordic's wording as-is (restyled to match the ESP32 conventions established
above — full-sentence text, `ERROR:`/`WARNING:` style — for consistency with everything else
converged in this pass), since there is nothing else to choose from.

### L7 — Dispatch/gate/reply/cmd plumbing (`od_dispatch.c`, `od_gate.c`, `od_reply.c`, `od_cmd.c`,
`od_core.c`)

Zero logging on **either** target today at this level — not a convergence-of-existing-text
question, a genuine gap. These are the busiest decision points in the whole stack (opcode
unrecognized, budget/reservation denied, gate refusal reason, frame deferred) and currently
produce no diagnostic trace anywhere. Treat as new logging, written fresh in the ESP32 conventions
established by L1/L2 (full sentences, `ERROR:`/`WARNING:` prefix, level choice matching severity).
Needs its own short design pass (candidate event list, chosen wording) before implementation —
Stage 8.

## 6. Style conventions established by this pass

- **Full-sentence, capitalized text** ("Authentication challenge sent"), not terse tag-prefixed
  lowercase ("auth: challenge sent") — ESP32's style, adopted as the shared default per § 1.
- **Severity mapping**: a dropped/refused/failed event that indicates a bug or resource exhaustion
  is `error`; a rejected-but-expected event (rate limit, malformed input from a possibly-hostile
  peer) is `warn`; a normal lifecycle event (challenge sent, session established) is `info`;
  per-frame/high-volume detail is `debug`.
- **No level word inside the message.** `od_log.c` already stamps the level letter in the record
  prefix (`[0012.345|C0] E: ...`), so an `ERROR:`/`WARNING:` text prefix makes the record read
  `E: ERROR: ...`. New logging never writes one (§ 9 Q4, resolved).

## 7. Implementation stages

0a. **[LANDED `286cc85`] Nordic newline correction (L0a).** Normalize only terminal `CR LF` to `LF` in the Nordic
   HAL, submit through `LOG_PRINTK`, and land the modeled production-adapter regressions. Keep
   this as a distinct commit within the refactor; it does not wait for the NCS 3.4.0 migration.
0. **[LANDED `7b399b3`] Compile-time selector contract (L0).** Add the shared CMake selector and isolated configure-time
   fixtures proving valid INFO/DEBUG output plus rejection of empty, unknown and duplicate
   `OD_LOG_LEVEL` selections. Target integration belongs to the build-profile plan.
1. **[LANDED with `6cb49cb`] RXQ prerequisite (small, mechanical).** Add `shared/core/od_rxq_app.h` with the encryption-enabled
   and quiet-frame predicates described in L1, implement it for ESP32 and Nordic, and add the host
   link/test fixtures before removing `od_rxq_app_report()`. Confirm host test CMake linkage covers any shared file
   gaining `od_log_*` calls — `tests/host/od_log_test_stub.c` already exists as the HAL_LOG test
   double (proven by the touch/pipe/sensor tests passing today); verify it's linked wherever
   `od_rxq.c`/`od_session.c`/etc. host tests build, and verify the `OD_CAP_LOG=0` path (BG22-style
   capability-off) still compiles clean for any file that gains calls.
2. **[LANDED `6cb49cb`] `od_rxq_app_report` → `od_rxq.c`** (L1). Self-contained, four events, both target
   implementations are ~40-70 lines each and disposable once the quiet-frame predicate is
   extracted to its own tiny seam.
3. **[LANDED `54e0663`] Session logging → `od_session.c`; retain BG22 callback** (L2). Self-contained, restores two missing Nordic
   auth cases for free, hoists the duplicated rate-limit throttle into one place.
4. **[LANDED `6776bbd`] `od_txq_app_dropped`** (L3) — Q2 answered as ESP32 wording at Nordic's INFO level.
5. **Transfer** (L4) — own audit pass, file-by-file over `display_service.cpp` and
   `opendisplay_display.cpp`/`opendisplay_pipe.c`, starting with the `imageWriteLog*` family as the
   first landed slice so Stage 5 has an early, demonstrable result rather than one giant patch.
6. **Config** (L5) — same method as Stage 5, applied to `config_parser.cpp` /
   `opendisplay_config_parser.c`.
7. **NFC** (L6) — after § 9 Q3 is answered.
8. **Dispatch/gate/reply/cmd** (L7) — design the candidate event list, confirm wording, implement.
9. **(Independent, can run any time) ESP32 HAL `ESP_LOGx` → `od_log_*`** — the 22 sites named in
   § 3. Not a convergence step (HAL stays target-owned either way), just closes the transport gap
   from the earlier finding in this conversation.

Stages 1-3 are small enough to land and verify together; 5-6 are the bulk of the effort and should
be tracked as their own checklist (either an appendix to this plan or a follow-on
`PLAN_LOGGING_CONVERGENCE_XFER_*`/`_CONFIG_*` once scoped in detail) rather than attempted in one
sitting.

## 8. Verification

- For L0a, run the Nordic production-adapter host test with exact-byte assertions: one terminal
  `CR LF`, no `CR CR LF`, no terminator added to raw unterminated data, and queued data surviving
  source-stack clobbering.
- Hardware-capture exact bytes from both Nordic logging backends: USB CDC ACM on `xiao_nrf52840`
  and RTT on an nRF54 board. Confirm the ESPHome web console renders CDC records on distinct lines.
  This is a logging-transport hardware gate, not a protocol wire-format gate.
- `tools/check.sh` (host suite, gcc + clang, ASan/UBSan) after every stage — pure C, no new vendor
  dependency, should be clean.
- `tools/check.sh --targets` after every stage — this is a `shared/` change, so it relinks every
  target family; a skip is not a pass per `CLAUDE.md`.
- Run L0's isolated CMake fixture for INFO, DEBUG, empty, unknown and pre-populated-definition-list
  cases. Inspection of emitted ESP32/Nordic compile commands belongs to the build-profile plan.
- **No wire-format change anywhere in this plan.** Logging is not on the wire; nothing here should
  touch `docs/HARDWARE_VERIFICATION_CHECKLIST.md`. If any stage's audit turns up a case where a
  message and a wire decision are entangled (e.g. a log line that's currently the only place a
  value gets clamped or validated — watch for this in L4/L5, config/transfer code sometimes hides
  a side effect in a "just logging" line), stop and flag it rather than silently move it.
- Update `shared/core/od_rxq.h`'s "THIS MODULE DOES NOT LOG" comment and `shared/sources.cmake`'s
  APP_RXQ tier comment ("named for a SEAM rather than a HAL... only `od_rxq_app_report()`") once
  L1 lands, so the docs don't keep asserting a premise this plan disproves. Same for
  `od_session_app.h`/`.c`'s "shared/ may not include od_log.h" comments once L2 lands.

## 9. Explicit questions for the user

- ~~**Q1 (L1).** Keep the 32-byte hex dump + ERX/URX token on the RXQ arrival line for both
  targets, or drop it to match Nordic's terser one-liner?~~ **Resolved: ESP32's richer line, for
  both targets** (`6cb49cb`). Nordic gains the hex dump and the encrypted/plaintext token, and
  loses default-build visibility of arrivals — the line is `debug`-gated, which is what
  `OD_LOG_PROFILE=debug` exists for. The block sits behind
  `#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG` rather than relying on `od_log_debug()`'s runtime
  test, so an INFO build spends nothing on the 192-byte record, the hex formatting or the two
  seam calls; the pre-convergence ESP32 code formatted the line and then discarded it.

- ~~**Q2 (L3).** For `od_txq_app_dropped`: ESP32's `warn` wholesale, ESP32's wording at Nordic's
  `info` level, or keep the two structurally different?~~ **Resolved: ESP32's wording at Nordic's
  `info` level** (`6776bbd`). Nordic's rationale held — the line fires once per queued frame on
  every normal disconnect mid-upload, so `warn` would be routine noise for an expected event.

- ~~**Q3 (L6).** NFC has no ESP32 behavior to default to. Restyle Nordic's existing wording into
  the established conventions, or hold NFC out of this pass?~~ **Resolved: promote it, held to the
  same weight any other command gets on ESP32.** NFC is not a privileged subsystem: it gets an
  ordinary command handler's diagnostic budget, not a running commentary on the tag state machine.
  Concretely, Nordic's seven `[OD][NFC]`-tagged lines in `opendisplay_nfc.c` are all `od_log_info`
  today, including five setup-refusal paths and one success line; restyle them into full-sentence
  text with no bracket tag, map severity by § 6 (a failed `payload_set`/`emulation_start`/
  `t2t_setup` is `error`, an absent or disabled config is `info`, an unsupported `nfc_ic_type` is
  `warn`), and keep per-frame assembly detail at `debug` or drop it. The `[OD][NFC]` prefix goes:
  no other subsystem tags its lines, and the record already carries a level letter and timestamp.

- ~~**Q4 (§ 6).** Drop the redundant `ERROR:`/`WARNING:` text prefix repo-wide, even though that's
  a wording change to ESP32's own text, not just Nordic's?~~ **Resolved: dropped, repo-wide.** 52
  sites carried it — 9 in `shared/`, 43 in `targets/esp32-idf/`, none in Nordic or BG22 — and no
  test asserted on the text. One site needed more than a strip: Nordic's
  `factory_config.c` reported a failed factory provision as `od_log_info("ERROR: ...")`, where the
  text was the only thing carrying severity; it is now `od_log_error()` with no prefix, which is
  what the record already had to mean.

- ~~**Q5 (L2).** Confirm no `tools/check.sh` ratchet targets `od_session_app_report` by name before
  it goes unreferenced on BG22.~~ **Resolved:** `grep -n od_session_app_report tools/check.sh`
  returns nothing — no ratchet names it. Moot regardless, since L2 now keeps the call to
  `od_session_app_report()` live for BG22's sake (see L2's "Land as" and BG22 note above), so the
  function never goes unreferenced in the first place.

## 10. Out of scope, deliberately

- **BG22 logging content and wording** — not touched this round, per instruction, and this plan
  now verifies that in both L2 and L3: the existing calls to `od_session_app_report()`
  (`od_gate.c`/`od_reply.c`) and `od_txq_app_dropped()` (`od_txq.c`) stay exactly where they are
  and keep firing unconditionally, so BG22's `printf`-based auth/decrypt/TX-drop diagnostics are
  untouched byte-for-byte. (An earlier draft of this plan claimed these functions would become
  harmless dead code on BG22 — that was wrong; they are BG22's only diagnostic output for these
  events, not unreferenced code, which is why the call is
  kept rather than removed.)
- **HAL implementation internals** (`od_hal_crypto.c`, `od_hal_nvs.c`, `od_hal_adc.c`,
  `od_hal_radio.c`, `od_hal_gpio.c`, `od_hal_wdt.c`) — stay target-owned by architecture (decision
  1 in `CLAUDE.md`: HAL implementations are per-target by construction).
- **BLE/Wi-Fi stack lifecycle, deep-sleep/RTC, watchdog subsystem specifics, panel/display driver
  timing** — no shared subsystem behind any of these; correctly target-local.
- **Any wire-format or protocol-behavior change.**

## 11. Definition of done

- **[PARTIAL]** Nordic CDC ACM and RTT each emit exactly one terminal `CR LF` for a normal complete record;
  `od_log_raw()` remains unterminated unless its caller supplies a terminator, and the ESPHome web
  console no longer overwrites adjacent Nordic records. Held by the modeled host adapter test,
  including a record larger than the adapter's own buffer; the § 8 hardware capture from CDC ACM
  and RTT has not been taken, so nothing here rests on bytes a board emitted.
- **[MET]** `od_rxq.c` and `od_session.c` own their logging directly. The RXQ report seam
  (`od_rxq_app_report()`) is gone from ESP32 and Nordic (or reduced to the minimal quiet-frame
  predicate noted in L1) — it has no BG22 consumer since BG22 doesn't take `APP_RXQ`. **The
  session report seam (`od_session_app_report()`) is deliberately not removed** — it stays
  callback-only, called unconditionally from `od_gate.c`/`od_reply.c` exactly as today, with
  trivial empty ESP32/Nordic implementations, solely so BG22's existing `printf` diagnostics keep
  firing unchanged (see L2's BG22 note). Do not read this bullet as requiring the session seam
  gone — that would either strand BG22 without its auth/decrypt diagnostics or duplicate them.
- **[MET]** `od_txq_app_dropped` converged per the § 9 Q2 answer, with the same BG22-callback-only caveat as
  the session seam above (see L3's BG22 note).
- **[OPEN]** NFC logging exists in `od_nfc.c` per the § 9 Q3 answer.
- **[OPEN]** Transfer and config each have a landed first slice (at minimum: the `imageWriteLog*` family for
  transfer) and a tracked checklist for the remainder — not required to be 100% complete in one
  pass given the size (151 + 180 call sites across the two subsystems).
- **[MET]** `tools/check.sh --targets` passes on every stage landed — all three families run
  green at `7d22fa8`.
- **[MET, one caveat]** The shared CMake selector returns exactly one correct INFO/DEBUG definition and rejects empty,
  unknown or duplicate selections in its isolated configure fixture. Target consumption is not
  part of this plan's definition of done. Caveat: the fixture runs in cmake **script** mode, not
  configure mode; the contract it proves is the one written here, but a `CMakeLists.txt`-scoped
  regression would not be caught by it.
- **[MET]** `shared/core/od_rxq.h`, `shared/sources.cmake`, and `od_session_app.h`/`.c` comments
  updated to match the new ownership.
- **[MET]** No change to any `docs/HARDWARE_VERIFICATION_CHECKLIST.md` row.
