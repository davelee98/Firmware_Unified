# Correctness and functionality review remediation plan

**Status:** proposed, 2026-08-22. Based on the repository-wide review at `9f76ed4` and the
required software gate `tools/check.sh --targets` passing 34/0/0. A green build is the baseline,
not evidence that the defects below are harmless.

**Objective:** close the review findings without weakening wire compatibility, target capability
boundaries, RAM ceilings, or the rule that hardware qualification is recorded only from hardware.
Every work package below has its own implementation boundary, regression tests, target builds and
exit criteria so it can land or revert independently.

**Repository boundary:** implementation changes in this plan land only in `Firmware_Unified`.
`../opendisplay-protocol` and `../py-opendisplay` are read-only references from this repository;
their required changes are explicit external follow-ups. Do not edit, branch, commit or push either
sibling while executing this plan unless a later user instruction explicitly authorizes it.

---

## 1. Findings, ownership and disposition

| ID | Priority | Severity | Finding | Primary owner | Planned work package |
|---|---|---|---|---|---|
| R1 | P0 | BLOCKER | BG22 LED runner can spin forever on an infinite-repeat, zero-delay pattern | BG22 target | A2 |
| R2 | P0 | BLOCKER | FastEPD trusts configured geometry beyond its fixed native framebuffer allocation | ESP32 FastEPD adapter | A3 |
| R3 | P2 | HIGH / protocol | CCM nonces repeat across client-to-device and device-to-client directions | protocol + host + session | D1 |
| R4 | P0 | HIGH / evidence | BG22 host libraries compile a different `struct od_config` profile from firmware | host build and gate | A1 |
| R5 | P1 | MAJOR | Nordic accepts illegal config-start lengths; no target binds chunked config state to its initiating connection | shared config-write policy + all command adapters | B1 |
| R6 | P1 | MAJOR | All targets discard intermediate config ACK failure; Nordic also ignores final config ACK failure after persistence | shared config-write policy + Nordic persistence | B1 |
| R7 | P3 | MAJOR | Nordic decodes the canonical buzzer note index with a linear frequency scale | shared pure helper + Nordic/ESP32 adapters | B2 |
| R8 | P1 | MAJOR / field blocker / external | `py-opendisplay.deep_sleep()` sends hard-power-off opcode `0x0052`; canonical deep sleep is unreachable across the current fleet | py-opendisplay | D2 |
| R9 | P2 | MEDIUM | BG22 bypasses the rollover-safe 64-bit time HAL at multiple production call sites | BG22 adapters | C1 |
| R10 | P2 | MEDIUM / liveness | `bbepWaitBusy()` blocks for up to 35 s and reports timeout as success | panel integration | C2 |

Lower priority numbers execute first; independent external work may proceed in parallel. IDs remain
in numerical order for lookup, while the priority column records why R8 is scheduled before R7.

R1 and R2 are release blockers. R3 remains a security release decision until the protocol has a
direction-separated construction. R4 must land before any new BG22 host result is cited as evidence.
R8 outranks R7: the host currently cannot request canonical deep sleep successfully on any of the
three families, while R7 affects an optional peripheral on one family.

---

## 2. Global execution rules

1. **One finding family per commit series.** Do not mix LED, FastEPD, config, buzzer, time or panel
   wait changes. The protocol work is a separate cross-repository program.
2. **Tests precede or accompany the fix.** Each regression must fail against the current defective
   implementation or, where the production file cannot be safely hosted, must pin the rejected
   input and the pure policy that the adapter calls.
3. **No target-private wire reinterpretation.** Config frame shapes, opcode mappings and session
   derivations remain canonical. A target adapter may validate target hardware geometry, not
   redefine the wire format.
4. **No capability tax.** Capability-off BG22 remains free of PIPE/partial/buzzer state; ESP32
   FastEPD checks must not add code to configurations that do not compile `OPENDISPLAY_FASTEPD`.
5. **Failure must be truthful.** A function that cannot initialize hardware, queue an ACK, wait for
   a panel or persist data returns failure to its shared caller. Do not log and continue as success.
6. **Every merge candidate runs `tools/check.sh --targets`.** The summary must be 0 failed and
   0 skipped. Target build output and size changes are recorded in the commit or review note.
7. **Hardware rows remain open until exercised.** A host test, fake driver, target build or linked
   image cannot close a hardware row.
8. **Wire-visible changes update the permanent records in the same commit.** Add or amend the
   corresponding row in `docs/DIVERGENCE_MATRIX.md`, and open the exact evidence rows in
   `docs/HARDWARE_VERIFICATION_CHECKLIST.md` when candidate code lands. The plan-local matrix in
   section 8 is routing, not release evidence.

---

## 3. Phase A — restore trustworthy evidence and remove remote blockers

### A1. Make the BG22 host build profile identical to production (R4)

**Why first:** eight BG22-oriented host executables currently compile shared code with members that
the production firmware compiles out. Later tests can be green while exercising different offsets,
sizes and capability arms.

**Implementation**

1. In `tests/host/CMakeLists.txt`, add the four production definitions to both
   `od_shared_silabs` and `od_shared_dispatch_fixture_silabs`:
   - `OD_CONFIG_WITH_TOUCH=0`
   - `OD_CONFIG_WITH_BUZZER=0`
   - `OD_CONFIG_WITH_WIFI=0`
   - `OD_CONFIG_WITH_DATA_EXTENDED=0`
