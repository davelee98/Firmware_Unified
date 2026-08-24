# Sensor and touch seam set — design

**Status:** proposed, revised 2026-08-23. Step 4 of
[PLAN_DEDUP_OUTSTANDING_2026-08-22.md](PLAN_DEDUP_OUTSTANDING_2026-08-22.md) § 8 — the written
design that row requires before code. No implementation is in this document.

**What it unblocks:** SHT40 + BQ27220 (~950 lines) and GT911 touch (~1,450 lines) in that plan's
§ 4. Roughly 2,400 lines, the largest remaining block.

---

## 1. The boundary in the live code

### The logical bus is already shared

`SensorData.bus_id` and `TouchController.bus_id` reference the canonical `DataBus` table.
`DataBus` supplies the bus type, SCL/SDA pins, speed and pull configuration. Both active targets
already resolve that same logical ID:

- ESP32: `initOrRestoreWireForBus(bus_id)` validates `globalConfig.data_buses[bus_id]`, switches
  the one live IDF bus when its pins change, and initializes it on demand.
- Nordic: `od_sensor_bus_for(bus_id, &bus)` validates the same entry and constructs a caller-owned
  bit-banged `struct od_i2c_bus`.

The low-level APIs do not currently take a bus ID, but the layer immediately above them does.
That is the natural shared-HAL boundary.

### There are three engines, not three contracts

| Implementation | Engine shape |
|---|---|
| ESP32 `hal/od_hal_i2c` | One implicit live bus, IDF `i2c_master`, switched between configured pin sets on demand |
| Nordic `opendisplay_i2c` | Caller-owned bit-banged bus with explicit STOP selection |
| Nordic `opendisplay_touch.c` | A second private bit-banger duplicating the first |
| BG22 `opendisplay_ble.c` | A fourth bit-banger for the TNB132M NFC tag. Its operations are ordinary writes and one repeated-START read — see T6 |

The Nordic touch copy is target-local duplication and must fold into `opendisplay_i2c.c` before
touch promotion. **The BG22 NFC one is in scope** — its transactions fit the four operations
exactly (T6), and leaving it out would make the "no second I2C engine" ratchet a false statement
about the tree. After both cleanups there are **three** engines, one per target, which is exactly
what a HAL is for.

### Four operations express every transaction in scope

| Consumer | Required framing |
|---|---|
| SHT40 | write command with STOP; yielding 12 ms wait; bare read |
| BQ27220 | selector write followed by repeated START and read |
| GT911 | both repeated-START read and STOP-then-START read; deployed clones differ |
| nPM1300 | ordinary writes and repeated-START reads |
| AXP2101 | probe, ordinary writes and reads |

ESP32's existing `write`, `read`, `write_read` and `probe` operations already express all
four. Nordic can implement the same operations over its bit-banger. A generic STOP flag is not
needed: `write_read` means no STOP between phases, while `write` followed by `read` means
STOP-then-START.

### I2C is necessary, not sufficient

| Driver | Additional seam |
|---|---|
| SHT40 | A yielding 12 ms delay |
| BQ27220 | Charger enable/state GPIO and MSD dynamic-byte output |
| GT911 | Reset/enable GPIO, interrupt attach/detach and long reset delays |

Those requirements do not argue against an I2C HAL. They stay narrow APP or GPIO seams alongside
it.

### The authority proves portable operations

`../Firmware` serves ESP32 and nRF52840 with the same SHT40, BQ27220 and GT911 driver sources over
Arduino `Wire`. The chip split is in bus setup, not device transactions. That proves the driver
logic is portable, but it does **not** justify rebuilding Arduino: this plan takes only the four
operations the live drivers require, with bounded buffers and explicit framing.

The earlier suggested trigger — a fourth I2C driver — has already fired: SHT40, BQ27220, GT911 and
nPM1300 exist now, with AXP2101 as a fifth target-private consumer. The HAL should be created in
this promotion rather than after another chip-specific APP surface is added.

## 2. Decisions

### T1 — Create a minimal shared I2C transaction HAL now

`shared/hal/od_hal_i2c.h` owns this surface:

```c
int od_hal_i2c_probe(uint8_t bus_id, uint8_t addr7);

int od_hal_i2c_write(uint8_t bus_id, uint8_t addr7,
                     const uint8_t *data, uint16_t len);

int od_hal_i2c_read(uint8_t bus_id, uint8_t addr7,
                    uint8_t *data, uint16_t len);

int od_hal_i2c_write_read(uint8_t bus_id, uint8_t addr7,
                          const uint8_t *tx, uint16_t tx_len,
                          uint8_t *rx, uint16_t rx_len);
```

