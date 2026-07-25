# Migration plan

Incremental, one target at a time, keeping the original repos buildable throughout. Nothing
is deleted from the source repos until the unified target has been flashed and verified on
hardware.

## Order and rationale

1. **`esp32-idf`** (from `Firmware`) — first, because it is the most actively developed and its
   protocol implementation is the most complete, so it becomes the reference for what
   `shared/core` must expose. It is also the largest piece of work by a wide margin, because it
   changes framework: PlatformIO + Arduino → ESP-IDF. See "The ESP32 import is different" below.
2. **`nordic-zephyr`, nRF54L15 boards** (from `Firmware_NRF54`) — second: Zephyr's build system
   is the most opinionated, so it stresses the `shared/` boundary hardest. If `shared/` survives
   being consumed by west/CMake with no vendor includes, the boundary is real.
3. **`efr32bg22-slc`** (from `Firmware_Silabs`) — third, and deliberately **not** last. Its
   32 KB of RAM, absence of any kernel, and absence of Kconfig are what keep `shared/` honest:
   any `shared/core` API that assumes a scheduler, a heap to lean on, or a 32 KB window buffer is
   excluded by this target before it can ship. Leaving it until the end means discovering those
   assumptions after three targets have baked them in. Its toolchain does not change; note that
   `slc generate` needs a full Simplicity SDK, which is not available on every dev box. Its
   import has one mandatory deviation from step 1 — see "The Silabs SDK is not imported as-is"
   below.
4. **`nordic-zephyr`, nRF52840 board** (from `Firmware`) — fourth, and a **port rather than an
   import**: it currently builds with PlatformIO + Arduino/Bluefruit and arrives as a third
   board on the target step 2 creates. It therefore cannot start until step 2 lands. Settle
   deployed-unit OTA/flash-layout compatibility before starting. It is last because it is the
   only step that is pure toolchain migration with no new capability — if priorities shift, this
   is the one to defer.
5. **`nrf52-sdk`** (from `Firmware_NRF`) — **not planned.** Legacy bare-C Nordic SDK; treated as
   end-of-life. Confirm it is no longer shipped, then delete the directory.

Note what steps 1 and 4 have in common: the `Firmware` repo is the source for **two** different
targets in this plan, because its ESP32 and nRF52840 halves go to different toolchains. It is
not retired until step 4 completes.

## Per-target procedure

1. Import the target's sources **unchanged** into `targets/<t>/` in one commit, so the diff
   afterwards is reviewable and blame stays meaningful.
2. Build it from the new location without touching logic. Fix only include paths and build
   files. Flash and verify on hardware. Commit.
3. Identify duplicated logic against `shared/` (or, for the first target, promote its logic
   *into* `shared/` behind HAL interfaces).
4. Replace the target's copy with the shared implementation **one subsystem at a time** —
   config parsing, then dispatch, then each transfer path. Build + flash + verify after each.
5. Only then delete the corresponding code from the original repo, and point its README at
   this one.

Do not batch step 4. Each subsystem swap is independently revertable; a combined swap is not.

### The ESP32 import is different

Steps 1–2 above assume the framework stays constant. It does not for `esp32-idf`: removing
Arduino is not an "include paths and build files" fix, so an unchanged import cannot build.
Substitute three phases for steps 1–2, keeping steps 3–5 as written:

- **Phase A — import unchanged, non-building.** Bring `Firmware`'s `src/`, `lib/`, `tools/`,
  `scripts/` across in one commit that is *expected* not to compile. The point is provenance and
  reviewable blame; record the source repo and commit SHA in the message.
- **Phase B — shim to a booting build.** Add the IDF CMake/sdkconfig skeleton plus
  `targets/esp32-idf/compat/arduino_compat.h`, a deliberately temporary shim mapping the Arduino
  surface onto IDF (`String` → a minimal `std::string` wrapper; `pinMode`, `digitalWrite`,
  `delay`, `millis`, `delayMicroseconds` → their IDF equivalents — the full census is in
  TOOLCHAINS.md). The goal is to **link and boot on hardware early**, so every later step is
  bisectable against a known-good baseline. Two pieces cannot be shimmed and must be written
  here: the NimBLE C-API port of `ble_init.cpp` + `esp32_ble_callbacks.h` (~450 lines), and the
  IDF backends for `bb_epaper` (already upstream) and Seeed_GFX/TFT_eSPI.
- **Phase C — demolish the shim.** Delete `arduino_compat.h` usage subsystem by subsystem as
  steps 3–4 pull logic into `shared/`, then delete the file.