2. Add `OD_CAP_LOG=0` to `od_shared_dispatch_fixture_silabs` so its dispatch archive has the same
   logging capability as the firmware.
3. Add a small compile-time profile test that includes `od_config.h` under the BG22 definitions and
   pins `sizeof(struct od_config)` plus the offsets of the fields BG22 production code consumes.
   The values come from the target compiler/profile, not from hand-maintained guesses.
4. Add a fail-closed `tools/check.sh` rule with an explicit **shared-profile parity allowlist**:
   `OD_CONFIG_MAX_SIZE`, `OD_TXQ_SLOTS`, the `OD_CAP_*` values consumed by the linked shared tiers,
   and the four `OD_CONFIG_WITH_*` layout switches. Compare only that named set between production
   and both host archives; a missing or conflicting value in the parity set fails.
5. Give the rule a named, commented tier exception list rather than pretending every definition
   must match:
   - host-only `OD_BOOT_LOGO_SIZES=2` is allowed because the host archive compiles APP_BOOT while
     BG22 production declines it;
   - production-only `NVM3_MAX_OBJECT_SIZE`, `SLI_PSA_*` and other SDK/driver definitions are not
     shared ABI inputs and must not be copied to the host;
   - production-only `OPENDISPLAY_ZLIB_WINDOW_BITS` remains target backend configuration unless a
     host test deliberately selects the production inflater profile.
   Each exception names the tier or source that makes it legitimate. Unclassified `OD_` drift is a
   review failure, not something the script silently ignores.
6. Re-run all eight consumers: `xfer_silabs`, `pipe_off`, `config_asm_cap`, `silabs_storage`,
   `time_silabs`, `dispatch_silabs`, `dispatch_corpus_silabs` and `silabs_fault`. The definitions
   are PUBLIC, so the corrected profile must propagate to every executable. No compile break is
   expected merely from disabling buzzer fields—only `dispatch_test.c` mentions the buzzer hook—but
   the full scope must still be rebuilt. Investigate changed results rather than updating expected
   values automatically.

**Verification**

- GCC and Clang host suites pass.
- ASan/UBSan suite passes with the corrected smaller layout.
- The new parity rule fails when any one definition is deleted or changed in a temporary negative
  check, then passes after restoration.
- BG22 headless image size is unchanged; this package changes host definitions only.

**Exit:** production and both host profiles have a mechanically checked match for the named shared
ABI/capability set, with every intentional tier difference explicit and commented.

### A2. Bound the BG22 LED runner and guarantee a yield (R1)

**Reachability and consequence:** `CMD_LED_ACTIVATE` is an ordinary gated opcode. On a
security-disabled, LED-provisioned BG22, any client that can connect can supply the hostile pattern.
BG22 declines HAL_WDT, so this is not a watchdog reset followed by recovery: the superloop wedges
permanently and only a power cycle recovers it. That is why R1 is BLOCKER.

**Required semantics:** `grouprepeats == 255` continues to mean repeat until stopped. It must not
mean “consume the superloop forever.” A zero configured delay is legal but still yields at least
once between bounded runner steps.

**Implementation**

1. Port the existing ESP32/Nordic runner shape to BG22: after each visible flash, schedule either
   the configured delay or `LED_MIN_STEP_DELAY_MS`, then return; on the group-closing edge, schedule
   the minimum delay and return even when all three loop counts are zero. These returns, not the
   timer helper's zero arm, close the current unbounded `for (;;)`.
2. Add a small transition budget as defense in depth against a future zero-work phase. Exhausting
   it schedules the minimum delay and returns; it is not the primary fix.
3. Make `led_schedule_delay_ms(0)` schedule the minimum delay rather than setting an immediately
   consumable due flag. Current call sites guard every multiplication with `> 0`, so this arm is
   unreachable today and is explicitly defense in depth.
4. Preserve `grouprepeats == 255` as infinite-repeat only while the runner has a guaranteed return
   path. Finite counts, including the `uint8_t` wrap boundary, must terminate exactly once.
5. Ensure `opendisplay_led_stop()` cancels the timer, clears active state and leaves all configured
   channels off after any phase.
6. Do not add a watchdog feed inside the runner. The main superloop remains the liveness owner.

**Regression coverage**

First budget the missing production-source host seams. Extend
`tests/host/fake_silabs/sl_sleeptimer.h` with timer handles, callbacks, start/stop and deterministic
expiry; add minimal `em_gpio.h` and `em_cmu.h` fakes with observable calls. Reuse the existing
`tests/host/fake_silabs/sl_udelay.h`, adding call counting only if flash observation needs it. Supply
a mutable post-A1 BG22-profile `GlobalConfig` stub and an `opendisplay_get_global_config()` test
implementation; this is the link seam that carries the hostile payload, and it must tolerate and
expose the production const-cast writes to `leds[i].reserved`. Reset that object between cases.
Then compile the production `opendisplay_led.c` against the fakes and test:

- infinite repeat plus all loop/inter-group delays zero returns promptly on every poll;
- it schedules a non-zero timer and remains active until STOP;
- STOP works from every phase and cancels the outstanding timer;
- finite repeat counts 1, 254 and 255-wire-encoding behavior are pinned;
- zero loop counts do not create a busy loop;
- invalid instance and short activation payload retain deployed NACK behavior;
- repeated activation replaces the previous run without leaving a stale callback active.

