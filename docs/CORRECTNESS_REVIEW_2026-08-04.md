# Correctness review — 2026-08-04

Comprehensive review of the current unified-firmware repository, with emphasis on the
ESP-IDF phase-C target and the BLE, WiFi/LAN, configuration, storage, transport-arbitration,
and panel-facing paths changed on `fix/esp32-phase-c-display`.

The report is written against current HEAD `ede197cee7b42439883b6d482deaa1d6789d622a`
(`ede197c`, `docs: FOLLOWUPS item 4 -- authentication refused with no stated reason`). The
automated validation was run at `504cc58`; the two commits between that revision and this
report changed only `docs/FOLLOWUPS.md` and `targets/esp32-idf/README.md`. None of the reviewed
source files changed, so the code results below apply to current HEAD.

## Executive conclusion

The repository is in substantially better condition than its change volume suggests: all ten
ESP-IDF board configurations build, the configuration baselines and Arduino-shim ratchet pass,
the connection-owner stress suite is clean under address, undefined-behaviour, and thread
sanitizers, and the available protocol vectors agree with the pinned host library.

It is not ready to treat the WiFi/LAN and BLE lifecycle work as correctness-complete. Three
issues should block release of the current ESP-IDF LAN feature:

1. an ordinary LAN peer close leaks its socket descriptor;
2. a TLS-authenticated LAN client cannot normally write configuration even though TLS is the
   authentication mechanism on that transport; and
3. chunked configuration reassembly does not enforce the declared byte count and can save the
   wrong record.

Six additional medium-severity issues affect task synchronization, protocol conformance,
response integrity, BLE lifecycle truth, secure-erase reporting, and WiFi event handoff. One
lower-severity issue affects the mDNS update limiter. None of these findings is a compiler-only
objection: each has a concrete runtime path and consequence.

No source changes were made as part of the review.

## Scope and method

The branch is large relative to `main`: at report time it contains 125 changed files,
approximately 46,246 insertions and 2,072 deletions. Review therefore combined four methods:

- repository guidance and architecture review (`CLAUDE.md`, `docs/`, the target README, board
  fragments, shim budget, and protocol contracts);
- static review of ownership, framing, configuration, BLE lifecycle, WiFi event, NVS, I2C,
  display, and teardown paths;
- all available local build and test gates; and
- adversarial comparison of implementation behavior with the normative protocol header and
  the repository's connection-policy documents.

Priority labels in this report mean:

- **High** — a normal supported workflow fails, persistent availability can be lost, stored
  state can be corrupted, or a security/lifecycle guarantee is false. Fix before release.
- **Medium** — a real runtime defect requiring timing, backpressure, an error path, or a client
  behavior outside the normal happy path. Fix before calling the subsystem complete.
- **Low** — bounded operational or specification-quality defect with no demonstrated state
  corruption.

## Findings

### F1 — High — orderly LAN disconnects leak the accepted socket

**Evidence**

- `targets/esp32-idf/src/wifi_service.cpp:88-105` — `lanClientConnected()` returns `false` when
  `MSG_PEEK` returns zero, and for reset/not-connected/broken/invalid descriptor errors.
- `targets/esp32-idf/src/wifi_service.cpp:1359-1368` — `wifiLanReapClosedSession()` detects the
  dead peer using `!lanClientConnected()` and calls `disconnectWiFiServer()`.
- `targets/esp32-idf/src/wifi_service.cpp:1287-1293` — `disconnectWiFiServer()` closes
  `s_lanClientFd` only inside `if (lanClientConnected())`.
- `targets/esp32-idf/src/wifi_service.cpp:1406` — the next admitted connection overwrites
  `s_lanClientFd`.
- The same faulty liveness guard is used by `wifiLanDropOwnedSocket()` at `:1265-1278` and
  `opendisplay_lan_teardown()` at `:1622-1626`.

**Failure sequence**

1. A host connects and is admitted; `s_lanClientFd` holds the accepted descriptor.
2. The host performs an orderly TCP close or TLS `close_notify` followed by close.
3. The early reap sees the peer as disconnected and calls the disconnect function.
4. The disconnect function asks whether the peer is connected again. It is not, so the close
   block is skipped.
5. Bookkeeping says no LAN session exists, but the descriptor remains open.
6. A later accepted socket replaces the only stored reference to the old descriptor.