Semantics:

- `write` completes with STOP.
- `read` is a complete START/read/STOP operation.
- `write_read` is one atomic transaction with a repeated START and no intervening STOP.
- Every call resolves and selects `bus_id` before touching hardware. A completed call retains no
  bus ownership.
- **The key is `DataBus.instance_number`, not an array index.** The canonical header settles it:
  `instance_number` is *"0-based bus-block index; referenced by SensorData.bus_id,
  TouchController.bus_id, etc."* (`opendisplay_structs.h:802`). BG22's NFC resolves it correctly
  by scanning for a match (`opendisplay_ble.c:1101-1103`); ESP32 and Nordic sensors index the
  array directly (`opendisplay_sensor_common.h:28`), which agrees only while records arrive in
  order with no gaps. **Out-of-order or sparse `DataBus` records bind those sensors to the wrong
  bus** — recorded in `DIVERGENCE_MATRIX` § 14, and the HAL's resolution is the scan.
- **The scan is one shared helper, and it defines malformed-config behaviour.** "Scan for a match"
  is underspecified: `od_config.c` appends repeatable packets without rejecting a duplicate
  `instance_number`, so a bare scan silently means *first match wins by packet order*. Put the
  policy in a pure `od_config` lookup rather than letting three HALs each invent one:

  ```c
  /* Resolve a DataBus by its instance_number. NULL on no match or on ambiguity. */
  const struct DataBus *od_config_data_bus(const struct od_config *cfg, uint8_t instance);
  ```

  | Case | Result |
  |---|---|
  | Exactly one record matches | return it |
  | No record matches | **refuse before touching hardware** |
  | More than one matches | **refuse as ambiguous** — never pick by packet order |

  Refusing a duplicate is a choice, not an obvious default: it turns a malformed config into no
  device rather than an arbitrary one. Host-tested with sparse, out-of-order and duplicated
  records.
- **The bus argument is taken literally; `0xFF` is not a HAL rule.** Normalisation — or refusal —
  belongs to the consumer. A transport that invents a default for its caller's sentinel cannot
  know which rule applies, and that is how the substitution defect in `DIVERGENCE_MATRIX` § 13
  arose.
- **`0xFF` means UNCONFIGURED. Project ruling, 2026-08-23, reaffirmed.** It is the contract's
  pervasive absent sentinel (`opendisplay_structs.h:295-298`) and it now means the same thing for
  every `bus_id`-shaped field. Shared sensor and touch policy **refuses** it; the device is not
  probed. `NfcConfig.bus_instance` was already literal and is unchanged.

  **This is a deliberate protocol change and it has two external consequences**, tracked rather
  than argued:

  | Consequence | Where |
  |---|---|
  | `opendisplay_structs.h:945` documents `TouchController.bus_id == 0xFF` as "bus 0". It contradicts the ruling and the header is frozen | `FOLLOWUPS` § 14 |
  | `py-opendisplay` defaults an **omitted** touch `bus_id` to `0xff` (`config_json.py:643`) and its model comments it as "default bus 0" (`config.py:886`). Under this ruling the host must default to `0` instead, or it emits configs the device rejects and touch stops being configured for any block that omits the field | `FOLLOWUPS` § 15 — a host defect, and **NOT a gate on the firmware change** (project ruling 2026-08-24): `0xFF` makes the config invalid, so refusing it stands on its own |

  Four firmware sites currently substitute bus 0 and are corrected to refuse
  (`DIVERGENCE_MATRIX` § 13); Nordic touch (`opendisplay_touch.c:299`) already behaves this way.
- Return values distinguish success, invalid bus/argument, address NACK and other transport
  failure. Exact constants are fixed in the header and host-tested.

There is no shared `init`, `deinit`, `set_clock` or STOP flag. Setup, caching, locking,
reconfiguration and invalidation are implementation details below each target's four operations.

### T2 — Bus setup and engine state stay target-owned

ESP32 folds `initOrRestoreWireForBus()` into the start of each HAL operation. It may keep one IDF
bus live and switch it when the requested configured pins differ. Its panel-refresh invalidation
remains target-private; the next operation restores the requested bus.

Nordic resolves the configured `DataBus` and initializes a local bit-banged bus inside each
operation. Its `write_read` adapter performs `od_i2c_write(..., stop=false)` and
`od_i2c_read()` with the same bus object.

BG22 implements the I2C header for its NFC transport (T6) and takes **neither** `APP_SENSOR` nor
the touch sources — it has no sensor and no touch consumer, and grows no dummy functions or
dormant symbols for them.

