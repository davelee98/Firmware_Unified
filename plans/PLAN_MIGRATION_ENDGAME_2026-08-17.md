# Plan: what is left in the migration

**Status:** roadmap, 2026-08-17. `main` is at `eab3bca`; C8-C12's software half is landed and
merged. This is the big-picture view above the individual execution plans, not a replacement for
any of them.

**Scope:** what remains between here and the migration being finished — that is, the point where
the three migrated donors `../Firmware`, `../Firmware_NRF54` and `../Firmware_Silabs` can be retired
and this repository stops being a fork with extra steps. `../Firmware_NRF` is deliberately outside
that finish line: it remains the maintenance repository for the shipped legacy nRF52 fleet and the
host's backward-compatibility floor (`docs/MIGRATION.md:30-48`).

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

Also closed on 2026-08-16: the unreachable ESP32 `TARGET_NRF` arms and translation units were
deleted, `targets/esp32-idf/compat/` was deleted, its ratchet was replaced by `tools/check.sh`'s
call-based Arduino-free gate, and the permanent FastEPD adapter moved to `vendor/fastepd/`.

`shared/` now carries the advertising controller, config parse, session, egress, ingress, watchdog
and the whole command path. What it does not carry is any transfer state machine, and what has
never happened at all is hardware verification of the result.

---

## 2. The five outstanding items

### 2.1 Hardware verification — the dominant item

**Nothing promoted since C6 has run on silicon**, and the debt is stacked rather than parallel:

| Target | Last evidence relevant to the current promoted stack |
|---|---|
| `xiao_nrf52840` | **C6** (Gate 2, 2026-08-15). C9, C10, C11 and C12 unflashed |
| ESP32-S3 | older target/F4 hardware evidence exists, but none for C1's mbedTLS arm, C5, C8, C9, C11 or C12 |
| `xiao_nrf54l15`, `xiao_nrf54lm20a` | build clean, never flashed |
| `efr32bg22-slc` | builds headless, never flashed |
| WiFi/LAN transport | never hardware-verified (`CLAUDE.md`) |

**The final release matrix is by silicon, bootloader, storage and transport class — not merely by
target directory.** One ESP32-S3 cannot stand in for classic ESP32, C3 and C6, and one nRF52840
cannot stand in for either nRF54 part. These are the minimum rows before a migrated donor retires:

| Unified target / representative | Boot and storage distinction | Required hardware evidence | Donor discharged |
|---|---|---|---|
| ESP32-S3, one PSRAM board | IDF second-stage; OTA-capable N8/N16/N32 partition families | BLE plus WiFi/LAN; config persistence; encrypted compressed and uncompressed image push; release artifacts for every S3 storage variant | `Firmware` ESP32-S3 arm |
| ESP32-C6 N4 | IDF second-stage; no PSRAM; single-app partition | BLE plus WiFi/LAN; config persistence; encrypted compressed and uncompressed image push | `Firmware` ESP32-C6 arm |
| ESP32-C3 N4 or N16 | IDF second-stage; no PSRAM; C3-specific radio and flash configuration | BLE plus WiFi/LAN; config persistence; encrypted compressed and uncompressed image push; the other storage variant remains a build/artifact row | `Firmware` ESP32-C3 arm |
| classic ESP32 N4 | IDF second-stage; no PSRAM; classic ESP32 peripheral path | BLE plus WiFi/LAN; config persistence; encrypted compressed and uncompressed image push | `Firmware` classic arm |
| `xiao_nrf52840` | Adafruit UF2; nRF52840 board init and EPD rail path | BLE Gate 2, interruption recovery and production UF2/merged-hex artifacts | `Firmware` nRF52840 arm |
| `xiao_nrf54l15` | MCUboot; nRF54L15 board definition | BLE Gate 2, interruption recovery and MCUboot artifacts | `Firmware_NRF54` L15 arm |
| `xiao_nrf54lm20a` | MCUboot; distinct LM20A DTS, pinctrl and second core | BLE Gate 2, interruption recovery and MCUboot artifacts | `Firmware_NRF54` LM20A arm |
| `efr32bg22-slc` | Gecko Bootloader + AppLoader; NVM3; 32 KB RAM | BLE Gate 2, `.gbl` update, config persistence and the C13 MTU/RAM gates | `Firmware_Silabs` |

All rows record board id, exact release SHA, bootloader, partition/storage layout, tool and host
versions, raw transcript, device log and per-observation PASS/FAIL. A row may be removed only by an
explicit support-scope decision; another silicon family cannot silently substitute for it.

The procedure exists and is specific: `plans/PLAN_OD_DISPATCH_C12_2026-08-16.md` § 7 defines H1
(Nordic C10 matrix at `a37c04b`), H2 (ESP32-S3 smoke at `a37c04b`) and H3 (the C11 exit matrix at
the final C12 software SHA), plus the mandatory OD-S1 replay injection in § 7.5. `dispatch-gate` in
`targets/esp32-idf/tools/od-device-cli.py` drives the two rows that cannot be driven by hand.

**Why this is first.** It gates C13's cutover and ultimately § 2.5, and every further promotion
widens the window a first-flash failure has to be attributed across. H1 alone retires four layers
of Nordic debt in one flash, on the only board that has ever passed a gate — which makes a failure
there maximally diagnostic.

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

