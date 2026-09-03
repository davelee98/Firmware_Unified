# Nordic dead code and name adapters — delete what nothing calls

**Date:** 2026-09-03

**Owns:** the fourteen unreferenced functions, the dead constants, and the two sensor
name-adapter files under `targets/nordic-zephyr/`.

**Companion:** [`PLAN_ESP32_ADAPTER_REMOVAL_2026-09-03.md`](PLAN_ESP32_ADAPTER_REMOVAL_2026-09-03.md),
landed as #87. § 3.1 below **corrects a factual error in that plan's § 6 item 1** — do not treat the
merged text as authority on Nordic.

---

## 1. Outcome

Three independent parts:

- **A. Dead code.** Fourteen functions and a set of constants that are declared, defined, and
  reached from nowhere. Mostly pure deletion.
- **B. Sensor name adapters.** `opendisplay_sensor_sht40.c` and `opendisplay_sensor_bq27220.c`,
  the exact analogue of what #87 removed on ESP32. Six call sites in two files.
- **C. Deep sleep on Nordic.** Not a defect, and not a deletion — a documented design whose
  divergence from the legacy nRF52840 firmware deserves a written decision. § 4 replaces this
  plan's first framing of it, which was wrong.

---

## 2. Part A — dead code

### 2.1 How this was found, and the one trap in the method

Every function declared in a `targets/nordic-zephyr/**/*.h` was checked for a call site.
Two things make or break this scan, and both bit the first attempt:

1. **Definitions must be found by brace-matching, not regex on the line.** A call statement
   `foo();` is textually identical to a prototype `foo();`. A scan that does not separate them
   reports nearly every function as dead — the first run said 113 of 122.
2. **The call-site search must be scoped to Nordic plus `shared/`, not the whole repo.** Target
   files share function names across targets. Searching everything made
   `opendisplay_display_power_off()` look alive on Nordic because **BG22** calls its own
   same-named function at `targets/efr32bg22-slc/opendisplay_ble.c:1975`. Nordic never calls it.
   That one slipped through the first pass and is included below.

**Fourteen functions are unreachable from Nordic or `shared/`.** That is the verified number:
two independent passes agree on the set, and a manual recount of every declaration under
`src/*.h` and `panel/*.h` found no fifteenth. No total-population figure is quoted here — the
regex that enumerates declarations undercounts multi-line and unusual signatures, so any
denominator it produces is an artifact of the tool rather than a fact about the target.

The scan only covers functions *declared in a Nordic header*. `_fini` (§ 2.3) is outside that
population — it has no declaration — and was noticed separately.

### 2.2 The fourteen

| Function | Definition | Declaration | Notes |
|---|---|---|---|
| `od_gpio_config_irq_arg` | `od_gpio.c:293` | `od_gpio.h:63` | see § 2.5 cascade |
| `od_gpio_irq_enable` | `od_gpio.c:338` | `od_gpio.h:72` | one-line forward to static `irq_set_enabled()` |
| `od_gpio_irq_disable` | `od_gpio.c:339` | `od_gpio.h:73` | ditto |
| `od_hwinfo_get_device_id` | `od_gpio.c:172` | `od_zephyr_compat.h:13` | one-line forward to Zephyr `hwinfo_get_device_id()`, **defined in the wrong file** — a compat shim living in the GPIO driver |
| `od_i2c_write` | `opendisplay_i2c.c:208` | `opendisplay_i2c.h:52` | superseded, see below |
| `od_i2c_read` | `opendisplay_i2c.c:241` | `opendisplay_i2c.h:57` | superseded, see below |
| `od_security_key_set` | `opendisplay_config_parser.c:53` | `opendisplay_config_parser.h:17` | § 2.4 |
| `opendisplay_ble_pipe_on_write` | `opendisplay_ble.c:657` | `opendisplay_ble.h:38` | pure forward to `opendisplay_pipe_on_write()` |
| `opendisplay_ble_pipe_on_connection_closed` | `opendisplay_ble.c:662` | `opendisplay_ble.h:39` | pure forward |
| `opendisplay_ble_set_connection_requested` | `opendisplay_ble.c:667` | `opendisplay_ble.h:34` | § 2.6 |
| `opendisplay_buzzer_stop` | `opendisplay_buzzer.c:207` | `opendisplay_buzzer.h:30` | delete; § 4 explains why it is safe to |
| `opendisplay_cs_scan_response_count` | `opendisplay_cs.c:30` | `opendisplay_cs.h:11` | sibling `_fill_scan_response()` is called (`opendisplay_ble.c:530`); this is not. Confirmed dead in CS-enabled nRF54 builds too |
| `opendisplay_display_power_off` | `opendisplay_display.cpp:221` | `opendisplay_display.h:13` | **the one the first scan missed**; live on BG22, dead on Nordic |
| `opendisplay_sensor_npm1300_is_available` | `opendisplay_sensor_npm1300.c:167` | `opendisplay_sensor_npm1300.h:14` | `_is_configured()` is what callers use |

