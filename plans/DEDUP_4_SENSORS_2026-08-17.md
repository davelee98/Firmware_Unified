# DEDUP 4 — Sensor drivers and input handling

2026-08-17. Analysis only; no source touched. Scope: SHT40, BQ27220, nPM1300, battery, buttons,
buzzer and touch across all three targets, and what — if anything — each is worth promoting to
`shared/`.

Sibling reports: `DEDUP_1_BOOT_SCREEN`, `DEDUP_3_LED`, `DEDUP_5_COMPRESSION`. **This report
depends on DEDUP_3 § 3.5's proposed `HAL_IO` tier** (`shared/hal/od_hal_gpio.h` +
`shared/hal/od_hal_time.h`) and does not re-argue it. LED is DEDUP_3's; it is not repeated here.

---

## 0. Inventory — verified, corrected, extended

The inventory supplied with this task has four errors. Measured values (`wc -l`, working tree):

| Subsystem | esp32-idf | nordic-zephyr | efr32bg22-slc |
|---|---|---|---|
| Touch (GT911) | `src/touch_input.cpp` **744** | `src/opendisplay_touch.c` **704** | **none** |
| SHT40 | `src/sensor_sht40.cpp` **284** | `src/opendisplay_sensor_sht40.c` **234** | **none** |
| BQ27220 | `src/sensor_bq27220.cpp` **185** | `src/opendisplay_sensor_bq27220.c` **184** | **none** |
| nPM1300 | — | `src/opendisplay_sensor_npm1300.c` **271** | — |
| Buttons | `src/device_control.cpp:595-800` (~205 of 907) | `src/opendisplay_button.c` **138** | `opendisplay_ble.c:281-470` (~190 of 2165) |
| Buzzer | `src/buzzer_control.cpp` **397** + `buzzer_hw.cpp` **103** | `src/opendisplay_buzzer.c` **401** | **none** (NACK stub) |
| Battery | `src/display_service.cpp:1564-1606` (**43**) | `src/opendisplay_battery.c` **234** | `opendisplay_ble.c:1698-1753` (~40) |

### Corrections

1. **`src/wake_button.cpp` (251 lines, not 262) is not button input handling.** It is deep-sleep
   wake-source arming — `esp_sleep_enable_ext0/ext1_wakeup`, `rtc_gpio_*` pull retention, per-SoC
   `SOC_PM_SUPPORT_EXT1_WAKEUP` branches (`targets/esp32-idf/src/wake_button.cpp:68-220`). It has
   no counterpart on any other target and is 100 % ESP-IDF deep-sleep API. ESP32's actual button
   input lives in `device_control.cpp`. **Not a de-duplication candidate; exclude it.**
2. **`shared/hal/od_hal_i2c.h` and `shared/hal/od_hal_adc.h` do not exist.** `shared/hal/`
   contains exactly four headers: `od_hal_adv.h`, `od_hal_crypto.h`, `od_hal_radio.h`,
   `od_hal_wdt.h`. The I2C and ADC HALs are **target-local** to
   `targets/esp32-idf/hal/od_hal_i2c.h` and `od_hal_adc.h`, and `od_hal_i2c.h:20-23` says so
   outright: *"THE CORE DOES NOT CALL I2C. Sensors and the PMIC are target drivers, which is why
   this lives in `targets/` and is not on the list of things heading for `shared/hal`."* Nordic
   has no `od_hal_i2c` at all — `find targets/nordic-zephyr -name 'od_hal_*'` yields only
   `od_hal_wdt.c`, `od_hal_radio.c`, `od_hal_crypto.c`. **This premise has to be established
   before anything here is promotable, and § 2 is about exactly that.**
3. **Silabs is not "check" — it is a definite answer, and mostly "absent".** No touch, no SHT40,
   no BQ27220, no nPM1300, no buzzer, and **no I2C sensor bus of any kind**. Its only I2C is a
   private NFC bit-bang (`opendisplay_ble.c:527-620`). It *does* have buttons and battery, both
   inline in `opendisplay_ble.c`, and it compiles the buzzer config packet out entirely
   (`cmake_gcc/opendisplay-bg22.cmake:388`, `OD_CONFIG_WITH_BUZZER=0`).
4. **ESP32 battery is confirmed in `display_service.cpp`**, `readBatteryVoltageUncached()` at
   `:1564-1592` and the TTL cache `readBatteryVoltage()` at `:1594-1606`.

### `opendisplay_sensor_common.h` — yes, it is already a partial abstraction

`targets/nordic-zephyr/src/opendisplay_sensor_common.h:15-39` is 25 lines: one `static inline`
`od_sensor_bus_for(bus_id, &bus)` that resolves a `data_bus` (0x24) instance out of
`struct od_config` into a bit-banged bus, applying the `0xFF → 0` default, the `bus_type == 0x01`
check, the `pin_1 = SCL / pin_2 = SDA` convention and the `bus_speed_hz ?: 100000` default. **It is
the exact shape the shared I2C seam needs** (§ 2.2) and its ESP32 counterpart is
`initOrRestoreWireForBus()` (`display_service.cpp:828-851`), which does the same job with the same
defaults. Two implementations of one function, already named.

### Test coverage

**Zero.** No host test touches any of these seven subsystems — `tests/host/` has no sensor, touch,
button, buzzer or battery test, and `docs/TEST_OWNERSHIP.md` assigns none. Nothing in
`plans/PLAN_MIGRATION_ENDGAME_2026-08-17.md` mentions them either. Every behaviour below is
unverified by anything but hardware.

