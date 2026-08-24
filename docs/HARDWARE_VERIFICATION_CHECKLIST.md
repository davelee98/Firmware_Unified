# Hardware verification checklist

**Purpose:** the living record of what has actually run on real silicon, per target and per
protocol behavior. Supersedes `plans/PLAN_OD_DISPATCH_C12_2026-08-16.md` §7's H1/H2/H3
procedure for day-to-day use: that procedure's requirement to run at the historical SHA
`a37c04b` before the final SHA (to bisect pre-C11 debt from C11-introduced behavior) is
**dropped here** — this tracks current-HEAD verification going forward, not point-in-time
attribution. If a genuine C8-C11 regression needs bisecting later, `a37c04b` is still the
right SHA to reach for; it just isn't a gate this checklist enforces.

## How to use this

- Check a row only against real on-air evidence: a raw frame transcript or a device-side log
  showing the actual behavior. A build pass, a host-suite pass, or a "notification queued" log
  line is not evidence.
- Record the date and a pointer to the evidence (log file, PR, or conversation) next to each
  checked row.
- Rebuilding or materially changing the code a row covers un-checks it until re-run. This file
  is a snapshot of verified behavior, not a promise about the current tree.
- `CLAUDE.md`'s Status section is the authoritative summary of what's verified; this file is
  the itemized detail behind it. Update both together.

---

## LED runner and panel rail — dedup D1/D2/D8 (2026-08-22)

Software-fixed on `fix/bg22-gate2-blockers`; nothing here is hardware-qualified. The
`group_repeats` sentinel changed on **all three targets**, so it carries rows on each. D1's yield
floor and D8 are BG22-only and are the rows
`plans/PLAN_DEDUP_OUTSTANDING_2026-08-22.md` § 8 names as blockers on that target's Gate 2 run.

Wire encoding for these rows: `group_repeats` is sent **minus one**, so "raw `0x02`" is a request
for three groups. `py-opendisplay` sends raw `0xFE` for indefinite and never sends a finite `0xFF`.

### EFR32BG22 (`efr32bg22-slc`) — Gate 2 blockers

- [ ] **D1 liveness:** send `0x0073` with every loop count and delay zero and `group_repeats` raw
      `0xFE`, then confirm the device still answers a subsequent command (e.g. FIRMWARE_VERSION)
      and advertising continues. Before the fix this wedged the superloop permanently.
- [ ] **D1 sentinel, raw `0xFE`:** an endless pattern runs until `LED_STOP`.
- [ ] **D1 sentinel, raw `0xFF`:** same — it must NOT stop immediately, which is what shipped.
- [ ] **D1 finite count:** raw `0x02` flashes exactly three groups, stops on its own, and a later
      `LED_STOP` reports "already idle" rather than stopping a live pattern.
- [ ] **D8 rail with `pwr_pin` configured:** an image upload renders after a cold panel bring-up;
      the 800 ms settle is charged once per switch-on, not per refresh.
- [ ] **D8 rail with `pwr_pin == 0xFF`:** the device must not drive PA0. Confirm on a board whose
      config leaves the rail unset.
      **Read this before the first flash:** the removed fallback drove PA0 whenever `pwr_pin` was
      unset, so a bench board that relied on it now sees no rail and the panel stays dark. That is
      the fix working, not a regression — put the real pin in the config.
- [ ] **D2 (build evidence, not on-air):** the flashed image is built from the same
      `shared/profiles.cmake` list the host suite compiles. Record the profile hash or the
      `opendisplay-bg22.cmake` SHA alongside the run.

### ESP32 (`s3-n16r8-extuart-debug`) — sentinel only

The yield floor and the rail fix are not ESP32 changes; only `repeat_forever` is.

- [ ] **Raw `0xFE`:** endless until `LED_STOP`.
- [ ] **Raw `0xFF`:** endless — before the change this stopped immediately, so this is the row
      that proves the fix rather than the absence of a regression.
- [ ] **Finite count:** raw `0x02` flashes exactly three groups and stops on its own.

### Nordic (`xiao_nrf52840`) — sentinel only

- [ ] **Raw `0xFE`:** endless until `LED_STOP`.
- [ ] **Raw `0xFF`:** endless — the row that proves the fix.
- [ ] **Finite count:** raw `0x02` flashes exactly three groups and stops on its own.

---

## Shared buzzer runner (2026-08-23)

Software candidate on `feat/buzzer-shared`; none of these rows is hardware-qualified. A build or
the 300-plus-check host suite is not evidence for a physical square wave.

- [ ] **ESP32 pitch and folding:** on `s3-n16r8-extuart-debug`, measure indices 120, 1 and 255 at
      the drive pin: 440.00 Hz, index 121's folded pitch, and index 231's folded pitch within the
      PWM clock tolerance. Confirm duty 0 selects 50% and a configured duty reaches the pin.
- [ ] **ESP32 schedule/cap:** a two-pattern melody preserves tones, rests, the 20 ms inter-pattern
      gap and outer repeats; a repeating 1275 ms step stops at 30,000 ms, not 5,000 ms.
- [ ] **ESP32 lifecycle:** a second melody preempts the first, session teardown leaves it running,
      and deep sleep/power-off silence it before the existing two-chirp shutdown alert.
- [ ] **Nordic pitch and folding:** on `xiao_nrf52840`, measure the same 120/1/255 vector. This row
      proves removal of the linear mapper; hearing a plausible tone does not.
- [ ] **Nordic schedule/cap:** the ESP32 timing vector matches, including the 30,000 ms cap and
      enable-pin polarity.
- [ ] **Nordic responsiveness:** commands and advertising continue during a 30-second melody, and
      a new melody preempts it without leaving the software square-wave timer armed on the old pin.
