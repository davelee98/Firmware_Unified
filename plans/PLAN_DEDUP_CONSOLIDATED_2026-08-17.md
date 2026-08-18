# Consolidated de-duplication plan

**Status:** proposed, 2026-08-17. Synthesis of six parallel surveys:
[boot screen](DEDUP_1_BOOT_SCREEN_2026-08-17.md) · [qrcode](DEDUP_2_QRCODE_2026-08-17.md) ·
[LED](DEDUP_3_LED_2026-08-17.md) · [sensors](DEDUP_4_SENSORS_2026-08-17.md) ·
[compression](DEDUP_5_COMPRESSION_2026-08-17.md) · [sweep](DEDUP_6_SWEEP_2026-08-17.md).
**Relationship to the roadmap:** additive to
[PLAN_MIGRATION_ENDGAME_2026-08-17.md](PLAN_MIGRATION_ENDGAME_2026-08-17.md). That plan covers the
*protocol* duplication (§ 2.3, five transfer units). This covers everything else — the UI, driver,
asset and build duplication that no plan has ever owned.

---

## The finding that matters most

The survey was commissioned to reduce source volume. It found **eleven live defects**, four of them
user-visible and two of them blocking. That inverts the priority: the duplication is the disease,
but several instances have already produced field bugs that need fixing on their own schedule, not
as a side effect of a promotion that will take weeks.

Every defect below was verified against source by the surveying agent **and independently
re-checked** before landing here. Line references are to the current working tree.

---

## Part 1 — Defects, fix independently of any promotion

| # | Sev | Defect | Evidence |
|---|---|---|---|
| **D1** | **BLOCKER** | **BG22 LED hang, remotely triggerable, unrecoverable.** `led_run_step()` (`:255`) wraps `for(;;)` (`:270`); zero-delay stages `break` and re-loop (`:305,361-363,372`) with no `LED_MIN_STEP_DELAY_MS` — the floor exists only on Nordic (`opendisplay_led.c:22`) and ESP32 (`device_control.cpp:280`). `grouprepeats == 255` skips termination (`:277`). Superloop, and BG22 is the one target with **no watchdog**. Requires physically removing power. | `targets/efr32bg22-slc/opendisplay_led.c`, byte-identical to `../Firmware_Silabs/` |
| **D2** | **BLOCKER** | **Silabs host tests validate a different `struct od_config` than the firmware.** `tests/host/CMakeLists.txt:49-54` sets 5 of the 11 BG22 definitions; the firmware sets 4 more — `OD_CONFIG_WITH_{TOUCH,BUZZER,WIFI,DATA_EXTENDED}=0` (`opendisplay-bg22.cmake:386-389`) — and each `#if`-gates real struct members (`od_config.h:125-147`). Different `sizeof`, different offsets. Five executables including the `target-production` corpus profile. The comment two lines above asserts the opposite. `tools/check.sh` has no gate. | C13, on `codex/silabs-c13` |
| **D3** | MAJOR | **Nordic plays every melody at the wrong pitch.** `buzzer_index_to_hz()` is a linear ramp (`opendisplay_buzzer.c:75-82`); ESP32 uses quarter-tones `100·13.75·2^(idx/24)` (`buzzer_control.cpp:22-28`). `py-opendisplay` documents the exponential scale as *the firmware scale*, idx 120 = 440.00 Hz. Host encodes exponential, Nordic decodes linear. | user-visible |
| **D4** | MAJOR | **Nordic renders no boot logo, silently.** Three `#ifdef BOOT_HAS_LOGO` blocks (`:896,929,1012`); the macro is defined only in ESP32's file (`:15`) and Nordic never includes `logo_bitmap.h`. Dead blocks, a dead 111 KB asset, and header text mis-scaled because it doesn't know a logo is absent. | user-visible |
| **D5** | MAJOR | **ESP32 is behind upstream on a shipped rendering fix.** `} else if (!useBitplanes) {` at `boot_screen.cpp:1016`; upstream `../Firmware` no longer has it. BWR/BWY boards show an empty black swatch. | user-visible |
| **D6** | MAJOR | **MIT notice stripped from all six QR files**, and `third_party/NOTICE.md` has no QR entry. Upstream is ricmoo/QRCode v0.0.1, © 2017 Richard Moore, © 2017 Project Nayuki. MIT requires retention. | legal |
| **D7** | MAJOR | **Silabs computes direct-write byte counts flat; ESP32 (authority) and Nordic row-pad.** 187-byte disagreement with the host on a 122-wide EP213. Also excludes GRAY4 from bitplanes and starts it on the wrong plane. | `opendisplay_display_color.c` |
| **D8** | MAJOR | **BG22 panel rail: no 800 ms settle, and substitutes pin `0x00` when `pwr_pin == 0xFF`.** Nordic's port of the same function documents driving a guessed pin as "the unsafe case". BG22 has never been flashed. | `efr32bg22-slc/opendisplay_display.cpp:105-126` |
| **D9** | MINOR | **ESP32's two inflate backends disagree on window width.** uzlib rejects CMF > 9 bits (`od_zlib_stream.c:641`); tinfl's `OD_TINFL_DICT_SIZE` defaults to 4096 = 12 bits (`od_inflate_tinfl.cpp:51-53`). PSRAM boards accept streams the fleet rejects. Latent only because py-opendisplay pins 9. | wire |
| **D10** | MINOR | Nordic's button IRQ flag is written and never read; no wakeup. Presses under ~1 s missable while advertising. Nordic also caps at 8 buttons against a contract of 32, silently. | |
| **D11** | MINOR | BG22 `od_hal_radio` returns `GONE` for a malformed call where others return `ERROR`. `od_txq.c:191` drops **every** queued frame for that tag on `GONE`. | |

