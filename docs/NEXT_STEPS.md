# Next steps

Where the migration stands after the ESP32 phase-B port, and what to do next, in order.
Written 2026-07-26. Companion to [MIGRATION.md](MIGRATION.md) (the plan) and
[FOLLOWUPS.md](FOLLOWUPS.md) (defects awaiting action); this file is the *sequence*.

## Where we are

Migration **step 1 (esp32-idf) has run on hardware** (S3, 2026-08-03). Arduino is gone, BLE
runs on NimBLE's native C API, config lives in NVS, LAN runs on lwip sockets, the panel
libraries are vendored and pruned, the ESP-IDF bb_epaper backend is ours, and there are no
linker workarounds. The target is also synced to Firmware `feat/psram-dram-reclaim`.

This page previously said **"Nothing has run — that is the single most important fact on this
page."** That is no longer true, and item 1 below is retired accordingly.

| | |
|---|---|
| `shared/` | still empty — by design |
| Arduino shim | **21 -> 5, and 5 is the FLOOR** (nRF-only arms; see item 4) |
| HALs | od_hal_{nvs,log,gpio,time,i2c,adc,panel} implemented in `targets/esp32-idf/hal/` |
| Host tests | green: 1 shared/ test + link_owner under ASan and TSan |
| ESP32 target | 10 boards build; **S3 flashed and exercised** — see targets/esp32-idf/README.md § Verified on hardware |
| Gate 2 | *mostly* met: uncompressed push and interrupted-transfer recovery still unexercised |
| Other three targets | not started |

---

## 1. ~~Flash it — Gate 2~~ — DONE 2026-08-03, partially

The S3 boots, renders, and completes encrypted compressed image pushes; deep-sleep button wake
works. Two Gate 2 items remain unexercised (uncompressed push; an interrupted transfer that
recovers), so this cannot yet license retiring the source repo. The original checklist and the
highest-risk items follow, kept because the two open items are in it.

Everything below is lower value than finding out whether the image boots. GATT registration,
advertising, the CCCD subscribe path, notify-under-load, the NVS record round-trip and the LAN
sockets all compile and link perfectly and can each fail silently on hardware.

MIGRATION.md § "Verification bar per subsystem" Gate 2 is the checklist: full image push
(compressed and uncompressed), config read/write round-trip, the encrypted path, and an
interrupted transfer that recovers.

Highest-risk items specifically, because they were rewritten rather than shimmed:

- **BLE**: does a client discover the service, subscribe, and receive notifications? The GATT
  layout is preserved byte-for-byte, but the advertisement is now built in one call and drops
  the 128-bit UUID when the MSD does not fit alongside it — verify host discovery still works.
- **NVS**: does a saved config survive a reboot? `nvs_commit()` is the failure mode to watch;
  without it a save looks successful and vanishes.
- **The panel**: the drawing layer collision was resolved by giving FastEPD a private copy.
  Verify both an SPI panel and a parallel (FastEPD) panel actually render.

## 2. ~~Measure the C6 image against 1.25 MB~~ — DONE 2026-07-26: it does not fit

`c6-n4` exists (`boards/c6-n4.cmake`, `boards/c6-n4.sdkconfig`, `sdkconfig.defaults.esp32c6`)
and builds on ESP-IDF v5.5.4. **The image is 1 498 256 bytes — 187 536 over the 1.25 MB slot**,
so `default.csv` is out and the decision changed to `min_spiffs.csv` (1.875 MB per slot, 24%
free, 128 KB of SPIFFS left). `partitions/min_spiffs_4MB.csv` is added and the fragment points
at it. Full numbers and the per-archive breakdown are in targets/esp32-idf/README.md
§ Partitions.

Two things worth carrying forward:

- The guess that "the IDF image will be smaller than the Arduino build" was not testable this
  way and the *intuition behind it was wrong*: the C6 is **257 KB larger than the S3** while
  compiling strictly less code (no FastEPD, no PSRAM). +146 KB of that is the C6's precompiled
  BLE controller blob and the rest is RISC-V code density plus a fatter WiFi stack. Nothing
  the application can shrink.
- Adding a board needed **zero changes outside `boards/`** — every S3-specific thing in the
  tree was already correctly gated. That is a real result about the phase-B port's quality and
  it de-risks item 3.

## 3. The remaining boards — DONE (2026-07-26), except `s3-e1004`

All ten fragments exist and all ten build. Sizes and slot headroom are in
[targets/esp32-idf/README.md](../targets/esp32-idf/README.md) § Boards.

`esp32-n4` was expected to be the troublesome one; it was not. `PIPE_SMALL_DRAM_WINDOW` was
already in `structs.h` from the import, and at 695 KB it is the *smallest* image of the ten by
45% — it is the only board that does not compile the WiFi/LAN transport. The board actually
worth watching is `c6-n4`: 23% slot headroom against 62-81% everywhere else, and it is shipped.

`s3-e1004` has no fragment on purpose. The source env pins `limengdu/bb_epaper` for T133A01;
this repo vendors a fork without it, so the board would build and be unable to drive its
panel. It needs a re-vendor plus a fix to `e1004_write_stream_bytes()` (calls `SPI.writeBytes()`
with nothing calling `SPI.begin()`). See third_party/NOTICE.md.

Three findings fell out of doing this, all fixed:

* `OD_FASTEPD_BOARDS` was wrong — it listed `s3-e1004`, whose env sets neither the FastEPD
  define nor the lib_dep, and omitted `s3-n8r8` and both `s3-n16r8-extuart` boards.
* `compat/HardwareSerial.h` did not exist. `main.cpp` has included it since the import, inside
  the `OPENDISPLAY_LOG_UART` guard that no board fragment defined until now — so the target's
  most Arduino-dependent file was also scoring zero against the ratchet.
* `OD_PARTITION_CSV` was documentation only; the layout is really chosen by
  `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`. Disagreement between the two is now a
  configure-time error instead of a silently wrong flash layout.

## 4. ~~Phase C — drive `SHIM_BUDGET` from 22 to 0~~ — **DONE 2026-08-05, and the target was not 0**

The ESP32 app-code work is finished. The budget went **21 -> 5** across fifteen recorded steps
(`targets/esp32-idf/compat/SHIM_BUDGET` has one dated paragraph each, including what each step
found). It started at 22 only because adding the `*-extuart` boards revealed `HardwareSerial.h`
was missing from the ratchet's pattern — the shim did not grow, the metric did, and that
distinction is recorded rather than quietly applied.

**ZERO WAS THE WRONG TARGET, and this is the correction that matters most on this page.** Two
things that were assumed to be temporary are not:

- **Five files stay at the floor.** `main.cpp`, `display_service.cpp`, `buzzer_hw.cpp`,
  `device_control.cpp` and `encryption.cpp` are counted **only for their `TARGET_NRF` arms**.
  Those arms do not compile on this target, so they cannot be verified here; converting them
  blind is exactly the unverifiable edit MIGRATION.md warns against. They leave with the nRF
  target at **migration step 4**, and `compat/` is deletable at that moment — not before, and
  not by finishing them early.
- **FastEPD needs a permanent adapter.** It is a vendored Arduino library whose IT8951
  transport is written against the Arduino `SPI` object; there is no version of it that is not.
  That surface now lives in `targets/esp32-idf/vendor/fastepd/`, deliberately outside `compat/`
  so the delete instruction stays unambiguous, and `ratchet.sh` no longer counts it.

Earlier text here and in MIGRATION.md said to "delete `compat/` and the workflow together when
the budget reaches 0". Read that as **when it reaches the floor and the nRF target has moved**.

## 5. Then, and only then: the first `shared/core` promotion

`od_config.c` — the config TLV parser. It is first because it is the pre-auth attack surface,
because the NRF54 size-table parser lands with it (which is what re-opens "add a new config
packet type" as a safe move), and because TEST_OWNERSHIP.md wants it promoted *with* its tests:
the promotion is exactly when its error handling gets rewritten.

This is also where `tests/host/` stops being a formality — the first real `shared/` source means
the first C vector runner, the first HAL test doubles, and the first fuzz harness.

---

## Running in parallel — these do not wait for the above

**Wire captures have a deadline.** Once `shared/core` starts replacing a target's logic there is
no untouched reference left to capture from, and the corpus becomes a description of what the
unified firmware does rather than a baseline for what the fleet did. Bench time is the scarce
input. ESP32 first. TEST_OWNERSHIP.md § "Capture is time-sensitive".

**File the `py-opendisplay` issues.** Five defects, all verified, all in
[FOLLOWUPS.md](FOLLOWUPS.md) § 1 with reproductions. `0x0052` first: the shipped host sends
power-off when asked for a timed sleep, on the version HA pins, and on latch hardware that is
recoverable only by physically pressing a button.

**The `opendisplay-protocol` PR.** Add `Firmware_Unified/shared/protocol/` to the sync tool's
copy map, after applying PR #120's wording to canonical. Until then these headers are unpoliced
and `--check` cannot catch drift. This has been outstanding since the first review.

**The three-toolchain CI matrix.** Now the largest gap in CI. All three toolchains are installed
locally, which makes this less urgent and no less necessary: one machine with one set of
versions is not a matrix, and nobody else can reproduce a build.

---

## Decisions still open

Carried from [FOLLOWUPS.md](FOLLOWUPS.md) and DESIGN_REVIEW's table. None blocks the sequence
above; each blocks something later.

| Decision | Blocks |
|---|---|
| Re-take the security-hotfix deferral? Its trigger is met — BG22 is shipped *and* field-updatable, so two defects sit on reachable hardware until step 3 | Nothing mechanically; it is an exposure question |
| `MAX_CONFIG_CHUNKS` 20 → 21, so 4096 is actually transferable | Queued behind the header freeze |
| Corpus schema: `forbids`, `expect.parsed`, one-frame-per-vector | Cheap now, expensive across a large corpus |
| Per-target fix levels under a single version line | The first host compatibility matrix |
| The capability opcode bitmap | Capability discovery |
| Submodule vs script-sync for `shared/protocol` | The header-drift class |
| Whether deployed nRF52840 units migrate to Zephyr at all | Whether step 4 is a port or a product split |

## The thing most likely to go wrong

Not a technical item: **phase C stalling.** The image links, so it is tempting to move to the
next target and leave 21 files on the shim. MIGRATION.md names the shim outliving its purpose as
the biggest risk in the plan, and the ratchet exists because good intentions were judged
insufficient. If `SHIM_BUDGET` has not moved in a month, the port is not progressing — it is
finished in the way that leaves a permanent Arduino dependency in a repo whose premise is that
it has none.