- [ ] **BG22 build exclusion:** the production map has no `od_buzzer*`, tone state or 256-byte
      melody buffer, and RAM remains at the pre-promotion value. This is build evidence, not on-air.

---

## Shared time HAL Phase 1

- [ ] ESP32: exercise the D-FF clock path and verify the retained 50 us setup/hold timing.
- [ ] Nordic: exercise the `bb_epaper` busy-wait path on each available board class after the
      shared-name migration.
- [ ] EFR32BG22: compare `od_hal_uptime_ms()` with a known hardware interval. Production currently
      has no caller and section GC removes the adapter, so use a temporary instrumented build or
      defer this row until the first linked consumer; an unchanged production image cannot qualify
      it.

---

## Shared logging promotion

- [ ] ESP32 UART profile: normalized boot-to-idle bytes, 232-byte normal/raw boundaries, concurrent
      producers and settled flush match the pre-cutover behavior.
- [ ] ESP32 stdout profile: the same normalized bytes and settled flush reach the configured IDF
      console transport.
- [ ] Nordic `xiao_nrf52840`: normalized boot-to-idle bytes, 253-byte normal / 255-byte raw
      boundaries, concurrent producers and settled `log_flush()` reach USB CDC.
- [ ] Nordic nRF54 class: repeat the native-log transport check on each available board class.
- [ ] BG22: console bytes remain unchanged. The software image contains no shared logger/HAL symbol
      or state and remains 250,196 B flash / 32,284 B static RAM; those build facts are not hardware
      evidence.

---

## Transfer Phase 1 — shared `od_zlib_pump`

These rows apply to the Phase 1 candidate introduced after the dated transfer results below; an
older compressed upload does not qualify the shared pump.

- [x] ESP32 tinfl profile: compressed direct, partial and PIPE through refresh — cleared 2026-08-18
- [x] ESP32 portable-inflater profile: compressed direct, partial and PIPE through refresh —
      cleared 2026-08-18
- [x] Nordic nRF54 class: compressed direct, partial and PIPE through refresh — cleared 2026-08-18
- [x] Nordic `xiao_nrf52840`: compressed direct, partial and PIPE through refresh — cleared
      2026-08-18
- [x] EFR32BG22: compressed direct through refresh — cleared 2026-08-18
- [x] Each exercised target: truncated/failed stream aborts, then a fresh compressed transfer
      succeeds — cleared 2026-08-18

Phase 1 was marked cleared by project direction on 2026-08-18. Phase 2 direct/partial production
cutover is unblocked; its own per-target hardware gates remain mandatory.

---

## Transfer Phase 2 — ESP32 steps 10a/10b

The ESP32 software candidate replaces `0x70`, `0x71`, `0x72` and `0x76` with shared `od_xfer`.
These rows are new evidence requirements; the older transfer results below do not qualify it.

- [ ] FastEPD: plaintext and encrypted raw/compressed direct through refresh
- [ ] bb_epaper: plaintext and encrypted raw/compressed direct through refresh
- [ ] Partial: etag match/mismatch, valid/invalid rectangles, both plane boundaries and failure
      clearing the etag
- [ ] Replacement in both directions: PIPE START aborts live legacy transfer; legacy START aborts
      live PIPE; the displaced owner's DATA/END is inert; a fresh transfer then succeeds
- [ ] Disconnect/reconnect during direct and partial, followed by a successful authenticated push
- [ ] END ACK observed before refresh begins; refresh success and timeout paths observed
- [ ] Plaintext LAN and TLS-LAN direct, including a 4,092-byte DATA chunk; LAN disconnect affects
      only a LAN-owned transfer

The Nordic software candidate was implemented by project direction before these ESP32 rows were
recorded. That sequencing exception is not hardware evidence and does not qualify either target.
The bidirectional-replacement row above qualifies ESP32 only. Nordic's 10a change must add and run
its own row for the same two directions; ESP32 evidence cannot qualify Nordic's separate interim
adapter/PIPE arbitration.

---

## Transfer Phase 2 — Nordic steps 10a/10b

The Nordic software candidate replaces `0x70`, `0x71`, `0x72` and `0x76` with shared `od_xfer`.
All rows are new evidence requirements; builds and the older transfer results below do not qualify
them.

- [ ] nRF54 class: plaintext and encrypted raw/compressed direct through refresh
- [ ] `xiao_nrf52840`: plaintext and encrypted raw/compressed direct through refresh
- [ ] Partial: etag match/mismatch, valid/invalid rectangles, both plane boundaries and failure
      clearing the etag
- [ ] Nordic replacement in both directions: PIPE START aborts live `od_xfer`; legacy START aborts
      live target PIPE; the displaced owner's DATA/END is inert; a fresh transfer then succeeds
- [ ] Disconnect/reconnect during direct and partial, followed by a successful authenticated push
- [ ] END ACK observed before refresh begins; refresh success and timeout paths observed
- [ ] Shared-policy normalizations: an etag-less successful full refresh clears the prior etag;
      controller-plane incomplete END refuses; packed-row incomplete END proceeds to refresh

The BG22 software candidate was implemented by project direction before these rows were recorded.
That sequencing exception is not hardware evidence. The bidirectional-replacement row is
Nordic-specific and cannot inherit the ESP32 result.

A `xiao_nrf52840` flash of post-step-11 HEAD on 2026-08-19 completed an encrypted PIPE upload and
a config read and config write (recorded in the board section below). That shows the promoted
routing did not regress the PIPE and command paths; it exercises none of the `0x70`/`0x71`/`0x72`/
`0x76` rows above, which stay open.

---