All fourteen were additionally confirmed absent from the existing Nordic release and debug ELF
symbol tables for all three boards — after `--gc-sections`, so absence there is consistent with
deadness rather than proof of it, but a *presence* would have refuted the claim.

**Why `od_i2c_write`/`od_i2c_read` are superseded, not merely unused.** They are `bool` wrappers
over `od_i2c_write_ex()`/`od_i2c_read_ex()`, collapsing `enum od_i2c_result` to a yes/no. Every
active consumer of the engine — `od_hal_i2c.c` at `:71`, `:84`, `:97`, `:114`, `:119` — uses the
`_ex` forms, because it must distinguish `OD_I2C_RES_NACK_ADDR` from `OD_I2C_RES_ERR`: "nobody is
at that address" and "the bus failed" are different answers to the HAL's caller. The `bool` form
cannot express that, which is why it has no callers. Deleting it removes a trap.

Comments at `opendisplay_i2c.c:57` and `od_hal_i2c.c:112` describe the repeated-start sequence
using the `od_i2c_write(...)` / `od_i2c_read()` spellings. Reword to the `_ex` names in the same
commit; the sequence is still real.

### 2.3 `_fini` — unresolved, do not delete on this plan's authority

`newlib_stubs.c:3` defines `void _fini(void) {}`. It has no caller in this repo.

An earlier draft of this plan asserted it must be kept because newlib references it. **That is not
established.** What the existing link artifacts actually show:

- `_fini` is present in all six current Nordic application ELFs (e.g. `build-xiao_ble/zephyr/zephyr/zephyr.map:10354`).
- newlib's archive contains an object (`__libc_fini_array`) with an undefined `_fini`.
- That object is **not** selected into the current application links, and the linked `exit()` has
  no call to `_fini`.

So "newlib can reference it" is true, and "this link needs it" is unproven — the evidence points
the other way. Resolving it needs the one thing a static review cannot do: delete it, relink all
three boards, and look. Until that runs, leave it. Do not delete it as part of the fourteen, and
do not keep it with a confident comment either.

### 2.4 `od_security_key_set` — one dependent, not two

An earlier draft claimed two dependents and a "delete both or neither" coupling. Corrected:

- **`tests/host/fake_nordic/fake_nordic.c:325` is not a dependent.** It is an uncalled duplicate
  stub. The focused Nordic config-parser test compiles the production parser and does not link
  `fake_nordic.c` (`tests/host/CMakeLists.txt:719`); the two executables that do compile it
  (`:822`, `:876`) never call this symbol. Deleting the production wrapper breaks no host test,
  and the stub can be deleted independently. **`fake_nordic.c:125` carries an equally uncalled
  `opendisplay_display_power_off()` stub** — delete it in the same commit, on the same reasoning.
- **`shared/core/od_config.h:194` is documentation, not a dependency.** It justifies exposing
  `od_config_security_key_set()` with *"targets ask it outside the parse (Nordic's
  `od_security_key_set`)"*. Deleting the Nordic wrapper makes that sentence false.