---

## 1. Per-subsystem comparison

### 1.1 SHT40 — the same driver twice, with no maths divergence

**Register sequence: identical.** `0xFD` measure-high-precision as its own transaction, STOP, 12 ms
conversion wait, then a bare 6-byte read — ESP32 `sensor_sht40.cpp:75-95`, Nordic
`opendisplay_sensor_sht40.c:57-66`. Soft reset `0x94` at init, ESP32 `:233-237`, Nordic `:179-185`,
both with the same 0x44/0x45 fallback pair and the same 2 ms settle.

**CRC-8: identical.** Poly `0x31`, init `0xFF`, MSB-first — ESP32 `:18-31`, Nordic `:15-30`.
Character-for-character the same loop.

**Conversion: identical, and this is the thing that matters.**

| | ESP32 | Nordic |
|---|---|---|
| `tc = -45 + 175·raw/65535` | `:110` | `:72` |
| `rh = -6 + 125·raw/65535` | `:111` | `:73` |
| RH clamp 0..100 | `:112-117` | `:75-80` |
| centi conversion | `:118-119` | `:81-82` |
| `round_centi_to_deci` (symmetric ±5) | `:193-198` | `:115-121` |
| temp clamp −400..1250 deci | `:204-209` | `:129-134` |
| MSD pack `v = rh_deci \| (tu << 10)`, 3 bytes LE | `:210-218` | `:135-145` |
| invalid = `FF FF FF` | `:184-191` | `:151-159` |

**No divergence in any of it.** Same defaults too: address `0 | 0xFF → 0x44` (`:33-39` / `:32-40`),
MSD start `0 | 0xFF → 7` (`:41-47` / `:42-50`), `start > 8` skip (`:265` / `:216`), 30 s TTL
(`:248` / `:13`).

**Where they differ — error handling and retry, not maths:**

| | ESP32 | Nordic |
|---|---|---|
| Retry | **two passes**, second after `invalidateOpenDisplayWire()` + re-init + 2 ms (`:133-138`) | **one pass** (`:96-111`) |
| Address sweep | preferred, 0x44, 0x45, de-duplicated (`:132`, `:139-150`) | same three, same de-dup (`:94`, `:96-107`) |
| Error codes | 7 distinct values into `err_out` — `0xFB` bus, `0xFC`/`0xFD` CRC, `0xFE` read, Arduino 0/2/4 for write (`:65-71`, `:92`, `:98`, `:104`, `:127`) | **none** — bare `bool` |
| Boot probe | `sht40_probe_bus_once()` sweeps 0x44/45/51/55/6A (`:163-182`) | absent |
| Fail log | pins + bus id (`:274-275`) | bus id only (`:224-225`) |

ESP32's error byte is explicitly a wire-visible value (`:61-64`: *"a value a host may already be
matching on"*), so the Nordic side is losing host-facing diagnostics.

**Portable fraction: ~150 of 284 / ~145 of 234.** Everything above the bus calls.

### 1.2 BQ27220 — identical maths, equivalent framing by different means

Registers `0x08` voltage (2 B LE) and `0x2C` SOC (1 B) on both (`sensor_bq27220.cpp:22-23`,
`opendisplay_sensor_bq27220.c:11-12`). Address default `0 | 0xFF → 0x55` (`:33-39` / `:39-47`).

**Transaction shape — same wire, different API.** ESP32 uses the single-transaction primitive
`od_hal_i2c_write_read()` (`:62`); Nordic composes `od_i2c_write(..., stop=false)` then
`od_i2c_read()` (`:60-63`). Both produce selector-write + repeated START + read with no intervening
STOP, which is what the part requires — `od_hal_i2c.h:72-77` explains why getting this wrong
*"produces plausible garbage rather than an error"*. **Equivalent, and the divergence is only in
which primitive expresses it.** This is the single most important constraint on the shared I2C
seam (§ 2.2): it must expose the STOP flag, not just a register-read convenience.

Conversion, clamping and MSD packing are identical: `mv = raw[0] | raw[1]<<8` (`:157` / `:154`),
`v = mv/1000` (`:158` / `:156`), `gauge_ok = mv > 0` (`:159` / `:157`), SOC clamp to 100 (`:163-165`
/ `:162-164`), MSD byte `soc & 0x7F | (charging ? 0x80 : 0)` else `0xFF` (`:175-183` / `:172-182`),
30 s TTL (`:136` / `:14`).

Charger GPIO is identical including the polarity inversion: `active_low ? (level==1) : (level==0)`
(`:107-115` / `:66-83`), and both configure `charge_enable_pin` as output `!active_low` and
`charge_state_pin` as input-pullup (`:92-105` / `:102-113`).

Only real difference: ESP32 caches SOC in a file static `s_soc` (`:28`, `:166`), Nordic uses a
local (`:159`). No wire effect.

**Portable fraction: ~90 of 185 / ~90 of 184.**

### 1.3 nPM1300 — one target, nothing to compare

`opendisplay_sensor_npm1300.c` (271 lines) exists only on Nordic. Base/offset register protocol
(`:62-98`), VBAT `code·5000/1024` mV (`:147`), linear `soc_from_voltage()` 3.30–4.20 V (`:100-109`),
charge status `0x0C/0x0D/0x0F` (`:151-152`), plus a hibernate path (`:257-271`). Its sensor type
`OD_SENSOR_TYPE_NPM1300 = 6` **is not in the canonical contract** — `shared/protocol/opendisplay_structs.h:766`
stops at `BQ27220 = 5`, and `targets/nordic-zephyr/src/protocol_pending.h:48,66` carries the value
behind a `_Static_assert` tripwire.

