# ESP32 name-adapter removal — delete the legacy-name layer in `targets/esp32-idf/src/`

**Date:** 2026-09-03

**Owns:** the four whole files and one in-file block under `targets/esp32-idf/src/` that exist
only to translate a legacy `Firmware`-era name into a `shared/` API, plus the call-site rewrites
that make them removable.

**Does not own:** any `od_*_app.{c,cpp}` seam file. Those look like adapters by line count but
`shared/core/` *declares* them and shared code *calls* them, so deleting one is a link error in
`shared/`, not a cleanup. § 2.2 lists them so a later sweep does not re-open the question.

---

## 1. Outcome

`targets/esp32-idf/src/` contains no file whose only content is a rename, and no function whose
only content is forwarding a legacy name to `od_*`. Every ESP32 call site names the shared API
directly, which is what Nordic already does for the session predicates.

Net: **5 files deleted** (52 lines), **12 function definitions** removed (6 sensor, 6 session),
**19 declarations** removed (6 in the two deleted sensor headers, 6 in `encryption.h`, 5 in
`main.h`, 2 local re-declarations in `communication.cpp`), **18 call sites** rewritten (6 sensor,
12 session), 2 dead includes removed. No wire-visible change, no intended behaviour change, no
`shared/` change.

---

## 2. Inventory

### 2.1 In scope — removable

| # | Item | Lines | Why removable |
|---|---|---|---|
| A | `src/driver.h` | 4 | **Empty.** Include guard and nothing else. Zero includers here; zero in `../Firmware/src/driver.h`, which is equally empty and equally unreferenced upstream. |
| B | `src/sensor_sht40.h` + `src/sensor_sht40_shim.cpp` | 19 | 2 legacy names over `od_sensor_sht40_*`. Nothing in `shared/` declares them. |
| C | `src/sensor_bq27220.h` + `src/sensor_bq27220_shim.cpp` | 29 | 4 legacy names over `od_sensor_bq27220_*`. Same. |
| D | The block `src/encryption.cpp:60-88`, self-labelled "THE COMPATIBILITY SHIMS" | 6 fns | 6 one-line forwards to `od_session_*`. Nordic calls `od_session_security_enabled()` directly (`od_cmd_config.c:381`, `opendisplay_config_parser.c:43`); BG22 reaches the same predicate through shared dispatch and never spells it in target source. Either way these wrapper names exist on ESP32 only. |

Two of D's six are **already dead**: `checkEncryptionSessionTimeout()` and
`updateEncryptionSessionActivity()` are declared in `encryption.h` *and* `main.h`, defined in
`encryption.cpp`, and called from nowhere in the tree. They are deleted, not inlined.

The rest of `encryption.cpp` — `getAuthDeviceIdBytes()`, `getChipIdHex()`, `secureEraseConfig()`,
`checkResetPin()` — is real target code (efuse MAC packing, NVS secure erase, reset-pin policy)
and stays. The file is not deleted.

### 2.2 Explicitly NOT in scope

- `od_rxq_app.cpp` (100% forwarding), `od_cmd_app.cpp` (72%), `od_session_app.cpp` (66%),
  `od_touch_app.cpp` (37%), `od_sensor_app.cpp`, `od_inflate_app.cpp` — all implement a seam
  declared in `shared/core/`. `od_cmd_app.cpp`'s one-function-per-opcode shape is deliberate:
  an incomplete opcode set must be a link error.
- `src/ble_transport_esp32.h` — a single `#include "od_ble.h"`. Its own header comment records
  why it survives: deleting it makes the next sync from `Firmware` diff as a deleted file plus
  an unexplained new include. Leave it.
- `src/protocol_pending.h` — debt with a written 3-step removal plan gated on the
  `shared/protocol/` editing freeze lifting. Not removable from here. One drive-by observation for
  whoever executes that plan, deliberately NOT fixed by this one: its step 3 names two include
  sites (`display_fastepd.cpp`, `display_service.cpp`) but there are four — also
  `panel/od_panel_fastepd.cpp:18` and `panel/od_panel_bbep.cpp:26`, and the latter appears not to
  use either pending constant.

### 2.3 Call sites to rewrite

**Sensors — all 6 in `src/display_service.cpp`:**

| Line | From | To |
|---|---|---|
| 948 | `initSht40Sensors();` | `od_sensor_sht40_init(&globalConfig);` |
| 949 | `initBq27220Sensors();` | `od_sensor_bq27220_init(&globalConfig);` |
| 1527 | `if (bq27220IsConfigured()) {` | `if (od_sensor_bq27220_is_configured(&globalConfig)) {` |
| 1528 | `float gaugeV = bq27220BatteryVoltageVolts();` | `float gaugeV = od_sensor_bq27220_voltage_volts();` |
| 1576 | `pollSht40SensorsForMsd();` | `od_sensor_sht40_poll(&globalConfig, od_hal_uptime_ms());` |
| 1577 | `pollBq27220ForMsd();` | `od_sensor_bq27220_poll(&globalConfig, od_hal_uptime_ms());` |