Set a CTest `TIMEOUT` property on the executable so the original infinite loop fails the test
instead of hanging the suite. An in-test wall-clock guard is insufficient: fake
`sl_udelay_wait()` does not block, so control never returns to code that could inspect a deadline.

**Target and hardware gates**

- BG22 headless build passes and RAM/flash deltas are recorded.
- On BG22 hardware: send the hostile zero-delay infinite pattern over BLE; verify command traffic,
  advertising housekeeping and STOP remain responsive for at least 60 s. Repeat with security on.
- Power-cycle recovery is not accepted as a pass; the device must never wedge.

**Exit:** no payload can keep a single `opendisplay_led_process()` call from returning.

### A3. Validate FastEPD native geometry and bound every framebuffer access (R2)

**Design boundary:** panel-native geometry is target hardware knowledge and stays in the FastEPD
adapter. The generic TLV parser must not learn ESP32 panel IDs.

This package is deliberately three landing units. A3a closes the remotely reachable memory-safety
hole by preventing any unvalidated geometry from reaching the framebuffer; A3b and A3c harden
failure reporting and I/O correctness without making the blocker wait for the full adapter rewrite.

#### A3a. Pure native-geometry resolver and admission checks

1. Add a target-private pure geometry resolver mapping each FastEPD-supported `panel_ic_type` to:
   - native width and height;
   - supported framebuffer mode/bits per pixel;
   - allocated current/previous buffer capacities;
   - whether the panel uses IT8951 or the parallel path.
2. At `od_xfer_app_panel_info()` and boot-screen begin, require configured width, height and color
   scheme to match the resolved native geometry exactly. Reject zero, mismatched or overflowed
   values before power, allocation, `memset` or pointer arithmetic.
3. Pin the current live call graph explicitly: `od_xfer_app_panel_info()` validates before
   `od_xfer_app_begin_full()` and `od_xfer_app_begin_partial()`; `initDisplay()` validates before
   the FastEPD boot begin; and the staged `fastepd_ops_init()` behind `od_panel_ops_fastepd` performs
   the same check before it initializes the backend.
4. Add a cheap `geometry_ok()` guard inside `display_fastepd.cpp` itself at every public entry that
   can initialize or touch a framebuffer: `fastepd_epaper_begin()`,
   `fastepd_boot_write_row()`, the `fastepd_direct_*` write/reset/refresh path and the
   `fastepd_partial_*` prepare/write/refresh path. An invalid geometry must return/no-op before
   buffer access even if a future caller bypasses the three admission sites; A3b subsequently
   makes those failures fully status-returning.
5. Use the same resolver at partial-write admission so region bounds are evaluated against native,
   validated dimensions rather than a self-consistent but false configured size.
6. Add pure resolver tests for every supported FastEPD ID/mode and rejection tests for dimensions
   ±1, zero and `UINT16_MAX`.

**A3a exit:** boot, full and partial entry points refuse mismatched FastEPD geometry before any
framebuffer or panel operation, and the driver-local `geometry_ok()` guard makes that invariant
hold even if another call site is added. This commit closes R2's remotely reachable overwrite.

#### A3b. Status-returning adapter operations and capacity bounds

1. Change the FastEPD adapter operations that currently return `void` to return status where their
   callers need a truthful verdict:
   - begin/init;
   - direct reset;
   - direct chunk write;
   - boot row write.
2. Store the resolved allocation capacity in adapter state after successful initialization. Bound
   `memset`, full writes, row writes and partial blits against that capacity, even after the config
   was validated. Use checked subtraction (`offset <= capacity`, `len <= capacity - offset`) rather
   than `offset + len` comparisons.
3. Bound boot rows by both `y < native_height` and exact native row pitch. A short or oversized row
   is an error, not a silent success.
4. Bound partial rectangles in the native domain: `x <= width`, `w <= width - x`, `y <= height`,
   `h <= height - y`; then bound every row copy against the resolved buffer capacity.
5. Make `od_xfer_app_begin_full/partial()` unwind touch suspension and panel power on any adapter
   failure, then return `false`. Preserve the shared state machine's existing outcome: on PIPE
   activation failure `od_xfer.c` enters `OD_XFER_FATAL`; do not promise or synthesize a “normal
   setup NACK” unless a separate wire-policy change explicitly requires one.

#### A3c. Refresh-status propagation

1. Make refresh trigger/completion operations return status.
2. Propagate `fullUpdate()` and IT8951 helper failures through `od_xfer_app_refresh()`.
3. Do not call `backupPlane()` after a failed trigger or completion wait.
4. Ensure a failure cannot lead to `completed=true` after initialization or I/O failure.

**Regression coverage**

- A3a pure resolver table tests for every supported FastEPD panel ID and mode.
- A3a exact native dimensions accepted; width/height ±1, zero and `UINT16_MAX` rejected.
- Where the production C++ driver can be hosted, A3a directly calls each guarded
  `display_fastepd.cpp` entry under invalid geometry, bypasses `od_xfer_app_panel_info()` and boot
  admission, and observes no allocation, panel or buffer access. Otherwise, pair pure-helper tests
  with a fail-closed source ratchet naming every guarded entry; the invalid-config hardware gate
  still exercises the live driver.
- Canary-backed buffers around full reset, last row, last byte and partial bottom-right rectangle.
- Oversized boot `y`, row pitch, direct chunk and partial rectangle do not alter canaries.
- Initialization failure follows the shared command's existing refusal/fatal policy and leaves
  touch/power state clean.
