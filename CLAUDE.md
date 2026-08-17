# CLAUDE.md

Firmware_Unified: one repo for all OpenDisplay firmware targets, replacing four repos
(`Firmware`, `Firmware_NRF54`, `Firmware_Silabs`, `Firmware_NRF`) that each reimplemented the
same wire protocol, config parsing, transfer state machines, compression and session encryption.
`../CLAUDE.md` covers the wider workspace.

**ALL SIX SOURCE REPOS ARE CHECKED OUT AS SIBLINGS AND ARE READABLE — USE THEM.** `../Firmware`,
`../Firmware_NRF54`, `../Firmware_Silabs`, `../Firmware_NRF`, plus `../opendisplay-protocol` (the
canonical wire contract) and `../py-opendisplay` (the host, and the answer to "does the client
actually do X?"). `targets/*/` here are import *snapshots* that drift; the siblings are live. Diff
before you port — see § "Migration constraints".

## Reading budget

Code first. Headers, build files and tests are ground truth; `docs/` explains *why* and is never a
prerequisite. This file already distils it — don't open a doc to re-confirm a rule stated here, or
to answer what a grep answers. Dated files (`*_REVIEW_<date>`, `FINDINGS*`, NEXT_STEPS.md) are
archive. One doc per task, at most; past that, write the code and flag the uncertainty.

## Comments

Say what the code does and why it is the way it is. Do not narrate history — no "this used to",
no "before the promotion", no recounting the bug that prompted the change. Git and the commit
message hold that. A comment longer than the code it explains is almost always history in
disguise. Keep the non-obvious constraint (a wire contract, a RAM ceiling, an ordering that
looks arbitrary and is not); delete the story.

## Status

- **Two HARDWARE-VERIFIED targets.** `targets/esp32-idf/` — 10 boards, run on an ESP32-S3. And
  `targets/nordic-zephyr/` **as of 2026-08-14, extended 2026-08-15, on the `xiao_nrf52840` board
  only**: image upload, config write + reload and host-side MSD decode, then MIGRATION.md's full
  Gate 2 including the encrypted/authenticated path, all exercised on a flashed device. Its other
  two boards (`xiao_nrf54l15`, `xiao_nrf54lm20a`) still build clean but have NOT been flashed, so
  the target is verified, the L15 is not. `efr32bg22-slc` builds headless
  (`./build-and-flash.sh --no-flash`) and has never been flashed.
- **THERE IS NO CI. `tools/check.sh` (repo root) is every gate this repo has, and nothing runs
  it but you.** Boundary greps, the C11 ownership ratchets, the host suite under gcc + clang, the
  same suite under ASan/UBSan, the pre-auth fuzz targets, the py-opendisplay wire corpus, and the
  shim ratchet;
  `--targets` adds both target families (ESP32 boards + sdkconfig baseline, all three Nordic
  boards) and is required before merge. **A SKIP IS NOT A PASS** — missing
  clang or ESP-IDF skips rather than fails, so read the summary, which reprints skips and exits
  2 when there were any.
- Paths in this bullet are relative to `targets/esp32-idf/`. `./build.sh` there builds every board
  fragment (it sources ESP-IDF itself; never on `PATH`). `tools/run_host_tests.sh` runs host tests
  — or drive them directly, `cmake -S tests/host -B <dir> && cmake --build <dir> && ctest
  --test-dir <dir>`, which is the repo-root path and needs no ESP-IDF. `compat/ratchet.sh` and
  `tools/sdkconfig_baseline.sh` are gates a change must not break.
- **`shared/` is no longer empty** —
  `core/od_{adv_control,advert,cmd,config,config_asm,config_read,config_tlv,core,dispatch,gate,reply,rxq,session,txq,watchdog}.c`
  listed in `shared/sources.cmake` (never globbed) in per-HAL tiers, plus the two all-inline
  headers `od_span.h` and `od_nonce_window.h` and the two pure seam headers `od_cmd_app.h` and
  `od_session_app.h`, which correctly have no entry there. Consumers:
  host tests and `esp32-idf` take the aggregate; `nordic-zephyr` takes PURE + HAL_CRYPTO +
  HAL_RADIO + HAL_WDT + APP_SESSION + APP_RXQ; `efr32bg22-slc` takes PURE only — called on Nordic,
  still compiled-only on Silabs except `od_advert`.
  Two tiers are named for a **seam** rather than a HAL, because what they need is a target
  function rather than a driver: APP_SESSION (`od_session_app.h`) and APP_RXQ
  (`od_rxq_app_report`).
- **THE WHOLE COMMAND PATH IS SHARED ON BOTH TARGETS, AND NONE OF IT HAS RUN ON SILICON**
  (C8 2026-08-15, C10 2026-08-15, C11 2026-08-16). `od_txq` is egress (capacity a counter,
  ownership a generation-tagged token), `od_reply` chooses seal-or-plain **at the call site**
  instead of inferring it from response bytes, `od_gate` maps every `od_session` result to a wire
  action, `od_dispatch` owns the ordering AND the opcode map, `od_config_read` makes CONFIG_READ a
  resumable producer, and `od_frame_policy()` is the outcome table as data.
  **The ordering IS the design** (`od_dispatch.h`): reservation precedes the *gate*, which answers
  `[00][cmd][FE]` and needs a slot of its own, and precedes *decrypt*, because decrypt advances the
  replay window — so a frame deferred after decrypting is a replay when re-offered. That is why
  `OD_FRAME_DEFERRED` is returnable only before decrypt.
- **C11 (2026-08-16) retired the dispatch scaffolding. NOT HARDWARE-VERIFIED, on either target.**
  - **The opcode map is `od_dispatch.c`'s, once.** Targets supply named per-command hooks
    (`shared/core/od_cmd_app.h`); `od_cmd_dispatch()` is gone from both. Every target defines
    every hook, so adding an opcode without every target stating its answer is a **link error** —
    and that check immediately found a live C8 defect: **ESP32 had answered nothing to
    `CMD_FIRMWARE_VERSION` since the cutover**, because the pre-gate arm moved into shared dispatch
    and no target case was left behind it. That is the one command a client must be able to issue
    before it can authenticate.
  - **One deliberate wire change:** Nordic `0x0052` now answers
    `{FF,52,OD_ERR_POWER_OFF_UNSUPPORTED,00}` instead of falling silent. It has no power latch, and
    silence left a host unable to tell that from firmware older than the command.
  - **Nordic PIPE commands return truthful verdicts.** The three entry points were `void`, so the
    caller had no choice but an unconditional `OD_CMD_OK` — a frame answered with a hard NACK was
    reported as accepted, and the verdict is what decides whether a frame stamps the session's
    activity clock. Silence is refusal too: DATA for a transfer that is not open draws nothing
    (a `0x81` NACK is fatal to a client's upload loop) but accepts nothing either.
  - **ESP32's frame context is an argument.** `g_commandOrigin`, `g_commandInstance`,
    `commandOrigin()` and `imageDataWritten()` are gone; both ingresses build an `od_reply_t` and
    call `od_dispatch_app_frame()`. `enum CommandOrigin` retired in favour of `od_origin_t`.
  - **`opendisplay_pipe.c` is transport and pump only**, 1194 → 200 lines. Commands moved with
    their state to `od_cmd_{device,config,direct,nfc}.c`, each exporting the one reset disconnect
    cleanup calls.
  - **Both session objects are private to their `od_session_app` translation unit**, and
    `od_core_reset()` is the shared half of a teardown (producer, egress, session — in that order).
    RX is deliberately not in it: its producer differs per target.
  - **Three defects fixed**: Nordic's prepared-key slot no longer latches on a failed
    `psa_destroy_key` (it dropped ownership only on success, so authentication was unavailable
    until reboot); ESP32's RNG is `psa_generate_random()` and can report failure, where
    `esp_fill_random()` returns `void`; a successful `od_session_seal()` stamps activity, and
    nothing else does. A fourth surfaced during the work: a failing `hwinfo_get_device_id()` folded
    an uninitialised stack buffer, so the wire-visible device identity could differ between boots.
  - Ratcheted by symbol in [tools/check.sh](tools/check.sh): no second opcode map, no implicit
    frame context, no exported session singleton, no byte-inferred sealing.
- **`od_rxq` is the inbound ring on BOTH targets** (C9, 2026-08-15), replacing ESP32's
  `command_queue.cpp` and Nordic's 40 × 509 B `k_msgq`. SPSC, peek/consume rather than a copying
  pop, and every slot carries its writer's identity so stale frames self-discard at dispatch.
  **Nordic's BLE value admission narrowed from 509 to 253 (ATT MTU 256)** and now refuses over-length writes at ATT
  with 0x0D as ESP32 does, rather than dropping them silently at the queue — which also keeps the
  245..253 dispatcher NACK reachable on both. RX storage stays 256 bytes wide. Measured on
  `xiao_nrf54l15`: RAM 176712 → 154700 B
  (**22.0 KB reclaimed**, more than the plan's ~12 KB estimate because narrowing the MTU shrinks
  the ACL buffers too). Both queue depths are now derived from the target's own `PIPE_MAX_W + 2`
  and asserted where both constants are visible. NOT hardware-verified on either target.
  **`od_session` is CALLED ON BOTH `esp32-idf` AND `nordic-zephyr`, AND HARDWARE-VERIFIED ON
  `nordic-zephyr`/`xiao_nrf52840` ONLY** (C5 2026-08-15, C6 2026-08-15; Gate 2 passed on the
  nRF52840 2026-08-15). It owns the 0x0050 handshake, the KDF, the replay window and the CCM
  envelope in both directions; each target keeps only its clock, its device identity and its
  logging. `esp32-idf/src/encryption.cpp` went 863 → 285 lines (the swap, plus
  the CC310 arm and the crypto forwarders it orphaned) and `nordic-zephyr/src/opendisplay_pipe.c`
  1320 → 1082; both `struct EncryptionSession`s are gone, and the session object went 632→112 B
  and 640→112 B respectively.
  **NATIVE PSA CCM IS PROVEN ON SILICON** (nRF52840, 2026-08-15) — `od_hal_crypto` (C1) replaced
  Nordic's hand-rolled RFC 3610 with `psa_aead_*`, and the shortened-tag key policy
  (`PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 12)`) accepts and authenticates real traffic.
  That was the single likeliest first-flash failure and it is retired: plain `PSA_ALG_CCM` pins a
  16-byte tag and would have failed every operation with `NOT_PERMITTED`.
  **`esp32-idf` IS STILL UNVERIFIED** — C5's swap, C1's mbedTLS arm and everything from C8 to C11
  have never run on a board, so the authority target is the one trailing. Nordic carries C9-C11
  unflashed too. Two things there that only hardware shows: the
  `diff == 0` replay fix and the exact inner-length check are the two behaviour changes
  (`DIVERGENCE_MATRIX` § 6.5-6.9) that can refuse a frame the old code accepted.
  **The `OD-S1` PIPE silence fix is UNPROVEN on either target** — though the stimulus for it now
  exists: `targets/esp32-idf/tools/od-device-cli.py dispatch-gate` (C12.2) seals one `0x0081` frame
  and writes the retained bytes twice, requires a fresh device-side `nonce_reason=3` report, and
  uses a corrupted-tag control so that silence is falsifiable. Its helpers are host-tested; it has
  never run on a board. The nRF52840 pass completed an
  encrypted upload but did not deliberately induce loss or reordering, so the path that stays
  quiet on a nonce-rejected `0x81` frame — instead of sending the NACK that kills the upload —
  has never been exercised. It is not host-testable; forcing reorder on a board is the only way.
  Nordic additionally caps NFC read tag data at 218 B (was 238) so a sealed response still fits a
  BLE frame; that cap is also unexercised.
  docs/OD_SESSION.md is the subsystem reference — wire shapes, the four derivations, the design
  decisions, and the verification state in one place.
  The one flaw the promotion could NOT fix is filed: `FOLLOWUPS.md` § 5, **bidirectional nonce
  reuse** — both directions share one `session_id` and both counters start at 0, so the same
  CCM nonce is used under one key each way. It needs a protocol revision, not a firmware change.
  **`od_watchdog` is no longer a scaffold, and NOT YET HARDWARE-VERIFIED on either target.**
  Both `esp32-idf` (`hal/od_hal_wdt.c`, over the IDF Task Watchdog) and `nordic-zephyr`
  (`src/od_hal_wdt.c`, over the devicetree `watchdog0` + `gpregret2` nodes) implement
  `od_hal_wdt.h` and call the policy through a per-target `od_watchdog_app` owner. Two things
  to know before flashing: arming widens the ESP32 idle-task TWDT check from 60 s to
  `OD_WDT_TIMEOUT_S` (300 s, the ~240 s panel-refresh bound), and on Nordic only `main()`
  feeds — a wedge confined to the display work queue does not trip it.
  **CALLED AND HARDWARE-VERIFIED on `esp32-idf` (2026-08-13):** `od_adv_control`, `od_advert`,
  `od_config_asm`, `od_config_tlv`, `od_span` — a board flashed with the `od_advert` swap wrote
  a config carrying an NFC packet and completed an encrypted image upload.
  **CALLED AND HARDWARE-VERIFIED on `nordic-zephyr` (2026-08-14, `xiao_nrf52840`):** `od_advert`
  and `od_config`. `struct od_config` is the parsed-config aggregate (the `struct GlobalConfig`
  copies are gone), `od_config_parse()` is the whole of `loadGlobalConfig()`, and Nordic's
  530-line packet switch, its own CRC-16 and its own size table went with it. Evidence from one
  flashed board: image upload completes; a config write is re-parsed across a reboot; the host
  decodes the MSD correctly, with battery and temperature right (the two fields `od_advert`
  re-encodes, including its float-domain clamp) and button presses reaching the dynamic block
  (so the `binary_inputs` packet survives the new parse and lands where the encoder reads it).
  That upload only passed once BLE TX power was fixed — see `zephyr/CMakeLists.txt`
  `OD_TX_POWER_DBM`, which also records why Zephyr's `BT_CTLR_TX_PWR_*` Kconfig is inert under
  the SoftDevice Controller.
  **CALLED, NOT YET FLASHED:** `od_advert` on `efr32bg22-slc` — with that, no open-coded MSD copy
  is left on any target, and `tests/host/advert_test.c` holds the two encoders they shipped as the
  differential reference (do not "update" those to match the encoder).
  `efr32bg22-slc` still open-codes the config parse: measured 2026-08-14 as
  +1 byte of RAM **only with `OD_CONFIG_WITH_{TOUCH,BUZZER,WIFI,DATA_EXTENDED}=0`** (gated 909 B
  vs its current 844 + 64; ungated 1617 B against 484 B of static slack at 98.5% RAM). Its real
  gate is `MAX_CONFIG_SIZE` 4096 + the NVM3 object-size check, not the aggregate. The remaining
  unpromoted protocol logic is **the transfer state machines** — direct, partial, PIPE and NFC,
  target-owned on purpose (C11 § 1) and now smaller, explicit inputs to their own promotions.
- `targets/esp32-idf/hal/` implements `od_hal_{nvs,log,gpio,time,i2c,adc,panel,crypto}`;
  `od_hal_crypto_random.c` is its own translation unit so a host test can compile the RNG arm
  without mbedTLS.
- **`shared/hal/od_hal_crypto.h` is the third shared HAL** (2026-08-15, with `od_hal_adv` and
  `od_hal_wdt`), implemented on both `esp32-idf` (mbedTLS) and `nordic-zephyr` (native
  `psa_aead_*`, which needed only `CONFIG_PSA_WANT_ALG_CCM=y` — the hand-rolled RFC 3610 both
  Nordic targets carried existed because that Kconfig was never set, not because PSA lacked CCM).
  Prepared **key slots**, not a key in the caller's struct: the targets clear a session with
  `memset`, which would drop a live PSA handle and exhaust a finite pool. Four-valued status so a
  tag mismatch and an engine fault stay distinguishable — the session's 3-strike policy depends on
  it. **NOT YET HARDWARE-VERIFIED**, and that commit also deletes Nordic's soft CCM (preserved as
  `tests/host/session_ccm_reference.inc`), so treat the CCM path as unproven until a board
  authenticates and completes an encrypted upload.
- **Never hardware-verified:** the WiFi/LAN transport, and the F4/F7 correctness fixes.
- **`compat/` (Arduino shim) is at its floor of 5 files** — `TARGET_NRF` arms that leave with
  migration step 4. Do not "finish" them (`targets/esp32-idf/compat/SHIM_BUDGET`).
- **`targets/esp32-idf/vendor/fastepd/` is not a shim** and outlives `compat/`: the permanent
  FastEPD adapter (Arduino `SPI` over IDF `spi_master`). It and both vendored panel libraries sit
  off the include path, granted per-source in `main/CMakeLists.txt` — adding a consumer is an edit
  there, not an `#include`.
- **THE WIRE CORPUS IS NOW CHECKED FROM BOTH ENDS** (C12, 2026-08-16). `tests/vectors/dispatch.json`
  was only ever replayed through py-opendisplay's public API, and `tests/host/replay_vectors.py:18`
  says outright that firmware replies are "never checkable here" — so every `expect.reply` had gone
  unchecked against firmware since the corpus was authored. `tests/host/corpus_runner.c` drives the
  same file through the production `od_dispatch_frame()`. It found a wire regression on its first
  run: C10 had silently replaced Nordic's oversize refusal `[FF][cmd_lo][FE]` — the bytes both
  donors ship — with `[00][cmd][FF]`, which is *also* the decrypt-failure answer, so a host could
  not tell the two apart. Restored, and pinned.
  **TWO EXECUTABLES, and the difference is what a pass MEANS.** `od_cmd_app_*` is static link-time
  composition, so two hook sets cannot share a binary — and the answer to that is not a runtime
  registry. `dispatch_corpus_portable` proves shared dispatch routed and plumbed a vector;
  `dispatch_corpus_nordic` links Nordic's production command code and proves the firmware emits
  those bytes. A `historical-fixture` vector is excluded from the production profile by
  construction, and a `target-production` vector that a capability predicate excludes there is a
  FAILURE — its claim would otherwise stand with nothing behind it.
  No fake ever sees an expected reply: the generated table is included by the runner and nothing
  else, and profiles get semantic knobs instead. That is what stops the corpus becoming its own
  oracle.
- Live plan: docs/NEXT_STEPS_2026-08-05.md (docs/NEXT_STEPS.md is historical). Dispatch: C8–C11
  landed, C12 in progress (plans/PLAN_OD_DISPATCH_C12_2026-08-16.md); Silabs is C13.

## The one rule

`shared/` compiles for **every** target: C standard library and `shared/hal` only. **No vendor or
framework header** — `esp_*`, `driver/*`, `soc/*`, `hal/*`, `freertos/*`, `nrf_*`, `nrfx`, `sl_*`,
`em_*`, `zephyr/*`, `Arduino.h`, `bluefruit.h`, `NimBLE*`, `bb_epaper`, `TFT_eSPI`. A file needing
one belongs in `targets/<target>/`. One slip and the repo is four codebases in a directory.
Enforced by the three `shared boundary:` checks in [tools/check.sh](tools/check.sh) — run them
before proposing anything under `shared/`; extend the pattern as targets are imported.

## Architectural decisions

1. **`shared/` is plain C.** Every interface across the boundary is a link-time `extern` C
   function the target implements. No C++ classes, no Arduino `String`.
   Re-argued 2026-08-13 and upheld — but not because C++ is unavailable (all three toolchains
   already compile target `.cpp`). It is that the boundary must be a C API for Zephyr's C
   drivers and the Silabs superloop regardless, and the code being ported is already C in
   `.cpp` files (~20k lines of `targets/esp32-idf/src/` hold 32 `new`, 9 `String`, one class).
   So C++ buys implementation ergonomics only, priced in a doubled host gate, an
   `-fno-exceptions -fno-rtti -fno-threadsafe-statics` contract spread across three build
   systems `shared/` does not own, and an enforcement grep that libstdc++'s transitive
   includes defeat. Three C-side rules buy back most of what RAII and strong types offered:
   - **`_Static_assert` every wire size.** The `sizeof`-derived size table can go silently
     wrong via include order (`shared/sources.cmake`, closing note) — a wrong *value*, not a
     build error. Assert the sizes so it is a build error. Zero cost, C99, unused today.
   - **Non-owning views are `od_span_t`** (`shared/core/od_span.h`), not ptr+len arguments.
     `od_span_split()` is the checked cut and the one place bounds arithmetic is written;
     `take`/`drop` saturate and are only for lengths already known to fit. The whole config
     parse path takes spans; new shared code does too.
   - **Single-exit + `goto cleanup`** in anything holding a resource across a fallible step.
   Revisit only at `od_session.c` / `od_xfer_partial.c` / `od_zlib_stream.c`: nested resource
   lifetimes with several failure exits per function are the one shape where manual cleanup
   reliably loses. That is also the last point where switching is cheap.
2. **One vtable, deliberately** — `od_panel_ops` (`targets/esp32-idf/hal/od_hal_panel.h`), for the
   one target with 2-3 panel backends. Keep it the only one.
3. **Three toolchains stay three** (ESP-IDF, west/Zephyr, SLC), all CMake. Unification is about
   shared *source*.
4. **Grouped by silicon vendor, not repo of origin.** nRF52840 is a *board* on `nordic-zephyr`,
   sharing its BT host, PSA Crypto, NVS and panel stack.
5. **No PlatformIO, no Arduino** — no `platformio.ini`, `lib_deps`, Arduino APIs or `build_flags`
   idioms, from any source repo.
6. **BG22 stays on Simplicity SDK.** Zephyr rejected on RAM: 32 KB total, no kernel at all
   (superloop + `sl_power_manager`).
7. **No Kconfig in `shared/`'s config surface** — Silabs lacks it. Plain preprocessor constants;
   Kconfig is only how two targets set them.
8. **No `targets/nrf52-sdk/`.** Legacy nRF52 stays in `Firmware_NRF` and sets the host's compat
   floor (no compression, no `0x76`, no PIPE, no NFC). A dir under `targets/` means "a target this
   repo builds".
9. **No lowest-common-denominator features.** Differences (PSRAM, ROM inflate, panel families,
   PIPE — absent on Silabs) go behind config and `#if`; a target must not pay for a feature it
   lacks.
10. **Import working drivers as-is**; only shared logic is refactored.
11. **First `shared/` source: `shared/core/od_adv_control.c`** (portable BLE advertising/
    lifecycle). `od_config.c` is the first *protocol* subsystem promoted.
12. **`MAX_CONFIG_SIZE` is 4096 everywhere** — a global cap, not a per-target macro; a uniform
    value removes a wire divergence a host could not discover. BG22 pays for it.
13. **`third_party/` is exempt from the one rule** — `bb_epaper` picks its IO backend by `#ifdef`.
    Still one vendored copy for all targets; do not move it into `shared/`.
14. **Headers beat design docs.** Where `targets/esp32-idf/hal/*.h` and docs/SHARED_API_DESIGN.md
    disagree, fix the doc.

## Layout

`shared/{protocol,core,compress,hal}` — wire contract / dispatch + config + transfer + auth /
inflate engines / interfaces targets implement.
`targets/{esp32-idf,nordic-zephyr,efr32bg22-slc}` — chip drivers + build system + HAL impl.
`third_party/` vendored cross-target libs (bb_epaper); `tools/`; `docs/`.

## Toolchains

Installed, but **none on `PATH`** — `which idf.py west` returning nothing ≠ absent.

| Toolchain | Version | Activate with |
|---|---|---|
| ESP-IDF | v5.5.4 | `source ~/esp/esp-idf/export.sh` |
| nRF Connect SDK / west | v3.3.1 / west v1.5.0 | `nrfutil toolchain-manager launch --ncs-version v3.3.1 -- <cmd>` |
| Simplicity SDK | 2025.12.2 | `slt`; `slc` also needs the bundled Java on `PATH` |

Pin one ESP-IDF release; floors ≥ 5.1 (C6), ≥ 5.2 (`driver/i2c_master.h` — not the deprecated
`driver/i2c.h`). Only ESP-IDF has built anything here. **Do not claim a build passes without
running it.** docs/TOOLCHAINS.md has the version pins and the Arduino-API replacement census.

## Protocol header — do not hand-edit

`shared/protocol/opendisplay_protocol.h` and `opendisplay_structs.h` are byte-for-byte copies of
`../opendisplay-protocol`. Edit the canonical file there, never these.

## Migration constraints

Rationale in [docs/MIGRATION.md](docs/MIGRATION.md) / [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
— open only if a change contradicts a rule here or adds a target.

- **One target at a time:** `esp32-idf` → `nordic-zephyr` (nRF54L15) → `efr32bg22-slc` →
  `nordic-zephyr` (nRF52840). ESP32 first as the reference for `shared/core`; Silabs third so its
  32 KB / no-kernel / no-Kconfig limits bite before other targets bake in assumptions.
- **Import unchanged in one commit**, then fix only include paths and build files; record source
  repo + SHA. Exceptions: the ESP32 framework change, and Silabs' 57 MB vendored SDK.
- **One subsystem per swap**, build + flash + hardware verify between each, independently
  revertable.
- **Delete nothing from the original repos** until the unified target is hardware-verified.
- **Resolve divergence deliberately and write it down** — not by whichever repo was copied first.
  **`Firmware` is the authority over `Firmware_NRF54`** when their algorithms disagree: it is the
  field-proven original and what the host tooling was validated against, and the NRF54 port
  re-derived several of them. So port the `esp32-idf` behaviour and make the Zephyr difference
  justify itself — in a differential test, the Firmware form is the reference. `esp32-idf` is
  C++ and `shared/` is plain C, so this usually means a C port, not a file move. A default, not
  a licence to skip the write-up.
- **THE AUTHORITY IS `../Firmware/`, THE SIBLING REPO — NOT `targets/esp32-idf/src/`.** That
  directory is a *snapshot* taken at import, and upstream keeps moving. Before porting or
  transcribing any algorithm, diff the two; when this repo and upstream disagree, upstream wins
  unless the difference is a deliberate Firmware_Unified adaptation (Arduino removal, `od_hal_*`,
  `od_log`, `struct od_config`), which the import is full of — so separate *drift* from
  *adaptation* rather than blanket-copying either way. `../Firmware/tools/` also carries host
  tests worth porting with the code they cover.
  Learned the hard way on `od_session` (2026-08-15): the replay window had been extracted upstream
  into `src/nonce_window.h` with an 816-line host test, and three rounds of design were built on
  the stale ring instead — two of them wrong in security-relevant ways (a forward window cap that
  strands a session, and counting nonce failures as integrity strikes so packet loss tears one
  down). A 30-second diff would have caught all of it.
  The same applies to the other three source repos for their targets.

## Memory sensitivity

EFR32BG22 32 KB, nRF52840 256 KB, ESP32-S3 512 KB + PSRAM. `shared/` must avoid buffers sized for
the biggest target, unbounded heap, and assuming a heap exists. Sizes go behind compile-time
constants the target sets, with a documented floor; decision 12 is the sole exception
(docs/MEMORY_CONSTRAINTS.md item 3 has the BG22 mitigations gating the Silabs swap).
