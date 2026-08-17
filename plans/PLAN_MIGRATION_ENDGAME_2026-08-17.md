# Plan: what is left in the migration

**Status:** roadmap, 2026-08-17. `main` is at `33337ef`; C8-C12's software half is landed and
merged. This is the big-picture view above the individual execution plans, not a replacement for
any of them.

**Scope:** what remains between here and the migration being finished — that is, the point where
`../Firmware`, `../Firmware_NRF54`, `../Firmware_Silabs` and `../Firmware_NRF` can be retired and
this repository stops being a fork with extra steps.

**What this is not.** It does not re-plan C13 or the transfer promotion; each needs its own
execution plan with the detail `PLAN_OD_DISPATCH_C11_2026-08-16.md` sets as the bar. It states what
those units are, why they are ordered as they are, and what blocks what.

---

## 1. Where the migration actually stands

`docs/NEXT_STEPS_2026-08-05.md` § "Executive sequence" lists eight steps. Six are done. The two
that are not are the two that matter most, and one of them is not an implementation step at all:

| Step | State |
|---|---|
| 1-4 advertisement baseline, F4 controller, `od_config`, ESP32 correctness | landed |
| 5 **release acceptance matrix and recorded hardware debt** | **OPEN — nothing has been run** |
| 6 nRF54L15 import | landed as `targets/nordic-zephyr/` |
| 7 **EFR32BG22 import** | **PARTIAL — imported as a target, never promoted** |
| 8 nRF52840 migrates | landed as a board of `nordic-zephyr` |

`shared/` now carries the advertising controller, config parse, session, egress, ingress, watchdog
and the whole command path. What it does not carry is any transfer state machine, and what has
never happened at all is hardware verification of the result.

---

## 2. The six outstanding items

### 2.1 Hardware verification — the dominant item

**Nothing promoted since C6 has run on silicon**, and the debt is stacked rather than parallel:

| Target | Last real hardware evidence |
|---|---|
| `xiao_nrf52840` | **C6** (Gate 2, 2026-08-15). C9, C10, C11 and C12 unflashed |
| ESP32-S3 | **none, ever** — C1's mbedTLS arm, C5, C8, C9, C11, C12 |
| `xiao_nrf54l15`, `xiao_nrf54lm20a` | build clean, never flashed |
| `efr32bg22-slc` | builds headless, never flashed |
| WiFi/LAN transport | never hardware-verified (`CLAUDE.md`) |

The procedure exists and is specific: `plans/PLAN_OD_DISPATCH_C12_2026-08-16.md` § 7 defines H1
(Nordic C10 matrix at `a37c04b`), H2 (ESP32-S3 smoke at `a37c04b`) and H3 (the C11 exit matrix at
current `main`), plus the mandatory OD-S1 replay injection in § 7.5. `dispatch-gate` in
`targets/esp32-idf/tools/od-device-cli.py` drives the two rows that cannot be driven by hand.

**Why this is first.** It gates § 2.6 outright, and every further promotion widens the window a
first-flash failure has to be attributed across. H1 alone retires four layers of Nordic debt in one
flash, on the only board that has ever passed a gate — which makes a failure there maximally
diagnostic.

**A note on the host.** The BLE side should run from Windows Python rather than WSL: `uv` and
Python 3.13 are already installed there, `bleak` uses the WinRT backend, and no USB passthrough or
BlueZ is involved. WSL2 has `CONFIG_BT=m` but no adapter, no BlueZ userspace, and would need
`usbipd` plus a dongle — and several gate rows turn on timing, where added jitter makes a false
PASS indistinguishable from a real one.

### 2.2 Silabs — C13, and the ordering is already inverted

`targets/efr32bg22-slc/` consumes only `OD_SHARED_SOURCES_PURE`
(`cmake_gcc/opendisplay-bg22.cmake:285`), defines no `od_cmd_app_*` hook, and still owns a complete
opcode switch in a 1,303-line `opendisplay_pipe.c`. It is migration step 3 and is three promotions
behind.