`od_config_security_key_set()` itself stays — shared config and session code use it. The comment
needs restating on its own terms rather than having Nordic's name struck out. That is a `shared/`
edit, so it gets its own commit.

### 2.5 Deleting the GPIO functions cascades — finish the job

Removing the three dead GPIO APIs orphans their implementation. The plan is to remove that too,
in the same commit, or the deletion just moves the dead code one level down:

- `od_gpio_irq_enable`/`_disable` are the only callers of static `irq_set_enabled()`
  (`od_gpio.c:318`).
- `od_gpio_config_irq_arg` is the only user of the `od_gpio_irq_arg_fn` typedef (`od_gpio.h:59`),
  the `fn_arg`/`arg` slot fields, the corresponding branch in the IRQ trampoline, and two of
  `irq_attach()`'s parameters (`od_gpio.c:185`). The header comment explaining why the `_arg`
  variant exists goes with it.
- Removing `od_hwinfo_get_device_id` orphans `<zephyr/drivers/hwinfo.h>` in `od_gpio.c` and
  `<stddef.h>` / `<sys/types.h>` in `od_zephyr_compat.h`.
- After the cascade `OD_GPIO_EDGE_RISING` has no caller and `OD_GPIO_EDGE_BOTH` loses its only
  explicit use. **Decide** whether the edge enum is deliberate API headroom or should be pruned
  too; do not prune it silently as part of the cascade.

### 2.6 `opendisplay_ble_set_connection_requested` is not a Nordic defect

It is the only writer of `s_connection_requested` (`opendisplay_ble.c:198`), which feeds MSD
status bit 2 (`OD_MSD_STATUS_CONNECTION_REQUESTED`, read at `:446`). With no caller the bit is
always 0.

**Checked on all three targets before concluding — no target sets bit 2:**

- ESP32: `main.h:94`, annotated *"Reserved for future features"*, never written; read at
  `display_service.cpp:1598`.
- BG22: initialised to zero at `opendisplay_ble.c:183`, only read at `:1622`.
- Nordic: this dead setter, and the read at `:446`.

Dormant fleet-wide by design. Delete the setter as dead code, and **do not** present this as a
bug fix or a divergence. Decide at the same time whether `s_connection_requested` and its read
go too, or whether a permanently-zero variable feeding a documented-dormant wire bit is worth
keeping as the placeholder it is — the shared field `od_advert.h:85` exists either way.

*(Unrelated defect noticed while checking: ESP32 defines `connectionRequested` as `uint8_t` in
`main.h:94` but declares it `extern bool` in `display_service.cpp:50`. Not this plan's business;
record it in `FOLLOWUPS.md`.)*

### 2.7 Two build-input findings

**`zephyr/prj_cs.conf` appears unused.** Nothing includes it programmatically; the supported builds
get the same Channel Sounding settings from the two nRF54 board `.conf` files. Delete it, or defer
it with a reason — but confirm no build script references it by name first.

**Do not trust `zephyr/CMakeLists.txt:29`.** Its comment claims the LM20 branch is dead because
`BOARD` is empty at that point. That is false: `west build -b` sets `BOARD` before project CMake
runs, and existing LM20 `build_info.yml` records `prj_lm20_extra.conf` as an active input. Neither
that branch nor `prj_lm20_extra.conf` is dead. Step 0 corrects the comment; the branch and the fragment stay. Recorded here so a future sweep
does not delete a live fragment on the strength of a wrong comment.

### 2.8 Dead constants

Confirmed to have no use of *these Nordic definitions* anywhere in `targets/` or `shared/`. The
qualifier matters: ESP32 actively uses identically-spelled `DEVICE_FLAG_*` macros of its own, so a
bare name grep across the repo will show hits that have nothing to do with these.