After enough normal host reconnects, the lwIP descriptor table is exhausted and LAN admission
fails until reboot. On the same path, `clearEncryptionSession()` is skipped because it shares
the incorrect conditional, although the later session abort may clear that state separately.

**Required correction**

Descriptor ownership and peer liveness must be separate predicates. Teardown should close when
`s_lanClientFd >= 0`, irrespective of whether the peer remains live. The close helper is already
idempotent and resets the descriptor, so all three teardown sites should call it
unconditionally. Peer-liveness probing should remain only in service/reap decisions.

**Regression test**

Run at least several hundred connect/command/orderly-close cycles and assert that the process
can still accept a client and that the count of open lwIP sockets returns to its baseline after
each cycle. Cover TCP FIN, reset, TLS `close_notify`, handshake failure, idle timeout, reboot
teardown, and session-guard abort.

### F2 — High — TLS LAN configuration writes incorrectly require app authentication

**Evidence**

- `shared/protocol/opendisplay_protocol.h:952-960` says the TLS handshake is authentication,
  app-layer `AUTHENTICATE` is not used on this port, and TLS-origin frames bypass AES-CCM.
- `targets/esp32-idf/src/communication.cpp:747-758` implements that rule in the main dispatcher
  by excluding `ORIGIN_LAN_TLS` from the app-authentication/decrypt gate.
- `targets/esp32-idf/src/communication.cpp:573-584` reintroduces
  `isEncryptionEnabled() && !isAuthenticated()` inside `handleWriteConfig()`.
- `targets/esp32-idf/src/communication.cpp:631-647` repeats the check for the first continuation
  chunk.
- `targets/esp32-idf/src/encryption.cpp:696` sets `encryptionSession.authenticated` only after a
  successful app-layer `0x0050` exchange. TLS handshake completion does not set it.

**Consequence**

An ordinary TLS-PSK client has proved possession of the configured secret and can execute most
commands, but `CONFIG_WRITE` receives `RESP_AUTH_REQUIRED` unless `REWRITE_ALLOWED` is set.
Requiring the client to run the app handshake inside TLS contradicts the wire contract and is
not something a conforming client should do. As a result, encrypted WiFi supports config read
but not the expected config-update workflow.

The handler-local gate appears to preserve the pre-TLS rewrite policy without taking frame
origin into account. That origin distinction must be applied consistently wherever a command
handler performs a second authorization decision.

**Required correction**

Create one explicit predicate for “this frame is authorized to mutate configuration.” It
should accept a TLS-origin frame after a successful TLS handshake and should apply the existing
session/rewrite rules to BLE and plaintext LAN. Do not infer TLS authorization merely from the
global encryption-enabled bit; origin and active TLS-session state are both required.

**Regression test**

With encryption enabled and `REWRITE_ALLOWED` clear:

- establish TLS-PSK without sending `0x0050`;
- read configuration;
- write a single-frame configuration;
- write a chunked configuration; and
- read it back and compare exact bytes after reboot.

The equivalent unauthenticated BLE and plaintext-LAN writes must remain rejected.

### F3 — High — chunked configuration reassembly does not enforce `totalSize`

**Evidence**

- `shared/protocol/opendisplay_protocol.h:310-337` requires a chunked start to be exactly a
  two-byte total plus the first 200 data bytes; continuations beyond the declared total must be
  NACKed.
- `targets/esp32-idf/src/communication.cpp:585-604` treats every payload greater than 200 bytes
  as chunked. A 201-byte payload takes the fallback branch, records `expectedChunks = 1`, ACKs,
  and does not commit or close the state.
- `targets/esp32-idf/src/communication.cpp:590-596` accepts the two-byte declared total without
  validating it against the first chunk, `MAX_CONFIG_SIZE`, or the legal chunk-count range.
- `targets/esp32-idf/src/communication.cpp:648-660` bounds received bytes only against
  `MAX_CONFIG_SIZE`, not `totalSize`, and commits when `receivedChunks >= expectedChunks`
  instead of when the byte count exactly matches the declared total.
- `shared/protocol/opendisplay_protocol.h:887-889` keeps `MAX_CONFIG_CHUNKS` at 20. Therefore a
  correctly declared 4,001-4,096-byte configuration needs 21 chunks and is rejected despite
  `MAX_CONFIG_SIZE` being 4,096. This existing inconsistency is also documented in
  `docs/FOLLOWUPS.md`.

