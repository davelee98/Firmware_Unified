# Target: EFR32BG22 (Simplicity SDK + CMake)

Source repo: `Firmware_Silabs` (https://github.com/davelee98/Firmware_Silabs)
Surveyed at commit `d1a866d` (2026-07-25). **Not yet imported.**

The smallest target by a wide margin and the one that constrains `shared/` the most. Read
this before designing anything in `shared/core` or `shared/compress` — several of the
memory rules in ../../docs/ARCHITECTURE.md exist because of this chip.

## Hardware and budgets

| | |
|---|---|
| Part | `EFR32BG22C222F352GM40` — Cortex-M33, 352 KB flash, **32 KB RAM** |
| App flash region | `0x12000`–`0x56000` = **272 KB** (bootloader + Gecko apploader occupy the first 72 KB) |
| RTOS | **none** — bare-metal superloop (`app_bm.c` + `sl_power_manager`); no kernel component in the `.slcp` |
| Radio | Silabs BLE stack (`sl_bt_*` BGAPI), peripheral role only |
| Log | SEGGER RTT (`od_rtt.c`, J-Link required); no UART console |

**Current occupancy** (`arm-none-eabi-size` on the checked-in build,
`cmake_gcc/build/base/opendisplay-bg22.out`):

```
text 236316   data 492   bss 31792
```

- Flash: 236.8 KB of 272 KB → **~87 % full, ~35 KB headroom.**
- RAM: static (`.data` + `.bss` + 2752 B `.stack`) ends at `__HeapBase = 0x200056b0`
  → **~22 KB static, 10.3 KB heap** (`heap_size = 0x2950`), and the two together are the
  whole 32 KB. There is no slack.

Largest single RAM consumers in the application's own code: `s_cfg_rec` (2064 B, config
storage record), the `od_zlib_stream` state `s` (1676 B), `s_od_global_config` (844 B),
`s_session` (640 B), `s_buttons` (640 B), `s_nfc_write_chunk` (520 B),
`s_crypto_payload_buf` (513 B). Anything `shared/` adds competes directly with these.

## Layout of the source repo

Application sources are flat at the repo root; everything else is SDK, generated, or
vendored.

| File | Lines | Notes |
|---|---:|---|
| `opendisplay_ble.c` | 1977 | GATT service, advertising, connection handling, buttons, battery (IADC), NFC endpoint |
| `opendisplay_pipe.c` | 1299 | command dispatch over the PIPE characteristic + session auth (PSA AES-CCM / CMAC) |
| `opendisplay_display.cpp` | 894 | panel driving via `bb_epaper`, streamed inflate, QR — **the only C++ application file** |
| `opendisplay_config_parser.c` | 529 | config TLV parsing |
| `opendisplay_config_storage.c` | 139 | NVM3-backed config blob |
| `opendisplay_led.c` | 485 | LED patterns |
| `opendisplay_epd_map.c` | 75 | 66 panel-id → `bb_epaper` panel constants |
| `opendisplay_display_color.c` | 65 | palette handling |
| `main.c`, `app.c`, `app_bm.c` | 269 | SDK entry + bare-metal loop |
| `third_party/uzlib/src/od_zlib_stream.c` | 723 | bit-serial streaming inflate |
| `third_party/bb_epaper/` | — | vendored, with `silabs_efr32_io.inl` + `silabs_bbep_busy.inl` backends |
| `third_party/segger_rtt/` | — | RTT log transport |
| `qr/qrcode.c` | 529 | QR generation |

~20.9 k lines total including vendored third-party, of which the OpenDisplay-authored
application is roughly 5.5 k.

## Build

SLC-managed project (`opendisplay-bg22.slcp`) with a **generated** CMake file
(`cmake_gcc/opendisplay-bg22.cmake`, 386 lines) and a hand-written wrapper
(`cmake_gcc/CMakeLists.txt`, 148 lines) that adds the `OD_*` options.

```bash
./build-and-flash.sh --no-flash            # build only
./build-and-flash.sh --no-flash --no-ota-image --no-bootloader --no-artifacts
./build-and-flash.sh                       # build + J-Link flash + .gbl OTA image
```