### 2.3 Transfer promotion — five independently gated units

This is the largest remaining protocol-state duplication, but it is not four identical state
machines copied three times. Capability and hardware ownership differ by target:

| Promotion unit | ESP32 | Nordic/Zephyr | BG22 | Boundary that remains target-owned |
|---|---|---|---|---|
| Compression stream/backend | yes | yes | yes | engine selection, window size and storage |
| Direct write | yes | yes | yes | panel begin/write/end and refresh |
| Partial write (`0x76`) | yes | yes | **unsupported** | region/plane application and panel refresh |
| PIPE (`0x80`-`0x82`) | yes | yes | **unsupported** | panel sink; window/reorder/SACK become shared only on enabled targets |
| NFC (`0x83`) | yes | yes | yes | NFC controller read/write and record storage |

Each row is a separate execution unit and independently revertible commit series. For one row:

1. settle its shared state/HAL boundary and capability permutations;
2. add host differential, malformed-sequence and interruption coverage;
3. promote only that state machine and remove only its corresponding target hooks;
4. build every enabled/disabled target permutation; and
5. pass Gate 2 on every capable hardware class before beginning the next row.

An unsupported target proves the capability-off build and wire response; it does not link dead
state or pay its RAM. Do not assert in advance that all eight transfer hooks disappear: each hook
leaves `od_cmd_app.h` only when its shared owner and target HAL seam are accepted.

**One prerequisite, and it is a decision rather than work.** `CLAUDE.md` architectural decision 1
names `od_xfer_partial.c` and `od_zlib_stream.c` as the single place the plain-C choice must be
re-argued, "the one shape where manual cleanup reliably loses," and says it is also the last point
where switching is cheap. Record that decision before the first of these five units starts.

### 2.4 Defects that need someone else, or a decision

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

### 2.5 Retire the three migrated source repositories; preserve legacy nRF

`../Firmware`, `../Firmware_NRF54` and `../Firmware_Silabs` are the three migrated donors still to
retire, and `CLAUDE.md` makes `../Firmware/` the **authority** over this repository's snapshot for
any algorithm being ported. `docs/MIGRATION.md` forbids deleting their corresponding code until the
unified replacements pass the matrix in § 2.1.

`../Firmware_NRF` is not a migrated donor. It stays active in maintenance for the shipped legacy
nRF52 fleet, and its strict feature subset remains a required `py-opendisplay` compatibility
profile. Retirement of the other repositories must not remove that profile or imply those devices
acquired capabilities they cannot be updated to support.

This is the actual finish line. It follows C13, all five independently verified transfer units and
the final release matrix in § 2.1. Until then every promotion carries the obligation to diff against
a live migrated sibling, and the "import snapshots drift" warning in `CLAUDE.md` stays
load-bearing.

---

## 3. Ordering, and what depends on what

```
C12 hardware debt ──► 2.2 Silabs C13 ──► C13 Gate 2 ──┐
                                                       ├─► 2.3 transfer units, one at a time
language decision ─────────────────────────────────────┘      │
  compression ─► direct ─► partial ─► PIPE ─► NFC            │ Gate 1 + Gate 2 after each
                                                               ▼
                                              final § 2.1 release matrix
                                                               │
                                                               ▼
                                              2.5 retire three migrated donors

`Firmware_NRF` ─────────────────────────────► retained maintenance + host compatibility floor
```

- **The final § 2.1 matrix before 2.5** is a rule, not a preference (`MIGRATION.md`). Historical
  C12 runs establish attribution; they do not verify code promoted afterwards.
- **2.2 before 2.3** is the argument in § 2.2: promoting transfers against only the two kernel
  targets designs around the constraint Silabs exists to impose. Doing it in the other order is how
  a 32 KB target discovers, late, that a shared buffer was sized for a device with PSRAM.
- **The five § 2.3 rows are sequential acceptance boundaries**, not one transfer mega-swap.
- **2.4 is independent**, and mostly belongs to other repositories.

**Recommended next unit:** H1 from `PLAN_OD_DISPATCH_C12_2026-08-16.md` § 7.2, when a board is
available. Without one, C13's non-cutover preparation and review can continue under
`PLAN_SILABS_C13_2026-08-16.md`, provided the capture question in § 2.2 is answered explicitly rather
than silently skipped.

---

## 4. What "done" means

The migration is finished when:

- every row of § 2.1's silicon/boot/storage/transport matrix has provenance-complete evidence on the
  final release SHA, or an explicit support-scope decision removes it;
- all three targets consume the shared command path, so `od_cmd_app.h` has three implementations
  rather than two;
- compression, direct, partial, PIPE and NFC have each crossed their own Gate 1 and Gate 2 boundary,
  with one shared protocol-state owner where the capability exists and no RAM/code cost where it
  does not;
- the three migrated donors are retired, their READMEs point here, and `CLAUDE.md` stops naming
  `../Firmware` as the authority;
- `Firmware_NRF` remains maintained and its legacy capability profile stays in host compatibility
  tests; and
- no status document claims a target is hardware-verified on build or host evidence.

The dead nRF arms, Arduino shim and `compat/` directory are already closed work and are not part of
this finish-line checklist.
