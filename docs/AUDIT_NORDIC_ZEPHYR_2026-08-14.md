# Audit: targets/nordic-zephyr vs targets/esp32-idf — 2026-08-14

Report-only audit of the nordic-zephyr target as it stands in the working tree (including
fixes landed earlier on 2026-08-14, listed in §6). Conducted ahead of promoting more code to
`shared/`; §7 evaluates that premise. `esp32-idf` is the comparison baseline per the
migration constraint: **Firmware is the authority over Firmware_NRF54** — the nordic port
must justify its differences, not the other way around.

Scope: all of `targets/nordic-zephyr/src/` and `panel/`, board overlays, Kconfig fragments,
and the flash/debug tooling; compared against `targets/esp32-idf/src/` and `shared/core/`.
Hardware evidence from today's LM20A and nRF52840 bench sessions is cited where it exists.

---

## 1. Verdict summary

The nordic target is structurally sound: the protocol surface (command dispatch, session
crypto, transfer state machines, config walk) closely tracks the reference, loops are
bounded with two exceptions, and most divergences are deliberate and documented in-line.
The serious findings are:

- one **latent crash** (`bt_enable` failure path, §3.1),
- one **live security divergence** (zero-key rule, §3.2) already documented in
  `shared/core/od_config.h` but not yet fixed on this target,
- one **config-driven unbounded loop** (LED pattern runner, §3.5),
- a handful of behavioural drifts (stale temperature, hand-packed MSD) that are exactly the
  class of bug the shared/ promotion exists to kill.

Today's bench work also showed where the *cost* actually lands: the failures that consumed
bench time (MCUboot image validation panic, `spi23` pad contention, panel BUSY silence)
were all board-glue, not protocol logic. Promotion helps correctness of the wire surface;
it does not shorten board bring-up. §7 discusses this.

---

## 2. Method

- Read: `opendisplay_ble.c`, `opendisplay_pipe.c`, `opendisplay_display.cpp`,
  `opendisplay_config_parser.c` / `_storage.c`, `factory_config.c`, sensors
  (nPM1300/BQ27220/SHT40), battery, LED, buzzer, button, touch, NFC, i2c (bit-bang),
  gpio, pin codecs, board files, `panel/od_bbep_zephyr*`, `main.c`, overlays, prj confs.
- Compared against: `main.cpp`, `communication.cpp`, `encryption.cpp`, `config_parser.cpp`,
  `display_service.cpp`, `device_control.cpp`, `ble_transport_esp32.cpp`, `power_latch.cpp`
  on the esp32 side, and `shared/core/*`.
- Loop/termination analysis of every `while`/`for(;;)` in the nordic protocol and driver
  paths (§4).
- Hardware evidence: LM20A flashed and instrumented over SWD/RTT today; nRF52840 flashed
  by the user (image upload + refresh confirmed working).

---

## 3. Open bugs (ranked)

### 3.1 `bt_enable` failure leaves work items uninitialised → NULL-handler jump
`opendisplay_ble.c` (`opendisplay_ble_init`): on `bt_enable() != 0` the function returns
**before** `k_work_init_delayable(&s_adv_restart_work, …)` (and `s_dfu_work`,
`s_boot_display_work`). `main()` does not check, keeps calling
`opendisplay_ble_process()`, whose 500 ms fallback calls `schedule_adv_restart(0)` →
`k_work_schedule()` on a work item whose handler is NULL. `CONFIG_ASSERT` is off in all
profiles, so the system workqueue jumps to NULL ~500 ms after boot;
`CONFIG_RESET_ON_FATAL_ERROR` is also off, so the device halts silently instead of
logging-and-idling. `flash.sh` already documents a real-world trigger ("west flash …
BUS FAULT in net_buf during bt_enable").

**Fix:** move the three `k_work_init*` calls (and `s_display_work_q` init) ahead of
`bt_enable()`, and/or have `main()` skip the process loop when init failed.
ESP32 equivalent: `ble_begin()` failure leaves the loop running but its queues are
statically initialised — no comparable trap.

### 3.2 Zero-key rule not applied (security divergence, documented but live)
The authority clears `encryption_enabled` when all 16 key bytes are zero
(`Firmware`/esp32 behaviour; see the long rationale in `shared/core/od_config.h`).
Nordic's gates are bare `sec->encryption_enabled != 0` (`opendisplay_ble.c:246`,
`opendisplay_pipe.c:165`) with no all-zero normalisation in
`opendisplay_config_parser.c`. A config with `enabled=1` + zero key leaves this target
demanding authentication against a key any client knows — reads as protected, is not.