- Refresh trigger and wait failures leave `completed=false` and do not back up the plane.
- Non-FastEPD ESP32 profiles retain their existing geometry behavior and binary contents except for
  normal build metadata.

Where hosting the C++ vendor adapter is impractical, keep the arithmetic in a target-private C/C++
pure helper compiled by the host and separately assert that production adapter call sites use it.

**Target and hardware gates**

- Build all ESP32 fragments; FastEPD code remains absent from non-FastEPD maps.
- Run valid direct, PIPE, partial and boot-screen paths on one IT8951 board and each available
  parallel FastEPD panel class.
- Store a deliberately mismatched config over the normal config-write path, then invoke boot and
  transfer entry points. Each operation must refuse before framebuffer access or panel power-up,
  the device must remain reachable, and a valid replacement config must restore operation. Config
  persistence semantics are outside A3; safe hardware admission is the required result.

**Exit:** no FastEPD pointer or byte count is derived solely from unvalidated configuration data.

---

## 4. Phase B — correct fleet config-write policy and Nordic device behavior

### B1. Bind config writes to an owner, handle ACK failure fleet-wide, and canonicalize Nordic (R5, R6)

Owner binding and intermediate-ACK disposition are fleet policy, not Nordic behavior. ESP32 and
BG22 also keep a file-static `od_config_asm` with no initiating-link identity, and both discard the
intermediate ACK result. Fixing Nordic alone would close one divergence by opening another.

#### B1a. Shared config-write guard with ESP32 and BG22 adoption

1. Add a small shared `od_config_write_guard` policy beside `od_config_asm`, containing the full
   initiating `od_reply_t`, a bound flag and the opening authentication class. Keep it separate from
   `struct od_config_asm`: BG22 overlays that struct's 16-byte header with the deployed NVM3 record
   header and cannot change `offsetof(buffer)`.
2. Provide shared start/chunk/reset helpers that take a `struct od_config_asm`:
   - a legal chunked START binds the full reply identity and authentication class;
   - a replacement START resets and rebinds, matching existing replacement semantics;
   - DATA requires `od_reply_same()` and the same authentication class before calling
     `od_config_asm_chunk()`;
   - owner/auth mismatch resets both guard and assembler and returns rejection without storage;
   - every disconnect/core reset clears both states.
   On BG22, also clear the guard in `saveConfig()`'s completion path, at the exact transition where
   the NVM3 record has overlaid the assembler's four state words and
   `od_config_asm_reset(&s_cfg.assembler)` currently runs after the `nvm3_writeData()` attempt.
   Clear both on the write-success and write-failure paths that reach that reset point, either
   through one storage-owned reset hook or by moving both resets to an equivalent common completion
   site. Do not clear the guard on `saveConfig()`'s early argument-validation return, where the
   union has not been touched. No path may leave a bound guard referring to assembly state that the
   union has destroyed.
3. Provide one shared intermediate-ACK disposition helper. Given the ACK's `od_txq_status_t`, it
   keeps the assembly active only for `OD_TXQ_OK`; every other status resets guard+assembler and
   yields `OD_CMD_NACK`. Do not retry sealing or manufacture a second NACK after `od_reply()` has
   already selected its wire disposition.
4. Migrate ESP32 and BG22 start/chunk handlers in B1a. Nordic cannot adopt the guard yet because its
   current `od_chunked_config_t` is not a `struct od_config_asm`; do not add temporary glue to its
   private parser. Nordic adopts the guard in B1b in the same change that swaps assemblers. By the
   end of B1b, no target may opt out of owner binding or ACK-failure teardown. Targets retain
   storage, reload and key-loss authorization policy.
5. Measure the separate guard's BG22 RAM cost against the 480-byte headroom and verify the NVM3
   overlay assertions and deployed 16-byte record header remain unchanged.

**B1a regression coverage — lands with B1a**

- Extend the portable shared-guard suite for bind, replacement START, owner/auth mismatch,
  intermediate-ACK success and every non-OK ACK result, disconnect and core reset.
- Compile the ESP32 and BG22 production command sources against their fake
  storage/session/egress drivers. Prove one connection cannot continue another's assembly,
  including BLE-tag and LAN/BLE-origin substitution where supported.
- On both targets, prove intermediate ACK success preserves state and every non-OK result resets
  guard plus assembler without a second NACK.
- On BG22, exercise NVM3 write success and failure after the union overlay and prove both reset the
  guard with the assembler. Exercise the early `saveConfig()` argument rejection separately and
  prove it does not clear an otherwise live, untouched assembly.
- Prove disconnect/reset destroys an in-progress ESP32 or BG22 assembly.

#### B1b. Replace Nordic's private parser with the canonical assembler

1. Replace Nordic's private `od_chunked_config_t` parser with `struct od_config_asm` and the B1a
   guard, using `od_config_asm_start/chunk` and matching ESP32 and BG22 legal shapes:
   - empty: reject;
   - 1..201 bytes: single frame;
   - exactly 202 bytes: chunked start;
   - greater than 202 bytes: reject without copying or persisting;
   - continuation chunks: 1..200 bytes, exact final byte count.
2. Keep Nordic's key-loss rewrite policy target-owned. Pass its authenticated/unauthenticated
   opening class into the shared guard; do not encode that policy inside `od_config_asm`.
3. Land Nordic's guard adoption in this same commit; there is no intermediate Nordic-only parser
   adapter and no release point at which Nordic has the new guard wrapped around the old parser.

**B1b regression coverage — lands with B1b**

