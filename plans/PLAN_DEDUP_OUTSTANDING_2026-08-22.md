# De-duplication — outstanding items

**Status:** proposed, 2026-08-22; revised same day after implementation review. **§ 8 step 1
(D1, D2, D8) implemented on `fix/bg22-gate2-blockers`, 2026-08-22 — software only; its hardware
rows are open in `docs/HARDWARE_VERIFICATION_CHECKLIST.md` § LED runner and panel rail, which
covers all three targets because D1's sentinel change is not BG22-only.** Supersedes
[PLAN_DEDUP_CONSOLIDATED_2026-08-17.md](PLAN_DEDUP_CONSOLIDATED_2026-08-17.md) in full.
Every item below was re-validated against `main` — file, line and symbol references are to
today's tree, not inherited from the 2026-08-17 survey. Items the survey listed that are
**not** here are closed; § 1 records by what, so nobody re-verifies them.

**Relationship to the roadmap:** additive to
[PLAN_MIGRATION_ENDGAME_2026-08-17.md](PLAN_MIGRATION_ENDGAME_2026-08-17.md), whose remaining
work is hardware evidence. **D1, D2 and D8 are explicit blockers on EFR32BG22 hardware Gate 2**
and are the only items here that gate a hardware row: D1 is a remotely-triggerable brick on the
one target with no watchdog, D8 can clock an image into an unsettled panel or drive an
unconfigured pad, and until D2 is fixed the Silabs host suite is not evidence for the target it
claims to cover. Everything else may proceed alongside hardware work.

**Two items are design steps, not implementation steps.** § 4's config-storage promotion and
§ 5's sensor/touch seam each need a written seam design before any code lands; the 2026-08-17
survey's one-line seam sketches for both do not survive contact with the two implementations
they have to span. Those steps are called out in § 8 and must not be skipped as "mechanical".

---

## 1. Closed since 2026-08-17 — do not re-do

| Survey item | Closed by |
|---|---|
| D4 (Nordic boot logo absent) | Boot-screen promotion: `shared/core/od_boot_screen.c:6` includes `boot_logo/logo_bitmap.h` unconditionally; `BOOT_HAS_LOGO` no longer exists anywhere |
| D5 (ESP32 behind upstream swatch fix) | Same promotion; `targets/esp32-idf/src/boot_screen.cpp` is now a 120-line `od_boot_app_*` adapter and the stale branch is gone |
| D7 (Silabs flat byte counts) | `od_color` promotion; `opendisplay_display_color.c` deleted, all four call sites use `od_color_direct_geometry()`, ratcheted at `tools/check.sh:531` |
| uzlib second copy; `third_party/uzlib/` itself | C14 canonical portable inflater + Transfer Phase 1 `od_zlib_pump` |
| `adler32.c`/`crc32.c`, uzlib heap window | C14 |
| `logo_bitmap.h` duplication | One copy at `third_party/boot_logo/` |
| Inflate pump (Tier B) | Transfer Phase 1, exactly as the survey's interleaving note instructed |
| Boot screen ESP32↔Nordic (Tier B) | `PLAN_BOOT_SCREEN_2026-08-17.md`; BG22 declined on a flash measurement (+3,772 B) and takes payload + QR only (`opendisplay-bg22.cmake:262`, APP_BOOT absent per `:275`) — that answers survey Q2 (yes) and Q3 (no) |
| Shared logging HAL (Tier C's largest; Part 3 prerequisite 2) | Shared logging promotion 2026-08-19; `od_log_hex_line` is shared, `od_hal_log.h` is the seam |
| Q6 (QR stack sizing) | Derived sizing restored: `od_boot_screen.c:549-550` uses `qrcode_getBufferSize()` against a caller buffer with a length check |
| Q4 (E1004 split-panel path) | Moot — the board variant is retired; no fragment exists |

---

## 2. Outstanding defects, in priority order

Severities carried from the survey; every claim re-verified against the tree.

### D1 — BLOCKER: BG22 LED hang, remotely triggerable, unrecoverable — and a sentinel bug underneath it

`targets/efr32bg22-slc/opendisplay_led.c:270` wraps the phase machine in `for (;;)`. Zero-delay
stages `break` and re-enter the loop with no minimum step delay — `LED_MIN_STEP_DELAY_MS` does
not appear anywhere in that file, and exists only on Nordic (`opendisplay_led.c:22`) and ESP32
(`device_control.cpp:278`). The termination check at `:277` is skipped whenever the *internal*
`grouprepeats` equals 255. BG22 declines HAL_WDT (`opendisplay-bg22.cmake:274`), so recovery is
physically removing power.

**The sentinel is also wrong, on all three targets, and it changes what triggers the hang.**
The canonical contract is `opendisplay_structs.h:1114` — "group repeat count minus 1 (value 255
= repeat forever)". Every runner computes `(uint8_t)(raw + 1)`: BG22 `:219`, Nordic
`opendisplay_led.c:170`, ESP32 `device_control.cpp:382`. In `uint8_t`:

