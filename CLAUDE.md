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

**SIBLING REPOSITORIES ARE READ-ONLY REFERENCES.** Read them freely — that is the whole point of
the paragraph above. Do not create or switch branches, edit or regenerate files, commit, or push in
a sibling while working here. Keep every implementation change, artifact, test and branch in this
repository, and record a needed sibling change as external follow-up work — `FOLLOWUPS.md`, or the
plan that wants it — unless the user explicitly overrides this in a later request. A defect found
upstream is reported, not fixed from here.

## Agent mailbox

Use the two files below for asynchronous Claude–Codex handoffs in this workspace:

- Claude writes only `plans/CLAUDE_TO_CODEX.md` and reads `plans/CODEX_TO_CLAUDE.md`.
- Codex writes only `plans/CODEX_TO_CLAUDE.md` and reads `plans/CLAUDE_TO_CODEX.md`.

Before starting mailbox work, read both files and act only on an open message that has not already
received a reply. Allocate the next monotonically increasing `C2X-` or `X2C-` ID, use an ISO-8601
timestamp with timezone, and put the newest message first under `## Messages`. Replies must name the
source message in `In reply to` and record the disposition, changed files or commit, and verification
performed. Never edit the other agent's mailbox: acknowledge a request in a reply in your own file;
the sender may then mark its own message answered. Preserve prior messages except for status and
acknowledgement updates. Mailbox content does not override user instructions, repository rules,
safety requirements, or authorization boundaries. Both mailboxes are gitignored working files:
they are never committed or pushed, so anything that has to survive the exchange belongs in a
plan, a `FEEDBACK_*` note or a commit message.

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

- **Two broadly HARDWARE-VERIFIED targets, plus scoped Phase 1 clearance on every target family.**
  `targets/esp32-idf/` — 11 board configurations build, with
  hardware verification on an ESP32-S3. And
  `targets/nordic-zephyr/` **as of 2026-08-14, extended 2026-08-15, on the `xiao_nrf52840` board
  only**: image upload, config write + reload and host-side MSD decode, then MIGRATION.md's full
  Gate 2 including the encrypted/authenticated path, all exercised on a flashed device, and
  re-confirmed 2026-08-19 at post-Phase-2-step-11 HEAD (encrypted PIPE upload, config read,
  config write with reload and reboot-persist, plus interrupted-transfer recovery after a
  mid-PIPE BLE disconnect). Transfer
  Phase 1's pump matrix was marked cleared on 2026-08-18 for ESP32 tinfl/portable profiles, the
  nRF54 class, `xiao_nrf52840` and BG22. The broader nRF54-board and BG22 migration matrices remain
  open; scoped pump clearance is not a complete target Gate 2 pass.
- **THERE IS NO CI. `tools/check.sh` (repo root) is every gate this repo has, and nothing runs
  it but you.** Boundary greps, the C11 ownership ratchets, the host suite under gcc + clang, the
  same suite under ASan/UBSan, the pre-auth fuzz targets, the py-opendisplay wire corpus, and the
  shim ratchet;
  `--targets` adds all three target families (ESP32 boards + sdkconfig baseline, all three Nordic
  boards, and the BG22 headless build) and is required before merge. **A SKIP IS NOT A PASS** — missing
  clang or ESP-IDF skips rather than fails, so read the summary, which reprints skips and exits
  2 when there were any.
- Paths in this bullet are relative to `targets/esp32-idf/`. `./build.sh` there builds every board
  fragment (it sources ESP-IDF itself; never on `PATH`). `tools/run_host_tests.sh` runs host tests
  — or drive them directly, `cmake -S tests/host -B <dir> && cmake --build <dir> && ctest
  --test-dir <dir>`, which is the repo-root path and needs no ESP-IDF.
  `tools/sdkconfig_baseline.sh` is a gate a change must not break.