Target-private nPM1300 and AXP2101 may call the canonical HAL without being promoted. Sharing an
engine does not imply sharing every device driver.

### T3 — Keep yielding delay narrow; do not create a sleep HAL

SHT40 needs a yielding wait between two completed I2C operations. The shared sensor driver uses:

```c
/* od_sensor_app.h */
void od_sensor_app_delay_ms(uint16_t delay_ms);
```

Nordic implements it with `k_msleep`; ESP32 uses its existing target-private millisecond delay.
This function belongs to `APP_SENSOR`, so BG22 does not implement it when it takes no sensor
driver.

This is not a general time or scheduler HAL. D8 already uses the BG22 sleeptimer directly and the
LED machine returns deadlines. No other consumer is waiting on a portable sleep contract.

### T4 — Shared sensor drivers own device policy, not only conversion

The shared SHT40 driver owns:

- config walk (**all** matching sensor entries — SHT40 is multi-instance today,
  `sensor_sht40.cpp:226,240`), and refusal of an unconfigured `bus_id`;
- default/candidate addresses and probing;
- soft reset at initialization;
- command byte, measurement sequence and retry policy;
- CRC-8, conversion, clamping, poll TTL and MSD packing.

The shared BQ27220 driver owns:

- config walk (**first match only** — `sensor_bq27220.cpp:64-70` returns the first BQ entry and
  the promotion must not silently make it multi-instance; this is why the BQ seams below need
  no instance argument while `od_sensor_app_msd_write()` takes an index), default address and
  initial probe;
- register selectors and widths;
- poll TTL and the “have polled” latch;
- cached voltage and failed/implausible-read behavior;
- SOC clamp and MSD packing.

There is no opaque `od_sensor_target_t`. It would put address discovery and retry policy into an
object populated by the target while claiming shared code owns both. Shared code instead reads the
shared `od_config` and passes explicit `bus_id` and address values to the HAL.

The remaining target functions are:

```c
/* od_sensor_app.h */
void od_sensor_app_delay_ms(uint16_t delay_ms);
bool od_sensor_app_bq_enable(bool on);
bool od_sensor_app_bq_charging(bool *charging);
void od_sensor_app_msd_write(uint8_t index, uint8_t value);
```

`od_sensor_app_bq_enable()` preserves the existing GPIO ordering: configure the output and then
establish the active level. These are GPIO operations, not BQ register writes. An absent enable
pin is a successful no-op. `od_sensor_app_bq_charging()` returns false when no state pin exists,
so “unknown” remains distinct from “not charging”.

The shared public surface preserves the callers both targets have today while making config
ownership explicit:

```c
void od_sensor_sht40_init(const struct od_config *cfg);
void od_sensor_sht40_poll(const struct od_config *cfg, uint32_t now_ms);   /* clock: see below */

void od_sensor_bq27220_init(const struct od_config *cfg);
void od_sensor_bq27220_poll(const struct od_config *cfg);
bool od_sensor_bq27220_is_configured(const struct od_config *cfg);
float od_sensor_bq27220_voltage_volts(void);
```

**`poll` takes the clock explicitly (amended 2026-08-24, during step 6).** The sketch above had
no `now_ms`, and the first implementation called `od_hal_uptime_ms()` — which `tools/check.sh`
rejects: `shared/core` does not sample the ambient time HAL, and only `od_log.c` is exempt. The
rule is the same discipline that makes `od_led` and `od_buzzer` return a delay instead of
sleeping. Policy that reads a clock it does not own cannot be tested against a 32-bit wrap and
cannot be driven by a target that schedules differently. The ratchet caught it; the signature
changed rather than the rule.

The functions do not retain `cfg` after return. Both current ports keep their TTL/cache statics
across an init call, so the shared version preserves that behavior rather than silently making
config reload a state reset. A separate reset policy can change it later with an explicit test.

### T5 — Touch uses the same HAL later

GT911 promotion happens after the sensor promotions and after the Nordic private bit-banger is
gone. Its shared driver calls the same I2C HAL directly and takes the donor/ESP32 two-framing
fallback because real clones require different forms. Nordic currently tries repeated START only;
adding the STOP-separated fallback belongs to the shared-driver promotion, not the mechanical
bit-banger cleanup.

GPIO reset and IRQ handling remain behind a touch/GPIO seam. ESP32's
`od_hal_gpio_{config_irq,config_irq_arg,clear_irq,irq_enable,irq_disable,irq_lock,irq_unlock}`
surface is the starting point for Nordic. The `_arg` variant is required by both multi-button
events and multiple touch instances.