Note it writes the **same MSD byte format** as BQ27220 (`:203-207` vs `:176-181`) into the same slot
— so the packed-SOC encoder is written three times across two files. That is the only shareable
fragment, and it is 6 lines.

**Verdict: n = 1. Nothing to de-duplicate. Do not promote.**

### 1.4 Battery — three genuinely different acquisitions, one shared shape

| | esp32-idf | nordic-zephyr | efr32bg22-slc |
|---|---|---|---|
| Source priority | BQ27220 → ADC (`display_service.cpp:1565-1571`) | nPM1300 → BQ27220 → SAADC (`opendisplay_battery.c:191-206`) | none — AVDD only |
| Sense | `od_hal_adc_read(pin)`, 12-bit raw, ATTEN_12DB default (`od_hal_adc.c:19,107-108`) | SAADC via `/zephyr,user` io-channels, → mV, → **renormalised to a 10-bit/3.6 V count** (`:180-182`) | IADC on `iadcPosInputAvdd`, int 1.21 V ref (`opendisplay_ble.c:1712-1737`) |
| Samples | 10, 2 ms apart (`:1580-1584`) | 10, 2 ms apart (`:163-170`) | **1** (`:1728-1732`) |
| Enable pin | drive high, 10 ms, then low (`:1576-1579`, `:1587-1589`) | same, **plus `od_gpio_park()`** (`:157-159`, `:172-175`) | n/a |
| Scaling | `raw · factor / 100000` (`:1591`) | `raw10 · factor / 100000` (`:184`) | `sample·4·1200/4095`, **no config factor** (`:1733`) |
| TTL | 30 s (`:1594`) | 30 s (`:37`) | 30 s (`:146`, `:1704`) |

The two ADC paths deliberately do **not** agree on raw units: Nordic converts SAADC millivolts back
into the nRF52840-Arduino 10-bit/3.6 V count space precisely so that unchanged
`voltage_scaling_factor` configs produce the same volts (`opendisplay_battery.c:26-34`), while ESP32
feeds a 12-bit ATTEN_12DB count to the same formula. **`voltage_scaling_factor` is therefore
calibrated against a target's ADC width and reference — it is a per-board constant, not a portable
one** (§ 5).

Shared shape is only: source priority, the 30 s TTL, and `od_advert_battery_10mv_from_mv()` (already
shared — `display_service.cpp:1629`, `opendisplay_battery.c:233`, `opendisplay_ble.c:1746`). ~30
lines.

**⚠️ Divergence — ESP32 caches a failed reading for 30 s.**
`display_service.cpp:1604` sets `haveReading = true` unconditionally, so a `-1.0f` failure is
returned from cache for the next 30 s. Nordic caches only successes —
`opendisplay_battery.c:222`, `have_reading = (cached >= 0.0f)`, with the comment *"Only TTL-cache
successful readings; retry soon after a miss."* Silabs likewise retries, because its gate includes
`s_batt_voltage_mv_cache == 0u` (`opendisplay_ble.c:1703`). **ESP32 is the odd one out and it is
the authority**, so this is a divergence that needs settling rather than silently keeping.

### 1.5 Buttons — three implementations, three execution models, four divergences

The encoding is the same everywhere:
`(button_id & 0x07) | ((press_count & 0x0F) << 3) | (state << 7)` —
ESP32 `device_control.cpp:614-616`, Nordic `opendisplay_button.c:130-131`,
Silabs `opendisplay_ble.c:357-359`. So is `press_count = (press_count + 1) & 0x0F` on press only
(`:701` / `:127` / `:355`) and `button_id = instance_number*8 + pin_idx`, folded to 3 bits
(`:719-720` / `:78-81` / `:432`).

**Execution model — all three different:**

| | Detection | Publish | Latency |
|---|---|---|---|
| esp32-idf | ISR does the state machine, sets `buttonEventPending` (`:694-711`) | `processButtonEvents()` from `main.cpp:1009,1024` | loop period |
| nordic-zephyr | **polled** in `opendisplay_button_process()` (`:109-137`) | same function | **≤10 ms connected, ≤1000 ms disconnected** |
| efr32bg22-slc | ISR does the state machine *and* writes the MSD byte, then `app_proceed()` (`:337-363`) | in the ISR | interrupt |

**⚠️ Nordic's button interrupt is inert.** `s_button_irq_pending` is declared at
`opendisplay_button.c:30`, set by the ISR at `:34`, cleared at `:107` — and **never read**. There is
no `k_wakeup`, `k_sem_give` or `k_poll_signal` anywhere in the file or in `main.c`, so the ISR
cannot shorten the idle sleep either. Button detection on Nordic is purely the poll, and
`main.c:15-32`'s `idle_delay_ms()` chunks at 1000 ms, so **a press-and-release shorter than ~1 s
can be missed entirely while the device is advertising** — which is the primary use case, since the
host reads button state out of the MSD. The comment at `:27-29` describes a design the code does
not implement.