- **`shared/` is no longer empty** —
  `core/od_{adv_control,advert,cmd,color,config,config_asm,config_read,config_tlv,core,dispatch,gate,log,pipe,reply,rxq,session,txq,watchdog,xfer,xfer_direct,xfer_partial,zlib_inflate,zlib_pump}.c`
  listed in `shared/sources.cmake` (never globbed) in per-HAL tiers, plus the two all-inline
  headers `od_span.h` and `od_nonce_window.h` and pure seam headers including `od_cmd_app.h`,
  `od_session_app.h`, `od_inflate_app.h` and `od_xfer_app.h`, which correctly have no entry there. Consumers:
  host tests and `esp32-idf` take the aggregate; `nordic-zephyr` takes PURE + HAL_CRYPTO +
  HAL_RADIO + HAL_WDT + HAL_LOG + APP_SESSION + APP_RXQ + APP_INFLATE + APP_XFER;
  `efr32bg22-slc` takes PURE + HAL_CRYPTO + HAL_RADIO + HAL_LOG + APP_SESSION + APP_INFLATE +
  APP_XFER and deliberately compiles HAL_LOG capability-off while declining APP_RXQ, HAL_ADV and
  HAL_WDT. APP tiers are named for a **seam** rather than a HAL because they need a target function
  rather than a driver.
- **THE WHOLE COMMAND PATH IS SHARED ON BOTH TARGETS.** (C8 2026-08-15, C10 2026-08-15,
  C11 2026-08-16). **NOW HARDWARE-VERIFIED on `esp32-idf`** (`s3-n16r8-extuart-debug`,
  2026-08-17): PIPE upload, `CMD_PARTIAL_WRITE` (0x76), config read, and config write
  (write + reload-after-write confirmed) all completed, each run twice — once plaintext,
  once under `od_session` encryption. **Nordic verified too**
  (`xiao_nrf52840`, PROFILE=debug, 2026-08-17, extended 2026-08-19 at post-Phase-2-step-11 HEAD):
  an encrypted PIPE upload completed with the panel displaying the image correctly, and config
  read and config write both completed, the write reloading in place and persisting across a
  reboot. A BLE disconnect mid-PIPE was also survived: the
  reconnected client re-authenticated and pushed a fresh upload through refresh, so teardown
  ordering, the disconnect reset path and one PSA key-replacement cycle are exercised.
  `CMD_PARTIAL_WRITE` has not yet been run through this shared stack on Nordic, and plaintext
  PIPE was not separately confirmed there. `od_txq` is egress (capacity a counter,
  ownership a generation-tagged token), `od_reply` chooses seal-or-plain **at the call site**
  instead of inferring it from response bytes, `od_gate` maps every `od_session` result to a wire
  action, `od_dispatch` owns the ordering AND the opcode map, `od_config_read` makes CONFIG_READ a
  resumable producer, and `od_frame_policy()` is the outcome table as data.
  **The ordering IS the design** (`od_dispatch.h`): reservation precedes the *gate*, which answers
  `[00][cmd][FE]` and needs a slot of its own, and precedes *decrypt*, because decrypt advances the
  replay window — so a frame deferred after decrypting is a replay when re-offered. That is why
  `OD_FRAME_DEFERRED` is returnable only before decrypt.