~~Q7 remains an entry condition: Nordic clears GT911 status register `0x814E` after consuming a
sample while ESP32 does not.~~ **CLOSED 2026-08-24, and the premise was wrong.** All three clear
`0x814E` after consuming a report, at the same point in the poll loop — `../Firmware:703`,
`targets/esp32-idf/src/touch_input.cpp:708`, `nordic-zephyr/src/opendisplay_touch.c:559`. There was
never a divergence there.

**The real divergence is the over-count branch, and Nordic is the one that is right.** When the
status byte's low nibble exceeds `GT911_MAX_CONTACTS` (5) the sample is nonsense and every port
skips it — but ESP32 and the authority skip *without clearing the status*, and GT911 holds that
byte until the host writes 0. So the next poll reads the same byte, takes the same branch, and
touch never reports again until an init or resume path runs. One glitched read wedges it
permanently. Nordic clears and does not wedge.

**Ruling: the shared driver clears the status on the over-count branch**, taking Nordic's
behaviour. This is a deliberate exception to the "`Firmware` is the authority" default, because the
authority's behaviour is a latent wedge rather than a considered difference. The upstream defect is
reported in `FOLLOWUPS` § 17.

`TouchController.bus_id == 0xFF` means **not configured**, so Nordic touch's refusal
(`opendisplay_touch.c:299`, "an explicit data_bus is required") is the behaviour to keep and
propagate. The divergence to record is the other way round: **four sites substitute bus 0** —
`display_service.cpp:726`, `sensor_sht40.cpp:52`, `sensor_bq27220.cpp:43` and
`opendisplay_sensor_common.h:22`. All four then validate the substituted bus, so the failure is
narrower than D8's: it misfires only when a valid `data_buses[0]` exists and a sensor was never
assigned a bus — that sensor is then probed on someone else's bus, where an address collision
yields plausible-but-wrong readings rather than an error.

### T6 — BG22 implements the HAL for its NFC transport, and takes nothing else

BG22 has no sensors and no touch, so it takes neither `APP_SENSOR` nor the touch sources. It does
implement `od_hal_i2c`, because it already has an I2C engine and its transactions are the same
four operations:

| TNB132M operation | HAL call |
|---|---|
| Block read (`opendisplay_ble.c:606-632`) — START, addr\|W, sub, **repeated START**, addr\|R, 16 bytes, NACK on the last | `write_read(bus, dev7, &sub, 1, out, 16)` |
| Block write — sub byte followed by 16 data bytes | `write(bus, dev7, buf, 17)` |
| Prime commands | ordinary `write`, or `write_read` |
| Presence | `probe` — unnecessary here, harmless |

**No NFC-specific operation belongs in the shared HAL.** Everything above the transaction stays
target-owned adapter policy, and there is a lot of it: NFC powers the controller, enters a
critical section, runs several transactions, powers down, then parks SCL and SDA. That session is
the adapter's, not the HAL's.

**The failure contract is a decision, not an extraction.** BG22's engine reads SDA only — for ACK
(`opendisplay_ble.c:566`) and for data (`:581`) — and **never checks that SCL was released**, so it
cannot detect clock stretching or a stuck bus, and a held-low SDA reads as ACK and as data `0`.
Nordic's engine does check (`opendisplay_i2c.c:35-41`, `OD_I2C_STRETCH_TIMEOUT_US`) but returns a
bool, so its caller cannot tell a stretch timeout from an address NACK.

So the portable status surface is **`OK`, `EINVAL`, `ENODEV` (address NACK) and an aggregate
`EIO`** — no distinct stuck-bus code, because two of three engines cannot produce one honestly.
And this cutover takes the first of two options, explicitly:

1. **Extract nearly verbatim and claim no stuck-bus detection** — chosen. The GPIO trace pins what
   the engine actually does. Neither the host contract nor the hardware gate may assert stuck-bus
   behaviour on BG22.
2. Authorise bounded idle/SCL-release checks as a **deliberate behavioural addition**, with the
   trace proving their timing. Not taken here: it is new behaviour on a transport no board in this
   fleet can exercise.

**Keep the bit-banging and its electrical timing.** Do not substitute the Silabs I2C peripheral
driver during this cutover — the existing `sl_udelay_wait()`-paced edges are what the deployed
TNB132M sequence was tuned against, and no board here can prove a replacement works (§ 5).

**Block writes need a 17-byte workspace** (sub + 16). Use a bounded local; if a stack measurement
rejects it on a 32 KB part, add a justified scatter-write operation rather than a heap allocation.

This makes the repo-wide "no second I2C engine" ratchet **truthful**, which it would not be with
BG22 excluded.

### T7 — Not promoted