## Transfer Phase 2 — EFR32BG22 step 10a

The BG22 software candidate replaces `0x70`, `0x71`, `0x72` and capability-off `0x76` with shared
`od_xfer`. All rows are new evidence requirements; builds and the Phase 1 result do not qualify
them.

- [ ] Plaintext and encrypted raw direct through refresh
- [ ] Plaintext and encrypted compressed direct through refresh
- [ ] Controller-plane split crossed in one DATA frame; both planes arrive in controller order
- [ ] Disconnect/reconnect during direct, followed by a successful authenticated push
- [ ] END ACK observed on air before refresh begins; refresh success and timeout paths observed
- [ ] TX-report failure and two-second timeout: no refresh, panel powered off, issuing connection
      closed, replacement connection left open
- [ ] `CMD_PARTIAL_WRITE` returns `FF 76 02 00`; map confirms no partial or displayed-etag state
- [ ] Shared-policy normalization: controller-plane incomplete END refuses, while packed-row
      incomplete END proceeds to refresh

BG22 has no PIPE, so it has no bidirectional legacy/PIPE replacement row and no Phase 2 10b debt.

---

## Transfer Phase 3 — PIPE

The shared-machine software candidate was implemented by project direction on 2026-08-20 while
the Phase 2 and Nordic SPIM hardware entry rows remained open. No compatible hardware was
available in the implementation environment. That sequencing exception is not qualification:
every row below requires new post-promotion evidence, and no Phase 1, Phase 2 or earlier PIPE run
qualifies it.

Software evidence captured 2026-08-20: the host suite passes 58/58 under GCC, Clang and
ASan/UBSan; W=32, W=16 and capability-off PIPE builds pass; the five pre-auth fuzz targets and
the pinned py-opendisplay 7.14.0 corpus pass; and all 11 ESP32 configurations, all three Nordic
boards and the BG22 headless image build. Handwritten production source is +938/−2,960 lines
(net −2,022); test/tool source is +1,517/−1,060 (net +457). The reorder array is 8,118 B at
W=32 and 4,182 B at W=16, plus 20 B of PIPE state in either profile. The BG22 link contains only
the three small `od_pipe_*` entry points, no
PIPE state/reorder symbol, and remains 250,292 B flash / 32,284 B static RAM (480 B headroom).
Board-only throughput, retransmission, refresh-time and stack-high-water measurements remain
unavailable and do not qualify any row below.

### ESP32 (`s3-n16r8-extuart-debug`, plus a classic ESP32 W=16 board if available)

- [ ] Plaintext and encrypted full-frame PIPE, raw and compressed, through refresh
- [ ] PIPE-partial: region streamed, partial refresh, new etag committed; etag mismatch refused
- [ ] Forced loss, reorder and retransmission; gap SACK observed and transfer recovery
- [ ] Negotiated on-wire maximum in plaintext and encrypted sessions: boundary accepted and
      one-byte-over refused, including a CCM frame whose decrypted body alone fits
- [ ] Tail below cadence completes without a stall
- [ ] Sequence wraps past 255 inside one transfer
- [ ] END ACK observed on air before refresh; refresh success and timeout both observed
- [ ] `OD-S1` replay injection via `dispatch-gate`, including the corrupted-tag control
- [ ] Inactive/fatal/zero-length DATA is silent; inactive/fatal END answers plaintext `FF 82`
- [ ] Second-connection DATA during a live PIPE is inert
- [ ] LAN and TLS-LAN `0x80`/`0x81`/`0x82` produce the exact four-byte refusals with the deployed
      seal-or-plain choice and leave a live BLE-owned transfer untouched
- [ ] Replacement in every direction: PIPE↔PIPE, PIPE↔legacy, and malformed BLE PIPE START
      displacing the live owner; displaced owner inert and a fresh transfer succeeds
- [ ] Disconnect mid-PIPE, reconnect, re-authenticate and complete a fresh upload
- [ ] Classic ESP32 `OD_PIPE_MAX_W=16`: negotiated W=16 honoured and reorder queue sized 17

### Nordic (`xiao_nrf52840` mandatory; one nRF54-class board)

- [ ] Plaintext and encrypted full-frame PIPE, raw and compressed, through refresh
- [ ] PIPE-partial: region streamed, partial refresh, new etag committed; etag mismatch refused
- [ ] Forced loss, reorder and retransmission; gap SACK observed and transfer recovery
- [ ] Negotiated on-wire maximum in plaintext and encrypted sessions: boundary accepted and
      one-byte-over refused, including a CCM frame whose decrypted body alone fits
- [ ] Tail below cadence completes without a stall
- [ ] Sequence wraps past 255 inside one transfer
- [ ] END ACK observed on air before refresh; refresh success and timeout both observed
- [ ] `OD-S1` replay injection via `dispatch-gate`, including the corrupted-tag control
- [ ] Inactive/fatal/zero-length DATA is silent; inactive/fatal END answers plaintext `FF 82`
- [ ] Replacement in every direction: PIPE↔PIPE, PIPE↔legacy, and malformed BLE PIPE START
      displacing the live owner; displaced owner inert and a fresh transfer succeeds
- [ ] Disconnect mid-PIPE, reconnect, re-authenticate and complete a fresh upload
- [ ] Compressed PIPE is accepted when the config lacks the streaming-decompression bit
- [ ] One nRF54-class board repeats full raw/compressed PIPE, forced reorder/recovery and the
      negotiated-frame bound

### EFR32BG22 (`efr32bg22-slc`)

- [ ] `0x80` answers `FF 80 04 00`; `0x81` and `0x82` answer nothing
- [ ] Link map confirms zero PIPE state/reorder storage and static RAM at or below the recaptured
      Phase 0 baseline