- Pin every Nordic start length from 0 through the transport ceiling, especially 200/201/202/203,
  plus oversized and zero declared totals.
- Pin short non-final chunks, exact final chunks and byte-count rather than chunk-count completion.
- Reuse the shared guard vectors for Nordic owner/auth substitution, intermediate ACK failure and
  disconnect/reset teardown.
- Add these cases to the Nordic target-production config-write corpus; the portable assembler and
  guard suites alone are not command-policy evidence.

#### B1c. Make Nordic final persistence/ACK ordering truthful

1. Create one Nordic `persist_config()` helper, analogous to BG22, used by single write, completed
   chunked write and clear:
   - persist/erase first;
   - commit the success ACK while the old session is live;
   - capture and inspect `od_txq_status_t`;
   - reload config and retire the old session exactly once;
   - return `OD_CMD_OK` only when persistence and response commit both succeed.
2. Propagate persistence failure without clearing the live session. Preserve the stored config and
   assembler invariants on failure.
3. Keep the existing reservation budgets unless tests prove they are insufficient; do not increase
   queue capacity to hide ignored return codes.

**B1c regression coverage — lands with B1c**

- Pin storage save/erase failure, ACK seal failure, queue-liveness failure and counter exhaustion.
- Prove reload observes a queued success frame and a still-live old session.
- Prove the session is cleared only after successful response commit, and persistence failure does
  not clear it.
- Exercise single write, completed chunked write and clear through the same persistence helper.

**Wire and evidence bookkeeping**

In each landing commit:

- **B1a:** update `docs/DIVERGENCE_MATRIX.md` for ESP32/BG22 owner-mismatch refusal and
  intermediate-ACK teardown. Open their legal chunked-write, wrong-owner and
  interrupted/reconnected rows in `docs/HARDWARE_VERIFICATION_CHECKLIST.md`.
- **B1b:** extend those matrix policies to Nordic and record its 201-byte START changing from a
  two-frame assembly to a single-frame config and its 203..transport-ceiling START changing from
  truncate/accept to NACK. Open Nordic ownership, interruption and 201/203 boundary rows.
- **B1c:** record Nordic's persistence/response-ordering change and open the save/erase,
  response-commit, reload and re-authentication rows.
- record host-only fault rows as software evidence, never as substitutes for hardware rows.

**Hardware gate**

On `xiao_nrf52840`, then each nRF54 board as available:

- single and multi-chunk config write;
- read-back byte equality and CRC;
- reload-in-place and reboot persistence;
- encrypted write with old-key ACK followed by required re-authentication;
- disconnect after first chunk, reconnect, and fresh successful write;
- illegal 203-byte start rejected while the previous stored config remains intact.

On ESP32 and BG22, exercise a chunked write across a disconnect/tag replacement and verify the new
connection cannot commit the old assembly. Where two transports can coexist, test cross-origin
substitution too.

**Exit:** every target binds chunked config state to the initiating link, every target tears it down
on intermediate ACK failure, and Nordic accepts exactly the canonical start/chunk shapes without
reporting failure after silently committing a configuration.

### B2. Make buzzer note decoding canonical (R7)

**Preferred implementation:** promote only the pure pitch mapping, not the timing runner.

1. Add `shared/core/od_buzzer_pitch.{c,h}` in the PURE tier. It returns canonical centi-Hertz for
   all 256 indices using the existing ESP32 quarter-tone table, including index 0 as silence.
   Add the `.c` file explicitly to the PURE list in `shared/sources.cmake`; sources are never
   globbed.
2. Move the generated table and octave-folding policy from ESP32 into the helper without changing
   ESP32 output. Retain integer arithmetic and compile-time bounds. ESP32's recursive C++11
   `constexpr` playable-window search does not port to C99: pin indices 117 and 234 with
   `_Static_assert` conditions against the canonical table entries and their adjacent bounds.
3. Make ESP32 and Nordic call the helper. Nordic converts centi-Hertz to its timer period with an
   explicitly tested rounding rule; do not truncate early to whole Hertz.
4. Remove Nordic's 400..12000 linear interpolation. Narrow the source comment accurately: its
   validation and error bytes match the reference, but frequency decoding did not, so timing is not
   the only pre-fix difference.
5. Keep BG22 capability-off: no table or helper symbol should appear in the BG22 linked image if
   buzzer support remains disabled. The target already uses `-ffunction-sections` and linker garbage
   collection; prove the same zero-cost mechanism used by the dormant time HAL removes this helper.

**Regression coverage**

- All 256 shared table entries match the pre-change ESP32 table.
- Pin indices 0, 1, 117, 120 (440 Hz), 234 and 255.
- Pin octave folding at both playable-window boundaries.
- Compare a representative melody encoded by current `py-opendisplay` with expected pitches.
- Fake Nordic timer tests verify period and duty calculations without hardware timing jitter.
- Link-map proof that BG22 pays no flash/RAM cost.

**Wire and evidence bookkeeping**

- Add the Nordic frequency-scale change to `docs/DIVERGENCE_MATRIX.md`, including representative
  pre/post indices and the unchanged validation/error behavior.
- Open ESP32 and Nordic measured-frequency rows in
  `docs/HARDWARE_VERIFICATION_CHECKLIST.md` when the candidate lands. Do not close them from fake
  timers or table equality.

**Hardware gate:** play a chromatic/octave reference sequence on ESP32 and Nordic; measure at least
A4/index 120 plus both playable-boundary notes with a logic analyzer or frequency counter. Record
tolerance and clock source.