That is not merely late. `CLAUDE.md` § "Migration constraints" puts Silabs third **so its limits
bite before other targets bake in assumptions** — 32 KB total RAM, no kernel, no Kconfig. Every
promotion since C9 has therefore been designed against two targets that both have a kernel and at
least 256 KB. The cost of that is unmeasured, and the way to measure it is to do C13.

`PLAN_OD_DISPATCH_C12_2026-08-16.md` § 2.5 adds one prerequisite: Silabs' untouched dispatcher
should be **captured** before the cutover, because once shared code replaces it there is no
pre-migration reference left to capture from. With no board available that capture cannot happen,
so C13 must either wait or record the skipped deadline deliberately — the same way § 3.3's capture
deadline was missed for ESP32 and Nordic and then had to be corrected in writing.

### 2.3 The transfer state machines

The largest remaining duplication in the repository. `shared/core` contains no transfer source at
all, while the same four state machines exist three times:

| | lines |
|---|---|
| `targets/esp32-idf/src/display_service.cpp` | 3402 |
| `targets/nordic-zephyr/src/opendisplay_display.cpp` | 1332 |
| `targets/efr32bg22-slc/opendisplay_pipe.c` | 1303 |
| `targets/efr32bg22-slc/opendisplay_display.cpp` | 903 |
| `targets/nordic-zephyr/src/opendisplay_pipe_write.cpp` | 595 |

C11 left them target-owned deliberately (`PLAN_OD_DISPATCH_C11_2026-08-16.md` § 1) and made them
smaller, explicit inputs to their own promotion: 8 of `od_cmd_app.h`'s 21 hooks — direct ×3,
partial, PIPE ×3, NFC — disappear when they go shared.

**One prerequisite, and it is a decision rather than work.** `CLAUDE.md` architectural decision 1
names `od_xfer_partial.c` and `od_zlib_stream.c` as the single place the plain-C choice must be
re-argued, "the one shape where manual cleanup reliably loses," and says it is also the last point
where switching is cheap. That argument belongs *before* the promotion starts.

### 2.4 The dead nRF arms, and `compat/` — DONE 2026-08-16

**Closed.** No `TARGET_NRF` arm was reachable in any built configuration, so all of them and both
nRF-only translation units are deleted (719 lines), the shim count reached its floor of 0, and all
ten boards build with `text`, `data` and `bss` **identical** to the previous build — which is the
proof that only never-compiled code went. What remains of `compat/` is `app_main.cpp` and
`arduino_compat.cpp`, still listed in `main/CMakeLists.txt`; deleting the directory means moving
the IDF entry point first, which is a build change rather than a deletion. The extraction into
`vendor/fastepd/` that `compat/SHIM_BUDGET` specifies is now unblocked.

The record below is what was true before that.

`targets/esp32-idf/src/` still carries 54 `TARGET_NRF` references and two nRF-only translation
units (`ble_transport_nrf.cpp`, `ble_transport_nrf.h`) — Bluefruit code for the nRF52840.

**No build fragment defines `TARGET_NRF`.** All ten board fragments under
`targets/esp32-idf/boards/` define `TARGET_ESP32` — the eleventh `.cmake` there is
`s3-n16r8.panel.cmake`, a panel sub-fragment holding one `set()` and no defines — and a search of
the target's CMake and shell files finds `TARGET_NRF` only in `compat/ratchet.sh`'s own prose. The
nRF52840 became a `nordic-zephyr` board at migration step 8.

That matters beyond tidiness. `compat/SHIM_BUDGET` records the floor of 5 as being held *only* by
those arms — "they leave with the nRF target at MIGRATION step 4, and `compat/` is deletable at
that moment, not before." Step 4's successor has happened. The condition the budget names may
already be met, and nobody noticed because the ratchet counts files rather than reachability.