---

## Transfer Phase 4 — NFC (0x0083)

The shared-machine software candidate was implemented by project direction on 2026-08-21, steps 5,
6 and 7 in sequence, while every hardware row below remained open. **NO NFC-ENABLED HARDWARE
EXISTS IN THIS FLEET**: no board carries an antenna, so none of these rows is merely awaiting a
free bench — they await hardware that has to be built or bought. That is a stronger form of open
than the Phase 2 and Phase 3 exceptions above, and it is why every row here is release debt rather
than a queued task.

The sequencing exception is not qualification. No Phase 1-3 run, and no amount of host coverage,
qualifies any row below.

Software evidence captured 2026-08-21: `tools/check.sh --targets` passes 33/0/0 with no skip; the
shared suite is 336 checks at `OD_CAP_NFC=1` and 32 at `OD_CAP_NFC=0`; `tools/mutate_nfc.sh`
reports all seven mandatory mutations detected; the Nordic and BG22 reference fixtures frozen at
step 1 pass against the shared machine on every input except the deliberate N1/N4/N6 changes, each
recorded in `docs/DIVERGENCE_MATRIX.md`. Nordic recovered 762 B of RAM at its cutover; BG22
recovered 512 B of heap (`heap_size` 0x2ad0 -> 0x2cd0).

X3's baseline is **measured, not inferred**: a clean BG22 build at `def82a1` — main before
`od_nfc.c` existed — gives `heap_size` **0x2cf0 (11,504 B)**, against 0x2cd0 (11,472 B) now. That
is **32 B of heap lost**, inside the 64 B ceiling. An earlier arithmetic estimate said 16 B and was
wrong by half the figure, which is the reason the plan asks for a build rather than a subtraction.

### Step 9 consolidation, 2026-08-22

Every figure below is a build at this branch's HEAD against a build at `def82a1`, not a running
total carried forward from the per-step commits.

| | flash | RAM | what it measures |
|---|---|---|---|
| **`xiao_nrf54lm20a`** | **+112** | **−234** | **the promotion alone** — NFC was already enabled here at `def82a1`, so nothing else moves |
| `xiao_ble` (nRF52840) | +7,660 | +2,602 | promotion **plus** enabling `CONFIG_NFC_T2T_NRFXLIB` |
| `xiao_nrf54l15` | +8,068 | +2,598 | same |
| `efr32bg22-slc` | +92 | `heap_size` −32 | `data + bss` unchanged at 32,284 **by construction** |

**lm20a is the number to quote for the promotion**, and it was nearly not measured: an earlier
draft of this table claimed it had no Phase 4 baseline because NFC was already on there. That is
exactly backwards — being already on is what makes it the only board where the library is not
confounding the result. It costs 112 bytes of flash and **saves 234 bytes of RAM**, which is the
244-byte response buffer and the 512-byte assembler leaving, less what the shared state costs.

The other two Nordic figures are dominated by enabling the T2T library, which is a capability
decision rather than a promotion cost. Netting them together would make the promotion look
expensive on Nordic when the library is what costs.

Its link map agrees: no `s_nfc_write_chunk` or `od_cmd_app_nfc`, both `od_nfc_*` entry points
present, and the two `od_nfc_app_*` seam symbols present because Nordic implements the tag.

**Source, whole phase, counted by CATEGORY rather than by path.** The distinction matters because
`targets/esp32-idf/tools/od-device-cli.py` lives under `targets/` and is a tool, not firmware:

| | added | removed |
|---|---|---|
| shared production (`od_nfc.{c,h}`, `od_nfc_app.h`) | 353 | 0 |
| shared wiring (`od_dispatch{.c,_ops.h}`, `od_caps.h`, `od_core.{c,h}`, `od_cmd_app.h`, `od_txq.h`, `od_xfer.c`) | 34 | 14 |
| build wiring (`shared/sources.cmake`) | 8 | 0 |
| **target firmware** | **82** | **347** |
| tests and tools | 2,196 | 102 |

Target firmware is a net deletion of 265 lines, and even that understates it: what left was wire
parsing, a chunk assembler and reply construction; what arrived is adapter and build wiring. Tests
and tools are reported separately per § 10 and deliberately not netted against production.

**Zero, and checked rather than assumed.** No target-side NFC assembler, parser, hook or
wire-response literal survives: the § 8 ratchet greps eight symbols over `targets/**`, and
`RESP_NFC_ENDPOINT` appears in no target `.c`/`.cpp`. All 11 ESP32 board images carry both
`od_nfc_*` entry points, no `s_nfc`, and no `od_nfc_app_*` reference.

**Software gate at step 9 close: `tools/check.sh --targets` 34 passed / 0 failed / 0 skipped.**
The 33/0/0 recorded above was the step 5-7 figure; the extra check is the § 8 ratchet that landed
with the dispatch reroute.

**Hardware rows: 0 passed, 16 open.** Not one `0x0083` row has run, and none can here.

### Phase 5 close, 2026-08-22

**Step 10 — deletion inventory: empty, and that is the result rather than an omission.** X1 forbids
speculative deletion, so the inventory was built from evidence and came back with nothing to
delete. Checked: orphaned declarations in target headers (none); target constants that lost
their last user (`OD_NFC_ASSEMBLY_MAX` and `OD_NFC_READ_MAX` are live in
`shared/core/od_nfc.{c,h}`;
**`OD_PIPE_MAX_PAYLOAD` has no user left anywhere** — the Nordic NFC cutover removed
its last one, and it stays because `opendisplay_protocol.h` is a byte-for-byte copy of the
canonical header and is not ours to edit, which is a different reason from being referenced);
target-side response literals for promoted opcodes (none);
target-local transfer, pump, PIPE or NFC state (none); build entries naming deleted files (none —
and a real one would fail the build); and every scaffold the plan created, all still referenced.