**Fix:** either swap in `od_config.c` (the planned fix — it normalises this) or, if the
swap is not imminent, add the normalisation to the nordic parser now. Three lines.

### 3.3 WITHDRAWN — nPM1300 "sampling failure" was an unpowered bench board
Originally filed from LM20A bench logs (`VBAT code=3/4 -> 14/19 mV`,
`sample failed, keeping last reading`, every TTL cycle). **The bench board had no battery
attached.** With no cell present those readings are what the part should report, and
`npm1300_sample()`'s `s_gauge_ok = s_batt_v > 0.5f` plausibility gate rejecting them is the
driver working as designed, not failing. No firmware defect; no action.

Numbering retained so the §-references in PR bodies and elsewhere stay stable.

One question the episode did surface, recorded as a question rather than a finding because
no reference behaviour or wire encoding settles it: with no valid source,
`opendisplay_battery_get_10mv()` falls through to the SAADC and returned ~0x86–0x88
(~1.35 V) — a plausible-looking low battery rather than a distinguishable "no reading".
The MSD battery field is a 9-bit 10 mV count with no reserved unknown value (0 is the only
candidate, and it is what a negative read already yields), so a host cannot tell "flat
cell" from "no cell". Whether that matters is a protocol question, not a nordic one, and
the ESP32 has the same fallback shape.

### 3.4 Chip temperature is read once at boot and never again
`read_chip_temperature_once()` runs once after `bt_enable()`; `update_msd_payload()`
reuses `s_chip_temperature` forever. ESP32 reads fresh per MSD update
(`updatemsdata()` → `readChipTemperature()` → HAL die-temp). The advertised temperature
byte on nordic is a constant from boot. With `CONFIG_TEMP_NRF5_MPSL=y` the read is
`mpsl_temperature_get()` — cheap and safe after `bt_enable()` — so there is no reason
not to refresh it in `update_msd_payload()`. **Not justified; conform to esp32.**

### 3.5 LED pattern runner spins instead of yielding
`opendisplay_led.c` `for(;;)` phase machine: when a configured delay is zero the phase
`break`s and the loop keeps running, so a pattern whose delays are all zero never returns
from `led_run_step()` — the main thread is monopolised (no watchdog on this target, §3.6).
`grouprepeats == 255` then makes it permanent.

**An infinitely repeating pattern is a supported feature and must stay supported.** The
defect is the missing yield, not the unbounded repeat, so a total-time cap would be the
wrong fix — it would terminate a pattern the config asked to run forever.

esp32 has the correct shape (`device_control.cpp:466-536`) and closes every cycle edge:

- after **every** flash it schedules and `return`s, using the configured delay when nonzero
  and `LED_MIN_STEP_DELAY_MS` (1 ms) when zero;
- at the group boundary (`LOOP3` exhausted with `ildelay3 == 0`) it increments `group_pos`,
  sets `LED_PHASE_GROUP`, schedules `LED_MIN_STEP_DELAY_MS` and `return`s — which also
  covers the degenerate all-zero-loop-count case.

No path there completes GROUP→LOOP1→LOOP2→LOOP3→GROUP within one invocation; the `break`s
only move between adjacent phases. Nordic lacks all of those returns.

**Fix: port the esp32 shape** — add `LED_MIN_STEP_DELAY_MS`, return after each flash, and
return at the group boundary. Infinite repeat is preserved exactly; it just yields 1 ms per
step.

Consequence to accept deliberately: a zero-delay pattern then advances at
`opendisplay_led_process()` cadence rather than at CPU speed — ~10 ms while connected, but
only once per `idle_delay_ms()` chunk (up to 1 s) when idle. Patterns *with* delays are
already gated by that same cadence, so this makes zero-delay patterns consistent with them
rather than uniquely fast-and-fatal. If idle LED responsiveness matters, the governing
constant is `idle_delay_ms()`'s 1000 ms chunk in `main.c`, not the LED runner.