Project options exposed by the wrapper: `OD_ENABLE_RTT`, `OD_GENERATE_GBL_OTA`,
`OD_APP_VERSION`, `OD_SL_APPLICATION_VERSION`, `OD_TNB132M_BOOT_PROBE`,
`OD_TNB132M_WRITE_NDEF`, `OD_TNB132M_NDEF_TEXT`.

**`slc generate` needs to be pointed at a real SDK.** It requires component metadata
(`.slcc`/`.slce`); the source-only `-cp` copy in the source repo at `simplicity_sdk_2025.12.2`
has zero `.slcc` files, so a bare `slc generate` fails there with "no valid sdk could be
loaded", and `cmake_gcc/opendisplay-bg22.cmake` is hand-maintained in that repo as a result.
The SDK it needs *is* installed on this box (below) — the failure is unregistered tooling, not
a missing download. **This target does not inherit that hazard** — see the pinned install below, which
restores `slc generate` and makes the CMake file genuinely generated again. Note the CMake build
itself needs neither `slc` nor the SDK metadata, so it keeps working either way.

Toolchain locations on this machine are recorded in ../../docs/TOOLCHAINS.md.

**Required: the SDK is a pinned install. The vendored tree is not imported.**
`simplicity_sdk_2025.12.2/` is 57 MB across 658 git-tracked files (~59 % of the 97 MB source
repo), it is a *generated* `slc generate --copy-sources` artifact, and it carries no `.slcc`
metadata so it cannot drive `slc generate`. Every clone of this repo would pay for it, including
people who only touch the ESP32 target. Install instead:

```bash
slt install simplicity-sdk        # pin 2025.12.2 — the version the tree used
slt where simplicity-sdk          # → SDK_PATH for the CMake build
```

The exact pinned version is available — `slt list simplicity-sdk --versions` (checked
2026-07-25) returns 2024.12.1, 2025.6.0, 2025.6.2, 2025.6.3, 2025.12.0, 2025.12.1, **2025.12.2**,
2025.12.3, 2026.6.0 — and `slt` is already the mechanism delivering ARM GCC, CMake, and
Commander, so this is
incremental to an install that was always mandatory, not a new prerequisite. The payoff beyond
size: a real SDK has the component metadata `slc generate` needs, so
`cmake_gcc/opendisplay-bg22.cmake` becomes genuinely generated again rather than hand-maintained
— which matters most during this migration, since adding `shared/` and `third_party/` sources
goes through the `.slcp`.

Rationale and the required install-verify-then-delete sequencing are in ../../docs/MIGRATION.md
§ "The Silabs SDK is not imported as-is". Do not delete the vendored tree in the source repo
until `slc generate` and a clean build are both verified against the installed SDK. If that
verification fails, escalate — falling back to vendoring reverses a recorded decision and needs
an explicit call.

## What this target contributes to `shared/`

- **`opendisplay_config_parser.c` has zero vendor includes today.** It is the only
  application file in any of the four repos that already satisfies the `shared/` rule
  as-is. At 529 lines it is also the smallest of the three config parsers
  (`Firmware` 919 C++, `Firmware_NRF54` 696 C) — the tightest starting point for
  `shared/core`, though it must be checked for missing features before it is treated as the
  donor rather than merely the cleanest.
- **`od_zlib_stream.c` is the reference streaming inflate** for a target with no room for a
  window buffer (see the divergence below).
- **The bare-metal constraint is the useful one.** This target has no threads, no
  scheduler, and no blocking `k_sleep`. Any `shared/core` API that assumes it can block, own
  a task, or be re-entered from another context excludes this target.

## Divergences to resolve deliberately

Per ../../docs/MIGRATION.md's "silent behavioural divergence" risk — these are the ones
already visible, before any code moves:

1. **Zlib window is 512 B, not 32 KB.** The generated CMake pins
   `OPENDISPLAY_ZLIB_WINDOW_BITS=9`. The workspace-level note that "existing targets pin
   32 KB windows for legacy-client compatibility" is **not** true of this target — a 32 KB
   window is a third of the chip's RAM and cannot exist here. Any stream this device must
   accept has to be encoded with a ≤512 B window. `shared/compress` must treat window size
   as a first-class per-target parameter with this as the documented floor, and the encoder
   side (`py-opendisplay`) has to know which devices are 9-bit.