The reason is that each cutover deleted its own orphans in the same commit: `7c4fcb5` removed
`od_cmd_nfc.h`, `5b6ecbf` removed `od_cmd_nfc.c`, and the BG22 and ESP32 wrappers went from files
that keep their other hooks. Dead *code* cannot survive `-Wall -Wextra -Werror`, so what a sweep
could still find is dead declarations and macros — and the ones present are pre-existing driver
registers and config-packet constants that no promotion orphaned. Deleting those would be exactly
the speculative deletion X1 rules out.

**Step 12 — every LINK MAP read, not merely produced.** The plan asks for maps, and a symbol
table is not one: `nm` says a symbol exists, the map says which object contributed it. That
difference is the whole value here, because the question is whether PIPE and NFC state comes from
`shared/` or from a target.

Criterion: for each `.bss.<symbol>` entry, the contributing object named on that line — or on its
continuation line, which is how the format wraps a long section name. All 15 maps, every image:

| image | `s_pipe` | `s_reorder` | `s_nfc` |
|---|---|---|---|
| `efr32bg22-slc` | — | — | `od_nfc.c.obj` |
| Nordic × 3 | `od_pipe.c.obj` | `od_pipe.c.obj` | `od_nfc.c.obj` |
| ESP32 × 11 | `od_pipe.c.obj` | `od_pipe.c.obj` | — |

Every occurrence is attributed to a `shared/core/` object; no target object contributes any of
them. The two dashes are the capability-off arms carrying no state at all — BG22 declines PIPE,
ESP32 declines NFC — which is the claim `nm` alone cannot make, since a symbol's absence looks the
same whatever removed it.

A first pass at this table read a one-line context window and attributed Nordic's `s_pipe` to
`od_nfc.c.obj`, because the next map entry happened to be `s_nfc`. The criterion above is what
fixed it, and it is stated so the next reader can reproduce it rather than trust the
result.

**Step 13 — release evidence.** Per-unit measurements are recorded above under Step 9
consolidation, per phase and per target rather than for a final squash. The § 9 matrix stands at
**0 passed, 16 open**, every open row named with what it needs. No row is closed by this phase, and
none is closed by any amount of the software evidence above it.

### Nordic — needs a board with an NFC antenna fitted

Enabling `CONFIG_NFC_T2T_NRFXLIB` (2026-08-21) makes the tag real on all three boards; none has an
antenna, so none of this has run.

- [ ] Inline write ≤ 120 bytes, read back by an independent NFC reader
- [ ] Chunked 512-byte write **as `OD_NFC_REC_RAW_NDEF`**, read back the same way. Every other
      record type is an NDEF short record capped at 255 payload bytes and is refused at END with
      `0x03` — that refusal is correct behaviour, not a defect
- [ ] A 218-byte read, in plaintext and encrypted sessions
- [ ] A 219-byte **verbatim or well-known** record truncated to 218 — Nordic's adapter behaviour,
      asserted here and nowhere else (N2b)
- [ ] A 219-byte **MIME** record **refused** with `0x02`. Nordic does not truncate uniformly: its
      MIME arm refuses anything that would not fit whole (`opendisplay_nfc.c`, the
      `out_pack > out_max` test), so a row exercising only truncation would report the adapter as
      uniform when it is not
- [ ] BLE disconnect mid-assembly, then a fresh START from a new connection
- [ ] Tag hardware absent or failing, answering `0x02` / `0x03`
- [ ] The § 3.4 divergence-3 frame (`00 83 01 00 FF FD`) answered `0x01`, device still alive
- [ ] **READ-path stack high-water** (N7): the response buffer is now a 224-byte stack local
      nesting with `od_reply()`'s sealed buffer
- [ ] A config that assigns P0.09 or P0.10 is refused. **That is the whole row**, because the
      complementary half has nothing to run against: every shipping nRF52840 image enables NFCT
      unconditionally (`xiao_ble_nrf52840.conf:70`, `.overlay:235`), so no GPIO-owning image
      exists. An earlier draft of this row claimed one and also asked for a config-driven form;
      both were wrong. UICR NFCPINS latches the pads at reset, so handing them to GPIO needs an
      image that disables NFCT *and* sets `CONFIG_NFCT_PINS_AS_GPIOS`, plus a UICR erase and
      reboot to take effect. That variant is unbuilt and unspecified — see `docs/FOLLOWUPS.md`.
      The refusal itself is host-tested (`tests/host/nordic_nfc_pins_test.c`), so this row is on-air
      confirmation, not first coverage

### EFR32BG22 — needs a board with a TNB132M fitted

- [ ] Inline write ≤ 120 bytes, read back by an independent NFC reader
- [ ] Chunked write **above 240 bytes** as `RAW_NDEF`: capture the I2C block sequence and read back
      every byte. The write loop's block offset truncates at `i == 15`
      (`opendisplay_ble.c:1526-1530`), so this is an **addressing investigation**, not a presumed
      pass. 240 must pass, 241 is the minimal reproducer, 512 is the deployed-scale case
- [ ] The controller limit: RAW_NDEF records of 128 and 129 raw bytes read back through the
      bespoke BLE tool — 128 whole, 129 refused `0x02`. Confirm the boundary against the adapter
      first; `sizeof(s_od_nfc_read_data)` is the constant, but which length is compared to it is
      what the row proves
- [ ] Both of N4's changed input classes confirmed on the wire (now `0x01`, was `0x03` / `0x05`)
- [ ] `heap_size` measured on the flashed image against X3's ceiling