### 3.6 No watchdog of any kind
`CONFIG_WATCHDOG`/`CONFIG_TASK_WDT` unset; `shared/core/od_watchdog.c` is compiled into
the esp32 aggregate but **called nowhere** (its HAL, `od_hal_wdt.h`, is unimplemented on
every target). ESP32 at least inherits the IDF task WDT and logs reset reasons
(`main.cpp:58-60`). A wedged nordic main thread (e.g. §3.5) stays wedged until battery
death. **Recommendation:** implement `od_hal_wdt` for Zephyr (nrfx WDT is trivial) and
arm `od_watchdog` — this is also the natural next shared-tier swap after `od_config`
because its policy layer is already written and reviewed.

### 3.7 Minor
- **Uptime-wrap in adv boost:** `s_adv_boost_until_ms` comparisons (`now < until`,
  `until != 0` sentinel) misbehave across the 32-bit ms wrap (49.7 days): a boost
  spanning the wrap ends instantly. Harmless outcome (steady interval), noted for
  completeness. The elapsed-style comparisons used elsewhere (`now - t >= d`) are
  wrap-safe; the boost code should use that form.
- **`opendisplay_ble_copy_msd_bytes` logs on every 0x0044 read** at INFO — chatty but
  bounded; esp32 logs at debug.
- **Tooling** (not firmware, but costs bench time): the trailing
  `flash bank … nrf5` line in both board `support/openocd.cfg` files makes OpenOCD's
  `auto_probe` fail on GDB attach — connection rejected outright unless
  `gdb_memory_map disable` is set (now encoded in `debug-nrf54.sh`); the same stale
  bank config makes `verify_image`'s CRC helper fault into a ~28 s read-back fallback.
  `rtt.sh` still defaults to `BUILD_DIR=build` (an nRF54L15 build), which is how a
  session today ended up scanning unmapped RAM with the wrong pyocd target. The RTT
  buffer (1024 B, `NO_BLOCK_SKIP`) silently drops everything after the first ~1 KB of
  boot log unless a reader is attached before reset.

---

## 4. Spin / termination analysis

Every loop in the protocol and driver paths, with verdicts:

| site | bound | verdict |
|---|---|---|
| `opendisplay_i2c.c` `scl_release()` | `OD_I2C_STRETCH_TIMEOUT_US` = 1000 µs | bounded |
| `opendisplay_display.cpp` `wait_for_refresh()` | explicit `timeout_ms` (60 s), 50 ms steps | bounded; requires assert→release edge, correct |
| `opendisplay_display.cpp` zlib poll loops (×2) | state machine: `OUTPUT_READY` only with progress (`produced == capacity` or forced flush), `NEEDS_INPUT`/`DONE`/`ERROR` exit — verified against `od_zlib_stream.c:626-712` | bounded |
| `opendisplay_config_parser.c` walk | offset strictly advances; unknown-size types abort | bounded |
| `opendisplay_pipe.c` msgq drain | `K_NO_WAIT` | bounded |
| buzzer `for(;;)` | `BUZZER_MAX_TOTAL_MS` hard cap | bounded |
| **LED `for(;;)`** | **none when `grouprepeats==255` and all delays 0** | **unbounded — §3.5** |
| `main.c` `idle_delay_ms` | chunked 1 s | bounded |
| `od_board_common.c` bit-bang (8 bits) | fixed | bounded |

The system-workqueue `recycled()` callback (a false positive in today's bench session —
a halted target re-read four times looks identical to a spin) queues one work item and
returns; it cannot spin. Noted here because the diagnosis trap is real: **GDB `detach`
does not reliably resume this target**, and `CONFIG_DEBUG_THREAD_INFO` is now enabled in
the debug profile precisely so thread-level stuckness can be answered directly.

---

## 5. Divergence catalogue vs esp32-idf

### 5.1 Not justified — conform (or promote)