| Constant(s) | Where |
|---|---|
| `OD_GPIO_PIN_UNUSED` | `od_gpio.h:11` |
| 5 of 6 `DEVICE_FLAG_*` — all but `DEVICE_FLAG_CHANNEL_SOUNDING` | `opendisplay_device_flags.h:6` |
| all 15 `CONFIG_PKT_*` | `opendisplay_constants.h:19` |
| 5 of 6 transmission-mode macros — all but `TRANSMISSION_MODE_CLEAR_ON_BOOT` | `opendisplay_constants.h:44` |

BG22 carries a near-identical block at `targets/efr32bg22-slc/opendisplay_constants.h:10`, equally
unused — not an exact twin, since it spells one id `CONFIG_PKT_BUZZER` where Nordic has
`CONFIG_PKT_PASSIVE_BUZZER`. Both are pre-`od_config_tlv.c` leftovers from when each target carried
its own packet-id table. **Delete Nordic's here; record BG22's as a follow-up** rather than widening
this plan into a second target.

---

## 3. Part B — the sensor name adapters, and a correction

`opendisplay_sensor_sht40.c` (2 functions, 18 lines) and `opendisplay_sensor_bq27220.c`
(4 functions, 28 lines) forward to `shared/core/od_sensor_*`, supplying
`opendisplay_get_global_config()` and `k_uptime_get_32()`.

### 3.1 The correction

`PLAN_ESP32_ADAPTER_REMOVAL_2026-09-03.md` § 6 item 1, now merged on `main`, says Nordic's wrappers
stay because *"its callers do not have `opendisplay_get_global_config()` and `k_uptime_get_32()`
equally to hand."*

**That is wrong.** Both callers have both:

- `opendisplay_ble.c` **defines** `opendisplay_get_global_config()` itself at `:609`, and already
  calls `k_uptime_get_32()` at `:770`, `:986` and `:997`.
- `opendisplay_battery.c` already calls `opendisplay_get_global_config()` at `:122` and
  `k_uptime_get_32()` at `:214`.

Nordic's wrappers are in exactly the position ESP32's were, and the argument used to keep them
argues for removing them. **§ 6 item 1 of the merged plan must be corrected whichever way part B is
decided** — a wrong justification left in `plans/` is worse than either outcome, because the next
person to ask this question will find it and stop there.

### 3.2 Call sites

| File:line | From | To |
|---|---|---|
| `opendisplay_ble.c:429` | `opendisplay_sensor_sht40_poll()` | `od_sensor_sht40_poll(opendisplay_get_global_config(), k_uptime_get_32())` |
| `opendisplay_ble.c:430` | `opendisplay_sensor_bq27220_poll()` | `od_sensor_bq27220_poll(opendisplay_get_global_config(), k_uptime_get_32())` |
| `opendisplay_ble.c:1025` | `opendisplay_sensor_bq27220_init()` | `od_sensor_bq27220_init(opendisplay_get_global_config())` |
| `opendisplay_ble.c:1027` | `opendisplay_sensor_sht40_init()` | `od_sensor_sht40_init(opendisplay_get_global_config())` |
| `opendisplay_battery.c:199` | `opendisplay_sensor_bq27220_is_configured()` | `od_sensor_bq27220_is_configured(opendisplay_get_global_config())` |
| `opendisplay_battery.c:200` | `opendisplay_sensor_bq27220_voltage_volts()` | `od_sensor_bq27220_voltage_volts()` — **takes no config argument** |

Swap the adapter-header includes in both callers for `od_sensor_sht40.h` / `od_sensor_bq27220.h`.
`shared/core` is already on the Nordic include path (`zephyr/CMakeLists.txt:99`,
`shared/sources.cmake:280`). Then delete the four files and their two `${SRC_DIR}` entries at
`zephyr/CMakeLists.txt:232-233`.

### 3.3 The cost, stated plainly

`opendisplay_battery.c:193-204` is a three-source fallback chain: nPM1300, then BQ27220, then
SAADC. **nPM1300 has no shared driver** — `opendisplay_sensor_npm1300.c` is a real 273-line target
driver — so after this change that function reads:

```c
if (opendisplay_sensor_npm1300_is_configured()) { ... }
if (od_sensor_bq27220_is_configured(cfg))       { ... }
```

Two naming conventions in six lines. Arguably honest — one source is shared, one is not — but a
readability cost ESP32's equivalent did not pay, and the strongest argument for the alternative:
rename the two files `*_adapter.c` and keep them. **Decide explicitly and record it.** § 3.1's
correction does not pre-decide this: that the old justification was wrong does not make removal
right.

---

## 4. Part C — deep sleep on Nordic: silent by inheritance, and one live hazard

An earlier draft of this plan asked "is Nordic's deep sleep a no-op, and does that strand the
buzzer?" **The question was already answered in the code, and the draft had not read it.**

`od_cmd_device.c:140-161` carries the design in a comment: the handler is *"RECOGNISED AND
SILENT, matching the reference nRF52840 build … the command is acted on but NO response is sent,
so clients do not treat deep sleep as supported on this target."* The nPM1300 hibernate in
`opendisplay_ble_schedule_deep_sleep()` (`opendisplay_ble.c:1109`) is an addition on top of that
silence, not the whole of an intended sleep.

The wire contract agrees: `shared/protocol/opendisplay_protocol.h:820` defines
`OD_ERR_DEEP_SLEEP_UNSUPPORTED 0x00 /* target has no deep sleep (nRF) */`.

**The buzzer is therefore ordinary dead code.** Deleting an uncalled wrapper cannot change
behaviour, so `opendisplay_buzzer_stop()` goes with the rest of part A. That is not the same as
saying the buzzer is fine: whether nPM1300 hibernate cuts its rail is still unverified, and a
future System OFF or hibernate implementation may well need a teardown again. The deletion is
safe; the hardware question stays open on the checklist.

**Silence is not the disclaimer the comment believes it is.** `od_cmd_device.c` reasons that
sending nothing stops clients treating deep sleep as supported. The wire contract makes silence
ambiguous — `opendisplay_protocol.h:457-464` allows both a successful sleep and an unsupported
target to be silent — and the host resolves that ambiguity the *opposite* way:
`../py-opendisplay/src/opendisplay/device.py:1301-1310` catches the disconnect/timeout and logs
*"device is sleeping"*. The obsolete `0x0052` it still sends limits the damage, but does not remove it: `device.py:1286-1296`
already treats a **write-time** `BLEConnectionError` as proof of sleep, so only the
successful-write-then-read path is covered by the opcode mismatch. **Updating py-opendisplay to
`0x0053` — which `od_cmd_device.c`'s own comment asks for — would make a Nordic board without an
nPM1300 stay awake while the host reports it asleep.** A live interop hazard behind a one-line
host change, and `Firmware` already noted the fix: the `OD_ERR_DEEP_SLEEP_UNSUPPORTED` NACK it
left unused "unless a caller needs the NACK". The canonical host path will need it the moment it
moves to `0x0053`.

**Two upstream references disagree about nRF deep sleep, and the unified target follows one of
them.** This is the part worth writing down:

- **`../Firmware`'s nRF arm is deliberately silent**, and says so:
  `src/device_control.cpp:931-933` — *"The protocol permits a NACK here … but we intentionally
  stay silent to preserve existing behavior — leave as-is unless a caller needs the NACK."* So
  the unified handler matches its stated reference. **The citation in `od_cmd_device.c:143` is stale,
  though — it points at `device_control.cpp:691-705`, which upstream is now LED-flash and
  button-ISR code; the deep-sleep arm has moved to `:895-937`.** Exactly the drift CLAUDE.md's
  "the authority is `../Firmware`, not the snapshot" rule warns about. That comment is in *this*
  repo, so fixing it is step 0's work, not an external follow-up.