**⚠️ `pins_used == 0` means opposite things.** ESP32 `device_control.cpp:108` and Nordic
`opendisplay_button.c:65` both use `if (pins_used != 0 && (pins_used & bit) == 0) continue;` — zero
means *all pins active*. Silabs `opendisplay_ble.c:418-421` uses a strict
`if (!pin_used) continue;` — zero means *no pins active*. The wire contract
(`shared/protocol/opendisplay_structs.h:856`) says only *"which pins are active (bit N-1 =
input_pin_N)"* and documents no zero escape hatch, so Silabs matches the letter and the other two
match the authority. **Pre-existing upstream, not a port artifact** — `../Firmware/src/device_control.cpp:770`
and `../Firmware_Silabs/opendisplay_ble.c:407` diverge the same way.

**⚠️ Nordic caps at 8 buttons; the contract allows 32.** `opendisplay_button.c:12`
`#define MAX_BUTTONS 8u` against `targets/esp32-idf/src/structs.h:139` `#define MAX_BUTTONS 32
// Up to 4 instances * 8 pins = 32` and Silabs' `s_buttons[32]`. Nordic's overflow arm is a bare
`continue` with no log (`opendisplay_button.c:69-71`), so a 4-instance config **silently loses 24
buttons** on a 256 KB part with no memory reason for the cap.

**⚠️ GT911 INT reservation is target-conditional.** ESP32 (`:713-716`) and Nordic (`:72-75`) skip a
pin claimed as a touch interrupt; Silabs does not — correctly, since it has no touch.

ESP32 additionally owns two features neither other target has: long-press power-off
(`:46-81`) and the ADC resistor-ladder input type (`:83-231`), both `n = 1`.

**Portable fraction: ~60 lines of encode + state machine, × 3.**

### 1.6 Buzzer — the most serious correctness divergence in this report

Silabs has none: `od_cmd_silabs.c:339-345` NACKs `0x0077` unconditionally (and does so with
`OD_ERR_PARTIAL_UNSUPPORTED` = 0x07, a *display* error code reused as "not implemented").

ESP32 and Nordic parse the same payload identically (`buzzer_control.cpp:318-338`,
`opendisplay_buzzer.c:336-352`) and share `kBuzzerDurationUnitMs = 5` and the 20 ms inter-pattern
gap. Then they diverge on the one thing that determines what a user hears.

**⚠️ Nordic plays every melody at the wrong pitch.**

- ESP32: a 256-entry quarter-tone table in centi-Hz, `round(100 · 13.75 · 2^(idx/24))`
  (`buzzer_control.cpp:29-62`), plus octave-folding of out-of-range indices into the playable
  window [117, 234] (`buzzer_fold_index()`, `:84-99`).
- Nordic: `400 + 11600·(idx-1)/254` — a **linear ramp**, no musical scale, no folding
  (`opendisplay_buzzer.c:75-83`).

| index | ESP32 | Nordic |
|---|---|---|
| 120 (`nA4`, concert A) | **440.00 Hz** | **5834 Hz** |
| 212 (`nG8`) | **6271.93 Hz** | **10036 Hz** |

**The host settles which is correct.**
`../py-opendisplay/src/opendisplay/models/buzzer_activate.py:10,29,33` encodes note indices as
`idx = round(24 · log2(hz / 13.75))`, documents `idx 120 -> 440.00 Hz`, and at `:36-40` even
describes the `[117, 234]` octave-folding window that only ESP32 implements. **ESP32 matches the
host; Nordic does not.** This is a live, host-visible wire bug, not a style divergence.

It is upstream, not a port artifact: `../Firmware_NRF54/src/opendisplay_buzzer.c:81` carries the
identical linear formula. This is the CLAUDE.md pattern exactly — *"the NRF54 port re-derived
several of them"*, and `Firmware` is the authority.

Secondary divergences, all Nordic-side: global cap 5 s vs 30 s (`:37` vs `:19`); zero-duration
steps sound for one loop period instead of being skipped (`:214-224` vs `:187-189`); no octave
folding; **no `buzzerStopForSleep()` equivalent and no power-off chirp** — ESP32 has both
(`:240-248`, `:371-397`, called from `main.cpp:1156` and `device_control.cpp:73`), so a Nordic tone
can be left driving a pin through teardown, which `main.cpp:1148-1152` calls out as a bug class.

**The policy/hardware seam already exists on ESP32 and is clean.**
`targets/esp32-idf/src/buzzer_hw.h:19,22` is a two-function interface —
`buzzer_hw_tone_start(pin, centihz, duty)` / `buzzer_hw_tone_stop(pin)` — with five call sites, and
`buzzer_control.cpp` contains **zero vendor headers**. Nordic has the same two-call shape implicitly
at `:222` / `:390` but the LEDC-equivalent (a `k_timer` bit-bang at up to 24 kHz ISR rate,
`:98-145`, `:257-272`) is interleaved into the same file.

**Portable fraction: ~340 of 397 (ESP32, already isolated) / ~210 of 401 (Nordic).**

### 1.7 Touch — one controller, not many; the fear is unfounded