| divergence | nordic | esp32 | recommendation |
|---|---|---|---|
| MSD encoding | hand-packed bytes at `[0][1][2][13][14][15]`, status bits open-coded, **with a comment citing esp32 line numbers as the spec** (`opendisplay_ble.c:400-440`) | `od_advert_encode()` (shared, hardware-verified 2026-08-13) | swap to `od_advert` — this is the designed next consumer; `od_advert.h` names this exact call site |
| chip temperature freshness | once at boot | per MSD update | read in `update_msd_payload()` (§3.4) |
| zero-key normalisation | absent | present | §3.2 — od_config swap or 3-line gate fix |
| config parser | own 699-line parser + `rescan_security_packet()` workaround | own 949-line parser (also pre-shared) | both die in the `od_config` swap; the rescan workaround must **not** be ported (od_config.h explains why); nordic is the natural first swap target since its parser has the fewest side effects |
| constant-time MAC compare | was `memcmp` (fixed today, §6) | `constantTimeCompare()` | done — but the fact it regressed silently is §7 evidence |
| advertised-identity derivation | was derived 3× (fixed today, §6) | single `getChipIdHex()` | done — same §7 evidence |
| watchdog | none | IDF task WDT + reset-reason log | arm shared `od_watchdog` (§3.6) |
| LED runner yield | `break`s on a zero delay and keeps looping — never returns | returns after every flash and at the group boundary, scheduling `LED_MIN_STEP_DELAY_MS` when the configured delay is 0 | port the esp32 shape; infinite repeat is a feature and stays (§3.5) |

### 5.2 Justified — keep, with the justification on record

| divergence | nordic behaviour | justification |
|---|---|---|
| Advertising interval fixed 1000 ms (boost 20–30 ms ×3 s) | esp32 uses NimBLE defaults; its own comment says "the temporary fast-advertising interval is nRF-only today" | matches deployed `Firmware_NRF` `APP_ADV_INTERVAL`, which field hosts were tuned against; the SoftDevice interval-window quirk is documented at the definition |
| `CMD_DEEP_SLEEP` recognised, no response, no sleep | esp32 does full `esp_deep_sleep_start()` with timer wake | matches the reference **nRF52840** build (device_control.cpp:691-705 cited in-line); clients treat no-response as unsupported. The opcode-renumber note (0x0052→0x0053) is correct against the canonical header |
| `CMD_POWER_OFF` absent | esp32 implements latch release | in-line comment documents a blocker that must be fixed first; the latch hardware concept (`pwr_pin_2/3`) exists in configs the nordic boards don't ship. Acceptable **iff** no deployed nordic board carries latch hardware — worth a config-inventory check before this hardens |
| `CMD_NFC_ENDPOINT` present (esp32: absent) | nRF54 has SoC NFCT; esp32 has no NFC. Commands may differ by hardware; the protocol obligation (parse/store `nfc_config`) is met on both | keep |
| Connection arbitration: `BT_MAX_CONN=1`, no link-owner machinery | esp32 needs `link_owner.cpp`/`session_guard.cpp` because NimBLE accepts multiple centrals | Zephyr host enforces the single link; the whole class of races the esp32 code guards is structurally absent. Keep — but any future `BT_MAX_CONN` bump must import the arbitration design |
| Crypto via PSA (`psa_mac_compute`/ECB) | esp32 uses mbedTLS primitives directly | platform-idiomatic; wire-identical (verified byte-for-byte against `deriveSessionKey` today) |
| Auth device_id = `DEVICEID[0]`, advertised name = `DEVICEID[1]` (nRF52840) | same contract in the authority (`getAuthDeviceIdBytes` vs `getChipIdHex`) | deployed-fleet contract; now documented at both nordic call sites so nobody "unifies" it |
| Command ingress: `k_msgq` + generation counter | esp32 `command_queue.cpp` with writer-tagged frames | same design (stale frames self-discard), simpler because single-conn; keep |
| Display pipeline (bit-bang GPIO SPI via bb_epaper) | esp32 drives panels via HAL/SPI + FastEPD for some families | hardware reality; panel families differ per target (decision 9) |
| No WiFi/LAN transport | esp32 has one (never hardware-verified) | scope, decision 9 |
| RTT/CDC logging via `od_log` shim over Zephyr `LOG_RAW` | esp32 `od_log.cpp` over IDF | equivalent record format by design; keep |

### 5.3 Fixed during this audit's bench session (2026-08-14, working tree)

For context — these were divergences/bugs this audit would otherwise list as open:

1. `pwr_pin == 0xFF` treated as error → display refused entirely; now matches esp32
   `pwrmgm()` ("not present" = permanently-powered rail). Hardware-verified on LM20A.