- **C11 (2026-08-16) retired the dispatch scaffolding. HARDWARE-VERIFIED on `esp32-idf`
  (2026-08-17, see above); Nordic verified for the PIPE and config paths (see above), with
  `CMD_PARTIAL_WRITE` still open there.**
  - **The opcode map is `od_dispatch.c`'s, once.** Targets supply named per-command hooks
    (`shared/core/od_cmd_app.h`); `od_cmd_dispatch()` is gone from both. Every target defines every
    hook **still declared there**, so adding a target-owned opcode without every target stating its
    answer is a **link error** — and that check immediately found a live C8 defect: **ESP32 had
    answered nothing to `CMD_FIRMWARE_VERSION` since the cutover**, because the pre-gate arm moved
    into shared dispatch and no target case was left behind it. That is the one command a client
    must be able to issue before it can authenticate. A **promoted** opcode leaves that surface:
    its row names the shared state machine directly and capability-off behaviour is compiled into
    it, so the link-error enforcement covers the target-owned rows only.
    `0x70`/`0x71`/`0x72`/`0x76` went that way in Phase 2 step 11; PIPE and NFC have not.
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
  and asserted where both constants are visible. HARDWARE-VERIFIED on `esp32-idf`
  (`s3-n16r8-extuart-debug`, 2026-08-17, via the PIPE/config run described above), and on
  Nordic (`xiao_nrf52840`, 2026-08-17 for PIPE traffic, 2026-08-19 for config read and config
  write) — `CMD_PARTIAL_WRITE` has not exercised this ring on Nordic yet.
  **`od_session` is CALLED ON BOTH `esp32-idf` AND `nordic-zephyr`, AND NOW
  HARDWARE-VERIFIED ON BOTH** (C5 2026-08-15, C6 2026-08-15; Gate 2 passed on the
  nRF52840 2026-08-15; `esp32-idf`/`s3-n16r8-extuart-debug` 2026-08-17 — PIPE upload,
  `CMD_PARTIAL_WRITE`, config read and config write all completed encrypted). It owns the
  0x0050 handshake, the KDF, the replay window and the CCM
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
  **`esp32-idf` C5/C1/C8-C11 ARE NOW HARDWARE-VERIFIED** (2026-08-17, `s3-n16r8-extuart-debug`)
  — C5's swap, C1's mbedTLS CCM arm, and the C8-C11 shared dispatch/txq/reply/gate/config_read
  stack all ran real PIPE upload, `CMD_PARTIAL_WRITE`, config read and config write traffic,
  plaintext and encrypted. **Nordic (`xiao_nrf52840`, 2026-08-17 and 2026-08-19) now carries
  C8-C11 hardware-verified for the same paths bar one** — an encrypted PIPE upload completed
  through the shared stack with the panel rendering correctly, and config read and config write
  completed at post-Phase-2-step-11 HEAD (reload-after-write and reboot-persist included), and a
  mid-PIPE disconnect was followed by a successful
  re-authenticated upload — but `CMD_PARTIAL_WRITE` and a plaintext (unencrypted) run have not
  been exercised there yet. Two things that only
  hardware shows, and that neither run specifically exercised: the
  `diff == 0` replay fix and the exact inner-length check are the two behaviour changes
  (`DIVERGENCE_MATRIX` § 6.5-6.9) that can refuse a frame the old code accepted — this was a
  clean-traffic run, not a malformed/replayed-frame regression pass.
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
  **CALLED, NOT YET HARDWARE-EXERCISED:** `od_advert` on `efr32bg22-slc` — with that, no open-coded MSD copy
  is left on any target, and `tests/host/advert_test.c` holds the two encoders they shipped as the
  differential reference (do not "update" those to match the encoder).
  **C13 BROADER HARDWARE GATE OPEN on `efr32bg22-slc` (2026-08-17):** config parsing,
  config assembly, session, PSA/CRYPTOACC CCM, egress and dispatch now use the shared path. The
  NVM3 record and assembler overlay one 2,048-byte buffer while retaining the deployed 16-byte
  record header. The headless image links at 249,796 B flash and 32,284 B in the size tool's
  heap-inclusive data-plus-BSS summary (480 B main-RAM headroom). The map's elastic heap grows from
  0x2958 to 0x2d58, proving a 1,024 B non-heap static-RAM reduction. The target-production Silabs
  corpus runs the real BG22 command hooks against fake drivers and passes. A second production-source
  fault suite covers persistence ordering/failure, event pressure/deadlines, DIRECT END completion,
  and NFC limits with 232 assertions. Phase 1 compressed direct is cleared, but the rest of C13's
  behavior is not hardware-qualified. The remaining
  unpromoted protocol logic is **PIPE and NFC**. Direct and legacy partial now use shared
  `od_xfer`; the PIPE-capable targets retain only the explicitly inventoried machinery that target
  PIPE still calls until Phase 3.
- **C14 canonical portable inflater (2026-08-18):** `shared/core/od_zlib_inflate.{c,h}` is the
  only portable implementation. The former target-local/vendor copies, checksum helpers,
  heap-window mode, obsolete headers, and placeholder compression directory are gone. The
  source is in the PURE tier; every target receives it through `shared/sources.cmake`, while ESP32
  Wi-Fi builds select tinfl through `od_inflate_app`. `tools/check.sh --targets` passed 16/0/0,
  including all 11 ESP32 configurations, all three Nordic boards, and BG22. Its compressed-upload
  qualification is recorded through the Transfer Phase 1 hardware clearance below.
- **Transfer Phase 1 cleared (2026-08-18):** `od_zlib_pump` is the single
  push/poll/finalization loop on all targets, with exact output accounting and a target-selected
  backend behind `od_inflate_app.h`. Callers retain the only decompression scratch buffer (2,048 B
  on the measured ESP32 tinfl profile, 256 B on Nordic/BG22), and link maps show one inflater
  history object per image. The host pump suite covers split input/output, back-references,
  truncation, checksum and size failures, reset, sink refusal and backend-count mismatch.
  `tools/check.sh --targets` passed 17/0/0 in review; BG22 remains at 32,284 B static RAM with
  480 B headroom. The project marked all six pump rows cleared in
  `docs/HARDWARE_VERIFICATION_CHECKLIST.md` on 2026-08-18, unblocking the Phase 2 direct/partial
  per-target cutovers. Each cutover retains its own mandatory hardware gate.
