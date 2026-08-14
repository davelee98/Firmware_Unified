# Dead-code findings — esp32-idf — 2026-08-14

Read-only audit of `targets/esp32-idf/` (src, hal, panel, compat, ble, the FastEPD adapter) for
code that is defined but unreachable. Report against HEAD `0ad8884` (`Merge pull request #27
from davelee98/feat/watchdog-hal`).

One item from this audit — the whole `BBEP_T133A01`/E1004 dual-CS branch in
`display_service.cpp`/`boot_screen.cpp` — was fixed and merged same-day at `7c2c8dc`. Everything
below is still open. No source changes beyond that one fix were made as part of this audit.

## Method

For every candidate, grepped the whole repo (`targets/`, `shared/core`, `shared/hal`,
`tests/host/`) for callers, checked per-board `#ifdef`s in `targets/esp32-idf/boards/*.cmake`,
and checked for callback/vtable/ISR registration by pointer before calling something dead —
those don't show up as ordinary call sites. Excluded from "dead": anything called across the
`shared/`↔target boundary via link-time `extern "C"` (architectural decision 1), anything gated
by a per-board `#ifdef` some board actually sets, `od_panel_ops` vtable members, and anything
exercised only by `tests/host/`.

## Genuinely dead — safe to consider removing

| Location | What | Why it's dead |
|---|---|---|
| `hal/od_hal_panel.c` (whole file, 48-118) + `od_hal_panel.h:116-147` | 10 functions (`od_hal_panel_init`, `_begin`, `_write`, `_refresh_start`, `_refresh_busy`, `_sleep`, `_abort`, `_mark_deinitialized`, `_backend_name`) | Zero callers anywhere. **Not cruft** — `main/CMakeLists.txt:200-204` documents it as the staged landing point for a not-yet-done `od_panel_ops` vtable "repoint". Also makes the `od_panel_bbep.cpp`/`od_panel_fastepd.cpp` vtable backends transitively unreachable at runtime today, even though each individual `od_panel_ops` entry is legitimately referenced by struct literal |
| `src/od_inflate_tinfl.cpp:199` | `od_inflate_tinfl_output_count()` | No caller under any build config; sibling `_reset/_push/_poll/_error` are wired via the `display_service.cpp` macro remap, this one isn't. The uzlib-side parallel (`od_zlib_stream_output_count`) is itself only called from `tools/test_zlib_stream.c` |
| `src/qr/qrcode.c:314` | `qrcode_getDataCapacityBytes(uint8_t version)` | Dead in **all three targets'** vendored copies (esp32-idf, nordic-zephyr, efr32bg22-slc); `qrcode_initBytes` computes capacity inline instead of calling it |
| `src/session_guard.cpp:172-174` | `abortToKnownState(const char*, bool)` — the 2-arg overload (`session_guard.h:38`) | Every real call site (`communication.cpp:209`, `main.cpp:585/846/1260`, `display_service.cpp`) uses the 3-arg form with an explicit owner snapshot |
| `src/main.cpp:1438` `xiaoinit()`, `:1536` `powerDownExternalFlash()`, `:1533` `powerDownExternalFlashFromConfig()` (the `#else` no-op stub, not the real body at :1483) | Only called from `config_parser.cpp` inside `#ifdef TARGET_NRF`, which no `boards/*.cmake` fragment ever defines on this target. `powerDownExternalFlash()`'s only caller is `xiaoinit()` itself — dead-via-dead |
| `src/config_parser.cpp:97` | `formatConfigStorage()` | Zero callers anywhere in the repo |
| `src/config_parser.cpp:310` | `hasValidStoredConfig()` | Zero callers anywhere in the repo |
| `src/communication.cpp:445` | `calculateCRC16CCITT()` | Declared in both `communication.h` and `main.h`, defined once, never invoked |
| `src/ble_transport_esp32.cpp:465` | `BleTransport::liveInstanceCount()` | Every other `BleTransport` method has a caller; this one has none, on either target's implementation |
| `src/display_service.cpp:1235` | `readAXP2101Data()` (declared `display_service.h:44`) | Zero callers — siblings `initAXP2101`/`powerDownAXP2101` are both called from `main.cpp`, this one isn't |
| `src/display_service.cpp:2413` | `pipeWriteActive()` (declared `display_service.h`) | Zero callers. The design-intent comment above `transferActive()` explains what a caller would look like; none exists |
| `src/display_service.h:47` | `writeTextAndFill(const char* text)` | Declared, **never even defined** anywhere in the tree, never called — a stale header entry, harmless only because nothing takes its address |
| `src/display_service.cpp:1467,1492,1518` | `renderChar_4BPP`/`_2BPP`/`_1BPP` (static) | Zero call sites anywhere, including within the same file. Reference `FONT_BASE_WIDTH`/`_HEIGHT`/`FONT_SMALL_THRESHOLD` locally, shadowing identical macros already in `main.h` — reads as leftovers of a removed manual-text-rendering path |