**The shim is the biggest risk in this plan.** Temporary compatibility layers become permanent
ones. Give it a mechanical ratchet rather than good intentions: a CI check that the number of
files including `arduino_compat.h` only ever *decreases*, and remove the check together with
the file when it reaches zero. The shim is scaffolding with a scheduled demolition, not a
portability layer — if it is still present when the last subsystem lands, the port is not done.

### The Silabs SDK is not imported as-is

`Firmware_Silabs` vendors `simplicity_sdk_2025.12.2/` — **57 MB across 658 git-tracked files,
~59 % of the 97 MB repo.** It is the source-only `-cp` copy (`slc generate --copy-sources`
output) that feeds the CMake build; it carries no `.slcc`/`.slce` component metadata, so it does
**not** enable `slc generate` and does not solve the hand-maintained-CMake hazard. Whatever the
import does, it must not silently carry 57 MB into the repo where every clone — including people
who only touch the ESP32 target — pays for it and every future SDK bump adds another permanent
copy to git history. Step 1's "import unchanged" is right about application sources and wrong
about a vendored SDK; this target gets one deviation, and the decision must be made *before the
first commit*, because a binary blob committed once is carried forever.

**Required: the SDK is installed at a pinned version, not vendored.** This is a decision, not a
preference — the vendored tree does not come across at import. It makes the rule uniform: *no
target vendors its SDK; every target requires one installed at a pinned version*, already true
of `esp32-idf` (IDF) and `nordic-zephyr` (NCS). The reasons:

1. **"Builds with nothing installed" is not a property this target actually has.** The build
   already needs `slt`-delivered ARM GCC 12.2, CMake, Ninja, and Simplicity Commander. The
   install is already mandatory; vendoring only makes *one component of an already-required
   install* checked-in instead of fetched.
2. **The exact version is installable — verified.** `slt list simplicity-sdk --versions`
   (checked 2026-07-25) returns `2024.12.1, 2025.6.0, 2025.6.2, 2025.6.3, 2025.12.0, 2025.12.1,
   2025.12.2, 2025.12.3, 2026.6.0` — including **2025.12.2**, the version in the tree today. A
   pinned `slt install` therefore reproduces the current build inputs exactly, the same way the
   other two targets pin IDF and NCS. This is the premise the requirement rests on; it has been
   confirmed rather than assumed.
3. **It fixes the regen hazard.** A real installed SDK has the `.slcc`/`.slce` metadata
   `slc generate` needs, so `cmake_gcc/opendisplay-bg22.cmake` becomes genuinely generated
   again rather than hand-maintained. That matters most *during* this migration, because adding
   `shared/`/`third_party/` sources goes through the `.slcp` — regen goes from ~never to routine.

Mechanically small: the generated CMake already reaches the SDK via `${COPIED_SDK_PATH}` /
`SDK_PATH`, so it is a variable repoint. Sequence it so the in-tree copy stays the safety net
until the replacement is proven: (1) install the pinned SDK and register it with `slc`;
(2) verify `slc generate` reproduces the CMake; (3) verify a clean `./build-and-flash.sh
--no-flash` against it; (4) only then import without the vendored tree, recording the pinned
version and install command in `targets/efr32bg22-slc/README.md`.

**Step 1 is already half-done on the primary dev box** (verified 2026-07-25): full Simplicity
SDK `2025.12.2` — the exact pinned version — is installed at
`~/.silabs/slt/installs/conan/p/simpl965e19baece23/p` with 2892 `.slcc` files. What remains is
registration: no `.slconf` exists in the project and `~/.uc/cli/cli.config` lists no SDK, so
`slc generate` must be handed `--sdk-package-path`. Treat step 1 as a configuration task, not a
download.

If step 2 or 3 fails, that is a **blocker to escalate, not a licence to fall back to
vendoring.** Reverting to a vendored SDK reverses a decision recorded here and needs an explicit
call; do not make it silently because an install was awkward on one box.