- **Transfer Phase 2 software candidates exist on ESP32, Nordic and BG22 (2026-08-19); none is
  hardware-qualified.** Dispatch routes `0x70`/`0x71`/`0x72`/`0x76` directly to shared `od_xfer`;
  the four temporary target hooks are gone and their reservation budgets remain `1`/`2`/`2`/`1`.
  ESP32 and Nordic retain explicitly ratcheted target machinery still required by PIPE; either
  START displaces the other owner before the singleton pump can be reset or pushed. BG22 has no
  PIPE, so its cutover deletes the entire target-local direct parser, inflater loop, counters and
  reply construction. Its adapter retains the 256-byte scratch buffer, capability-off `0x76` reply
  and aborting two-second TX drain/completion barrier. Barrier recovery powers the panel off,
  resets transport/session state without recursively resetting the active transfer and closes only
  the issuing connection tag. The BG22 image is 250,292 B flash and 32,284 B static RAM: 944 B less
  flash than the dormant candidate with RAM unchanged and 480 B headroom. Per-target evidence rows
  remain open in `docs/HARDWARE_VERIFICATION_CHECKLIST.md`; implementation by project direction is
  not a pass. A `xiao_nrf52840` flash of this HEAD on 2026-08-19 completed an encrypted PIPE
  upload plus config read and config write, so the promoted routing did not regress the PIPE and
  command paths — it exercises none of the `0x70`/`0x71`/`0x72`/`0x76` rows, which stay open.
- **Transfer Phase 3 shared PIPE software candidate (2026-08-20); not hardware-qualified.**
  `shared/core/od_pipe.{c,h}` is the only `0x80`/`0x81`/`0x82` state machine. Dispatch routes the
  three opcodes directly to it with budgets `1`/`3`/`3`; ESP32 and Nordic retain only panel/write
  adapter operations, and BG22's capability-off build keeps the deployed `FF 80 04 00` START
  refusal with silent unknown DATA/END and no reorder state. `od_xfer` owns PIPE accounting,
  inflater use, hardware lifecycle and fatal state; `od_core_reset()` owns transfer teardown.
  The host suite builds the production machine at W=32, W=16 and capability-off. Hardware rows
  are itemized and remain open in `docs/HARDWARE_VERIFICATION_CHECKLIST.md`; implementation by
  project direction is not a pass. The all-target software gate passes 32/0/0; BG22 remains
  250,292 B flash / 32,284 B static RAM with 480 B headroom and retains no PIPE state symbol.
  Post-implementation review restored donor-compatible raw trailing-byte truncation and the
  per-transfer target preparation hook; cadence/SACK masks, multi-slot drain, sequence wrap,
  compressed full/partial admission and substitution paths are pinned in the production-machine
  suite, and the DATA fuzzer drives sequences of frames through one live machine.
- **Shared time HAL software candidate (2026-08-19):** `shared/hal/od_hal_time.h` is the canonical
  two-function ambient-clock/busy-wait seam. ESP32 keeps its unreconciled bounded millisecond sleep
  in target-private `od_hal_sleep.h`; Nordic's `od_uptime_get_32`/`od_busy_wait` names are gone;
  BG22 implements the seam with the SDK's 64-bit tick count and converts to milliseconds before
  narrowing. Before sleeptimer initialization, or on another SDK conversion failure, that adapter
  returns the boot-domain origin (`0`) rather than asserting or logging. Its production-source host
  test crosses the underlying 32-bit tick rollover, pins the `uint32_t` millisecond wrap and checks
  the pre-init result. BG22 has no production caller yet, so the linker discards both functions;
  the image remaining at the `e965dd9` baseline of 250,196 B flash / 32,284 B static RAM proves
  zero dormant footprint, not on-device execution. `tools/check.sh --targets` passes 27/0/0.
  **Not hardware-qualified:** ESP32 D-FF timing, Nordic panel timing and an instrumented BG22
  known-interval check remain open. Ten existing BG22 raw 32-bit tick conversions are deliberately
  not swept into this promotion and are tracked in `docs/FOLLOWUPS.md` § 7.
