# Architecture

## The one rule

`shared/` must compile for **every** target. It may use the C/C++ standard library and
`shared/hal` interfaces, and nothing else.

Forbidden in `shared/`: `esp_*.h`, `driver/*.h`, `soc/*.h`, `hal/*.h`, `freertos/*.h`,
`nrf_*.h`, `sl_*.h`, `zephyr/*.h`, `Arduino.h`, `bluefruit.h`, `NimBLEDevice.h`,
`bb_epaper.h`, `TFT_eSPI.h` — any vendor or framework header. If a file needs one, it belongs
in `targets/`.

`shared/` must also be **plain C**, not C++. Two targets are C-only, and the Zephyr sources
that are the best donor for `shared/core` are already C. In particular, Arduino's `String`
must not appear anywhere in `shared/` — see docs/TOOLCHAINS.md, where removing its 575 call
sites is the largest single line item in the ESP32 port.

Enforce it mechanically (CI grep is enough) rather than by review habit. The moment one
vendor include slips into `shared/`, that file stops being shareable and the repo quietly
reverts to four codebases in one directory.

## Layers

```
        ┌───────────────────────────────────────────┐
        │  targets/<t>/    chip drivers, build sys, │
        │                  HAL implementation       │
        └───────────────┬───────────────────────────┘
                        │ implements
        ┌───────────────▼───────────────────────────┐
        │  shared/hal/     abstract interfaces      │
        └───────────────▲───────────────────────────┘
                        │ calls
        ┌───────────────┴───────────────────────────┐
        │  shared/core/    dispatch, config, xfers  │
        │  shared/compress/ inflate engines         │
        │  shared/protocol/ wire contract (#defines)│
        └───────────────────────────────────────────┘
```

Dependencies point **inward only**. `shared/core` never calls into a target; a target calls
`shared/core` and supplies HAL implementations.

## What belongs in `shared/core`

Target-agnostic logic that today exists in near-duplicate across the four repos:

- **Command dispatch** — the `CMD_*` opcode switch, response framing, error codes.
- **Config-packet parsing** — TLV walk into the `opendisplay_structs.h` types, validation,
  CRC, the chunked `CONFIG_WRITE` assembler.
- **Transfer state machines** — direct-write (`0x70`/`0x71`/`0x72`), partial-region (`0x76`),
  and the PIPE sliding window (`0x80`-`0x82`) including the reorder queue. These are pure
  bookkeeping over byte streams; only the final "write these bytes to the panel" call is
  target-specific.
- **Session auth / AES-CCM** — nonce handling, replay window, session timeout. The AES
  primitive comes from the HAL (each SDK has its own accelerated implementation).
- **Advertisement payload assembly** — the manufacturer-specific data layout.

## What belongs in `shared/hal`

Keep the surface small and behavioural, not a thin wrapper over one vendor's API. Proposed
starting set:

| Interface | Purpose |
|---|---|
| `od_hal_time` | monotonic ms, delay, deadline helpers |
| `od_hal_gpio` | pin mode, read/write, interrupt attach |
| `od_hal_spi` | panel bus: begin, write bytes, speed |
| `od_hal_i2c` | sensors, PMIC |
| `od_hal_nvs` | config blob load/save/erase |
| `od_hal_radio` | transport-agnostic send/receive of framed payloads (BLE GATT, LAN TCP) |
| `od_hal_crypto` | AES-CCM, CMAC, RNG |
| `od_hal_log` | line-oriented log sink |
| `od_hal_panel` | row/plane writes + refresh, the one genuinely display-specific call |

Design each as a plain C struct of function pointers or a compile-time-selected set of
functions — not C++ virtual classes, since two targets are C-only.

Two of these already exist in embryo and should be promoted rather than designed from scratch:
`Firmware_NRF54/src/nrf54_zephyr_compat.h` is `od_hal_time` plus device-id
(`od_msleep`, `od_uptime_get_32`, `od_busy_wait`, `od_hwinfo_get_device_id` — already
`od_`-prefixed), and `nrf54_gpio.c` is `od_hal_gpio`. For `od_hal_panel`/`od_hal_spi`, the
working model is `bb_epaper`'s own `*_io.inl` backends, which already abstract exactly this
surface across Arduino, Zephyr, ESP-IDF, Silabs, Linux, and memory-only.

## Non-goals

- **One build system.** Three build systems remain three: ESP-IDF, west/Zephyr, and Simplicity
  SDK + SLC each own their target directory. This is *three*, chosen per silicon vendor — not
  one, and not one per repo of origin. See docs/TOOLCHAINS.md.

  All three are CMake, which is deliberate and load-bearing: it is what lets `shared/` be a
  single library — an `idf_component_register()` component, a `target_sources(app ...)` list,
  and an SLC-declared source set — from one source list. It is not an invitation to unify the
  three builds.
- **Lowest-common-denominator features.** Targets legitimately differ (PSRAM, ROM inflate,
  panel families). Capability differences are expressed through config and `#if`, not by
  removing features.
- **Rewriting working drivers.** Import them as-is into `targets/`; only the shared logic is
  refactored.

## Memory-sensitivity note

Targets differ enormously in RAM (nRF52840 256 KB, EFR32BG22 32 KB, ESP32-S3 512 KB +
PSRAM). `shared/` code must therefore avoid:

- large static buffers sized for the biggest target,
- unbounded heap allocation, and
- assuming a heap exists at all.

Buffer sizes belong behind compile-time constants the target sets, with a documented floor.
The ESP32-S3 work that motivated this repo (window-sized inflate dictionaries, pre-reserved
TLS record buffers, de-triple-buffered config) is exactly the class of tuning that must stay
target-parameterised rather than hardcoded in `shared/`.

**`shared/` takes these as plain preprocessor constants — do not design its configuration
surface around Kconfig.** ESP-IDF and Zephyr both have Kconfig, and on those two targets an
option should get a type, default, range, and help text rather than a raw `-D`. But the Silabs
target has no Kconfig at all (SLC + `target_compile_definitions`), so Kconfig is merely *how
two of the three targets set* these constants, never the interface `shared/` presents.

The knobs that exist today: `OPENDISPLAY_ZLIB_WINDOW_BITS`, `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW`,
and `PIPE_SMALL_DRAM_WINDOW` (the classic ESP32's 320 KB of RAM cannot link the full 33-slot
PIPE_WRITE reorder queue).

The zlib window is the worked example of why this must be a parameter and not a constant: the
EFR32BG22 pins a **512-byte** window (`OPENDISPLAY_ZLIB_WINDOW_BITS=9`) because 32 KB is a third
of the whole chip's RAM, while other targets pin 32 KB for legacy-client compatibility. 512 B is
the documented floor, and the encoder side must know which devices are 9-bit.