**Exit:** one pure mapping defines pitch for every capable target and host-encoded melodies have the
same notes on ESP32 and Nordic.

---

## 5. Phase C — long-uptime and panel-failure correctness

### C1. Route every BG22 clock user through the 64-bit time HAL (R9)

This is the tracked work in `docs/FOLLOWUPS.md` section 7, including the panel `.inl` call site and
the same ten raw conversions. Update that entry with disposition, commit and verification in the
same commit that removes the final caller; do not leave a closed defect listed as open.

**Implementation**

1. Replace direct `sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count())` calls in session,
   transfer, BLE, NFC and panel code with `od_hal_uptime_ms()`.
2. For vendor-facing `millis()` adapters, return the HAL value and preserve the required target
   type only at the boundary.
3. Audit all deadline arithmetic for wrap-safe unsigned subtraction. Absolute comparisons such as
   `now >= deadline` must either use a signed-delta helper with a bounded horizon or become elapsed
   checks.
4. Preserve `od_hal_uptime_ms()`'s pre-initialization `0` behavior; callers must tolerate boot-domain
   origin without logging recursively.
5. Add a fail-closed gate forbidding raw 32-bit sleeptimer conversion in BG22 production code,
   except inside an explicitly named compatibility seam if one is unavoidable.

**Regression coverage**

- Fake 64-bit ticks cross the underlying 32-bit wrap with monotonic millisecond results.
- Session timeout, TX/event hold, config-read deadline, NFC field detect and transfer timing each
  cross the boundary without early expiry or an extended lifetime.
- The public `uint32_t` millisecond wrap remains tested independently with wrap-safe subtraction.

**Hardware gate:** instrument a known interval against the sleeptimer and verify session/event
deadlines. A 36-hour soak is desirable but not a substitute for the deterministic rollover tests.

**Exit:** no BG22 production policy clock reads the 32-bit tick counter directly.

### C2. Make panel BUSY timeout observable, actionable and eventually non-blocking (R10)

Split this into two commits so truthful failure can land without waiting for the larger scheduling
change.

#### C2a. Truthful timeout propagation

1. Add `bbepWaitBusyStatus()` returning READY, TIMEOUT or INVALID, and keep `bbepWaitBusy()` as a
   thin compatibility wrapper. OpenDisplay-owned paths use the status API; the stable wrapper keeps
   the next vendor re-import from becoming a signature-conflict exercise. Preserve the existing
   OD-PATCH marker and re-vendor note.
2. Update every local OpenDisplay call site and add status-returning wrappers where a vendor helper
   currently hides the wait. Initialization, wake, address-window preparation and refresh must
   abort their operation on TIMEOUT; no caller may log and continue as success.
3. Propagate the result through ESP32, Nordic and BG22 panel adapters into `od_xfer` completion and
   boot-screen status.
4. On failure, power the panel down, clear transfer ownership and leave connection/session state in
   the target's documented recovery state.
5. Keep the vendored change minimal and documented as an OpenDisplay patch; do not edit sibling
   vendor repositories.

#### C2b. Remove long waits from command-serving contexts

1. Represent long BUSY waits as start/poll/deadline state owned by the target panel adapter.
2. Let the main loop continue draining RX/TX and feeding the watchdog between polls.
3. Preserve the existing per-panel 5 s/35 s ceilings initially; changing waveform timing or timeout
   policy is a separate hardware decision.
4. Ensure disconnect/reset cancels a pending poll and powers down safely.
5. Do not feed the watchdog from an ISR or BUSY poll callback.

**Regression coverage**

- Fake BUSY: immediate ready, delayed ready, never ready, polarity variants and no BUSY pin.
- Timeout emits failure exactly once and never calls later panel operations.
- RX/TX processing advances while a long wait is pending.
- Reset/disconnect at every wait phase leaves no live callback or powered panel.

**Hardware gate:** on each target family, run a normal refresh and then force BUSY asserted or
disconnect the panel. Verify bounded failure, continued/recovered transport service and truthful
host result. Record watchdog behavior separately; a watchdog reset is recovery evidence, not a
successful panel operation.

**Exit:** a stuck panel cannot be reported as refreshed, and long waits do not monopolize the
command-serving loop.

---

## 6. Phase D — external protocol and host corrections

These packages need coordinated changes in read-only sibling repositories. Record them as external
work; do not partially change firmware wire behavior in this repository.

### D1. Define session v2 with direction-separated CCM nonces (R3)

**Required protocol properties**

- client-to-device and device-to-client never use the same `(key, nonce)` pair;
- direction separation is cryptographically bound, not inferred from transport;
- downgrade behavior is explicit and testable;
- replay windows and counter exhaustion remain independent per direction;
- existing v1 devices remain detectable by the host, but a v2-required device never silently
  falls back after an active downgrade attempt.

**Decision before design:** this repository does not own the protocol choice. Carry the costed
options from `docs/FOLLOWUPS.md` section 5 into a protocol decision record before specifying v2:

- **Nonce-domain bit:** reserve and authenticate a direction bit in the effective CCM nonce,
  retaining one session key and one prepared PSA slot. This is the lowest firmware/RAM cost but
  changes nonce construction and direction validation on both sides.
- **Directional keys:** derive c2d and d2c keys with canonical domain-separated labels while
  retaining the current counter shape. This has a clean cryptographic separation argument but can
  require two prepared PSA keys/slots and more session state.