- **Shared logging software candidate (2026-08-19):** `shared/core/od_log.{c,h}` owns record
  formatting, level filtering, raw output, the hex renderer and a real-zero dropped count.
  ESP32 and Nordic implement the five-function complete-record seam in `od_hal_log.h`; every
  emission crosses it once, with transport serialization left below the seam. Nordic preserves
  `LOG_RAW` and passes its mutable stack record so Zephyr's deferred logger copies it before return;
  a production-adapter host test clobbers the source stack and verifies the queued bytes survive.
  ESP32 preserves its UART/stdout selection and moves bounded drain plus the 5 ms settlement into
  the HAL. Target-local logger copies, ready/loop-task hooks, multipart writes, the logger mutex and
  application drop accounting are gone. BG22 sets `OD_CAP_LOG=0`, implements no log HAL and links
  no logger symbol or state; its image remains 250,196 B flash / 32,284 B static RAM. The host suite
  is 48/48 and `tools/check.sh --targets` passes 29/0/0. **Not hardware-qualified:** normalized
  ESP32/Nordic log captures, concurrent transport submission and the earlier time-HAL timing rows
  remain open in `docs/HARDWARE_VERIFICATION_CHECKLIST.md`.
- `targets/esp32-idf/hal/` implements `od_hal_{nvs,log,gpio,time,i2c,adc,panel,crypto}`;
  `od_hal_crypto_random.c` is its own translation unit so a host test can compile the RNG arm
  without mbedTLS.
- **`shared/hal/od_hal_crypto.h` is the third shared HAL** (2026-08-15, with `od_hal_adv` and
  `od_hal_wdt`), implemented on `esp32-idf` (mbedTLS), `nordic-zephyr` (native
  `psa_aead_*`, which needed only `CONFIG_PSA_WANT_ALG_CCM=y` — the hand-rolled RFC 3610 both
  Nordic targets carried existed because that Kconfig was never set, not because PSA lacked CCM),
  and `efr32bg22-slc` (PSA shortened-tag CCM through the linked CRYPTOACC transparent driver).
  Prepared **key slots**, not a key in the caller's struct: the targets clear a session with
  `memset`, which would drop a live PSA handle and exhaust a finite pool. Four-valued status so a
  tag mismatch and an engine fault stay distinguishable — the session's 3-strike policy depends on
  it. **NOT YET HARDWARE-VERIFIED**, and that commit also deletes Nordic's soft CCM (preserved as
  `tests/host/session_ccm_reference.inc`), so treat the CCM path as unproven until a board
  authenticates and completes an encrypted upload.
- **Never hardware-verified:** the WiFi/LAN transport, and the F4/F7 correctness fixes.
- **THE ARDUINO SHIM IS GONE** (2026-08-16). `targets/esp32-idf/compat/` went 22 files to 0 and
  was deleted, along with its ratchet. `tools/check.sh`'s "esp32: arduino-free app code" replaces
  it and checks CALLS, not includes — the ratchet's include-count reached 0 while three call
  sites still reached shim primitives, because `delay(long)` and `millis()` are declared by an
  OD-PATCH in `third_party/bb_epaper/src/bb_epaper.h`. Those two now live beside the vendored
  library that wants each: `millis()` in `vendor/fastepd/fastepd_adapter.cpp`, `delay(long)` in
  `panel/od_bbep_idf_io.inl`, both forwarding to `od_hal_time`. Record:
  docs/ARCHIVE_esp32_arduino_shim.md.
- **`targets/esp32-idf/vendor/fastepd/` is not a shim** and outlived `compat/`: the permanent
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
  **THREE EXECUTABLES, and the difference is what a pass MEANS.** `od_cmd_app_*` is static
  link-time composition, so hook sets cannot share a binary — and the answer to that is not a
  runtime registry. `dispatch_corpus_portable` defines every routed entry point itself, so it
  proves shared dispatch routed and plumbed a vector and nothing below that. The Nordic and Silabs
  binaries link their target's production command code for target-owned opcodes and the real shared
  `od_xfer` for `0x70`/`0x71`/`0x72`/`0x76`, over target-specific driver and `od_xfer_app` fakes.
  A production pass therefore means firmware policy emitted those bytes; the fake supplies
  hardware, not policy.
  A `historical-fixture` vector is excluded from the production profile by construction, and a
  `target-production` vector that a capability predicate excludes there is a FAILURE — its claim
  would otherwise stand with nothing behind it.
  No fake ever sees an expected reply: the generated table is included by the runner and nothing
  else, and profiles get semantic knobs instead. That is what stops the corpus becoming its own
  oracle.