- **`../Firmware_NRF`, the legacy nRF52 SDK repo, does the opposite:** `EPD/EPD_service.c:607`
  `handle_deep_sleep()` sends `{0x00, RESP_DEEP_SLEEP}`, waits 100 ms, then calls
  `enter_deep_sleep()` (`main.c:309`), which tears down BLE, display and session and calls
  `sd_power_system_off()` (`:359`, `:365`), falling back to `nrf_power_system_off()` (`:373`).
  (The `nrf_pwr_mgmt_shutdown(…GOTO_SYSOFF)` form belongs to the separate `sleep_mode_enter()` at
  `:300` — a different path, easy to conflate.)

**CLAUDE.md does not settle this, and the plan should not pretend it does.** Its authority rule
names `Firmware` over **`Firmware_NRF54`** (line 248) — a different sibling — and decision 8 calls
`Firmware_NRF` the host's *compat floor* in terms of features it lacks (no compression, no `0x76`,
no PIPE, no NFC), which says nothing about ranking its deep-sleep behaviour. Following the silent
arm is defensible by analogy, not by rule. That is exactly why step 0 has to make and record the
decision instead of inheriting one. Note also that the protocol header's
`/* target has no deep sleep (nRF) */` annotation is false of `Firmware_NRF`, which plainly has it.
Three questions worth a written answer:

1. Is following `Firmware`'s silent arm rather than `Firmware_NRF`'s System OFF the intended
   call? Decide it on the merits — CLAUDE.md's authority rule does not reach this pair — and
   record the reasoning.
2. If the nRF54's nPM1300 hibernate is a real (partial) deep sleep, how should that be represented
   on the wire, given the target currently answers with silence?
3. How is a host to tell "slept" from "did nothing"? Answering this before py-opendisplay moves to
   `0x0053` is the ordering that matters — an explicit `0xFF53` unsupported NACK on boards that
   cannot sleep would resolve it, but that is a wire decision, not a firmware tidy-up.

**Output is a decision recorded in `FOLLOWUPS.md` or `docs/DIVERGENCE_MATRIX.md`, not code.** If
question 1 resolves toward restoring System OFF, the resulting implementation may need
`opendisplay_display_power_off()` (§ 2.2) and a buzzer/LED teardown — so **run part C before part A
deletes them**, or accept re-adding them later. Recording a decision does not by itself create a
caller; the point is only to avoid deleting and re-adding the same code. That ordering is the only coupling between the parts.

---

## 5. Steps

| # | Content | Notes |
|---|---|---|
| 0 | Part C: answer the three questions, record the decisions. Then fix the two wrong comments this plan found **in this repo**: `od_cmd_device.c:143`'s citation of `device_control.cpp:691-705` (upstream's deep-sleep arm is now `:895-937`), and `zephyr/CMakeLists.txt:29`'s false "dead branch" claim (§ 2.7). | Gates whether step 2 may delete `opendisplay_display_power_off` and `opendisplay_buzzer_stop`. Comment-only code change; no sibling edit. |
| 1 | Delete the dead constants (§ 2.8); file the BG22 counterpart as a follow-up. Resolve `prj_cs.conf` (§ 2.7) here too. | Independent of everything. |
| 2 | Delete the fourteen minus `od_security_key_set` (step 3) — **thirteen if step 0 confirms the silent no-op stands, eleven if it decides to restore System OFF**, in which case `opendisplay_display_power_off` and `opendisplay_buzzer_stop` are kept for the teardown that implementation needs. (`_fini` is not among the fourteen; it is step 4.) Includes the § 2.5 GPIO cascade, the two `od_i2c_*` comment rewordings, and the § 2.6 decision on `s_connection_requested`. | Count is set by step 0. |
| 3 | `od_security_key_set`: delete the wrapper, delete the dead `fake_nordic.c` stub, restate `shared/core/od_config.h:194`. | Separate because it touches `shared/`. |
| 4 | `_fini`: delete, relink all three boards, keep the deletion only if the link survives. Otherwise restore it with the evidence in a comment. | The one item needing a build to decide. |
| 5 | Part B, per § 3.3's decision. **If remove:** inline the six call sites, swap the two includes, delete the four files, drop the `opendisplay_sensor_sht40.c` / `_bq27220.c` entries from `zephyr/CMakeLists.txt` (the nPM1300 entry at `:234` stays). **If keep:** rename both pairs to `opendisplay_sensor_*_adapter.{c,h}`, update the two includes and the two CMake entries, and leave the call sites alone. Correct the merged plan's § 6 item 1 **either way**. | Independent. |