Battery acquisition (three different ADCs), nPM1300, AXP2101 and the ADC ladder remain
target-private. nPM1300 and AXP2101 may consume the shared I2C HAL without moving their policy.

## 3. Staging

Each numbered cutover is independently revertable and receives its own software gate before the
next subsystem promotion. **Steps that touch silicon this fleet has also receive a hardware gate;
step 9 does not, and says so** — no board here carries a TNB132M, so that cutover ships as a
software candidate with its rows open.

1. **Correct the bus-default divergence:** `0xFF` means *not configured*, so the four sites that
   substitute bus 0 (T5) refuse instead. Nordic touch (`opendisplay_touch.c:299`) already does and
   is the reference. Recorded in `DIVERGENCE_MATRIX` § 13; the canonical header line is reported
   via `FOLLOWUPS` § 14.

   **Project ruling 2026-08-24: this does NOT wait on `py-opendisplay`.** A `bus_id` of `0xFF`
   makes the config invalid and a substituted bus 0 is invalid whatever the host sends, so
   `FOLLOWUPS` § 15 is a host defect on its own schedule rather than a precondition. The cost is
   stated rather than discovered: a config omitting `bus_id` stops configuring touch, which is the
   point — the alternative is probing an unassigned device on another bus, where an address
   collision gives plausible-but-wrong readings instead of a failure. Devices already holding
   `0xFF` in that field need re-provisioning.

   **Fix § 14's array-index defect in the same step.** `bus_id` names a `DataBus.instance_number`,
   but `display_service.cpp:729` and `opendisplay_sensor_common.h:28` index `data_buses[bus_id]`,
   and `od_config.c`'s `store_repeatable()` appends in arrival order without ever reading
   `instance_number`. So slot 0 is "the first `0x24` packet that arrived", not instance 0. The two
   defects share all four call sites, and correcting only the sentinel would leave a refusal that
   still resolves to the wrong bus.
2. **Nordic-local cleanup:** fold `opendisplay_touch.c`'s private bit-banger into
   `opendisplay_i2c.c`. Preserve its current repeated-START behavior exactly; do not add the
   donor's STOP-separated fallback in this mechanical step. Hardware-check touch before proceeding.
   **This step changes bit timing and that is the risk, not the framing.** The private bit-banger
   runs at a fixed `I2C_HALF_BIT_US 5` (`opendisplay_touch.c:44`); `opendisplay_i2c.c` derives
   `half_period_us` from `DataBus.bus_speed_hz` (`:167`). Fold it at the half-period the touch code
   uses today and change the rate only as a separate, measured step — a timing difference may be
   why the fork exists at all.
3. **Add `shared/hal/od_hal_i2c.h` and its scripted host contract test.** A header has no source
   tier; the target implementations remain in their target builds. No shared device driver yet.
4. **ESP32 HAL cutover:** add `bus_id` to the four operations, move bus selection beneath them,
   and repoint the existing SHT40, BQ27220, GT911 and AXP2101 callers. Hardware gate.
   **PREREQUISITE ADDED 2026-08-24, after step 4's review: bind the production adapter to
   `tests/host/i2c_contract.inc` before repeating this on Nordic.** Step 4 shipped a swapped
   SDA/SCL pin order — the caller passed `(sda, scl)` and the function was redefined as
   `(scl, sda)` — which would have crossed the lines on **every configured ESP32 bus**, and it
   passed the whole gate: two same-typed parameters reorder silently and nothing on the host
   drives them. Review caught it; no automated check could have. The same review found the
   transport refusing a literal `0xFF` instance, contradicting T1 and the contract's own case.

   Both live in the *resolution* half (`display_service.cpp`), which is C++ and reads
   `globalConfig`, so it is not host-bindable as written. Binding it means moving bus resolution
   into a translation unit a host test can link — a design change, not a fix, and the reason it
   is written down here rather than done inside step 4. **Until it is done, the pin order and the
   sentinel policy on both targets are protected by the hardware gate alone.**