2. Boot-display retry loop `return` → `continue` (the "bounded retry" now retries).
3. LM20A overlay disabled `spi00` (L15's bus) instead of `spi23` (the actual XIAO SPI on
   P1.04/P1.06 = panel SCK/MOSI). Fixed; devicetree verified. **Panel still does not
   refresh on the bench LM20A** (BUSY never asserts) — with the nRF52840 confirmed
   refreshing on the same code, remaining suspects are board wiring / panel power, not
   firmware.
4. Constant-time compare for the auth proof (was `memcmp`).
5. Boot screen / QR now derive the advertised identity from the single exported helper
   (was an independent derivation missing the nRF52840 FICR-word shift: screen said
   `OD45349B`, radio said `OD3AC317`).
6. `CONFIG_DEBUG_THREAD_INFO=y` in the debug profile + `debug-nrf54.sh` (thread-aware
   GDB; 11 named threads verified on hardware).
7. Flash tooling split per board (`flash-nrf54l15*.sh` / `flash-nrf54lm20*.sh` over one
   OpenOCD engine) with SoC and profile guards; `build.sh` hint updated.

---

## 6. Premise evaluation: "promote more shared code to make targets more similar"

**The premise is sound, and this audit is itself the evidence — with two calibrations.**

Supporting evidence from this session alone:

- The **chip-identity bug** (§5.3.5) existed because one value was derived three times in
  one target; two of the three drifted. That is the intra-target version of exactly the
  drift `shared/` kills, and `od_advert.h`'s header makes the cross-target version of the
  same argument ("a comment that points at another target's line numbers IS a shared
  module — just one the compiler cannot check").
- The **constant-time-compare regression** (§5.3.4) is a silent security downgrade that a
  shared `od_session` would have made impossible: the reference had it right, the port
  dropped it, nothing failed.
- The **zero-key divergence** (§3.2) has been *known and documented* in `od_config.h`
  since the module landed, yet is still live on this target — documentation without the
  swap does not converge behaviour.
- The session-key KDF, handshake, CCM envelope, rate limiting and status codes verified
  **byte-for-byte identical** across targets today — that equivalence is currently
  maintained by hand and by luck, at ~940 lines (esp32) vs ~1476 lines (nordic) of
  parallel code.

Calibration 1 — **promotion does not buy bring-up time.** Every failure that actually
consumed bench hours today was board glue outside any promotable module: MCUboot image
validation (FIH panic), `spi23`/`i2c22`/`uart0` pad contention (three boards, same class,
three hand-written overlays), panel BUSY silence, nPM1300 driver, debug-probe halts
killing the radio. The unified repo's overlay-conflict pattern has now recurred enough
times (xiao_ble `spi2`+`uart0`, L15 `spi00`+`i2c22`, LM20A `spi23`+`i2c22`) that the
*mechanical check* is promotable even if the DTS is not: a boot-time assertion (or CI
devicetree lint) that no enabled node's pinctrl claims a config-assigned pin would have
caught all three before hardware.

Calibration 2 — **promotion boundaries must include behavioural cadence, not just byte
layout.** `od_advert` guarantees the 16 bytes are packed identically, but nordic's stale
temperature (§3.4) is an *acquisition-cadence* divergence the encoder cannot see. The
swap checklist for each module should state what the caller must do and how often
(od_advert's header already does this for sensor acquisition — the nordic swap must
honour it, not just link it).

**Recommended promotion order for this target** (each independently revertable, per the
migration rules):

1. **`od_advert`** — smallest, already hardware-verified on esp32, the nordic call site
   is named in the module header, and it deletes §5.1 row 1. Fix §3.4 in the same swap
   (the encoder takes temperature as an input; pass a fresh read).
2. **`od_config`** — deletes the 699-line parser, the rescan workaround, and closes the
   zero-key hole (§3.2). Nordic first, esp32 second (its parser carries LAN/LED side
   effects that need the documented carve-outs).
3. **`od_watchdog` + Zephyr `od_hal_wdt`** — closes §3.6; policy layer already written.
4. **`od_session`** (auth + CCM) — the largest win (≈1 kLoC per target, security-critical,
   proven drift-prone) and the module for which decision 1 already reserves the option of
   revisiting the plain-C choice. Do it after od_config so the SecurityConfig it consumes
   is already normalised in one place.
5. `od_adv_control` last of this batch — needs `od_hal_adv` on Zephyr and touches the
   part of the target (advertising lifecycle) that is currently field-proven; lowest
   drift risk relative to swap risk.