### ESP32-S3 — capability-off

- [ ] `0x0083` probed in plaintext and encrypted sessions draws nothing, the link is not held
      open, and the client raises `NfcNotSupportedError`

### The bespoke BLE READ tool — built, and no longer the blocker

`py-opendisplay` implements no `NFC_SUB_READ` (`commands.py:103`, "not built here"), so every READ
row needed a sender and decoder written first. **That exists**:
`targets/esp32-idf/tools/od-device-cli.py nfc-read`, covered by `tests/host/nfc_read_tool_test.py`
(41 checks, `bleak` stubbed). `--expect-silence` drives the capability-off row, and it requires a
`FIRMWARE_VERSION` canary to answer before it will accept silence — a dead notify path is silent
too, and without the control the flag would pass on a board that was not answering at all.

**Hardware is still the blocker; the tool is not.** And an independent NFC reader remains stimulus
and oracle only: it talks to the tag directly, bypassing `CMD_NFC_ENDPOINT`, dispatch, the seam and
response framing. **A read row backed by a reader alone is not a pass.**

## ESP32-S3 (`s3-n16r8-extuart-debug`, FastEPD)

- [x] Two fresh authentications — 2026-08-17
- [x] Encrypted command round-trip — 2026-08-17
- [x] Encrypted PIPE upload through refresh — 2026-08-17
- [x] `CMD_PARTIAL_WRITE` (0x76) — 2026-08-17
- [x] Config read — 2026-08-17
- [x] Config write, write + reload-after-write confirmed — 2026-08-17
- [x] Plaintext (unencrypted) run of the above (PIPE, `CMD_PARTIAL_WRITE`, config read/write) — 2026-08-17
- [x] Direct/PIPE END: ACK (`00 82`) observed on air *before* the multi-second physical refresh
      begins — 2026-08-17, confirmed directly in device log