**Contained, cheap, and worth confirming before assuming**: establish whether any `TARGET_NRF` arm
is reachable in any built configuration. If none is, the arms and the two files are deletable, the
budget reaches 0, and the surviving primitives are extracted into the permanent FastEPD adapter as
`SHIM_BUDGET` already specifies. If some arm *is* reachable, that is a more interesting finding.

### 2.5 Defects that need someone else, or a decision

None of these is blocked on migration work; all are tracked in `docs/FOLLOWUPS.md`.

- **§ 5 — one CCM nonce in both directions under one key.** High severity, and **no firmware change
  can fix it**: both directions share a `session_id` and both counters start at 0, so `od_session`
  reproduces it deliberately. Needs a protocol revision with directional key separation or a
  nonce-domain bit. Three costed options are recorded there.
- **§ 1.1 — `py-opendisplay`'s `deep_sleep()` sends `0x0052`.** Sharper since C11: Nordic now
  answers that opcode with an unsupported NACK, but on latch hardware `0x0052` **cuts the rail**.
  A host defect with a hardware consequence.
- **§ 2.1, § 3.1 — canonical header defects** blocked on the freeze: the `0x52` NACK width
  contradiction, and `MAX_CONFIG_CHUNKS` 20 → 21 which makes the 4001-4096 band deliverable.
- **§ 3.5 — FastEPD writes no pixels to an IT8951 panel.** High severity, S3-only, needs hardware.
- **§ 3.6 — `bbepWaitBusy` blocks the loop task** for the length of a refresh.

### 2.6 Retire the four source repositories

`../Firmware`, `../Firmware_NRF54`, `../Firmware_Silabs` and `../Firmware_NRF` all still exist, and
`CLAUDE.md` makes `../Firmware/` the **authority** over this repository's snapshot for any algorithm
being ported. `docs/MIGRATION.md` forbids deleting anything from them until the unified target is
hardware-verified.

This is the actual finish line, and it is gated entirely on § 2.1. Until it happens the project is
a fork with extra steps: every promotion carries the obligation to diff against a live sibling, and
the "import snapshots drift" warning in `CLAUDE.md` stays load-bearing.

---

## 3. Ordering, and what depends on what

```
2.1 hardware ──────────────┬──────────────────────────► 2.6 retire the source repos
                           │
2.4 dead nRF arms (DONE)   │   2.2 Silabs C13 ──► 2.3 transfers
                           │        ▲                 ▲
                           └────────┘                 └── language decision first
```

- **2.1 before 2.6** is a rule, not a preference (`MIGRATION.md`).
- **2.2 before 2.3** is the argument in § 2.2: promoting transfers against only the two kernel
  targets designs around the constraint Silabs exists to impose. Doing it in the other order is how
  a 32 KB target discovers, late, that a shared buffer was sized for a device with PSRAM.
- **2.4 is done** (2026-08-16); it was independent of everything else.
- **2.5 is independent**, and mostly belongs to other repositories.

**Recommended next unit:** H1 from `PLAN_OD_DISPATCH_C12_2026-08-16.md` § 7.2, when a board is
available. Without one, C13's non-cutover preparation — reading Silabs' dispatcher, sizing its
constraints against `shared/`, writing the plan — is the work that does not need hardware, provided
the capture question in § 2.2 is answered explicitly rather than skipped.

---

## 4. What "done" means

The migration is finished when:

- every promoted subsystem has hardware evidence on at least one board per target family, recorded
  with the provenance rules in `PLAN_OD_DISPATCH_C12_2026-08-16.md` § 7.1;
- all three targets consume the shared command path, so `od_cmd_app.h` has three implementations
  rather than two;
- the transfer state machines exist once;
- `compat/` is reduced to the permanent FastEPD adapter, and the ratchet is deleted with it;
- the four source repositories are retired, and `CLAUDE.md` stops naming `../Firmware` as the
  authority; and
- no status document claims a target is hardware-verified on build or host evidence.

Five of those six are software work with a known shape. The first is the one that has never
started.