5. **Nordic HAL cutover:** implement the same four operations over `opendisplay_i2c.c`, then
   repoint SHT40, BQ27220, GT911 and nPM1300. Hardware gate.

   **GT911 IS repointed, and the rate question is settled: follow `bus_speed_hz`** (project
   ruling 2026-08-24). **There is no "touch rate" in the authority.** `../Firmware` brings a bus
   up at `bus_speed_hz ? bus_speed_hz : 100000` (`display_service.cpp:946`) and
   `touch_input.cpp` never names a clock — every device on the bus, GT911 included, runs at the
   configured rate. Nordic's private bit-banger hardcoded a 5 µs half-period, which is 100 kHz;
   it ignored config rather than deciding anything, and *that* was the divergence.

   So routing GT911 through `od_hal_i2c` is not a rate change smuggled into a refactor — it is
   the correction. Step 2's caution ("a timing difference may be why the fork exists") is
   answered by the authority: the fork predates the shared engine and hardcoded a number.

   **The bench condition this creates:** a Nordic board declaring `bus_speed_hz = 400000` now
   clocks GT911 five times faster than the private engine did. That row is in the hardware
   checklist. If touch fails at 400 kHz on real clones, the answer becomes a documented clamp
   with measured evidence — not a hardcoded 5.

   ~~**GT911 was NOT repointed here, because this step and step 2 contradict each other.**~~
   Superseded by the ruling above. The original reasoning was: Step 2 pinned touch at 100 kHz to
   reproduce the private bit-banger's fixed 5 µs, and said to change the rate only as a separate
   measured step. The shared seam has **no speed argument** — `od_hal_i2c_*` derives the rate from
   `DataBus.bus_speed_hz` — so routing GT911 through it hands the touch clock to whatever the
   config declares, quintupling it on a 400 kHz entry. That is precisely the change step 2
   forbade, and there is no way to have both through a seam with no rate.

   T5 already places GT911's shared driver in **step 8**, after the sensor promotions, so the
   touch repoint belongs there — with the rate decision made explicitly and measured, rather than
   arriving as a side effect of a cutover. Until then Nordic touch keeps calling
   `opendisplay_i2c.c` directly at its pinned rate, which is one consumer of that engine not
   going through the HAL. The "one engine per target" ratchet is unaffected: it forbids a second
   engine, not a second caller of the one engine.
6. **SHT40 promotion:** add `shared/core/od_sensor_sht40.{c,h}` and the `APP_SENSOR` tier,
   whose linkage contract requires both `od_hal_i2c` and the delay/MSD APP seams. Delete both
   target policy copies after their last callers move. Hardware gate.
7. **BQ27220 promotion:** add `shared/core/od_sensor_bq27220.{c,h}` and the charger GPIO seams.
   Delete both target policy copies. Hardware gate.
8. **Touch promotion:** only after Q7 is decided and the IRQ seam exists. Hardware gate.

   **NO NORDIC BOARD IN THIS FLEET HAS A TOUCH CONTROLLER** (project direction, 2026-08-24). So
   the Nordic half of this step ships as a software candidate under the standing missing-hardware
   constraint, exactly as Transfer Phase 4's NFC did, and **ESP32 is the only target where the
   shared GT911 driver can be qualified at all**. Two consequences worth stating before the work
   starts rather than discovering them in review:

   - The Nordic GPIO IRQ seam this step needs — ESP32 has the full
     `od_hal_gpio_{config_irq,config_irq_arg,clear_irq,irq_enable,irq_disable,irq_lock,irq_unlock}`
     surface and Nordic has none of it — would be built to serve a consumer no board here can
     run. That is still worth doing if the seam has another user (multi-button events need
     `_arg` too), and hard to justify if it does not. **Decide which before building it.**
   - The two GT911 read framings can only be distinguished on ESP32 hardware. The host decoder
     that would tell them apart in a test is therefore the *only* mechanism covering the Nordic
     side, which raises its value rather than lowering it.

   Q7 is closed (clear the status on the over-count branch; `FOLLOWUPS` § 17) and the rate is
   settled (follow `bus_speed_hz`), so those are no longer entry conditions.

   **FIRST ATTEMPT AT THE DRIVER FAILED REVIEW, 2026-08-24. Its findings are the specification
   for the next one.** `shared/core/od_touch_gt911.c` exists on `feat/sensor-drivers-and-touch`
   marked DRAFT — DO NOT MERGE. Three of the six are API-shape problems, not bugs, which is why
   patching the draft is the wrong move:

   1. **The coordinate map cannot live in an adapter.** Both donors apply
      `apply_touch_map()` — swap, invert, clip to display size — **before caching and before
      packing** (`touch_input.cpp:743`). By the time a byte-write seam sees the MSD the sample and
      its `TouchController` are gone, so the transform has to be inside the shared driver. Every
      config using those flags gets wrong coordinates without it, for both live contacts and the
      latched release.
   2. **Cadence and publication cannot be split out either.** The donors carry per-controller
      `poll_interval_ms`, poll only the controller an IRQ or held-low line selected, back off
      100 ms after an I2C failure using private failure state, detach the IRQ when a controller is
      disabled, and publish the MSD **only when a decoded sample changed**. A `poll(cfg, now_ms)`
      that walks every controller and writes bytes cannot express any of it. The core needs to
      expose per-controller control and a changed/commit signal.
   3. **`enable_pin` is never asserted.** The donors drive it before the bus check
      (`touch_input.cpp:548`); a controller behind that enable never probes at all.
   4. **The address cascade is wrong.** After an explicitly configured address fails the donors
      `return 0` (`touch_input.cpp:355`); the draft falls through to auto-detection, which can
      bind a *different* controller. Its auto-detect also probes both addresses after each reset
      where the donors do reset-low → probe 0x5D, reset-high → probe 0x14.
   5. **Resume was gutted.** The donors probe the PID at the retained address, clear and wake INT
      on success, do a full reset and re-resolve on failure, and recover a controller whose `ok`
      is false but which is not disabled. The draft clears two fields and the status.
   6. **ESP32's no-DataBus default-pin path is dropped.** That target accepts `data_bus_count == 0`
      with a non-`0xFF` `bus_id` and transacts on the already-selected board-default bus. The step-1
      ruling retired the literal `0xFF` case, **not** this one, and the shared HAL cannot name that
      bus — so preserving it needs its own decision.

   Correct in the draft and worth keeping: the reset edge order, the three-attempt
   repeated-START-then-STOP-separated retry with its 500 µs spacing, the register byte-order
   fallback, the MSD packing cases and bound, the failure-streak reset point, and Q7's clears.