- [ ] Disconnect/reconnect, then re-authenticate — confirm a *new* session succeeds
- [ ] Config read under TX backpressure (CCCD-disabled induced backpressure, per
      `PLAN_OD_DISPATCH_C12_2026-08-16.md` §7.2's row)
- [ ] LED / buzzer / READ_MSD / FIRMWARE_VERSION
- [ ] No-session and decrypt-failure plaintext gate (`{00,cmd,FE}` / `{00,cmd,FF}` visible
      unencrypted)
- [ ] Unknown opcode silence; 245-byte value ceiling NACK
- [ ] NFC 218/219-byte ceiling
- [ ] Plaintext LAN and TLS-LAN commands, PIPE-over-LAN refusal, 4092-byte LAN DIRECT_WRITE
- [ ] Long idle + accepted-traffic cycle (owner-clock stamp)
- [ ] `OD-S1` replay injection via `dispatch-gate` (see § below)
- [ ] Uncompressed image push
- [ ] Interrupted-transfer recovery

## Nordic `xiao_nrf52840`

- [x] Encrypted PIPE upload, image displayed correctly — 2026-08-17; re-run at current HEAD
      (post Transfer Phase 2 step 11) — 2026-08-19
- [x] Advertising resumes reliably after disconnect, repeated connect/disconnect cycles —
      2026-08-17 (the `od_adv_control` wiring fix, PR #40)
- [x] Direct/PIPE END: ACK observed on air before refresh begins — 2026-08-17, confirmed in
      device log
- [x] Config write + reload verified — 2026-08-15 (Gate 2 pass), re-run 2026-08-19
- [x] Disconnect/reconnect, then re-authenticate — confirm a *new* session succeeds — 2026-08-19
      (BLE dropped mid-PIPE, reconnected, new session carried a fresh upload through refresh)
- [x] Config read, re-run against current HEAD — 2026-08-19
- [x] Config write, re-run against current HEAD — 2026-08-19
- [x] Config reload-after-write and reboot-persist, re-run against current HEAD — 2026-08-19
- [ ] Config read under TX backpressure
- [ ] `CMD_PARTIAL_WRITE`
- [ ] LED / buzzer / READ_MSD / FIRMWARE_VERSION
- [ ] No-session and decrypt-failure plaintext gate
- [ ] Unknown opcode silence; 245-byte value ceiling NACK
- [ ] NFC 218/219-byte ceiling
- [ ] Plaintext (unencrypted) PIPE upload and config round-trip
- [ ] PIPE-partial (0x0080 flags bit1): START accepted, region streamed, partial refresh, new
      etag committed. Refused on every attempt until 2026-08-19 — the START handler passed the
      PIPE flags word to a validator that only defines the 0x76 partial flags, so bit1 always
      read as an unknown flag (`FOLLOWUPS.md` § 6). Fixed and host-tested; never run on a board.
- [x] Successful PSA key-replacement / re-authentication cycle — 2026-08-19, one cycle via the
      mid-PIPE disconnect above (the prepared-key slot released and re-prepared; a repeated
      many-cycle soak has not been run)
- [ ] `OD-S1` replay injection via `dispatch-gate`
- [x] Interrupted-transfer recovery — 2026-08-19, PIPE abandoned mid-transfer by BLE disconnect;
      a fresh upload then completed through refresh

**Unretired pre-promotion hardware observation:** the 2026-08-17 report records small/sub-window
PIPE uploads stalling indefinitely and states that ESP32 behaved identically. Later source and
sender-probe analysis conflicts with that diagnosis, but no hardware transcript retired it.
Hardware was unavailable on 2026-08-20, and shared D11 deliberately preserved the target cadence
policy without adding a timer. Treat the stall as live until the post-promotion tail-below-cadence
rows above close with on-air evidence.

## `xiao_nrf54l15` / `xiao_nrf54lm20a`

- [x] Transfer Phase 1 nRF54-class compressed direct, partial and PIPE gate — cleared 2026-08-18.
- [ ] Remaining board-specific boot, storage, NFC and full migration matrix.

## `efr32bg22-slc`

- [x] Transfer Phase 1 compressed-direct pump gate — cleared 2026-08-18.
- [ ] Remaining C13, boot/storage, NFC and capability-off migration matrix. No PIPE or
      `CMD_PARTIAL_WRITE` is implemented on this target.

## WiFi/LAN transport

- [ ] Never hardware-verified on any target.

---

## `OD-S1` replay injection (mandatory per-target, once available)

Per `PLAN_OD_DISPATCH_C12_2026-08-16.md` §7.5 — this is the one row that cannot be satisfied
by a normal successful upload, on either target:

1. Authenticate, open an encrypted PIPE transfer with a small `ack_every`.
2. `dispatch-gate` seals one valid `0x0081` DATA frame, sends it, retains the exact raw bytes.
3. The identical sealed bytes are written again while the transfer is live (same session id/
   counter) — must reach the application as a replay.
4. Require: no notification attributable to the replay, one throttled replay/nonce telemetry
   record, no integrity-strike/session teardown, successful continuation through END with a
   rendered refresh.
5. In a fresh transfer, send a newly-counted `0x0081` with a corrupted tag — require the normal
   plaintext hard NACK (proves the capture window could observe a response at all).

The raw duplicate must be byte-identical; re-encrypting the same sequence number under a new
CCM counter tests PIPE duplication, not this. Tooling: `targets/esp32-idf/tools/od-device-cli.py
dispatch-gate`.

---

## Release matrix — per silicon/boot/storage/transport class

From `plans/PLAN_MIGRATION_ENDGAME_2026-08-17.md` §2.1. One unified target directory can cover
several rows here; a single ESP32-S3 does not stand in for classic ESP32, C3, or C6, and one
nRF52840 does not stand in for either nRF54 part.

| Row | Boot/storage distinction | Status |
|---|---|---|
| ESP32-S3, one PSRAM board | IDF 2nd-stage, OTA-capable N8/N16/N32 partitions | Partial — see above, WiFi/LAN and uncompressed push outstanding |
| ESP32-C6 N4 | IDF 2nd-stage, no PSRAM, single-app partition | Not started |
| ESP32-C3 N4/N16 | IDF 2nd-stage, no PSRAM, C3 radio/flash config | Not started |
| Classic ESP32 N4 | IDF 2nd-stage, no PSRAM, classic peripheral path | Not started |
| `xiao_nrf52840` | Adafruit UF2, EPD rail path | Partial — see above |
| `xiao_nrf54l15` | MCUboot | Phase 1 nRF54-class pump gate cleared; full board row open |
| `xiao_nrf54lm20a` | MCUboot, distinct DTS/pinctrl, second core | Phase 1 nRF54-class pump gate cleared; full board row open |
| `efr32bg22-slc` | Gecko Bootloader + AppLoader, NVM3, 32 KB RAM | Phase 1 compressed-direct pump gate cleared; full board row open |

Each row, when run, should record: board id, exact release SHA, bootloader, partition/storage
layout, tool and host versions, raw transcript, device log, per-observation PASS/FAIL — per
`PLAN_OD_DISPATCH_C12_2026-08-16.md` §7.1's evidence rules (still binding; only the SHA-pinning
*gate* is dropped, not the record-keeping standard).

---

## Config storage seam — `od_config_store` + `od_hal_nvs` (2026-08-24)

Landed as commit `490415d`; **nothing here is hardware-qualified**. `tools/check.sh --targets`
passes 40/0/0 and the host suite covers the framing against a fake medium, but a host test cannot
qualify silicon and **a config a device cannot read back is a bricked configuration** — these rows
gate the promotion, not the build.

Two behaviour changes make a device REFUSE a record it used to accept
(`DIVERGENCE_MATRIX` § 17), and both are Nordic-only, so a Nordic board that will not boot on its
existing config is the expected first symptom rather than a surprise: a physically truncated
record now fails, and a record larger than the caller's buffer is refused instead of ignored.
Re-provision rather than debug if that appears.

### Every target

- [ ] **Write, reboot, reload:** write a config over BLE, confirm it takes effect, power-cycle, and
      confirm the device comes back on the stored config and the panel renders from it.
- [ ] **Unrecognised version still loads:** rewrite the stored record's `version` field out of band
      to 2 and confirm the device still boots on it. This is the behaviour § 4 says must NOT
      change — every target writes 1 and none reads it back, and enforcing it would strand a
      device on a record it has been using.
- [ ] **A corrupted record boots on defaults:** flip one payload byte out of band and confirm the
      CRC rejects it and the device comes up unconfigured rather than on garbage.

### ESP32-S3 (`s3-n16r8-extuart-debug`)

- [ ] **Factory-provisioned config loads at first boot** — the path that passes a flash pointer
      into `saveConfig`. The core copies it into the workspace; confirm a
      `OPENDISPLAY_FACTORY_CONFIG_HEX` build comes up configured on a blank NVS.
- [ ] **Secure erase still wipes:** `secureEraseConfig()` now drops the HAL's cached record
      unconditionally before touching NVS. Confirm a subsequent read reports an unprovisioned
      device *without* a reboot — the cache is the only thing that could have hidden that.

### Nordic (`xiao_nrf52840` mandatory)

- [ ] **Write, reload in place, reboot-persist** — the three the 2026-08-19 run covered, re-run
      against the shared framing.
- [ ] **A failed clear reports failure.** `clearStoredConfig()` used to discard
      `settings_delete()`'s result and return true regardless. Inducing a settings delete failure
      on the bench may not be possible; **if it is not, say so here and move this row to a
      production-source fault test** rather than leaving it open forever.
- [ ] **A failed write leaves the previous config readable** (S1a). Same caveat: if a
      `settings_save_one` failure cannot be induced on hardware, this belongs in a fault test and
      this row should say that instead of sitting open.

### EFR32BG22 (`efr32bg22-slc`) — needs a J-Link attached

- [ ] **Write, reload, reboot-persist** through the union overlay.
- [ ] **An over-size declaration is refused at the start frame with nothing stored** — refuse,
      never truncate, because the 2048-byte cap is one a host cannot interrogate
      (`MEMORY_CONSTRAINTS.md` item 3).
- [ ] **An in-flight chunked transfer survives a refused save.** The header write eats the four
      live assembler state words, so every refusal that can still leave a transfer open has to
      happen before it. Covered by `tests/host/silabs_storage_test.c` and falsifiable there, but
      unexercised on silicon.
- [ ] **RAM unchanged:** `heap_size` and `data + bss` against the pre-change image. Measured at
      32,284 B static RAM with 480 B headroom (flash 250,148 -> 250,552 B). The overlay existing is
      what makes this promotion affordable there, and losing it would be invisible except here.


---

## Sensor/I2C seam — steps 1-4 (2026-08-24)

Nothing here is hardware-qualified. `tools/check.sh --targets` passes 40/0/0 and the shared
resolver and I2C contract are host-tested and mutation-checked, but **the gate cannot see pin
order or bus selection**: step 4 shipped, and review caught, a swapped SDA/SCL argument order that
would have crossed the lines on every configured ESP32 bus while passing every automated check.
Treat the rows below as the only thing standing behind that.

### ESP32-S3 (`s3-n16r8-extuart-debug`)

- [ ] **SDA and SCL are the right way round.** Scope or logic-analyse one transaction, or simply
      confirm any I2C device answers at all — SHT40, BQ27220, GT911 or the AXP2101 probe. A
      crossed pair fails every device on the bus, so one working device clears this row.
- [ ] **SHT40, BQ27220, GT911 and AXP2101 all behave as before** the operations gained a bus
      argument.
- [ ] **Bus switching:** two configured `DataBus` entries selected in sequence, and returning to
      the first restores its pins **and its speed** — the speed half is newly part of the
      selection identity and was previously ignored on a same-pins switch.
- [ ] **A sensor or touch entry with `bus_id == 0xFF` is not probed**, and is not attached to
      bus 0.
- [ ] **Out-of-order `DataBus` records bind correctly**: declare instance 1 before instance 0 and
      confirm each device talks on its own pins. This is the § 14 defect the shared resolver
      fixes, and it is invisible on an in-order config.
- [ ] **A duplicated `instance_number` yields no device** rather than an arbitrary one.

### Nordic (`xiao_nrf52840`)

- [ ] **Touch still reports contacts** after its private bit-banger was folded into
      `opendisplay_i2c.c` — the plan requires this before the cutover proceeds past step 2.
- [ ] **BQ27220 and nPM1300 reads still return plausible values.** Their repeated START gained a
      half-period `tLOW` that it never had; the change is meant to be an improvement, and this
      row is what shows it is not a regression.
- [ ] **GT911 bit timing unchanged**: touch pins 100 kHz explicitly rather than taking
      `bus_speed_hz`, which reproduces the private engine's fixed 5 µs. A config declaring
      400 kHz must not change the touch clock.
- [ ] **A clone that stretches the clock now fails the transfer** rather than sampling garbage,
      and five consecutive failures disable touch. Confirm ordinary hardware never reaches that.

### EFR32BG22 NFC transport — step 9, a software candidate (2026-08-24)

**No board in this fleet carries a TNB132M**, so every row here is open on arrival, exactly as
Transfer Phase 4's NFC rows were. Merged code is not evidence.

What *is* checked, and what it is worth: `tests/host/silabs_i2c_trace_test.c` binds the production
`hal/od_hal_i2c.c` to fake GPIO and reads the **edge sequence** back — START and STOP detected as
SDA transitions while SCL is high, the repeated START in a block read, one clock per bit plus the
ACK slot, the address-NACK path, and that an unresolvable or ambiguous bus drives no edge at all.
That is real coverage of framing. It is not coverage of *timing*, of the TNB132M's response to it,
or of anything above the transaction.

The existing BG22 NFC host tests fake `od_nfc_app_read`/`write`, which sit **above** the transport
and say nothing about it. They must not be cited for these rows.

- [ ] **A tag reads.** `0x48` sub 0 returns plausible Attribute Information after the prime
      sequence.
- [ ] **A tag writes and reads back**, including the re-prime that the byte-offset window needs
      after an EEPROM write.
- [ ] **The edge pacing is unchanged.** The `sl_udelay_wait()` counts were extracted verbatim
      because they are what the deployed sequence was tuned against; a scope trace against the
      pre-change image is the only thing that shows the extraction preserved them.
- [ ] **The prime commands still work with their results discarded.** They always were discarded;
      the transactions now return a status that nothing reads, and that is deliberate — starting
      to check it would be a behaviour change on a transport nothing here can exercise.
- [ ] **Power sequencing and SCL/SDA parking still bracket a session.** Those stayed in the NFC
      adapter; only the transactions moved.

**Explicitly out of scope for any of these rows: stuck-bus behaviour.** This engine drives SCL
push-pull and only ever reads SDA, so it cannot detect clock stretching, and a held-low SDA reads
as ACK and as data 0. Do not write a row that asserts otherwise.