Steps 1, 3, 4 and 5 are independent of each other. Step 2 depends on step 0 only for whether two
of its members are deleted or kept.

---

## 6. Gates

- `tools/check.sh --targets` — must end `0 failed, 0 skipped`; it exits 2 on any skip
  (`check.sh:1483`). Do not quote an expected pass count; read the summary.
- All three Nordic boards build (`check.sh --nordic` or the per-board scripts). Needs
  `nrfutil toolchain-manager launch --ncs-version v3.3.1 -- <cmd>`.
- **The `i2c: one engine per target` ratchet is not affected** (`check.sh:807-839`). It requires
  `targets/nordic-zephyr/src/opendisplay_i2c.c` to exist and greps for bit-level and master
  primitives (`i2c_start`, `i2c_write_byte`, …). `od_i2c_write`/`od_i2c_read` match neither the
  file list nor the primitive pattern. The file must not be emptied to the point of deletion.
- **Nordic's CMake source list is explicit** (`zephyr/CMakeLists.txt:215-238`), not a glob — the
  opposite of #87's `CONFIGURE_DEPENDS` trap. A forgotten entry fails at configure time on a fresh
  configure; on a warm tree it fails from ninja's missing-source rule instead. Either way it is
  loud, and no `--clean` is required.
- **A fresh git worktree cannot run the full gate.** `.gitignore:40` keeps 14 of the 17
  `targets/efr32bg22-slc/autogen/` files out of git (`slc generate` output), so BG22 fails to
  configure until they are copied from a working checkout. Learned on #87.
- **Hardware:** none for parts A and B. Part C produces a decision, not a build.

---

## 7. Definition of done

- The functions deleted in step 2 have no definition and no declaration **in Nordic production
  sources** — `tests/host/fake_*` stubs are handled by step 3 — and the § 2.5 cascade
  (`irq_set_enabled`, `od_gpio_irq_arg_fn`, the `fn_arg`/`arg` slot fields, the trampoline branch,
  the two `irq_attach()` parameters) is gone with them.
- The § 2.8 constants are gone; the BG22 `CONFIG_PKT_*` counterpart is filed as a follow-up.
- § 2.6's decision on `s_connection_requested` and its MSD read is recorded, whichever way it went.
- `prj_cs.conf` is deleted or deferred with a written reason, and both wrong comments are fixed:
  `od_cmd_device.c:143`'s stale citation and `zephyr/CMakeLists.txt:29`'s false "dead branch".
- `od_security_key_set` is deleted, its `fake_nordic.c` stub with it, the equally dead
  `opendisplay_display_power_off()` stub at `fake_nordic.c:125` too, and
  `shared/core/od_config.h:194` no longer justifies an export by naming a function that no longer
  exists.
- `_fini` has been decided by an actual relink, not by argument.
- Part B has an outcome recorded either way (§ 3.3): **remove**, or **keep and rename to
  `*_adapter.c`**. If removed: four files gone, the `opendisplay_sensor_sht40.c` and
  `opendisplay_sensor_bq27220.c` entries gone from `zephyr/CMakeLists.txt` — the nPM1300 driver
  at `:234` stays, it has no shared counterpart — six call sites naming `od_sensor_*`, includes
  swapped.
- `PLAN_ESP32_ADAPTER_REMOVAL_2026-09-03.md` § 6 item 1 no longer claims Nordic's callers lack the
  config accessor and clock — **required whether or not part B lands**.
- Part C has a written conclusion, even if that conclusion is "no change needed".
- `tools/check.sh --targets` clean with no skip; all three Nordic boards build.
- No `shared/` change except the one comment at `od_config.h:194`.