`display_service.cpp` already includes `od_hal_time.h` and already sees `globalConfig`.
Includes at `:24-25` are replaced by `od_sensor_sht40.h` / `od_sensor_bq27220.h`.

**Session predicates — 12 sites across 4 files.** Preferred spelling reaches the config through
the existing seam accessor `od_session_app_security()` rather than the `securityConfig`
reference, so the rewrite does not spread `encryption_state.h` into files that do not have it:

| File:line | From | To |
|---|---|---|
| `wifi_service.cpp:280` | `isEncryptionEnabled()` | `od_session_security_enabled(od_session_app_security())` |
| `wifi_service.cpp:283` | `isEncryptionEnabled()` | same |
| `wifi_service.cpp:420` | `!isEncryptionEnabled()` | same, negated |
| `wifi_service.cpp:798` | `isEncryptionEnabled()` | same |
| `wifi_service.cpp:834` | `isEncryptionEnabled()` | same |
| `wifi_service.cpp:478` | `deriveTlsPsk(tlsPsk)` | `od_session_derive_tls_psk(od_session_app_security(), tlsPsk)` |
| `wifi_service.cpp:1400` | `clearEncryptionSession()` | `od_session_clear(od_session_app_state())` |
| `communication.cpp:418` | `isEncryptionEnabled()` / `isAuthenticated()` | `od_session_security_enabled(od_session_app_security())` / `od_session_alive(od_session_app_state(), od_hal_uptime_ms(), NULL)` |
| `communication.cpp:191` | `clearEncryptionSession()` | `od_session_clear(od_session_app_state())` |
| `config_parser.cpp:299` | `clearEncryptionSession()` | `od_session_clear(od_session_app_state())` |
| `od_rxq_app.cpp:17` | `return isEncryptionEnabled();` | `return od_session_security_enabled(od_session_app_security());` |

Comment-only mentions at `wifi_service.cpp:245`, `wifi_service.cpp:1375`, `wifi_service.h:132`,
`communication.cpp:402-403` are reworded to the new names in the same commit.

**Stale declarations to delete:** `main.h:202-206` (five), `communication.cpp:38`
(`bool isAuthenticated();`), `communication.cpp:199` (`bool isEncryptionEnabled();`),
`encryption.h` decls for all six.

**Includes to add:** `wifi_service.cpp` and `config_parser.cpp` each need `od_session.h` +
`od_session_app.h`; `communication.cpp` already has both. `wifi_service.cpp` **keeps**
`encryption.h` — it still uses `getChipIdHex()`, `getAuthDeviceIdBytes()` and
`OD_CHIP_ID_HEX_LEN`. `config_parser.cpp` keeps it for `checkResetPin()`.

**Dead includes to remove:** `display_service.cpp`'s `#include "encryption.h"` (it uses no
symbol from that header today — verified independently of this change) and `od_rxq_app.cpp`'s,
replaced by `od_session.h` + `od_session_app.h`.

---

## 3. The one behavioural trap

`isAuthenticated()` is **mutating by design**: `od_session_alive()` clears an expired session as
a side effect of being asked, and `od_session.h:265-275` says every shipped call site relies on
it. The name `isAuthenticated()` hid that; `od_session_alive()` hides it slightly less but not
enough. The single rewritten site (`communication.cpp:418`) gets a one-line comment saying the
call can tear the session down, so a later reader does not "optimise" the short-circuit order in
`!enabled || alive`.

Do **not** substitute `od_session_authenticated()`, which is the pure non-mutating predicate and
a different function. That swap would leave expired sessions standing.

---

## 4. Steps

Each step is one commit, independently revertable, and builds clean on its own.

**Step 1 — delete `src/driver.h`.** No other edit. If anything fails to compile, the premise
that it is unreferenced was wrong and the step reverts on its own.

**Step 2 — inline the sensor calls.** Rewrite the 6 sites and the 2 includes in
`display_service.cpp`. Build. `src/sensor_*.h` and `src/sensor_*_shim.cpp` are now unreferenced
but still compiled.

**Step 3 — delete the four sensor files.** No build-file edit is needed, but a plain rebuild is
NOT sufficient. `main/CMakeLists.txt:18` globs `src/*.cpp` **without `CONFIGURE_DEPENDS`**, so the
glob is evaluated at configure time only: every existing
`targets/esp32-idf/build/<board>/build.ninja` still names `sensor_sht40_shim.cpp` and
`sensor_bq27220_shim.cpp` as inputs, and `build.sh` removes a board directory only under `--clean`
(`build.sh:163`). Deleting the sources without reconfiguring makes ninja fail on a missing input —
loud, but confusing, and it reads as though the deletion was wrong. **Build this step with
`targets/esp32-idf/build.sh --clean`** (or reconfigure each board directory). The same applies to
any later step that adds or removes a file under `src/`.