> **Contingency only — if the requirement is ever formally reversed.** Should vendoring come
> back, do not re-import the tree as-is: drop what the GCC build provably never links. The
> toolchain is pinned to ARM GCC 12.2 (hardcoded fallback in `cmake_gcc/toolchain.cmake`), so
> IAR/LLVM build trees and non-GCC RAIL libraries are dead weight — measured 2026-07-25 at
> **17.6 MB** (`*/build/iar/` + `*/build/llvm/`) plus **3.8 MB** (non-GCC RAIL) = **21.4 MB
> removable, ~36 MB remaining**. `grep -ciE '/iar/|/llvm/' cmake_gcc/opendisplay-bg22.cmake`
> returns `0`, and all 22 linked archives resolve under `build/gcc/cortex-m33/` or
> `build/gcc/xg22/`.
>
> Note that "the tree is already a `-cp` subset dominated by genuinely-linked archives" and
> "21.4 MB of it is removable" are both true and not in conflict: the large subtrees
> (`bluetooth_le_host` ~20 MB, `bluetooth_le_controller` ~15 MB, `rail_library` ~7.5 MB) each
> ship GCC, IAR **and** LLVM builds of the same libraries, and only the GCC ones are linked. An
> earlier note treated these as contradictory; they are not.

## Verification bar per subsystem

A subsystem is "migrated" only when, on real hardware for that target:

- a full image push renders correctly (compressed and uncompressed),
- a config read/write round-trips,
- the encrypted/authenticated path works if the target supports it, and
- an interrupted transfer recovers (disconnect mid-stream, then retry).

## Risks to watch

- **Concurrent development on `Firmware` during the ESP32 port.** This is the risk most likely
  to derail the migration, and it is a scheduling problem rather than a technical one. The IDF
  port is a multi-week change to the most actively developed repo; feature work landing
  alongside it produces brutal rebase conflicts. Either pause feature work on `Firmware` for the
  duration or accept that the port gets re-done against a moving target. Decide up front.
- **Silent behavioural divergence.** The repos do not implement identical semantics today. When
  promoting logic to `shared/`, differences must be resolved deliberately and written down — not
  settled by whichever repo was copied first. Note the two donors pull in different directions
  and both are right about something: `Firmware` has the most complete **feature coverage**,
  while `Firmware_NRF54` has the better **structure and language** for `shared/` (it is already
  C, which `shared/` must be). Take coverage from the first and shape from the second.
- **The Arduino shim outliving its purpose.** See "The ESP32 import is different" above.
- **Protocol header drift.** Until the sync tool's copy map includes
  `Firmware_Unified/shared/protocol/`, the headers here are unpoliced. Fix that early. Two
  further facts, verified with `sync_protocol_header.py --check` / `git show` (see
  DIVERGENCE_MATRIX.md §8): canonical is 2.2 + an unreleased `OD_BLE_MAX_FRAME`, but **the true
  ESP32 import source, `upstream/main` of `Firmware`, is still at protocol 2.1** — missing all
  of SECTION 9 (LAN) — so the first import lands a stale header that must be re-synced from
  canonical, not trusted as-is. And `upstream/main` PR #120 made a comment-only Seeed_GFX→FastEPD
  edit to the *vendored* `opendisplay_structs.h` that canonical does not yet carry; apply that
  wording to the canonical structs header *before* any `--push`, or a push will revert it.
- **Scaffold docs disagree on the nRF52840 step number (pending fix).**
  `targets/nordic-zephyr/README.md` calls the nRF52840 port "step 3 of the migration order,"
  but the order in this file makes it **step 4** (Silabs is step 3). The order here is
  authoritative — Silabs must bite third to keep `shared/` honest. Correct the target README
  to say step 4 when it is next touched (it is outside the docs-only scope of the analysis that
  found this).
- **Memory regressions on small targets.** Shared code sized for the ESP32 will not fit the
  EFR32BG22's 32 KB. Keep buffer sizes target-parameterised as plain preprocessor constants —
  *not* as Kconfig options, since the Silabs target has no Kconfig. See ARCHITECTURE.md.
- **Designing `shared/` against the wrong target's assumptions.** The EFR32BG22 has no kernel,
  no threads, no blocking sleep, and 32 KB of RAM. An API shaped by the ESP32's FreeRTOS and
  PSRAM will not retrofit onto it. This is why Silabs is step 3, not step 4.
- **No dev box builds every target.** Neither `idf.py` nor `west` is installed on the primary
  machine today, and the Silabs target additionally needs a full Simplicity SDK for
  `slc generate`. Stand up a CI build matrix early — the `shared/` boundary grep is necessary
  and nowhere near sufficient.
- **Vendored binaries bloating history permanently.** The Silabs SDK is the large case (see "The
  Silabs SDK is not imported as-is"), but the rule is general: a binary committed once is
  carried forever, and every target here vendors *something*. Decide what a blob's pruned form
  is before its first commit, not after it has been bumped twice.
- **Losing history.** Prefer imports that preserve provenance where practical; if a plain
  copy is used, record the source repo and commit SHA in the import commit message.