2. **No PIPE sliding-window transfer.** Despite the file name, `opendisplay_pipe.c`
   implements the PIPE *characteristic* (command channel), not the `0x80`–`0x82`
   sliding-window image path in ARCHITECTURE.md's `shared/core` list. Opcodes handled here:
   `AUTHENTICATE`, `CONFIG_READ`/`CONFIG_WRITE`/`CONFIG_CHUNK`, `DIRECT_WRITE_START`/
   `_DATA`/`_END`, `PARTIAL_WRITE_START`, `HEADER_SIZE`, `FIRMWARE_VERSION`, `READ_MSD`,
   `LED_ACTIVATE`/`LED_STOP`, `BUZZER`, `NFC_ENDPOINT`, `DEEP_SLEEP`, `POWER_OFF`,
   `REBOOT`, `ENTER_DFU`. So `shared/core`'s transfer state machines must be selectable —
   a target that does not implement PIPE must not pay for its reorder queue in flash or
   RAM.
3. **`CMD_NFC_ENDPOINT` / TNB132M is Silabs-only.** It is in the protocol header, so it is
   shared surface, but this is the only implementation. Decide whether it lands in
   `shared/core` behind a capability flag or stays in `targets/efr32bg22-slc/`.
4. **`opendisplay_display.cpp` is C++ while everything around it is C**, purely because
   `bb_epaper` is a C++ class. Consistent with `Firmware_NRF54`, and fine — but it means the
   `od_hal_panel` boundary is where C++ has to stop.

## Protocol-header hazard (specific to this repo)

There are **two** in-tree copies and they do not agree:

- `Firmware_Silabs/opendisplay_protocol.h` (root) — the copy the sync tool maintains, and
  the one the compiler actually resolves (quoted include from a root-level source). One
  push behind canonical: missing the unreleased `OD_BLE_MAX_FRAME` addition.
- `Firmware_Silabs/include/opendisplay_protocol.h` — **stale at protocol 2.1** (canonical is
  2.2), missing all of SECTION 9. It is unmaintained by the sync tool, yet it is the path
  the `.slcp` declares for the header, and `../include` is on the include path. Nothing
  compiles it today only because `../.` precedes it and quoted includes hit the source's own
  directory first.

`opendisplay_structs.h` has the same duplication but both copies match canonical.

Do not carry the duplicate into `targets/efr32bg22-slc/`. On import, keep exactly one
reference — `shared/protocol/` — and delete the local copies. (Related open item: the sync
tool's copy map still does not list `Firmware_Unified/shared/protocol/` at all.)

## What must be installed to build this

See ../../docs/TOOLCHAINS.md § EFR32BG22. Short version: ARM GCC 12.2.Rel1, CMake, Ninja,
Simplicity Commander (for `.gbl` + flashing), and **the pinned Simplicity SDK 2025.12.2 via
`slt install simplicity-sdk`** — required, not optional, because this target does not vendor
the SDK. Regenerating the CMake additionally needs `slc-cli` and the bundled Java 21 (present
but **not on `PATH`**; prepend
`~/.silabs/slt/installs/archive/java21-v21.0.5/java21_linux_x86_64/jre/bin` before running
`slc`).

**On this dev box the SDK is already installed** (verified 2026-07-25): full Simplicity SDK
`2025.12.2` at `~/.silabs/slt/installs/conan/p/simpl965e19baece23/p` — 2892 `.slcc` files,
2.4 GB, resolvable via `slt where simplicity-sdk`. It is a real SDK with component metadata,
not a `-cp` copy, so the pinned-install requirement is already satisfied here.

What is **not** yet done is wiring `slc` to it: there is no `.slconf` in the project and
`~/.uc/cli/cli.config` registers no SDK, so `slc generate` must be given
`--sdk-package-path <the path above>` (or a project `.slconf`) to find it. That is a
configuration step, not an install — do not record it as a missing prerequisite.