- Evaluate every other costed option already recorded in FOLLOWUPS rather than narrowing the choice
  here.

Start with one disposable BG22 decision spike, not a six-arm, three-target prototype campaign:

1. Prove whether the CRYPTOACC-backed PSA implementation can own two prepared volatile keys at the
   same time and release both across success, authentication failure, reset and disconnect.
2. Compile the one-key and two-key candidate session-state layouts and record the
   `sizeof(struct od_session)` delta against BG22's 480-byte RAM headroom. The nonce-domain-bit
   option uses the existing one-key/one-slot layout as its firmware-cost baseline.

Those are the two constrained inputs most likely to decide the construction. If the two-key arm is
clearly infeasible or materially worse, carry that evidence directly to the protocol decision. Only
if the result is close or ambiguous should the spike expand to linked-flash, cleanup and full
two-arm measurements on ESP32 and Nordic. Delete the losing prototype; no candidate wire behavior
lands before the canonical choice.

**Protocol-repository work (`../opendisplay-protocol`)**

1. Select and justify the direction-separation construction using the security argument plus the
   measured firmware/slot cost. Record why the rejected alternatives lost.
2. Specify version negotiation in the authentication transcript. Do not infer v2 from firmware
   version strings.
3. Specify the selected nonce/KDF construction, role-to-direction mapping, counters, replay
   behavior and downgrade policy byte-for-byte.
4. Add known-answer vectors for handshake proof, counter 0 in each direction, sealing/opening and
   wrong-direction rejection, plus directional keys if that option wins.
5. Version generated C/Python bindings and update the canonical wire corpus.

**Host work (`../py-opendisplay`)**

1. Negotiate v2 and implement the selected direction separation exactly.
2. Refuse silent fallback when the device/config requires v2.
3. Retain an explicit v1 compatibility path for deployed devices, clearly surfaced to callers.
4. Add cross-version, replay, wrong-direction and downgrade tests.

**Firmware_Unified work, only after the canonical revision exists**

1. Extend `od_session` to hold only the direction-specific ownership/counter state required by the
   selected canonical construction.
2. If directional keys win, extend all three crypto HALs for two prepared keys without leaking
   finite PSA slots across authentication, failure, reset or disconnect. If the nonce-domain bit
   wins, retain one slot and add strict per-direction nonce construction/validation instead.
3. Keep v1 compatibility only as specified; do not invent target-specific negotiation.
4. Import canonical vectors into host tests and fuzz both session versions.

**Verification and release gate**

- Canonical C and Python implementations pass the same byte vectors.
- Wrong-direction ciphertext fails authentication before command dispatch.
- Re-authentication and every failure path release every prepared key slot owned by the selected
  construction (one for a nonce-domain design, potentially two for directional keys).
- Encrypted upload, config read/write and interrupted-transfer recovery pass on ESP32, Nordic and
  BG22 hardware with v2.
- Release notes state the v1 residual risk and the policy for disabling it.

**Exit:** no supported v2 session repeats a `(key, nonce)` pair across directions, and compatibility
cannot be downgraded implicitly.

### D2. Correct deep sleep and add an explicit power-off API in py-opendisplay (R8)

**Current field behavior and priority:** all three firmware families already implement the
canonical opcode map in `od_dispatch_ops.h`: `0x0052` is power-off and `0x0053` is deep sleep. The
host's current `deep_sleep()` therefore hard-powers-off a D-FF ESP32 and requires a physical wake;
BG22 and Nordic refuse that same power-off command outright. The canonical host-initiated deep
sleep path is unreachable on those two families and misdirected on the latch-equipped ESP32. This
is a fleet API blocker, so D2 is scheduled ahead of the optional-peripheral B2 work.

**External host changes**

1. Regenerate or update constants so `POWER_OFF = 0x0052` and `DEEP_SLEEP = 0x0053` match the
   canonical protocol.
2. Make `deep_sleep()` send `0x0053`, including the optional big-endian one-shot duration.
3. Add a separately named `power_off()` API for `0x0052`. Its documentation must state that latch
   hardware requires a physical wake and that the operation is fire-and-forget.
4. Decode the opcode-scoped NACK namespaces separately: unsupported power-off is not disabled or
   refused deep sleep.
5. Treat silence/disconnect according to each command's canonical fire-and-forget semantics without
   turning an explicit NACK into success.
6. Add tests proving `deep_sleep()` can never emit `0x0052` and that `power_off()` can never emit
   `0x0053`.

**Integration sequence**

1. Start this external host package immediately alongside Phase A; it does not depend on a firmware
   change.
2. Land/release the host fix.
3. Update this repository's pinned wire-corpus version only after that release exists.
4. Run the pinned and latest-advisory corpus.
5. Hardware-test both commands on D-FF latch ESP32 and non-latch ESP32, plus canonical deep sleep
   and explicit power-off refusal on BG22 and Nordic. A D-FF deep-sleep test must wake by timer; a
   D-FF power-off test must require the physical wake path.

**Exit:** host API names, opcodes, wake behavior and NACK interpretation are one-to-one.

---

## 7. Integration order and commit boundaries

Recommended order:

```
A1 test-profile parity
  ├─► A2 BG22 LED liveness ──► BG22 hostile-pattern hardware gate
  └─► A3a geometry admission ─► A3b bounded/status I/O ─► A3c refresh propagation
                                  └──► staged FastEPD hardware gates

A1 ─► B1a ESP32/BG22 guard ─► B1b Nordic guard/framing ─► B1c Nordic persistence
                                                           └──► fleet config gates
   └► B2 buzzer pitch       ──► ESP32/Nordic frequency measurement

A1 ─► C1 BG22 time
   └► C2a truthful BUSY timeout ─► C2b non-blocking wait

D2 host opcode fix (starts with Phase A) ─► new pinned host corpus ─► power/sleep matrix
D1 BG22 key-slot/size spike ─► protocol decision/vectors ─► host ─► firmware ─► hardware gate
```

Suggested commit series:

1. `test(silabs): match host capability profile to production`
2. `fix(silabs): bound zero-delay LED runner`
3. `fix(esp32): reject invalid FastEPD native geometry`
4. `fix(esp32): bound FastEPD adapter operations`
5. `fix(esp32): propagate FastEPD refresh failures`
6. `core: bind ESP32 and BG22 config assemblies`
7. `fix(nordic): adopt guarded canonical config framing`
8. `fix(nordic): make config persistence responses truthful`
9. `core: share canonical buzzer pitch mapping`
10. `fix(silabs): use rollover-safe time HAL everywhere`
11. `fix(panel): propagate bb_epaper busy timeouts`
12. `refactor(panel): poll long busy waits outside command loop`
13. Session-v2 firmware commits only after the BG22 decision spike, protocol decision and host
    revision.

The D2 host release is external and should proceed in parallel with commits 1–5; its eventual corpus
pin is a separate local commit. B1a updates ESP32 and BG22; B1b then swaps Nordic to the canonical
assembler and adopts the guard atomically, with no temporary private-parser adapter. A3a, A3b and
A3c remain separate even if reviewed in one series: the geometry-admission blocker fix must be
independently reviewable and revertible.

Do not merge A2/A3 into a de-duplication mega-change. Their security/liveness regressions must be
reviewable and revertible without reverting unrelated promotion work.

---

## 8. Verification matrix

Every local package runs the relevant focused tests, then the full gate before merge:

| Package | Focused host evidence | Required builds | Required hardware evidence |
|---|---|---|---|
| A1 | profile parity, all Silabs production suites | BG22 | none; evidence repair only |
| A2 | production LED fake-driver suite + hang timeout | BG22 | hostile infinite pattern, STOP, security on/off |
| A3a | resolver/admission suite | all ESP32 fragments | invalid/valid geometry admission on FastEPD classes |
| A3b | canary and adapter-failure suite | all ESP32 fragments | direct/PIPE/partial/boot I/O on FastEPD classes |
| A3c | refresh-failure suite | all ESP32 fragments | successful and forced-failure refresh |
| B1a | shared guard + ESP32/BG22 command suites | ESP32 + BG22 | owner/interruption tests on both families |
| B1b | Nordic framing suite + guard reuse | all Nordic boards | ownership, interruption and 201/203 boundaries |
| B1c | Nordic persistence/session fault suite | all Nordic boards | write/read/reload/reboot, response failure and re-authentication |
| B2 | all-index pitch vectors + fake timer | ESP32 + Nordic + BG22 no-symbol proof | measured reference notes on ESP32/Nordic |
| C1 | rollover and deadline suites | BG22 | known-interval timing; soak advisory |
| C2 | fake BUSY and async-progress suites | all three target families | normal and forced-stuck BUSY per family |
| D1 | BG22 PSA/size spike, then canonical KATs, fuzz and cross-version tests | all targets after protocol choice | encrypted Gate 2 on all capable families |
| D2 | host unit and wire corpus | no firmware change initially | sleep/power-off target matrix |

For each hardware row record board, firmware SHA, bootloader, configuration/security state, host
package version, raw command transcript, device log and observed recovery behavior.

---

## 9. Final acceptance criteria

This remediation plan is complete only when:

- BG22 cannot be monopolized by any LED pattern and the hostile case passed on hardware;
- every FastEPD buffer operation is native-geometry-validated and capacity-bounded, with malformed
  configuration rejected before memory or hardware mutation;
- BG22 host tests compile the exact production capability/config profile and a ratchet prevents
  drift;
- every target binds chunked config state to its initiating connection and resets it on a failed
  intermediate ACK; Nordic framing is canonical and persistence cannot be reported inconsistently;
- ESP32 and Nordic decode all buzzer indices through one canonical mapping;
- every BG22 policy clock uses the 64-bit-backed HAL and deterministic rollover tests pass;
- a panel BUSY timeout propagates as failure and long waits no longer starve command service;
- the protocol project selects a measured, direction-separated session-v2 construction with
  explicit downgrade policy; the host and firmware then implement it with hardware evidence;
- `py-opendisplay.deep_sleep()` sends only `0x0053`, while explicit power-off sends only `0x0052`;
- every B1/B2 wire change is recorded in `docs/DIVERGENCE_MATRIX.md` and has open, target-specific
  rows in `docs/HARDWARE_VERIFICATION_CHECKLIST.md` until the listed hardware evidence exists;
- `tools/check.sh --targets` passes with 0 failed and 0 skipped at each final local SHA; and
- no hardware checklist row is closed from software-only evidence.

Until R1 and R2 are closed, affected BG22 LED and ESP32 FastEPD builds should not be treated as
release candidates. Until R3 is closed or v1 risk is explicitly accepted, encrypted sessions must
be described as functionally interoperable but cryptographically affected by bidirectional nonce
reuse.