**Step 4 — delete the two dead session predicates.** (Removing functions from an existing file
needs no reconfigure; a plain build is enough from here on.) `checkEncryptionSessionTimeout()` and
`updateEncryptionSessionActivity()`: definitions in `encryption.cpp`, declarations in
`encryption.h` and `main.h:205-206`. Build. This step alone proves they were dead.

**Step 5 — inline the four live session predicates.** All 12 call sites, the comment rewordings,
the trap comment from § 3, then delete the definitions from `encryption.cpp`, the declarations
from `encryption.h`, `main.h:202-204` and `communication.cpp:38,199`, and fix the includes.
Build.

Steps 1, 2-3 and 4-5 are independent of each other and may land in any order or separately.

---

## 5. Gates

**Software (required, all steps):**

- `tools/check.sh` — plain invocation, then `--targets`. Read the summary: it exits 2 on a skip
  and a skip is not a pass. Nothing in `check.sh` names any file or symbol in § 2.1, so no
  ratchet needs updating; if one trips, that is a real finding, not an expected edit.
- `targets/esp32-idf/build.sh --clean` — every board fragment. `--clean` is not optional for any
  step that deletes a file (§ 4, step 3): the source glob has no `CONFIGURE_DEPENDS`, so a warm
  build directory keeps the deleted file in its ninja graph. Both `display_service.cpp` and
  `wifi_service.cpp` are fragment-sensitive, and `wifi_service.cpp` is behind
  `OPENDISPLAY_HAS_WIFI`: step 5 must be built with WiFi **on** and **off**, since 7 of its 12
  sites are in the WiFi-only file.
- Host tests are unaffected: `tests/host/sensor_sht40_test.c` and `sensor_bq27220_test.c`
  already drive `od_sensor_*` directly and never mention the legacy names.

**Hardware:** none required. There is no intended protocol, state-machine or timing-policy
change. (Deleting cross-translation-unit forwarders does move code size and link layout — that is
covered by the all-board build, not by a board.) Do not add
rows to `docs/HARDWARE_VERIFICATION_CHECKLIST.md` for it, and do not claim it re-verifies
anything already there.

---

## 6. Judgment calls, recorded

1. **The sensor shims diverge ESP32 from Nordic, and that is accepted here.** Nordic keeps
   functionally identical wrappers in `opendisplay_sensor_sht40.c` /
   `opendisplay_sensor_bq27220.c` — same shape, just not named `*shim`. Removing ESP32's makes
   the two targets differ in style. The alternative was renaming ESP32's files to
   `sensor_*_adapter.cpp` to match. Removal was chosen because ESP32's *sole caller already holds
   both dependencies the wrapper supplies* — `display_service.cpp` has `globalConfig` in scope and
   already includes `od_hal_time.h` — so the forward buys nothing there. That is the actual
   argument; it is not that ESP32's wrappers bridge only a name. They bridge the same config and
   clock Nordic's do (`sensor_sht40_shim.cpp:11-12`), which is why Nordic's stay: its callers do
   not have `opendisplay_get_global_config()` and `k_uptime_get_32()` equally to hand. Revisit if
   a second ESP32 caller appears.
2. **`od_session_app_security()` over `&securityConfig`.** Both compile. The accessor keeps
   `encryption_state.h` — which exports a C++ *reference* — out of files that do not already
   have it, and matches how the shared code reaches the same object.
3. **`isEncryptionEnabled()` is 7 of the 12 sites and the rewrite is longer at each.** That is
   the real cost of step 5 and the one place a reviewer might prefer to keep a wrapper. It is
   spent because the wrapper is ESP32-only: the same question already reads
   `od_session_security_enabled(...)` on the other two targets, and one spelling per fleet is
   worth more than 7 shorter lines on one target.

---

## 7. Definition of done

- `src/driver.h`, `src/sensor_sht40.h`, `src/sensor_sht40_shim.cpp`, `src/sensor_bq27220.h`,
  `src/sensor_bq27220_shim.cpp` do not exist.
- This returns nothing:

  ```
  git grep -nE 'initSht40Sensors|pollSht40SensorsForMsd|initBq27220Sensors|pollBq27220ForMsd|bq27220IsConfigured|bq27220BatteryVoltageVolts|isEncryptionEnabled|isAuthenticated|checkEncryptionSessionTimeout|clearEncryptionSession|updateEncryptionSessionActivity|deriveTlsPsk' -- targets
  ```

  **`git grep`, not `grep -rn`.** A recursive grep walks `targets/*/build/` and
  `targets/efr32bg22-slc/cmake_gcc/build/`, whose objects, `.map` files and IDF logs carry these
  symbol names as build residue; it will report hits long after the source is clean. (That residue
  is also what made this plan's first draft claim BG22 calls `od_session_security_enabled()` in
  source — the only hit was a `.map`.)
- `tools/check.sh --targets` clean with no skip; `targets/esp32-idf/build.sh --clean` clean for
  every board fragment, WiFi on and off.
- No file under `shared/` changed by this plan.