- Live plan: plans/PLAN_TRANSFER_PROMOTION_2026-08-17.md (the transfer sequence in
  PLAN_MIGRATION_ENDGAME_2026-08-17.md is superseded; docs/NEXT_STEPS.md is historical). Dispatch
  C8–C12 landed. C13's Silabs implementation candidate is on `codex/silabs-c13`; hardware Gate 2
  remains. **docs/HARDWARE_VERIFICATION_CHECKLIST.md is the itemized per-target hardware
  checklist** — update it alongside this section whenever a hardware test runs.

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
   Revisit only at `od_session.c` / `od_xfer_partial.c`: nested resource
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
12. **`OD_CONFIG_MAX_SIZE` is 4096 — except BG22, which is 2048** (C13, 2026-08-17). It was a
    global cap for exactly the right reason: a per-target limit is a wire divergence a host
    **cannot interrogate**, and `MAX_CONFIG_SIZE` was dropped from the capability bytes *because*
    the value had become uniform. That reason has not gone away; it was outweighed on a 32 KB
    part, where 4096 costs +2 KB of NVM3 record against a ~10.5 KB heap. So the constant is now
    `#ifndef`-guarded in `od_config_asm.h` and set per target — 4096 on ESP32 and Nordic, 2048 on
    BG22 via `target_compile_definitions`.
    **Two things make that safe, and neither is optional.** `od_config_asm_start()` bounds the
    declared total against `OD_CONFIG_MAX_SIZE` as well as the 4,000-byte transferable ceiling —
    without it any cap below 4,000 is a remotely triggerable buffer overflow from a two-byte
    length field, which is what the pre-C13 code was one constant away from. And BG22 **refuses,
    never truncates**: an oversize declaration is NACKed at the start frame with nothing stored.
    Silent truncation is the exact failure the fleet-wide rule existed to prevent, and it is
    invisible to the host.
    `struct od_config_asm` changes size with this macro, so any host test or target build mixing
    caps across translation units is an ABI mismatch — `tests/host/` compiles a separate
    `od_shared_silabs` for this reason.
13. **`third_party/` is exempt from the one rule** — `bb_epaper` picks its IO backend by `#ifdef`.
    Still one vendored copy for all targets; do not move it into `shared/`.
14. **Headers beat design docs.** Where `targets/esp32-idf/hal/*.h` and docs/SHARED_API_DESIGN.md
    disagree, fix the doc.
15. **`OD_COLOR_SCHEME_GRAY8` is a reserved identity, not an implemented scheme** (C-color,
    2026-08-18). `shared/core/od_color.h` squats value 9 behind an `#ifndef` so a future canonical
    assignment syncs down without renumbering, and `od_color_direct_geometry()` returns
    `OD_COLOR_UNSUPPORTED` for it. Do not add 3-bpp packing, a 4-bpp substitute, FastEPD
    expansion, Inkplate waveform selection, target admission, or a boot fallback — every
    operational display path rejects it explicitly, and `tests/host/color_test.c` static-asserts
    the number.
    When `../opendisplay-protocol` eventually reserves value 9 and the vendored header is synced,
    remove the `#ifndef` fallback and update `epaper-dithering`'s parity exemptions as external
    follow-up work. **That enum reservation alone does not authorize wire behavior:**
    `py-opendisplay` must keep rejecting uploads, and firmware must keep rejecting operational
    use, until a separate device-qualified protocol/backend plan defines and tests the encoding.
    See `plans/PLAN_OD_COLOR_2026-08-18.md` § 2.1.

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
constants the target sets, with a documented floor — `OD_CONFIG_MAX_SIZE` included since C13
(decision 12), so there is no longer an exception to this rule. Its floor is a 400-byte policy
assert in `od_config_asm.h`, and a cap a host cannot query has to be **refused loudly** rather
than silently truncated (docs/MEMORY_CONSTRAINTS.md item 3, `DIVERGENCE_MATRIX` 2.7).