**Concrete malformed cases**

- A 201-byte `CONFIG_WRITE` start is ACKed as an active one-chunk transfer but never saved.
- A declared total of 201 followed by a 200-byte continuation commits 400 bytes because the
  chunk count reaches two; it should reject the 199 excess bytes.
- A declared total of zero or 200 in the chunked shape produces nonsensical expected counts and
  can commit only after an unrelated continuation.
- A declared total above 4,096 is not rejected at start; the transfer fails later for an
  indirect reason.
- A valid 4,096-byte upload is rejected at the twenty-first frame.

Saving a byte sequence inconsistent with the declared outer config can make immediate reload
fail and leave the invalid blob as the persistent record. Even when downstream CRC parsing
rejects it, storage has already been changed.

**Required correction**

At start, require the precise chunked-start shape, require
`CONFIG_CHUNK_SIZE < totalSize <= MAX_CONFIG_SIZE`, verify the first data count does not exceed
the total, and compute an allowed expected count. For every continuation, require
`receivedSize + len <= totalSize`; require full 200-byte non-final chunks; and commit only when
`receivedSize == totalSize`. A short final chunk is valid only when it completes the exact
total. Resolve the known 20-versus-21 canonical constant before advertising 4,096 as fully
transferable.

**Regression test matrix**

Test totals 201, 399, 400, 401, 4,000, 4,001, and 4,096, plus zero, 200, 4,097, truncated first
frames, short middle chunks, excess final bytes, duplicate finals, continuations without a
start, and disconnect during reassembly. For every rejection, verify no NVS record changes.

### F4 — Medium — BLE lifecycle and advertising state has cross-task data races

**Evidence**

- `targets/esp32-idf/ble/od_ble_nimble.cpp:100-121` declares `s_addr_resolved`, `s_inited`,
  `s_name`, `s_msd`, and `s_msd_len` as ordinary objects.
- `targets/esp32-idf/ble/od_ble_nimble.cpp:123-138` explicitly documents that
  `s_adv_wanted` is shared between asynchronous host events and loop-task requests, but leaves
  it as a plain `bool`.
- The NimBLE host task writes `s_addr_resolved` and reads advertising state at `:579-591` and
  `:490-524`.
- The loop task polls `s_addr_resolved` at `:691-701`, writes MSD data at `:811-818`, and writes
  `s_adv_wanted` at `:821-836`.

These are C++ data races. `vTaskDelay()` and calls into NimBLE do not establish a C++ memory
ordering relationship for unrelated plain objects. On a dual-core S3, the behavior is not
only theoretical: sync can be observed late or not at all, an advertisement can consume a
torn MSD update, and an `ADV_COMPLETE` event can read stale “wanted” state and restart
advertising after deep-sleep teardown requested a stop.

The existing TSan suite does not cover this file; it exercises `link_owner.cpp` only.

**Required correction**

Move all advertising-state mutations onto the NimBLE host task through its event queue, or
protect a coherent snapshot with an explicit synchronization primitive. Atomic booleans alone
are insufficient for the MSD buffer: the byte array and length must publish as one consistent
record. The initialization/sync state should also have an explicit acquire/release handoff.

The cross-target resolution is developed in
[`F4_PORTABLE_BLE_LIFECYCLE_PLAN.md`](F4_PORTABLE_BLE_LIFECYCLE_PLAN.md). It selects a
loop-owned, plain-C reconciler rather than making the NimBLE host task the policy owner, so the
same design applies to Bluefruit, Nordic/Zephyr, and the kernel-free Silabs superloop.

### F5 — Medium — LAN opcode and client-admission behavior disagrees with the canonical protocol

This is partly an implementation defect and partly a contract-resolution defect.

**PIPE over LAN**

- `shared/protocol/opendisplay_protocol.h:944-947` says `0x0080`, `0x0081`, and `0x0082` MUST
  NOT be used over LAN.
- `docs/DIVERGENCE_MATRIX.md:450` restates that the dispatcher must reject them based on origin.
- `targets/esp32-idf/src/communication.cpp:863-874` dispatches all three opcodes without an
  origin gate.
- `targets/esp32-idf/src/display_service.cpp:2788` begins a PIPE session without rejecting LAN.