- raw `0xFE` (a request for 255 repeats) → internal 255 → **treated as forever**;
- raw `0xFF` (the contract's "forever") → wraps to **0** → `group_pos >= 0` on the first pass →
  **stops immediately**.

So the documented forever value is the one that does nothing, and the hang's actual trigger is
raw `0xFE` with all-zero delays. Any fix or promotion that "corrects" the sentinel to `0xFF`
alone silently changes behaviour for deployed configs built against the shipped runners.

**Fix (~6 lines, plus a test).** Port the minimum step delay to BG22 so every phase transition
yields. Accept **both** raw `0xFE` and raw `0xFF` as indefinite, for deployed compatibility, and
record the divergence — the contract wants only `0xFF`, the field has both.

**Implemented 2026-08-22.** The floor and the group-closing yield are ported from the authority;
`repeat_forever` replaces the wrapped-count sentinel on all three targets;
The original production-machine suite carried 146 checks; it is now incorporated into
`tests/host/led_test.c`, with BG22 scheduling covered separately by
`tests/host/silabs_led_adapter_test.c`. The divergence is recorded in
`docs/DIVERGENCE_MATRIX.md` § 11. Removing the group-closing yield makes the machine suite hang
rather than fail, so its ctest entry carries a timeout.

**Acceptance test, mandatory and not optional:** a production-machine host case driving an
all-zero-delay pattern at raw `0xFE` and at raw `0xFF`, asserting the runner yields — performs
at most one **LED emission or other yield-requiring action** per service call and returns —
rather than asserting on a reply byte. Define the bound that way, not per phase transition:
internal zero-work transitions (`GROUP` → `LOOP1` → `LOOP2` with empty loops) legitimately occur
within one call and are not the defect. Without this test the hang can return during the § 4 LED
promotion with nothing to catch it.

### D2 — BLOCKER (evidence integrity): the Silabs host archives compile a different `struct od_config` than the firmware — HALF-FIXED

`tests/host/CMakeLists.txt:49` (`od_shared_silabs`) carries 7 of the 11 `OD_`-prefixed
definitions the firmware sets at `opendisplay-bg22.cmake:374-384`. Still absent:
`OD_CONFIG_WITH_{TOUCH,BUZZER,WIFI,DATA_EXTENDED}=0`, each of which `#if`-gates real members of
`struct od_config` (`shared/core/od_config.h:125-147`). Different `sizeof`, different offsets.
`od_shared_dispatch_fixture_silabs` (`:83`) additionally lacks `OD_CAP_LOG=0`. Both archives set
`OD_BOOT_LOGO_SIZES=2`, which the firmware does not set at all and has no use for since BG22
declines APP_BOOT — so the host list is not a subset of the firmware list in either direction.

**Eight executables are affected, not five.** Five link `od_shared_silabs` directly —
`od_xfer_silabs_test`, `od_pipe_off_test`, `od_config_asm_cap_test`, `od_silabs_storage_test`,
`od_time_silabs_test` (`:148,175,201,216,227`). Two more reach it through
`od_session_fake_silabs` — `od_dispatch_corpus_silabs_test`, `od_silabs_fault_test`
(`:703,724`), the **first** of which is the target-production corpus profile; the second is the
production-source failure-policy suite. `od_dispatch_silabs_test`
(`:409`) reaches the fixture archive. **All eight currently pass under the wrong profile**, which
is the point: passing is not evidence when the ABI differs.

**Implemented 2026-08-22** as `shared/profiles.cmake`, included by `shared/sources.cmake` and
consumed by `opendisplay-bg22.cmake` and both host archives; ratcheted by "silabs: one profile
definer". The four missing gates changed `struct od_config`'s layout under all eight executables
and the suite still passes, 65/65.

**Fix — one definition, two consumers; not a list diff.** A raw `OD_`-definition diff is brittle:
it has to special-case test-only definitions like `OD_BOOT_LOGO_SIZES`, and it re-breaks whenever
a legitimately host-only knob is added. Instead define the BG22 shared profile **once** in a
common CMake fragment (alongside `shared/sources.cmake`, which is already the one place both
build systems read), and have `opendisplay-bg22.cmake` and both host archives consume it, each
adding only its own extras. `tools/check.sh` then asserts the fragment is the sole definer of
those macros in both trees — an absence rule, in the style of the § 5 permanent ratchets, which
fails closed rather than comparing two lists.

### D3 — MAJOR: Nordic plays every melody at the wrong pitch

`targets/nordic-zephyr/src/opendisplay_buzzer.c:75-82` `buzzer_index_to_hz()` is the linear ramp
`MIN + span*(idx-1)/254`. ESP32 ships the 256-entry quarter-tone centi-Hz table
(`buzzer_control.cpp:22-23`, `100·13.75·2^(idx/24)`), and py-opendisplay documents that as *the
firmware scale* — `../py-opendisplay/src/opendisplay/models/buzzer_activate.py:29`, "Inverse of
the firmware scale Freq(idx) = 13.75 * 2**(idx/24) Hz". Host encodes exponential, Nordic decodes
linear. **The table alone is not the whole promotion** — see § 4's buzzer row for the folding and
duration-cap decisions that come with it. If the promotion is deferred, porting the table alone
is a valid standalone fix and leaves those decisions open.

### D6 — QR licence — DROPPED BY PROJECT DIRECTION, 2026-08-22

Not in scope for this plan and not to be re-raised as an outstanding item. The finding stands as
described in the 2026-08-17 survey (`third_party/qrcode/qrcode.c` carries no notice, there is no
`LICENSE` file, and `third_party/NOTICE.md` has no QR entry); the decision is that no licence work
is done here. Recorded rather than deleted so a later reader does not re-survey it as an omission.

### D8 — MAJOR: BG22 panel rail — no settle, and drives pin 0x00 when unset

`opendisplay_display.cpp:115-118`: `pwr_pin == GPIO_PIN_UNUSED (0xFF)` is replaced by
`OD_FALLBACK_DISPLAY_PWR_PIN` = `0x00` (`:46`) and driven push-pull. The authority skips an
unset pin entirely (`targets/esp32-idf/src/display_service.cpp:833`), and Nordic documents
driving a guessed pin as "the unsafe case" (`opendisplay_display.cpp:153`). Neither BG22
`display_power_set(true)` site has a settle: `:647` goes straight into `bbepSetPanelType()`, and
`:716` is the refresh path. Nordic carries `OD_PANEL_RAIL_SETTLE_MS 800` with the rationale at
`:114-121` — 48,000 bytes clocked into a dead controller, only symptom a 60 s timeout.

**Implemented 2026-08-22.** `0xFF` now returns before touching a pad, the settle uses
`sl_sleeptimer_delay_millisecond()` (the mechanism already accepted on this path), and it is
charged on the off→on edge only. The dead `OD_FALLBACK_DISPLAY_PWR_PIN` copy in
`opendisplay_ble.c` went with it.

**Two fixes, and the second needs a mechanism decision.**

1. Treat `0xFF` as "there is no rail to drive" and skip the GPIO entirely, as ESP32 does.
2. Settle **only when a rail was actually enabled** — an unconfigured or already-powered panel
   must not pay 800 ms. **Not via `od_hal_time`**: `shared/hal/od_hal_time.h:22-23` offers only
   `od_hal_delay_us()`, explicitly documented as non-yielding and for short hardware timing, and
   deliberately has no millisecond sleep. Either use BG22's existing
   `sl_sleeptimer_delay_millisecond()` (already used for 50 ms at `opendisplay_display.cpp:582`,
   so the superloop-blocking behaviour is already accepted on this path), or first design a
   yielding-sleep seam — which is the same decision ESP32's target-private `od_hal_sleep.h` is
   parked on ("kept target-private until its contract is reconciled with Nordic's signed
   `k_msleep` wrapper"). Pick one explicitly; do not invent a third.

### D9 — MINOR (wire): ESP32's two inflate backends disagree on accepted window width

C14 made `shared/core/od_zlib_inflate.c:612` the canonical check — reject
`((cmf >> 4) + 8) > OPENDISPLAY_ZLIB_WINDOW_BITS`, i.e. > 9 everywhere (BG22 is the only definer
at `=9`; `od_zlib_inflate.h:19-20` defaults to 9 and no ESP32 fragment overrides it). ESP32 tinfl
profiles size the ring at 4096 (`od_inflate_tinfl.cpp:51-53`) and tinfl's only window check is
against that buffer, so those boards **accept** 10–12-bit streams the rest of the fleet refuses.
Latent because py-opendisplay pins 9 — but the wire contract should not depend on the client's
politeness.

**Implemented 2026-08-22.** `shared/core/od_zlib_header.h` is the rule, all-inline: both engines
call it, and the tinfl adapter observes across pushes so the refusal lands before tinfl can accept
a completed header (the pump polls after every push, so a lone CMF byte does reach tinfl — it
simply cannot finish a header from it), and
`tests/host/zlib_header_test.c` covers every window 8..15 plus the split-push cases. Ratcheted by
"transfer: one zlib header rule", which finds engines by what they do rather than by a fixed list.

**The fix is not two unconditional lines, and the obvious test would not cover it.**

- The check needs adapter **state**: the CMF byte may arrive in an empty or fragmented first
  push, so the adapter has to remember whether the zlib header has been consumed and validate it
  once, across pushes — not inspect `input[0]` on every call.
- `tests/host/zlib_pump_test.c:32-53` defines its own scripted `od_inflate_app_*` backend, so it
  binds neither engine; a case added there is green regardless of what tinfl accepts. Cover it
  one of two ways: compile the ESP32 adapter on the host against a miniz seam, or — preferable —
  **extract the zlib-header validator into `shared/` and have both engines call it**, which makes
  the divergence structurally impossible and gives `zlib_inflate_test.c` something real to pin.

Delete the stale `env:esp32-s3-E1004 pins BITS=15` comment at `od_inflate_tinfl.cpp:42` with this
fix — see § 6.

### D10 — MINOR: Nordic button IRQ flag write-only; 8-button cap against a 32-pin contract

`opendisplay_button.c:30` `s_button_irq_pending` has exactly three references in the target — the
declaration, the ISR set (`:34`), and the clear (`:107`). No reader. **Reading it would not fix
anything on its own**: a single anonymous boolean cannot say *which* button, and a press that
began and ended between polls is already lost by the time anyone looks. So the choice is
explicit — either delete the flag and accept documented polling semantics, or capture per-button
edge events in the ISR and drain them at service time. Do not add a reader and call it fixed.

**Resolved 2026-08-22: delete, by project direction.** The flag, its handler and the orphaned
`od_gpio_configure_interrupt()` are gone, the 8-of-32 cap now logs its refusal, and the window is
recorded in `docs/FOLLOWUPS.md` § 12. The comprehensive fix — ESP32 already implements it — is
`plans/PLAN_NORDIC_BUTTONS_2026-08-22.md`, which needs a hardware gate this step could not carry.

Separately, `MAX_BUTTONS 8u` (`:12`) silently drops pins past 8 (`:69`) against a contract of 4
`BinaryInputs` blocks × 8 pins (`shared/core/od_config.h:72`). Either raise the cap or log the
refusal; silent truncation is the failure mode this repo has a standing rule against.

### D11 — MINOR: BG22 maps a malformed send to `GONE`, which purges the tag's whole queue

`targets/efr32bg22-slc/od_hal_radio.c:15-17` folds `frame == NULL || len == 0u` into the same
`OD_RADIO_GONE` return as a dead tag. Both peers separate them: Nordic returns `OD_RADIO_ERROR`
at `:31-32` and reserves `GONE` for `!od_hal_radio_tag_is_live()` at `:39-40`; ESP32 does the
same at `hal/od_hal_radio.cpp:49-50` and `:75-76`. `shared/core/od_txq.c:191-194` answers `GONE`
by dropping **every** queued frame for that `{origin, tag}` — so one malformed call discards
unrelated queued replies. One-line verdict change; the dead-connection arm keeps `GONE`.

**Implemented 2026-08-22**, with the LAN-origin arm folded into the malformed case for the same
reason both peers do: this target has no LAN transport, so that origin is a routing bug.

---

## 3. Tier A remnants — mechanical, no seam

| Item | Validated | Work |
|---|---|---|
| Dead LED symbols — **done 2026-08-22** | `ledFlashActive`/`ledFlashPosition` **defined in a header** (`targets/esp32-idf/src/main.h:102,104`), written at `device_control.cpp:349,351,564,565`; **zero** non-write references repo-wide. `opendisplay_led_is_active()` defined on Nordic (`opendisplay_led.c:396`) and BG22 (`:482`), **zero** callers | Delete all three |
| FastEPD board-set cross-check — **done 2026-08-22** | `OD_FASTEPD_BOARDS` still hand-maintained at `targets/esp32-idf/main/CMakeLists.txt:122`, the comment still narrating both historical mismatches. The survey's ~8-line assertion ("best value-per-line") was never written | Assert the list against what the board fragments declare; fail configure on mismatch |
| QR differential corpus — **done 2026-08-22** | **Correction to the survey's "no host test":** `od_boot_screen_test` compiles `third_party/qrcode/qrcode.c` (`tests/host/CMakeLists.txt:127-128`) and hashes QR-containing output, so the encoder is exercised today — just not *differentially* | **Done 2026-08-22:** `tests/host/qrcode_test.c` freezes **45** module bitmaps — versions 1..10 crossed with short, at-capacity, one-under-capacity and byte-mode payloads, plus the boot-screen and BG22 payloads — alongside geometry and capacity-refusal checks. The survey's "43 cases" named a corpus this repo does not vendor; this is a characterization baseline of the vendored encoder, which is what a re-vendor needs |

## 4. Tier B — needs a seam

Ordered as the survey ordered them. Each is one promotion, one commit series, hardware-verified
per capable target where behaviour can differ on silicon.

| Item | Validated size | Blocked on | Decisions that must be made first |
|---|---|---|---|
| **LED runner** (designed: `PLAN_LED_RUNNER_2026-08-23.md`) | Nordic 399 + BG22 485 + **≈380 LED-related lines** of ESP32's `device_control.cpp` (`led_*` plus `processLedFlash`/`ledStopForSleep`/`flashLed`, `:278`–`:660`; the file is 905 lines total, and the survey's 1,130 figure counted all of it) | D1 + the yielding-sleep decision | `od_led_service(now_ms)` returning ms-to-next-step; `od_hal_gpio` + the yielding-sleep decision shared with D8. Land D1's fix and its regression test **first**, so the promotion inherits a proven contract rather than re-deriving it |
| **Buzzer** (designed: `PLAN_BUZZER_2026-08-23.md`) | Nordic 401 + ESP32 398 + 103 | **closed:** software candidate on `feat/buzzer-shared`; hardware rows open | Authority resolved to the live `Firmware` donor and host: exponential quarter-tone table, octave-fold indices into 117–234, 30,000 ms cap. All 256 indices and the exact cap are pinned by `buzzer_test.c` before the target copies are removed |
| **Config storage record + CRC-32** (designed: `PLAN_STORAGE_SEAM_2026-08-23.md`) | Three `0xEDB88320` bit-loops — Nordic `opendisplay_config_storage.c:22`, BG22 `:63`, ESP32 `config_parser.cpp:215` — and three `0xDEADBEEF` record headers (ESP32's at `config_parser.h:12`). Nordic 106 lines, BG22 165 | **a storage-seam design step — this row is NOT unblocked** | The survey's "`od_hal_nvs.h` already exists" is wrong as a readiness claim: it exists only as the ESP32-local `targets/esp32-idf/hal/od_hal_nvs.h`, its API is **whole-blob** (`od_hal_nvs_load/save/erase`, `:46-52`), and it carries an ESP32-only `od_hal_nvs_secure_erase()` (`:68`). BG22 reads its header separately from its payload (`opendisplay_config_storage.c:74,107`) and its 2,064-byte record **overlays the assembler buffer** at offset 16, statically asserted at `:25,37-39`. A whole-blob seam would force staging a second full copy on the 32 KB part. Design the seam for partial access, the overlay, and secure erase before writing any of it |
| **SHT40 + BQ27220** (designed: `PLAN_SENSOR_SEAM_2026-08-23.md`) | 888 lines across the four files | the shared I2C HAL and per-target adapter gates in that plan | See § 5 |
| **Touch (GT911)** | Nordic 704 + ESP32 745; both name `GT911_REG_STATUS 0x814E` | **§ 5's seam set**, and Q7 | Do after the sensors prove the seams. Q7 is a real authority question: Nordic clears `0x814E` after read, ESP32 wedges — the second place `Firmware`-as-authority points at the defective behaviour |

## 5. The sensor/touch seam set — shared I2C plus narrow APP seams

Designed in `plans/PLAN_SENSOR_SEAM_2026-08-23.md`. The canonical config already supplies the
logical boundary the low-level implementations lack: `SensorData.bus_id` and
`TouchController.bus_id` reference `DataBus`, and both targets resolve that ID immediately
above their I2C engines.

The design therefore creates `shared/hal/od_hal_i2c.h` now, keyed by `bus_id`, with four
bounded operations:

- `probe`;
- `write` with STOP;
- `read`;
- atomic `write_read` with repeated START.

It does **not** expose a generic STOP flag or recreate Arduino `Wire`. ESP32 keeps its one live
IDF bus and switches configured pin sets beneath each operation; Nordic resolves the same
`DataBus` into its bit-banger. Nordic touch's private second bit-banger folds into that engine
before promotion.

I2C remains necessary rather than sufficient. SHT40 gets a narrow yielding-delay APP function;
BQ27220 gets charger GPIO and MSD-output functions; GT911 later gets reset and IRQ seams. BG22
**implements the I2C HAL for its existing TNB132M NFC transport** — it already has a private
bit-banger whose operations are ordinary writes and one repeated-START read — but takes **no
sensor or touch tier**. That cutover is a separate, explicitly software-only step: no board in
this fleet carries a TNB132M. ADC acquisition, nPM1300 and AXP2101
policy stay target-local, though the latter two may consume the shared bus HAL.

## 6. Documentation and small records

- **The `../CLAUDE.md` window-bits claim is outside this repository** — the workspace root is not
  a git repo, so it cannot be part of the independent commits in § 8. Verified today:
  `../Firmware/platformio.ini` contains **8** occurrences of `OPENDISPLAY_ZLIB_WINDOW_BITS=15`,
  every one commented out, and `:343` says outright "The Seeed reTerminal E1004 has no env of its
  own"; no board fragment here sets it. The 9-bit wire contract is absolute. **Record it as
  `docs/FOLLOWUPS.md` § 11** (external doc correction, with the measured counts) so the finding
  survives in-repo, and fix the workspace file opportunistically.
- **The same stale claim was copied into this repo** at `od_inflate_tinfl.cpp:42` — delete it
  with the D9 fix. That one *is* an in-repo commit.
- **Q8 → `docs/DIVERGENCE_MATRIX.md`, now.** `pins_used == 0` means "all pins" on ESP32
  (`device_control.cpp:743`) and Nordic (`opendisplay_button.c:55`) — both special-case `!= 0` —
  while BG22 tests the bit directly (`opendisplay_ble.c:414`), so `0` means "no pins". The struct
  doc defines neither, and the matrix has **zero** `pins_used` matches. Record first; deciding
  which is normative is host-visible and separate.

## 7. Tier C remainder — host-test duplication

`tests/host/od_check.h` exists as of 2026-08-22 and the three suites authored that day use it, so
the count is back to the **35** files that predate it (the survey counted 25; it had already grown
before anyone acted). `od_session_app_now_ms` is still redefined in 13 files and
`od_txq_app_dropped` in 12 — a session-app default fixture is the other half and is not written.

**The rule, restated because the original was too weak to hold.** "Add the helper before the next
suite" did not survive contact with a session that authored three: each one reached for the
boilerplate that was already in every neighbouring file. So: **a new suite uses `od_check.h`**, and
migrating the existing 35 stays piecemeal and low priority. A suite that needs different reporting
should say why in its header rather than quietly re-declaring the macro.

## 8. Sequencing

1. **BG22 Gate 2 blockers, first and together:** D1 (fix + the yield regression test + the
   `0xFE`/`0xFF` sentinel decision recorded in `DIVERGENCE_MATRIX`), D2 (shared CMake profile
   fragment + absence ratchet), D8 (`0xFF` skip + settle, with the sleep mechanism chosen).
2. **Independent small commits, any order:** D9 (with its validator extraction and the tinfl
   comment), D10 (decide delete-vs-events first), D11, the Q8 matrix entry, `FOLLOWUPS` § 11,
   and § 3's Tier A remnants. (D6 is dropped — see above.)
3. **Design step — the storage seam** (§ 4 config-storage row). **Written 2026-08-23:**
   `plans/PLAN_STORAGE_SEAM_2026-08-23.md`. Partial-offset seam (a whole-blob one is disqualified
   by BG22's assembler overlay), framing and CRC in `shared/`, caching behind the HAL, secure
   erase left ESP32-only, and `MAX_CONFIG_SIZE` collapsed into `OD_CONFIG_MAX_SIZE`.
4. **Design step — the sensor/touch seam set** (§ 5). **Written 2026-08-23:**
   `plans/PLAN_SENSOR_SEAM_2026-08-23.md`, revised after checking the logical bus layer. It
   creates a `bus_id`-keyed shared I2C HAL with explicit STOP-separated and repeated-START
   operations, while keeping delay, GPIO, MSD and IRQ as narrow adjacent seams.
5. **Tier B, each on its own branch with its own hardware gate** — the one-subsystem-per-swap
   rule makes them un-batchable. The order below is by readiness, not by dependency: the three
   designs are independent of each other.
   - **LED runner** — designed, `plans/PLAN_LED_RUNNER_2026-08-23.md`. Creates the `APP_LED` tier.
     **Not** the survey's `HAL_IO` tier: that design creates no shared GPIO HAL, because one seam
     function avoids needing one.
   - **Buzzer** — software candidate, `plans/PLAN_BUZZER_2026-08-23.md`. Creates `APP_BUZZER` and
     leaves BG22 outside it. Its authority entry condition is resolved and pinned in the suite.
   - **Config storage** — designed, `plans/PLAN_STORAGE_SEAM_2026-08-23.md`. Independent of the
     LED work; it schedules nothing.
   - **SHT40 / BQ27220** — designed, `plans/PLAN_SENSOR_SEAM_2026-08-23.md`. Independent of LED
     scheduling: its yielding 12 ms wait is a narrow `APP_SENSOR` call. The I2C HAL and both
     per-target adapter gates land before either shared device driver.
   - **Touch** — last. Needs Q7 decided and the GPIO IRQ seam restored.
6. **§ 7's test helper** whenever the next host suite is authored, at latest.

## 9. Open questions carried forward

1. **Q7 (touch authority exception)** — Nordic's `0x814E` clear vs ESP32's wedge. Needs an
   explicit divergence decision when touch is promoted; the default authority rule points at the
   defective behaviour.
2. **Q8 normative `pins_used == 0`** — record in `DIVERGENCE_MATRIX.md` now (§ 6); *deciding* is
   host-visible and may touch py-opendisplay expectations.
3. **The `group_repeats` sentinel** (D1) — the contract says `0xFF` means forever; the field says
   `0xFE` does. Accepting both is the compatible fix; whether the contract or the fleet is
   normative is a protocol question, and the header is frozen.
4. ~~**Buzzer authority**~~ — **closed 2026-08-23.** The live `Firmware` donor and host agree on
   exponential pitch plus octave folding; the donor's 30,000 ms cap wins over Nordic's 5,000 ms.
   `PLAN_BUZZER_2026-08-23.md` B1–B3 and `DIVERGENCE_MATRIX` § 15 record the ruling.
5. ~~**The yielding-sleep seam**~~ — **closed 2026-08-23.** No consumer is waiting on one: D8
   landed using BG22's own sleeptimer call, SHT40 uses a narrow `APP_SENSOR` delay rather than a
   general sleep HAL (sensor plan T3), and the LED machine returns a deadline instead of sleeping
   (LED plan L1). ESP32's `od_hal_sleep.h` stays target-private. Do not build one speculatively.
6. **BG22 buzzer/touch scope** — BG22 compiles `OD_CONFIG_WITH_{TOUCH,BUZZER}=0`; those
   promotions are two-target promotions and must stay zero-cost on BG22 (decision 9).