9. **BG22 NFC transport cutover — a software candidate, explicitly NOT hardware-qualified.**
   Separate from every sensor step, because BG22 takes no sensor code and because no board in this
   fleet carries a TNB132M.
   a. Extract the existing bit-banger **nearly verbatim** into
      `targets/efr32bg22-slc/hal/od_hal_i2c.c`. Preserve the `sl_udelay_wait()` edge pacing; do
      not substitute the Silabs peripheral driver.
   b. Bind that production file against fake GPIO and delay functions and pin the **trace**:
      START, STOP, repeated START, per-byte ACK/NACK, NACK on the final read byte, and the
      address-failure path. Current BG22 NFC host tests fake `od_nfc_app_read`/`write`, which sits
      **above** the transport — they cannot qualify this and must not be cited as if they do.
   c. Repoint the TNB132M helpers to `write` / `write_read`. Power sequencing, the critical
      section, prime commands and SCL/SDA parking stay in the NFC adapter.
   d. Use a bounded 17-byte workspace for block writes, or add a justified scatter-write operation
      if a stack measurement on a 32 KB part rejects the copy.
   e. Confirm BG22 flash, `data + bss` and `heap_size` against the pre-change image; run
      `tools/check.sh --targets`.
   f. **Every BG22 NFC hardware row stays open.** This is a software candidate under the standing
      missing-hardware constraint, exactly as Transfer Phase 4 was — merged code is not evidence.
10. **Ratchet:** **no second I2C engine on any target** — one named HAL implementation each, and
    the statement is now truthful because BG22's NFC transport is inside it (T6). No second
    SHT40/BQ CRC, conversion, polling or register-policy copy. BG22 links no **sensor** or touch
    symbol.

## 4. Host and production-source tests

The HAL test uses a scripted fake medium and binds the production adapters where practical. It
must pin:

- the requested bus reaches every operation **by `DataBus.instance_number`**, proven with
  out-of-order and sparse records where index and instance disagree;
- the HAL takes its bus argument literally — `0xFF` is not special-cased there; the refusal is
  asserted in the shared sensor/touch policy instead;
- `write` ends with STOP and `write_read` does not;
- STOP-separated `write` + `read` remains distinguishable from `write_read`;
- address NACK, invalid bus and stuck-bus failures remain distinct;
- Nordic `write_read` reuses one initialized bus object for both phases;
- ESP32 switches configured buses and reuses an already-selected bus;
- no operation retains ownership after return.

The shared SHT40 suite pins reset/fallback addresses, write–delay–read ordering, both CRC bytes,
conversion/clamping, TTL and failure output. The BQ suite pins two-byte voltage, one-byte SOC,
enable ordering, TTL/cache behavior, charging-state tri-state and MSD packing. Mutation checks
must prove the framing, widths, CRC and cache-order assertions are live.

GT911's later suite must exercise both register byte orders and both I2C read framings even if the
available board carries only one clone.

**BG22's transport suite is a GPIO trace, and it is mandatory** (staging step 9b). The production
`od_hal_i2c.c` is bound to fake GPIO and delay functions and the emitted edge sequence is pinned:
START, STOP, repeated START, per-byte ACK/NACK, NACK on the final read byte, address failure. The
existing BG22 NFC tests fake `od_nfc_app_read`/`write` — above the transport — so they say nothing
about it.