The current implementation therefore exposes an explicitly forbidden protocol over LAN.
TCP already supplies the ordering and retransmission PIPE implements, and a host is directed to
use DIRECT_WRITE instead.

**Evict versus refuse**

- `shared/protocol/opendisplay_protocol.h:948-951` says a new LAN client evicts the prior one.
- `targets/esp32-idf/src/wifi_service.cpp:1398-1403` refuses the new connection whenever the
  ownership slot is held.
- `docs/CONNECTION_POLICY.md` deliberately chooses refusal because unauthenticated eviction can
  disrupt an in-flight transfer.

The refusal policy is defensible and likely safer, but the canonical normative header still
promises eviction. A conforming reconnecting client can therefore be refused until the old
socket is detected dead or times out. One source must become authoritative and the other must
be updated; leaving both normative behaviors in the same repository is not correct.

**Required correction**

Reject PIPE START/DATA/END from both plaintext and TLS LAN with the documented unsupported/error
shape, without touching transfer state. Separately, decide whether refusal supersedes the
canonical eviction rule and update the protocol version/change notes and host expectations if
it does.

### F6 — Medium — LAN response writes do not preserve framed-message integrity

**Evidence**

- `targets/esp32-idf/src/wifi_service.cpp:513-531` stages a small frame but calls TCP or TLS
  write once.
- A positive short `mbedtls_ssl_write()` is treated as complete because only negative results
  are checked.
- A plaintext short write is logged at `:528-529`, but the unwritten suffix is discarded.
- The oversized fallback at `:534-545` sends header and payload independently. A short or
  failed second write leaves the peer waiting for the length already announced by the header.

Stream sockets do not promise that one write consumes the requested length. TLS can also
return WANT_READ/WANT_WRITE or a positive partial count. Under send-buffer pressure, the peer
receives a truncated framed response and the stream is no longer self-synchronizing.

**Required correction**

Maintain a per-session transmit cursor/queue and resume until the complete length-prefixed
frame is accepted. Do not block the main loop indefinitely; WANT_WRITE and partial progress
should become normal deferred states. If the session closes before completion, discard the
whole pending frame as part of session teardown and report the transport failure.

### F7 — Medium — BLE deinitialization failure is hidden by the transport wrapper

**Evidence**

- `targets/esp32-idf/ble/od_ble_nimble.cpp:866-887` correctly leaves internal state unchanged
  when `nimble_port_stop()` fails, because the host task and controller remain live.
- `od_ble_deinit()` returns `void`, so the caller cannot observe that outcome.
- `targets/esp32-idf/src/ble_transport_esp32.cpp:360-363` unconditionally marks `s_ready` false
  and clears every instance after calling it.

The lower layer says BLE remains up while the upper layer says it is down. Any subsequent
lifecycle operation now reasons from inconsistent state; connection callbacks may still run
against an emptied instance table.

There is a related initialization cleanup gap at
`targets/esp32-idf/ble/od_ble_nimble.cpp:660-666`: after successful `nimble_port_init()`, GATT
registration failure returns without `nimble_port_deinit()`. A retry can attempt a fresh init
over partially initialized state.

**Required correction**

Return an explicit result from deinit and clear transport readiness/instances only on confirmed
success. Give `od_ble_init()` a single unwind path that deinitializes every successfully
acquired layer in reverse order.

### F8 — Medium — secure erase reports success when the zero-write failed

**Evidence**

- `targets/esp32-idf/hal/od_hal_nvs.c:157`, `:165`, and `:168` discard the results from the
  same-size zero blob write and its commit.
- `targets/esp32-idf/hal/od_hal_nvs.c:171-181` returns only the erase/second-commit status and
  always logs “zero-written and erased.”
- `targets/esp32-idf/src/encryption.cpp:899-901` discards even that result and always logs
  “Config securely erased.”

The HAL contract already states that NVS is log-structured and the overwrite is best-effort.
That limitation does not justify claiming that an attempted zero-write succeeded when NVS
returned an error. In the failure case, the old entry containing the AES master key may remain
recoverable from raw flash while the operator is told the secure erase completed.

**Required correction**

Track three distinct outcomes: no record, zero-write+commit succeeded followed by erase, and
erase succeeded without a successful zero-write. Propagate the result to the reset/config
caller and log it accurately. Decide whether failure to perform the zero-write should abort the
erase, fall back to whole-partition destruction with explicit authority, or erase while
reporting that forensic wipe was not achieved.