**Also documentation, not code:** `../CLAUDE.md` states only `esp32-s3-E1004` sets
`OPENDISPLAY_ZLIB_WINDOW_BITS=15`. All three occurrences in `../Firmware/platformio.ini` are
commented out (`:80,203,233`) and that env does not exist. The 9-bit wire contract is *absolute* —
stronger than documented. Fix the doc.

**Recommended immediate action:** D1 and D2 before BG22 hardware Gate 2 — D1 because a
remotely-triggerable brick should not reach a bench, D2 because until it is fixed the Silabs host
suite is not evidence for anything, including C13. D1 is a ~6-line change. D3–D6 are small and
independent. D7 and D8 land naturally with their promotions but should not wait for them if BG22 is
about to be flashed.

---

## Part 2 — De-duplication, ranked

### Tier A — mechanical, no seam, no design decision

| Item | Removed | Notes |
|---|---|---|
| **uzlib second copy** | 947 lines | `targets/esp32-idf/lib/uzlib/src/` is byte-identical to `third_party/uzlib/src/`; only `LICENSE`, `README.md`, `library.json` are one-sided. Every sibling repo agrees. **4 path edits.** |
| **`logo_bitmap.h`** | 1,176 lines / 111 KB | Byte-identical on two targets. Blocked only by D4 — decide whether Nordic *should* render a logo first. |
| **QR encoder → `third_party/`** | ~1,060 lines | Pure computation, caller-supplied buffer, no HAL, no allocation, already plain C. Zero flash saved; the wins are one licence surface (D6) and somewhere to put the host test that doesn't exist. Watch the ESP32 `file(GLOB)`, which would fail quietly at link. |
| **Dead code** | ~120 lines | `adler32.c`/`crc32.c` (no caller); the uzlib heap window (a 512-byte `malloc` on a 512 KB part whose NULL path leaves the inflater permanently broken); `ledFlashActive`/`ledFlashPosition` (write-only); `opendisplay_led_is_active()` (no callers). |
| **Build-surface cross-checks** | ~280 lines | `main/CMakeLists.txt:131-136` restates a FastEPD board set five fragments already declare; the file records this having gone wrong twice. ~8 lines of assertion is the best value-per-line in the survey. |

### Tier B — worth doing, needs a seam