## 5. Hardware gate

- [ ] **ESP32 HAL:** SHT40, BQ27220, GT911 and AXP2101 behavior unchanged after their callers gain
      explicit bus IDs.
- [ ] **ESP32 bus switching:** two configured bus entries can be selected in sequence and returning
      to the first restores its pins and speed.
- [ ] **Nordic HAL:** SHT40, BQ27220, GT911 and nPM1300 behavior unchanged after the adapter cutover.
- [ ] **Nordic touch cleanup:** touch still reports contacts after its private bit-banger is removed.
- [ ] **Repeated START:** BQ27220/nPM1300 reads return plausible values; a production-source fake
      proves an inserted STOP fails the contract.
- [ ] **SHT40, each target:** temperature and humidity match a reference instrument and the
      pre-change image; a production-source fault test rejects either CRC-byte mutation.
- [ ] **BQ27220, each target:** voltage and SOC match the pre-change image, charging tracks a
      plugged/unplugged transition, and MSD bytes are unchanged.
- [ ] **Failure path:** disconnecting a sensor or holding SDA low reports failure rather than a
      plausible value. If unsafe on the bench, the production-source fault test is the gate.
- [ ] **Unconfigured bus:** a sensor or touch entry with `bus_id == 0xFF` is not probed, on either
      target — it must not be attached to bus 0. Nordic touch already behaves this way; the four
      substituting sites are what this row is checking.
- [ ] **A configured bus still resolves:** the same entry with an explicit `bus_id` works, so the
      refusal above did not simply disable the device.
- [ ] **Touch promotion:** multi-touch points, the Q7 status-clear decision and the 100 ms process
      interval are unchanged.
- [ ] **BG22:** image links no **sensor** or touch symbol, and `data + bss` plus `heap_size` are
      unchanged against the pre-change image.
- [ ] **BG22 NFC transport — OPEN, AND EXPECTED TO STAY OPEN.** No board in this fleet carries a
      TNB132M, so on-air qualification of the repointed transport is not available here. The GPIO
      trace test is the software gate; it does not qualify silicon. Same standing constraint as
      Transfer Phase 4's `0x0083` rows.

## 6. Open questions

1. ~~**Q7 — GT911 status authority.**~~ **Closed 2026-08-24.** The premise was false — all three
   clear `0x814E` after consuming a report. The real divergence is the over-count branch, where
   not clearing wedges touch permanently; the shared driver takes Nordic's clear. See T5 and
   `FOLLOWUPS` § 17.
2. **Bus serialization.** Confirm which ESP32 tasks can submit I2C concurrently and put the lock
   below the HAL if more than one can. The contract already requires each operation to be atomic;
   this question chooses the implementation, not the API.
3. **Hardware inventory.** Identify which available boards carry SHT40, BQ27220, GT911, nPM1300
   and AXP2101 before scheduling the per-target gates. Missing hardware leaves its row open; a host
   fault test does not qualify silicon. **BG22's TNB132M is already known absent** — that row is
   open on arrival, not pending an inventory.
4. ~~**Which key does the HAL take?**~~ **Answered by the contract**: `DataBus.instance_number`
   (`opendisplay_structs.h:802`). BG22's NFC scan is right and the sensors' array indexing is the
   defect (`DIVERGENCE_MATRIX` § 14). ~~Open only in *when*~~ **Closed 2026-08-24**: corrected in
   staging step 1, alongside the `0xFF` refusal, since both defects live at the same four sites.

5. ~~**Does the host change gate this?**~~ **Closed by project ruling 2026-08-24**: it does not.
   See staging step 1.

---

## 7. Reading this design against the survey

| Survey proposal | Decision here |
|---|---|
| One shared I2C seam | **Accepted.** The canonical config already supplies a logical bus ID |
| `bus_id`-keyed | **Accepted at the operation boundary.** Each target resolves it differently; `0xFF` is refused, not defaulted |
| Expose a STOP flag | **Rejected.** `write_read` versus `write` + `read` expresses the two legal shapes more precisely |
| I2C alone unblocks sensors/touch | **Rejected.** Delay, GPIO, MSD and IRQ remain narrow adjacent seams |
| Recreate `Wire` | **Rejected.** Only four bounded transaction operations are shared |
| — | **BG22 is in.** It implements the HAL for its existing NFC transport and takes no sensor code, which is what makes the "no second engine" ratchet true |

The result is a real HAL boundary without an Arduino compatibility layer: shared device policy,
target-owned engines, explicit framing and no chip-specific transaction RPC for every new driver.