Inject failures for `nvs_set_blob`, the first commit, `nvs_erase_key`, and the second commit.

### F9 — Medium — WiFi event-to-loop handoff uses `volatile` instead of synchronization

**Evidence**

- `targets/esp32-idf/src/wifi_service.cpp:788-799` describes system-event-task writers and
  loop-task readers, then declares the flags/scalars `volatile`.
- `roamPending`, `usingCachedAp`, and `rescanReconnectPending` at `:785` and `:820-821` are not
  even volatile.
- Event handlers modify these objects at `:1038-1043` and `:1055-1075`; loop code consumes
  them at `:1146-1173`.

`volatile` prevents certain compiler elisions; it is not an atomic operation or inter-task
publication mechanism in C++. The paired scalar-then-flag writes also have no ordering, so the
loop can observe a pending flag with an old reason/channel/RSSI value. Unsynchronized
`usingCachedAp` decisions can miss the full-scan fallback after a cached BSSID fails.

**Required correction**

Use an ESP event queue or a small lock-free record with acquire/release publication. Preserve
each event's type and payload together rather than distributing it across several independent
flags. If coalescing is intentional, document which events may be lost and make the retained
state authoritative.

### F10 — Low — the mDNS MSD “400 ms floor” is not a rate limit

**Evidence**

- `targets/esp32-idf/src/wifi_service.cpp:600-603` says every update is subject to a 400 ms
  floor and an unchanged-payload check.
- The condition at `:617` returns only when the payload is unchanged **and** less than 400 ms
  have elapsed.

A changed payload bypasses the time floor entirely, while an unchanged payload is sent again
as soon as 400 ms have passed. This is the opposite of a conventional “unchanged means no
update; changed is rate-limited” policy and permits bursts of multicast re-announcements on a
battery device sharing its radio with BLE.

The correction needs a pending-latest-value mechanism: suppress unchanged values indefinitely,
publish a changed value immediately only if the floor has elapsed, otherwise remember it and
publish once the remaining interval expires. Simply returning for a changed value would lose
the update if no later caller arrives.

## Automated validation

### Firmware build matrix

`targets/esp32-idf/build.sh` was run across all configured targets. Final result: **10 of 10
boards built successfully**, producing:

- `c3-n4`
- `c3-n16`
- `c6-n4`
- `esp32-n4`
- `s3-n8r8`
- `s3-n16r8`
- `s3-n16r8-extuart`
- `s3-n16r8-extuart-debug`
- `s3-n32r8`
- `s3-n32r8-extuart`

The first incremental matrix invocation reported failures for `s3-n16r8`, `s3-n32r8`, and
`s3-n32r8-extuart`. Their logs showed invalid/stale generated CMake state, including missing
managed-component temporary files and a truncated Ninja file. Re-running those named targets
regenerated their build trees and all passed. This is not evidence of a source compilation
failure, but it is a reproducibility warning: CI/release builds should start from clean or
validated build directories rather than trust generated state indefinitely.

All ten merged binaries and `release/MANIFEST.txt` were produced. `release/` is ignored and the
review left the tracked worktree clean.

### Host and contract gates

| Gate | Result | Notes |
|---|---|---|
| `targets/esp32-idf/tools/run_host_tests.sh` | pass | ASan+UBSan and TSan+UBSan; 70,170 checks and zero failures in each sanitizer run |
| LeakSanitizer | environment-limited | LSAN cannot operate under the runner's ptrace setup; rerun used `ASAN_OPTIONS=detect_leaks=0` while retaining ASan/UBSan |
| `targets/esp32-idf/compat/ratchet.sh` | pass | compatibility budget remains at the documented floor of 5 |
| `targets/esp32-idf/tools/sdkconfig_baseline.sh` | pass | all ten board baselines |
| host C99/CMake canary | pass | 1/1 CTest; shared source list remains empty by design |
| Python 3.13 protocol replay | pass | 23 vectors: 15 passed, 0 failed, 8 explicitly skipped because the public host API cannot express the firmware-side assertion |
| shell syntax checks | pass | key build, ratchet, baseline, and test scripts |
| shared-boundary forbidden-include scan | pass | no target/vendor include leaked into `shared/` |
| branch whitespace check | one documentation nit | trailing whitespace in `targets/esp32-idf/README.md:191`; no correctness impact |

