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

## Shared time HAL Phase 1

- [ ] ESP32: exercise the D-FF clock path and verify the retained 50 us setup/hold timing.
- [ ] Nordic: exercise the `bb_epaper` busy-wait path on each available board class after the
      shared-name migration.
- [ ] EFR32BG22: compare `od_hal_uptime_ms()` with a known hardware interval. Production currently
      has no caller and section GC removes the adapter, so use a temporary instrumented build or
      defer this row until the first linked consumer; an unchanged production image cannot qualify
      it.

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

**Known live defect, not yet fixed:** small/sub-window PIPE uploads can stall indefinitely —
`opendisplay_pipe_write.cpp` only SACKs on an N-chunk cadence, with no time-based flush for a
trailing batch smaller than N, so the client's own probe timeout aborts a transfer the device
had actually completed. Diagnosed 2026-08-17, confirmed identical on ESP32
(`display_service.cpp:2780`). Not yet filed as a fix — file in `FOLLOWUPS.md` and re-check
this row before considering PIPE hardware-verified.

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