Plus two trivially-unreachable defensive `return` lines (harmless, low-value to fix): a
`return false;` in `main.cpp`'s dead `powerDownExternalFlash()` (nested inside the finding
above), and `config_parser.cpp:94`'s `return false; // Should never reach here` at the end of
`initConfigStorage()` — `TARGET_ESP32` is always defined so the preceding branch always returns.

## Needs a maintainer call, not a blind delete

- **`factory_config.cpp:22,35`**: `factoryPacketValid()`/`factoryEmbedPresent()` are `static`,
  called only from `tryProvisionFactoryEmbed()`'s `#ifdef FACTORY_HAS_EMBED` block. That macro is
  only ever set by `scripts/factory_config_gen.py`, a **PlatformIO/SCons** pre-build script
  (`Import("env")`, `env.Append(CPPDEFINES=...)`) that isn't referenced anywhere in
  `targets/esp32-idf/CMakeLists.txt`, `main/CMakeLists.txt`, or `build.sh` — this repo has no
  PlatformIO build (decision 5). So the whole path, and these two functions, are unreachable in
  every currently-buildable configuration. Reads as a stranded migration artifact (an unwired
  build step) rather than a deliberate branch: either wire the script into CMake, or delete both
  it and this `#ifdef` arm.

## Compile-dead but deliberately kept — do not touch

Each of these is a whole-branch `#ifdef` that never compiles on esp32-idf today, and each is
self-documented as intentional Bluefruit/nRF parity scaffolding scheduled to leave at the
nRF52840 migration step (`compat/SHIM_BUDGET` governs the first two explicitly):

- `src/od_log.cpp`'s non-`TARGET_ESP32` branches (`millis()`/TinyUSB CDC paths)
- `src/buzzer_hw.cpp`'s `#if defined(ARDUINO_ARCH_NRF52)` arm
- `src/wake_button.cpp`'s `#else` (not-ESP32) stub

## Separate: potential simplifications (not dead code)

- `sensor_bq27220.cpp`: `initChargerGpio()` has exactly one caller, in the same file
  (`initBq27220Sensors()`) — could drop the header exposure and make it `static` unless an
  independent early-boot charger-only path is intended.
- `power_latch.cpp:238`: `powerLatchMosfetConfigured()` is a one-line pass-through of the
  internal `latchEnabled()` predicate with a single external caller — looks like a deliberate
  public-API boundary, not obviously worth inlining.
- `display_service.cpp`'s `getplane()` has a redundant branch: the `OD_COLOR_SCHEME_GRAY4` case
  and the final fallback both `return PLANE_1` — same value, two paths.
- `transferSessionOrigin()` (`display_service.h`) now has only one caller, and it's internal
  (`checkTransferTimeouts`, same file) — header exposure could shrink to file-static.