The sanitizer host suite covers the connection-owner arbiter, not the BLE and WiFi task-shared
state identified in F4/F9. A green TSan result must not be read as a whole-firmware race audit.

### Standalone inflater test

`targets/esp32-idf/tools/test_zlib_stream.c` was selected for an additional ASan/UBSan host
run, but the environment lacks the development `zlib.h` header. The test could not compile.
The firmware's target-side inflater still compiled for all relevant boards, but the standalone
differential test remains unexecuted here. The test is not currently wired into repository CI,
as already noted in `docs/TEST_OWNERSHIP.md`.

## Coverage limitations

This review did not have attached hardware or live BLE/WiFi peers. Static proof is sufficient
for F1-F3 and F5-F8, but hardware/timing validation remains necessary for the surrounding
subsystems:

- repeated real lwIP/TLS connect and close behavior;
- BLE advertising, stop/deinit, sync timing, CCCD subscription, and notify under coexistence;
- WiFi cached-BSSID failure and RSSI-low roaming;
- mDNS announcement cadence on the air;
- NVS failure injection and raw-flash verification of the secure-erase policy;
- config read/write survival across reboot;
- uncompressed image push and interrupted-transfer recovery;
- FastEPD and bb_epaper refresh behavior on every supported controller family; and
- deep-sleep teardown while BLE/LAN events are arriving.

The repository's existing S3 hardware result remains valuable, but its target README states
that the exercised configuration had WiFi disabled. It therefore does not cover the LAN defects
that dominate this report.

## Recommended correction order

1. **Make teardown mechanically safe:** fix F1 and add the repeated-close socket test before
   changing other LAN lifecycle behavior.
2. **Repair configuration invariants:** fix F2 and F3 together, because both touch config
   authorization/reassembly and should share one integration matrix.
3. **Enforce origin policy:** reject PIPE on LAN and resolve evict-versus-refuse in the
   canonical protocol and host behavior.
4. **Make egress lossless:** introduce deferred complete-frame writes for TCP/TLS.
5. **Make task ownership explicit:** serialize BLE advertising/MSD state and replace WiFi's
   volatile flag bundle with an ordered event handoff.
6. **Make lifecycle/error truth propagate:** return BLE teardown status and secure-erase
   outcomes, including init unwind and NVS fault tests.
7. **Correct and test mDNS coalescing.**
8. **Repeat the full clean board matrix and hardware Gate 2 tests.**

## Suggested release acceptance criteria

The ESP-IDF phase-C work should be considered correctness-ready when all of the following are
true:

- every finding F1-F9 has either a merged fix with regression coverage or an explicit,
  versioned protocol/design decision explaining why current behavior is correct;
- at least 500 sequential LAN connect/close cycles complete without descriptor growth;
- TLS config read, single-frame write, chunked write, reboot, and exact read-back work without
  app-layer authentication inside TLS;
- malformed chunk sequences cannot change the stored record;
- LAN PIPE frames are deterministically rejected without disturbing a BLE or DIRECT_WRITE
  session;
- forced short TCP/TLS writes still deliver exactly one complete framed response;
- advertising cannot restart after stop/deinit, and MSD snapshots cannot tear under concurrent
  host events;
- injected BLE-stop and NVS failures are accurately reflected at the public caller and in logs;
- all ten targets build from clean directories and pass the ratchet/baseline gates;
- the host sanitizer suite, protocol replay, and standalone inflater differential test pass;
  and
- an ESP32-S3 with WiFi enabled completes LAN plaintext/TLS image pushes, BLE coexistence,
  interrupted-transfer recovery, config persistence, and deep-sleep teardown on hardware.

## Overall assessment

The core direction is sound: target-specific dependencies are contained, the compatibility
ratchet has reached its intended floor, board configuration is explicit, ownership has a real
atomic model, and the build/test documentation is unusually candid about what has and has not
run. The remaining defects cluster at boundaries where “state exists” was confused with “peer
is live,” or where transport/task origin was applied in one layer but not the next. Those are
fixable without reversing the phase-C architecture.

The appropriate disposition is therefore **hold ESP-IDF LAN correctness sign-off, fix the
three high-severity findings first, then close the synchronization and error-propagation gaps
before declaring the port complete**.