| Item | Removed | Seam required |
|---|---|---|
| **LED runner** | ~710 (1,130 → 420) | **No new HAL.** `od_hal_gpio_{config_output,write}` + `od_hal_time`. `od_led_service(now_ms)` returns ms-to-next-step, so BG22 arms its own wake without a second vtable. Fixes D1 by construction and a Nordic 1-step/second stall. New `HAL_IO` tier; buzzer is its second tenant. |
| **Inflate pump** | ~300 | Duplicated five times across three targets. Five link-time `extern "C"` functions (`shared/compress/od_inflate.h`), engine chosen by file-level `#if` — the pattern `od_inflate_tinfl.cpp:11` already uses. Engine goes to `shared/compress/`, not `third_party/`; `third_party/` is invisible to `tests/host/`. **`third_party/uzlib/` then ceases to exist.** |
| **SHT40 + BQ27220** | ~420 | Conversion maths, CRC, clamping and MSD packing **verified identical**. Needs the I2C seam below. The clean one. |
| **Boot screen (ESP32 ↔ Nordic)** | ~1,050 | Same program; ~85% of the 405 differing lines are comments and renames. Caller-supplied *row* buffer that refuses rather than truncates — a full framebuffer is impossible (48 KB vs BG22's 32 KB). Link-time seam header, new `APP_BOOT` tier. **BG22 is a separate decision** — it has a third, independent 210-line implementation with a 30-glyph font, not a subset. |
| **Config storage record + CRC-32** | ~180 | `od_hal_nvs.h` already exists and was written for this. |
| **Buzzer** | ~400 | Fixes D3. Same `HAL_IO` tier as the LED runner. |
| **Touch (GT911)** | ~450 | Ranks last on seam cost, not variance — the protocol defines exactly one controller and the register maps are character-identical. Do it *after* SHT40/BQ27220 so the I2C seam is proven on simpler drivers. |

### Tier C — blocked or not worth it

- **~500 lines blocked on one decision**: a shared logging HAL. `od_watchdog_app`, `od_session_app`, `od_rxq_app`, `od_txq_app_dropped` and `od_log_hex_line` are near-identical per target *only* because `shared/` cannot log. One `od_hal_log` collapses all of them.
- **~625 lines of host-test duplication**: link seam ×9, `CHECK` macro ×25, CMake triplet ×28.
- **Not worth promoting:** battery acquisition (three genuinely different ADCs), nPM1300, the ADC ladder, `wake_button.cpp` (deep-sleep wake arming, 100% ESP-IDF, one target).

---

## Part 3 — Two cross-cutting prerequisites

Neither is large; both gate a whole tier.

1. **There is no shared I2C seam.** `od_hal_i2c.h` and `od_hal_adc.h` are target-local to
   `targets/esp32-idf/hal/`, and that header states explicitly it is *not* heading for
   `shared/hal`. Nordic has none at all. Every sensor item is blocked on creating one. It must be
   **`bus_id`-keyed, not a vtable** (decision 2), and must expose the STOP flag — both BQ27220 and
   the touch dual-framing fix depend on it. `opendisplay_sensor_common.h` on Nordic is already
   close to the right shape.
2. **There is no shared logging HAL**, which is the sole reason five seam files are duplicated
   per target. Deciding this unblocks Tier C's largest item.

---

## Part 4 — Sequencing

**Phase 0 — defects (days).** D1, D2 now. D3–D6 as independent commits. Fix the `../CLAUDE.md`
window claim.

**Phase 1 — Tier A (days).** Mechanical, individually revertable, no hardware gate. Do the uzlib
collapse and the dead-code deletions first: both are provably behaviour-free and neither touches
`shared/`.

**Phase 2 — the two prerequisites (Part 3).** Decisions, then small headers.

**Phase 3 — Tier B**, in the order listed within each group. Each is one promotion, one commit
series, hardware-verified per capable target — the same gating § 2.3 uses.

**Interleaving with the endgame plan.** § 2.3's transfer promotion is the higher-value work and
should not be starved. Phase 0 and Phase 1 are cheap enough to run alongside it. Phase 3 competes
for the same hardware-verification budget and should follow it — with one exception: the **inflate
pump is § 2.3 row 1**. Do not do it twice. That row should absorb this survey's findings (D9, the
heap window, the dead checksums) rather than run separately.

**One prerequisite is a decision, not work.** CLAUDE.md decision 1 names `od_zlib_stream.c` as a
place the plain-C choice must be re-argued. The compression survey answers it: once the heap window
is deleted, the "nested resource lifetimes, several failure exits" shape **does not occur** — one
static state struct, one idempotent error state, nothing to destruct. Plain C. That explicitly does
**not** settle `od_xfer_partial.c`.

---

## Part 5 — What should not be de-duplicated

Chip drivers, board glue, devicetree/overlays, and the three build systems — "three toolchains stay
three" (decision 3). Panel backends behind `od_panel_ops`, the one permitted vtable (decision 2).
`components/mdns` is a vendored ESP-IDF component, not duplication, and accounts for 13,906 of
ESP32's line count.

**Honest accounting.** Roughly 5,000–6,000 source lines are removable. Flash savings are close to
zero for several items — QR compiles once per image either way. **The return is drift elimination,
not binary size.** Eight of the eleven defects above exist *because* a second copy drifted; that is
the argument, and it is sufficient on its own.

---

## Part 6 — Open questions

1. **D1 and D2 before BG22 Gate 2 — confirm.** Both are on `codex/silabs-c13`.
2. **Should Nordic render a boot logo at all** (D4)? The answer decides whether `logo_bitmap.h` is
   shared or deleted, and it is a product decision, not a technical one.
3. **Does BG22 join the shared boot screen**, or keep its independent 210-line renderer? It is not
   a subset — different font, no rotation, zones, logo or swatches.
4. **Is the E1004 split-panel path deliberately absent** from both unified boot-screen copies? It
   shapes the seam contract; getting it wrong means redesigning after the fact.
5. **Shared logging HAL: yes or no?** ~500 lines and five files hang on it.
6. **Where does the QR stack fix land** — restore upstream's derived sizing (464 B, ~17% of BG22's
   main stack, verified output-identical over 43 cases) as part of the move, or before it?
7. **Does the touch authority rule get an exception?** Nordic clears `0x814E`; ESP32 wedges. This is
   the second place `Firmware`-as-authority points at the wrong answer — Nordic's QR-position fix is
   the first.
8. **`pins_used == 0`** means "all pins" on ESP32/Nordic and "no pins" on Silabs. The contract
   documents neither. Which is normative?