**The controller sets are identical, and there is exactly one.**
`shared/protocol/opendisplay_structs.h:930-931` defines only `OD_TOUCH_IC_NONE = 0` and
`OD_TOUCH_IC_GT911 = 1`. No CST816, FT6236, FT5x06 or CST328 exists anywhere in the tree — not in
the enum, not in either driver, not in the parser. Both targets reject anything else with a
near-identical log line (`touch_input.cpp:507-512`, `opendisplay_touch.c:510-514`) and gate
`touch_ic_type != OD_TOUCH_IC_GT911` at every entry point. A second controller would require a wire
change first (`opendisplay_structs.h:954`, `reserved[20]` is reserved for a *"GT911 register
profile"*).

**So "too controller-specific to promote" is not what the evidence shows.** The register map is
character-identical (`touch_input.cpp:22-32` vs `opendisplay_touch.c:28-40`): PID `0x8140`, STATUS
`0x814E`, POINT1 `0x814F`, ready bit `0x80`, 5 contacts, 3 retries, 300 ms pre-reset / 200 ms
settle. The hardware reset sequence matches step for step and delay for delay
(`:236-271` vs `:334-358`), as does address resolution (INT low at RST rising ⇒ 0x5D, high ⇒ 0x14),
the LE-then-BE product-ID probe, and the 8-byte point decode
(`tid = p[0]`, `x = p[1]|p[2]<<8`, `y = p[3]|p[4]<<8`).

**Coordinate transformation agrees exactly** — `apply_touch_map()` at `:453-477` vs `:460-487`, same
five steps in the same order (SWAP_XY, fetch w/h, INVERT_X, INVERT_Y, clamp), and both apply it only
when `n > 0` so a release reuses already-mapped coordinates rather than double-inverting. **No
gestures, taps, long-press, swipes or debounce exist on either target**; the only edge detection is
a change comparison, and the MSD 5-byte encoding is byte-identical.

**Two genuine correctness divergences:**

**⚠️ 1.7a — read framing.** ESP32 tries **both** bus forms per retry — repeated-START, then
STOP-then-START, three rounds (`touch_input.cpp:196-208`), with `:69-72` explaining *"GT911 clones
differ: some accept only the repeated-START read, others only STOP-then-START."* Nordic hardcodes
the repeated START (`opendisplay_touch.c:224`) and retries the same framing three times
(`:250-260`). **A clone that needs STOP-then-START is undetectable on Nordic** — the most likely
cause of a "same panel, works on ESP32, no touch on Nordic" report.

**⚠️ 1.7b — stale status latches ESP32.** When the contact count is out of range, Nordic clears
`0x814E` and continues (`:631-635`); ESP32 `continue`s **without clearing** (`:668-674`). The GT911
will not refresh its coordinate buffer until the status byte is cleared, so one corrupt read wedges
ESP32 touch until an EPD refresh triggers `touchResumeAfterEpdRefresh()`. **Here Nordic is right and
the authority is wrong** — `../Firmware/src/touch_input.cpp` carries the same omission.

Lesser: Nordic ignores `DataBus.bus_speed_hz` entirely (hardcoded `I2C_HALF_BIT_US 5`, `:44`);
Nordic rejects a missing `data_bus` where ESP32 falls back to an implicit bus (`:299-301` vs
`:282-296`); Nordic has no interrupt path, no transfer-in-flight gate, and no EPD-refresh suspend
(only a resume); a Nordic controller that fails at boot can never recover, because
`s_any_initialized` (`:69`, `:557`, `:583`) gates both the poll and the resume.

**Vendor coupling is thinner than expected.** Nordic's only framework header is
`<zephyr/kernel.h>` (`k_busy_wait`, `k_msleep`, `k_uptime_get_32`); ESP32's only vendor header is
`"esp_attr.h"` for `IRAM_ATTR` on four ISRs. Everything else is target-local module coupling
(`od_log.h`, `structs.h`, `opendisplay_ble.h`), not SDK coupling.

Nordic additionally carries **105 lines of software I2C master** (`:71-175`) that exist only because
it has no `od_hal_i2c` — that code is the Nordic implementation of the missing HAL, sitting in the
wrong file.

**Duplication: ~450 lines of the same GT911 chip driver written twice, plus ~475 of the same
acquisition policy.** ~60 % of both files.

---

## 2. The chip-driver / acquisition-policy split, and the HAL that does not exist

### 2.1 The split, made explicit

Every one of these seven has the same two layers, and in six of seven the boundary is already
visible in the source:

| Layer | What it is | Portable? |
|---|---|---|
| **Chip driver** | register addresses, transaction framing, CRC, reset timing, raw→engineering-unit conversion | **Yes**, given a bus HAL. Contains no clock policy and no config lookup. |
| **Acquisition policy** | when to sample, the TTL, retry/backoff, what to do on failure, clamping to the wire field, which MSD byte to write | **Yes**, given a clock and a publish callback. Contains no register knowledge. |
| **Target glue** | bus lifecycle, pin decoding, ISR attributes, logging, config access | **No.** |

The cleanest existing example is the buzzer: `buzzer_control.cpp` (policy, zero vendor headers)
against `buzzer_hw.cpp` (LEDC, ~86 % vendor). The muddiest is Nordic touch, where a bit-banged I2C
master, the GT911 register driver and the poll policy share one 704-line file.

### 2.2 What `shared/hal` actually needs

`shared/hal/` today has four headers, none of them I2C, GPIO, time or ADC. DEDUP_3 § 3.2 already
proposes promoting minimal subsets of **`od_hal_gpio`** and **`od_hal_time`** into `shared/hal/`
(the `HAL_IO` tier, § 3.5). Sensors need those *plus* one more:

```c
/* shared/hal/od_hal_i2c.h -- keyed by bus_id, resolved by the target. */
#define OD_HAL_I2C_OK       0
#define OD_HAL_I2C_ERR    (-1)
#define OD_HAL_I2C_ENODEV (-2)
#define OD_HAL_I2C_EINVAL (-3)

int od_hal_i2c_probe(uint8_t bus_id, uint8_t addr7);
int od_hal_i2c_write(uint8_t bus_id, uint8_t addr7, const uint8_t *tx, uint16_t len, bool stop);
int od_hal_i2c_read (uint8_t bus_id, uint8_t addr7, uint8_t *rx, uint16_t len);
int od_hal_i2c_write_read(uint8_t bus_id, uint8_t addr7,
                          const uint8_t *tx, uint16_t tx_len, uint8_t *rx, uint16_t rx_len);
```

Four properties, each forced by evidence above:

1. **Keyed by `bus_id`, not by a handle and emphatically not by a vtable.** Decision 2 reserves the
   only vtable for `od_panel_ops`. Both targets already have the resolver this needs —
   `initOrRestoreWireForBus(bus_id)` (`display_service.cpp:828`) and `od_sensor_bus_for(bus_id, &bus)`
   (`opendisplay_sensor_common.h:15`) — so each implements `od_hal_i2c_*` by calling its own, and
   `struct od_config` lookup stays on the target side where the pin encoding lives.
2. **`write()` must expose `stop`.** BQ27220 needs `stop=false` (§ 1.2) and touch needs *both*
   framings (§ 1.7a). ESP32's existing `od_hal_i2c.h:3-18` already argues this at length against the
   register-shaped sketch in `docs/SHARED_API_DESIGN.md`; keep that shape and add the flag.
   Expressing both framings in shared code is what lets § 1.7a be fixed once instead of twice.
3. **`probe()` stays a primitive.** `od_hal_i2c.h:58-66`: IDF rejects a zero-length transmit
   outright, so a scan built on it reports every address absent.
4. **Nordic's implementation already exists** — `opendisplay_i2c.c` (219 lines) plus the touch
   bit-bang (`opendisplay_touch.c:71-175`). This is a re-home, not a new driver.

`od_hal_adc` is **not** on this list. Its three consumers are the ESP32 battery sense, the ESP32 ADC
ladder and Nordic's SAADC — and the three ADCs disagree on raw units by design (§ 1.4). There is no
portable driver to write behind it.

### 2.3 Two seams that are not HALs

- **Publish.** Nordic has a bounds-checked `opendisplay_ble_set_dynamic_byte()`
  (`opendisplay_ble.c:537`-adjacent, definition bounds-checks against `sizeof(dynamic_return)`);
  ESP32 writes the raw global `dynamicreturndata[]` from **27 sites across 6 files** with ad-hoc
  `if (idx < 11)` guards; Silabs writes a file static directly. A shared sensor module needs the
  Nordic shape: `void od_advert_dynamic_set(uint8_t index, uint8_t value)` in the **PURE** tier next
  to `od_advert.c`, which already owns `OD_ADVERT_DYNAMIC_LEN` (`od_advert.h:50`). This is a small,
  independently landable promotion with no HAL dependency at all.
- **Log.** All three sensor stacks log, and `shared/` has no log seam. Follow `od_config.c`'s
  precedent — return a status/report struct and let the caller log (`od_config.h:171-183`).

---

## 3. Cadence divergence

### 3.1 `AUDIT_NORDIC_ZEPHYR_2026-08-14.md` § 3.4 is fixed

The audit found Nordic read the chip temperature once at boot and reused it forever. **It now reads
per publish**: `read_chip_temperature()` is called from `update_msd_payload()` at
`targets/nordic-zephyr/src/opendisplay_ble.c:434`, with `:438-439` citing the reference ordering.
ESP32 (`display_service.cpp:1609`) and Silabs (`opendisplay_ble.c:1700`) do the same. **Closed.**

### 3.2 What § 6 "Calibration 2" still catches

The audit's rule — *promotion boundaries must include behavioural cadence, not just byte layout* —
is the right lens, and four cadence divergences remain, all invisible to `od_advert`:

1. **ESP32 caches failed battery readings for 30 s** (§ 1.4). The MSD battery field is a shared
   encoding fed by a target-specific acquisition whose *failure* semantics differ. Exactly the
   § 6 failure mode: identical bytes, one of them stale by policy.
2. **Nordic's ≤1 s process loop while disconnected** (`main.c:15-32`, `opendisplay_ble.c:1059-1072`).
   This is one cause with three wire-visible effects: buttons can miss a short press (§ 1.5), touch
   latency is unbounded by `poll_interval_ms`, and buzzer note boundaries quantise to a second. It
   is the same defect DEDUP_3 § 3.3 proposes fixing via a deadline return value.
3. **Nordic's button ISR is inert** (§ 1.5) — the interrupt exists, is armed, and changes nothing.
4. **Silabs skips battery measurement on a `quick` publish** (`opendisplay_ble.c:1702`), a third
   distinct rule for when the battery field is allowed to be stale.

The **sensor TTLs themselves agree**: 30 s everywhere, for SHT40, BQ27220, nPM1300 and battery on
all three targets. That is the one cadence that is already uniform, and it is uniform by
coincidence — three independently written constants (`sensor_sht40.cpp:248`,
`opendisplay_sensor_sht40.c:13`, `opendisplay_ble.c:146`), which is precisely the shape
`od_advert.h:9-15` describes as *"a shared module — just one the compiler cannot check."*

---

## 4. Ranking

Scored as (duplication removed) ÷ (risk + seam cost). "Risk" weighs wire-visibility and the absence
of any test.

| # | Subsystem | Dup. removed | Seam cost | Verdict |
|---|---|---|---|---|
| **1** | **SHT40 + BQ27220** (one module) | ~240 lines × 2 | `od_hal_i2c` + `HAL_IO` | **Promote first.** |
| **2** | **Buzzer sequencer + note table** | ~210 lines × 2 | `od_hal_tone` (2 fns) + `HAL_IO` | **Promote — fixes a live wire bug.** |
| **3** | **Button state machine** | ~60 lines × 3 | `HAL_IO` only | **Promote — cheapest seam of all.** |
| **4** | **GT911 touch** | ~450 lines × 2 | `od_hal_i2c` + ISR attr + C port | **Promote, but last.** |
| **5** | `od_advert_dynamic_set()` | 3 call conventions | none (PURE) | **Promote — prerequisite, trivial.** |
| — | Battery acquisition | ~30 lines | high | **Do not promote.** Fix in place. |
| — | nPM1300 | 0 (n = 1) | — | **Do not promote.** |
| — | ADC ladder, power-off buttons, `wake_button.cpp` | 0 (n = 1) | — | **Do not promote.** |

**1 — SHT40 + BQ27220.** The best ratio in the report. ~240 portable lines duplicated with
**verified-zero divergence in conversion, CRC, clamping and MSD packing** (§ 1.1, § 1.2) — so the
promotion is a pure de-duplication with no behaviour to settle first, which is rare here. They share
one bus, one TTL, one MSD-byte convention and one address-default idiom; splitting them into two
modules would duplicate all four. Silabs declines the tier entirely. Risk is bounded by the fact
that a differential host test can be written against both existing encoders the way
`tests/host/advert_test.c` was.

**2 — Buzzer.** Slightly less duplication than touch but a much cheaper seam and a far higher payoff:
it is the only item here that fixes a **currently-wrong wire behaviour** (§ 1.6 — every melody plays
at the wrong pitch on Nordic, confirmed against the host encoder). The policy side is already
vendor-free on ESP32 and the two-function `buzzer_hw.h` interface promotes verbatim. DEDUP_3 § 3.5
already names the buzzer runner as `HAL_IO`'s expected second tenant. Promoting the ESP32 table
*is* the fix.

**3 — Button state machine.** Smallest absolute win (~60 lines × 3) but the cheapest seam — GPIO and
time only, no new HAL beyond DEDUP_3's — and it forces three open behavioural questions to be
answered in one place: `pins_used == 0`, `MAX_BUTTONS`, and Nordic's inert IRQ. Three copies rather
than two also makes it the only subsystem here where all three targets participate.

**4 — Touch: promote, but not yet, and not for the reason feared.**
The concern was that touch is too controller-specific. **The evidence says otherwise:** there is
exactly one controller in the entire protocol (`opendisplay_structs.h:930-931`), the register map
and reset timing are character-identical, and the coordinate transform agrees step for step
(§ 1.7). It is the *largest* duplication in this report — ~450 lines of the same chip driver twice.

What makes it fourth is seam cost, not controller variance: it needs `od_hal_i2c` on Nordic (which
means re-homing 105 lines of bit-bang, `opendisplay_touch.c:71-175`), an `OD_HAL_ISR_ATTR` to retire
`esp_attr.h`, a publish callback, a `bool (*busy)(void)` predicate to give Nordic the transfer gate
ESP32 has, and a C++→C99 port of `touch_input.cpp`. **It should follow item 1**, which builds and
proves `od_hal_i2c` against two much simpler drivers first. Doing touch first would mean designing
the I2C seam against the hardest consumer with no test coverage anywhere.

Two things must be settled *before* the port, not during it, because each side is right about one of
them: adopt ESP32's dual read framing (§ 1.7a) **and** Nordic's status-clear (§ 1.7b). The latter
also needs fixing in `../Firmware/`.

**Not worth promoting.** Battery acquisition is three genuinely different hardware paths whose only
common ground is a 30 s TTL and a source-priority list (§ 1.4); the shared part is already shared
(`od_advert_battery_10mv_from_mv`). nPM1300, the ADC ladder, long-press power-off and
`wake_button.cpp` are all `n = 1` — promoting a single implementation buys nothing and costs a seam
(decision 9).

---

## 5. Per-board vs per-target

**Genuinely per-board (a build- or calibration-time property):**

- **`voltage_scaling_factor`** — calibrated against the ADC's bit width *and* reference voltage
  (§ 1.4). The same config value means different volts on ESP32 (12-bit, ATTEN_12DB) and Nordic
  (renormalised to 10-bit/3.6 V, `opendisplay_battery.c:26-34`).
- **SAADC pin→AIN mapping** — `opendisplay_battery.c:66-88` already `#if`s nRF52840 (P0.02-05,
  P0.28-31) against nRF54L (P1.00-07), with the comment recording that the nRF54 rule was rejecting
  a perfectly good nRF52 analog pin.
- **ADC ladder thresholds** — carried per board in `BinaryInputs.reserved[]`
  (`device_control.cpp:96`, `:144-175`), and `:124-126` records the feature is unvalidated on nRF.
- **LEDC channel/timer** — `7 % LEDC_CHANNEL_MAX` lands differently per ESP32 variant
  (`buzzer_hw.cpp:36-39`).

**Per-target (a whole capability is present or absent):** Silabs has no I2C sensor bus, no touch
and no buzzer; nPM1300 is Nordic-only; the ADC ladder and long-press power-off are ESP32-only.

**Everything else is per-*device*, from the parsed config at runtime — and this is the important
finding.** There is **no board `#ifdef`, no pin literal and no per-board table in any sensor, touch,
button or buzzer file.** Sensor presence, bus id, I2C address, MSD slot, touch flags, button pins
and buzzer pins all come from `struct od_config` (`sensors[]`/`sensor_count`,
`data_buses[]`/`data_bus_count`, `binary_inputs[]`, `touch_controllers[]`, `passive_buzzers[]`,
`power_option`). No ESP32 board fragment under `targets/esp32-idf/boards/` sets a sensor flag —
every board compiles every driver and discovers hardware at runtime.

So **decision 9 is satisfied at the tier level, not the board level**: a target that lacks a sensor
class declines the tier in `shared/sources.cmake` and pays nothing, exactly as `efr32bg22-slc`
already declines `APP_RXQ`, `HAL_ADV` and `HAL_WDT`
(`targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:289-292`). Per-board gating would need
Kconfig-equivalent machinery that decision 7 forbids, and is not needed.

### `shared/sources.cmake` tiers

```cmake
# HAL_I2C needs shared/hal/od_hal_i2c.h implemented (probe/write/read/write_read, bus_id-keyed)
# AND the HAL_IO pair (od_hal_gpio, od_hal_time).
set(OD_SHARED_SOURCES_HAL_I2C
    "${CMAKE_CURRENT_LIST_DIR}/core/od_sensor_i2c.c"   # SHT40 + BQ27220   (item 1)
    "${CMAKE_CURRENT_LIST_DIR}/core/od_gt911.c"        # touch             (item 4)
)

# HAL_TONE needs shared/hal/od_hal_tone.h (tone_start/tone_stop) plus HAL_IO.
set(OD_SHARED_SOURCES_HAL_TONE
    "${CMAKE_CURRENT_LIST_DIR}/core/od_buzzer.c"       # item 2
)
```

`od_button.c` (item 3) joins DEDUP_3's **`HAL_IO`** tier; `od_advert_dynamic_set()` (item 5) goes in
existing **`PURE`** beside `od_advert.c`. Consumers: `esp32-idf` and the host tests take the
aggregate; `nordic-zephyr` adds `HAL_I2C`, `HAL_TONE`, `HAL_IO`; **`efr32bg22-slc` takes only
`HAL_IO`** (for buttons) and declines `HAL_I2C` and `HAL_TONE`.

---

## 6. Open questions, ranked

1. **Which buzzer frequency mapping is canonical — and who fixes Nordic?** The host
   (`../py-opendisplay/.../buzzer_activate.py:29,33`) and ESP32 agree on
   `13.75·2^(idx/24)`; Nordic and its `Firmware_NRF54` donor use a linear ramp (§ 1.6). ESP32 is the
   authority and the host confirms it, so this looks settled — but it means **every melody ever sent
   to a Nordic tag has played wrong**, and the fix belongs upstream in `../Firmware_NRF54/` as well.
   Is this a known-broken feature or has nobody tried it on Nordic?
2. **`pins_used == 0`: all pins, or none?** ESP32 and Nordic say all; Silabs says none; the contract
   (`opendisplay_structs.h:856`) documents no zero case (§ 1.5). A config relying on the
   authority's reading is silently button-less on BG22 today. Settle it in the contract before
   promoting `od_button.c`, or the shared module just picks a winner by accident.
3. **Should a failed battery read be TTL-cached?** ESP32 (the authority) says yes for 30 s; Nordic
   and Silabs retry (§ 1.4). Two targets diverge from the authority and both look *more* correct.
   Which becomes the shared policy?
4. **Touch: adopt each target's better half?** ESP32's dual read framing (§ 1.7a) and Nordic's
   status-clear (§ 1.7b) are each right, and § 1.7b is a live bug in `../Firmware/`. Confirm both
   before the port — a shared `od_gt911.c` that takes only one target's behaviour would ship a
   regression.
5. **Why is Nordic capped at 8 buttons, and does anything depend on it?** `MAX_BUTTONS 8u`
   (`opendisplay_button.c:12`) against 32 everywhere else, dropped with no log (§ 1.5). Looks like
   an oversight on a 256 KB part, but it may be a deliberate RAM choice from the nRF54L15 work.
6. **Is Nordic's button interrupt meant to work?** `s_button_irq_pending` is write-only and there is
   no wakeup mechanism (§ 1.5), so presses shorter than ~1 s can be missed while advertising.
   Whether this is a promotion concern or an immediate bug depends on whether DEDUP_3's deadline
   return value (§ 3.3 there) lands first.
7. **Does `od_hal_i2c` want an `od_hal_adc` sibling eventually?** This report says no (§ 2.2) — the
   three ADCs disagree on raw units by design. But if a second target grows an ADC ladder, that
   judgement changes.
8. **Should `OD_SENSOR_TYPE_NPM1300 = 6` land in the canonical contract?**
   `protocol_pending.h:27-48` has a documented plan and a `_Static_assert` tripwire; the protocol
   header is frozen. Not blocking anything here (nPM1300 is not promoted), but the tripwire fires
   the moment the header reopens.
9. **Is a differential host test acceptable as the gate for items 1-4?** There is zero test coverage
   for any of these seven subsystems and no hardware verification of Nordic touch, Nordic buzzer, or
   any Silabs input path. `tests/host/advert_test.c`'s pattern — keep both shipped implementations
   and sweep them against the promoted one — is the only precedent, and for SHT40/BQ27220 it would
   be conclusive. It would **not** be conclusive for touch, which needs a bus.
