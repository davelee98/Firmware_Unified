# Plan: C12 — close the wire corpus and hardware evidence gaps

**Status:** software half LANDED 2026-08-16 (C12.0-C12.2); hardware half open and ACCEPTED-UNRUN. `main` points at C11's merge commit
`dfde3bd`; the parent records C8-C11 as landed and C12 as the remaining unit
(`.git/refs/heads/main:1`, `plans/PLAN_OD_DISPATCH_2026-08-14.md:3-8`).

**Parent plan:** [`PLAN_OD_DISPATCH_2026-08-14.md`](PLAN_OD_DISPATCH_2026-08-14.md), especially
sections 7 and 9. This document replaces its one-line “C corpus runner + hardware passes” C12 row
(`plans/PLAN_OD_DISPATCH_2026-08-14.md:439-454`) with an executable software-first sequence.

**Baseline:** the recorded software gate is 13 passed, 0 failed, 0 skipped; there is no CI and a
skip is not a pass (`docs/OD_SESSION.md:131-150`, `tools/check.sh:15-19`). The execution gate is
**ACCEPTED-UNRUN**: the C10 Nordic matrix, ESP32-S3 smoke gate and C11 exit matrix were not run
because no board was available (`plans/PLAN_OD_DISPATCH_2026-08-14.md:6-9`). C12 preserves that
label until physical evidence exists.

---

## 1. Outcome

C12 closes two independent gaps, in this order:

1. **Software, landable with no board.** The single authoritative
   `tests/vectors/dispatch.json` corpus is schema-validated, generated into a build-directory C
   table, and replayed through the production `od_dispatch_frame()` path. Every applicable H2D
   vector checks the ordered firmware reply list, including explicit silence. A current-target
   profile uses production reply-producing command adapters where that target owns the bytes; a
   separately labelled behavior fixture covers historical/capability variants current production
   cannot emit. No fake may read `expect.reply` and echo it back, and fixture-produced bytes are
   never reported as production-handler coverage.
2. **Hardware, runnable later without redesign.** Three previously skipped matrices are executed
   and recorded separately: the C10 Nordic baseline, the ESP32-S3 pre-C11 smoke, and the final C11
   exit matrix. Deliberately replaying an already-sealed PIPE DATA frame is mandatory on every
   available promoted board; an ordinary encrypted upload cannot satisfy that item
   (`plans/PLAN_OD_DISPATCH_2026-08-14.md:593-602`).

The software series merges first and remains useful if no board appears. It does **not** convert
ACCEPTED-UNRUN into a pass. C12 as a whole closes only after the hardware evidence commits land; if
they cannot run, the software half may be accepted while C12 remains explicitly open.

---

## 2. Corrections and decisions before implementation

### 2.1 The C11 plan's check count is stale

The C11 plan says 12/0/0 in its sequence and definition of done
(`plans/PLAN_OD_DISPATCH_C11_2026-08-16.md:380-395`,
`plans/PLAN_OD_DISPATCH_C11_2026-08-16.md:538-551`). C11 actually finished with **13/0/0** after
the ownership ratchet became a named check; the current inventory is recorded at
`docs/OD_SESSION.md:131-150` and implemented by `tools/check.sh:133-193`,
`tools/check.sh:195-226`, `tools/check.sh:257-333`. C12's acceptance number is therefore 13/0/0
unless this plan deliberately adds a new top-level `check` invocation. Registering a new CTest
binary inside both host-suite invocations does not change that top-level count.

### 2.2 “C corpus runner” means the dispatch corpus, not every vector file

`dispatch.json` starts C12 with 16 vectors; three are D2H observations rather than inputs to a
dispatcher (`tests/vectors/dispatch.json:71-97`, `tests/vectors/dispatch.json:193-205`).
`config_tlv.json` contains the other seven vectors (`tests/vectors/config_tlv.json:25-158`). C12
adds one H2D vector for the current firmware-version-with-patch response, so its post-migration
inventory is **24 total: 17 dispatch vectors (14 H2D, 3 D2H) plus 7 config vectors**. It drives the
14 H2D dispatch vectors through `od_dispatch_frame()` and schema-validates all 24. It does not
pretend that `od_dispatch_frame()` can execute a D2H notification or that a dispatch runner is a
config-object runner.

Nor can all 14 be called “current Nordic replies” without qualification. The existing H2D firmware-version
vector intentionally expects a captured no-patch response (`tests/vectors/dispatch.json:55-68`),
while the C11 Nordic handler always appends the patch byte
(`targets/nordic-zephyr/src/od_cmd_device.c:58-87`). C12 preserves that historical vector, labels
its execution fixture-owned, and adds a separate
`dispatch/firmware-version-current-with-patch` H2D vector whose reply is produced by the Nordic
production profile. It sends `0043` over BLE with security enabled/no session, supplies version
1.5, SHA `1a2b3c4d` and patch 3 as independent fixture inputs, and expects
`0043010508316132623363346403`.

**Two of those three are runtime knobs and one is not.** Version and patch come from the faked
`opendisplay_ble_get_app_version{,_patch}()`, but the SHA is a COMPILE-TIME macro: `od_cmd_device.c`
defaults `SHA` to `""` and stringifies it (`targets/nordic-zephyr/src/od_cmd_device.c:21-26`), and
the host build defines none, so today it yields the 40-zero placeholder. The Nordic corpus
executable therefore needs its own `target_compile_definitions(... SHA="1a2b3c4d")`. That is still
an independent semantic input -- production code derives the bytes -- but it is a per-executable
define, so it interacts with the link isolation in § 4.2 and must not be set globally. A vector has one expectation; C12 does not add profile-selected
alternative answers inside one vector. The new H2D vector shares bytes with the existing D2H
firmware-version-with-patch observation, but each retains its direction and proof classification.
**They must fail together.** Two vectors asserting one byte string is only safe if a change to
Nordic's reply shape breaks both; if the D2H one can stay green while the H2D one moves, they will
drift and the stale one becomes a false witness. The generator therefore emits a build-time
assertion that the two byte strings are equal, and any change to either must change both.
The direct-END vector and its separate notification
already document one multi-reply operation (`tests/vectors/dispatch.json:177-205`); C12 expresses
that ordering with `expect.replies` while retaining the D2H vector's host-decoder purpose.

The Python runner remains the host-side half. It states that firmware replies and firmware state
are outside its reach (`tests/host/replay_vectors.py:14-23`) and reports every H2D reply as
unchecked (`tests/host/replay_vectors.py:44`, `tests/host/replay_vectors.py:144-170`). The new C
runner closes that reply-side gap; it does not replace the public-API host check.

### 2.3 Resolve the schema gaps now

Three gaps were found while the corpus was still only 23 vectors and were explicitly marked for
decision before growth (`docs/FOLLOWUPS.md:217-232`). Review found a fourth: dispatch origin was
implicit even though it changes shared behavior. C12 takes these decisions before adding vector 24:

1. **Adopt `forbids`.** It is an optional array with exactly the inverse meaning of `requires`.
   Names are stable corpus capability symbols; each runner profile maps them to its build/runtime
   configuration. Unknown names, duplicates, or the same name in both arrays are schema errors.
   The current `cap_partial`, `cap_buzzer` and `cap_power_latch` booleans are migrated to named
   negative predicates rather than kept as an undocumented workaround
   (`tests/vectors/dispatch.json:11-20`, `tests/vectors/dispatch.json:223-268`). No production
   Kconfig option is created merely to support the corpus.
2. **Bless `expect.parsed`.** It remains an optional map of dotted field path to JSON scalar, with
   the meaning already documented in `tests/vectors/config_tlv.json:10-13`. The schema validator
   checks it now. Execution is explicitly deferred to a later config-corpus adapter because
   `od_dispatch_frame()` exposes an outcome and replies, not a parsed `struct od_config`; silently
   treating it as checked in C12 would make the highest-value config vector assert nothing
   (`tests/vectors/config_tlv.json:49-67`).
3. **Adopt sequences without rewriting all existing vectors.** A legacy `{dir,frame,expect}` is
   shorthand for one step. A vector may instead carry ordered `steps`, and each H2D step may use
   `expect.replies` as an ordered array; `expect.reply` remains the zero-or-one shorthand.
   This expresses both an ACK followed by a refresh notification and a 21-frame config transfer,
   the two concrete missing shapes (`docs/FOLLOWUPS.md:230-232`). The generator must accept both
   forms, reject a vector that mixes them ambiguously, and preserve state between steps but reset
   it between vectors.
4. **Make dispatch origin explicit.** An H2D vector or step may name `origin` as `ble`, `lan-plain`
   or `lan-tls`; a step inherits its vector's value. Existing H2D vectors receive an explicit
   `ble` value during C12.0 rather than relying on a runner default. This is behavior, not capture
   provenance: BLE has a 244-byte dispatcher ceiling, LAN has a 4094-byte ceiling, and TLS-LAN
   bypasses the CCM gate. D2H-only observations may omit it because they are not dispatched.
   Unknown origins and an H2D step with no effective origin are schema errors.

Each executable expectation also gains a coverage classification: `shared`, `target-production`
or `historical-fixture`, plus the profile predicates that make it reachable. This is not a fourth
wire-schema feature; it is the proof boundary the runner needs so a fake-produced legacy response
cannot be reported as current firmware coverage.

These are backward-compatible schema additions under `opendisplay-wire-vectors/1`; the migration
materializes the old BLE assumption and adds one vector but changes no existing frame or reply
bytes. It also corrects the oversize vector's stale note: current unified ESP32 and Nordic admit
BLE values through 253 bytes, so a 245-byte value reaches the shared 244-byte dispatcher ceiling.
If implementation discovers that compatibility requires ambiguous precedence, stop and version
the schema as `/2` rather than guessing.

### 2.4 Authored contracts and legacy captures are different evidence

The current prose is internally inconsistent. `docs/FOLLOWUPS.md:234-241` says every reply was
authored and nothing was observed, but `dispatch/firmware-version-plaintext-exempt` copies its
expected reply from `02_read_firmware_response.bin`, and several direct-write request frames are
also labelled real captures. Their original target/SHA/panel/date metadata is not in this
repository. C12 must preserve the bytes without inventing provenance or relabelling them authored.
The capture deadline has passed for the ESP32 and Nordic dispatch cutovers: shared dispatch is
already live on both (`CLAUDE.md:61-71`). A green C runner therefore proves conformance to the
recorded contract; only provenance-complete capture can prove historical fleet behavior.

C12 makes provenance machine-readable instead of leaving it buried in `expect.note`. Each side
uses one of three kinds:

- `authored` requires a specification/source reference;
- `captured` requires target, firmware SHA, protocol version, panel, host version, transport and
  date, matching the ownership record (`docs/TEST_OWNERSHIP.md:291-309`);
- `captured-unattributed` is allowed only for the legacy external fixtures already present at the
  start of C12. It requires the external fixture name/source and a limitation string, cannot be
  used by a newly added vector, and never counts as a provenance-complete regression baseline;
- a later capture against the unified firmware is labelled with that post-migration SHA and may
  corroborate bytes, but must not be described as an untouched regression baseline;
- secrets, addresses, session keys, nonces and MAC material never enter the corpus
  (`docs/TEST_OWNERSHIP.md:302-305`).

Authored negative/error vectors remain valuable because a working host cannot generate many of
them (`docs/TEST_OWNERSHIP.md:253-266`). C12.0 corrects the false blanket-authored statement in
`docs/FOLLOWUPS.md`. C12 does not block the runner on unavailable capture; it blocks any claim that
authored, legacy-unattributed and provenance-complete capture are the same evidence.

### 2.5 Silabs is C13, before transfers

`efr32bg22-slc` is **not** part of C12. It consumes only `OD_SHARED_SOURCES_PURE`
(`targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:275-285`), while `od_dispatch.c` belongs to
the APP_SESSION tier (`shared/sources.cmake:127-138`). It defines no `od_cmd_app_*` hook and still
owns a complete opcode switch in its 1,303-line pipe file
(`targets/efr32bg22-slc/opendisplay_pipe.c:1090-1186`,
`targets/efr32bg22-slc/opendisplay_pipe.c:1303`). Calling that a C12 “hardware pass” would hide a
full target migration inside evidence collection.

Silabs gets its own C13 execution plan and comes next. That order is architectural, not cosmetic:
BG22 has 32 KB RAM, no kernel and no Kconfig (`CLAUDE.md:235-238`, `CLAUDE.md:315-320`), and the
declared migration order puts it third so those constraints bite before later shared assumptions
become expensive (`CLAUDE.md:281-300`). Its untouched dispatcher should be captured before C13;
with no board, C13 must not begin the behavioral cutover under a claim that the time-sensitive
capture gate was met (`docs/TEST_OWNERSHIP.md:268-300`).

### 2.6 Transfer promotion follows Silabs and has its own plan

C12 does **not** promote direct-write, partial-write, PIPE or NFC state machines. C11 deliberately
left them target-owned (`plans/PLAN_OD_DISPATCH_C11_2026-08-16.md:40-42`), and `shared/core` still
contains no transfer source (`shared/sources.cmake:108-160`). The scale is not an adapter cleanup:
ESP32's display state machine reaches `targets/esp32-idf/src/display_service.cpp:3402`, Nordic's
display/PIPE implementations reach `targets/nordic-zephyr/src/opendisplay_display.cpp:1332` and
`targets/nordic-zephyr/src/opendisplay_pipe_write.cpp:595`, and Silabs' display implementation
reaches `targets/efr32bg22-slc/opendisplay_display.cpp:903` before their command adapters are
counted.

That promotion needs its own design plan after C13. Before any `od_xfer_partial.c` or
`od_zlib_stream.c` lands, re-argue the plain-C decision: the repository names those nested-resource
lifetimes as the last cheap decision point (`CLAUDE.md:204-226`). Promoting against only the two
kernel targets would design around the constraint the migration order exists to expose.

---

## 3. Invariants C12 may not move

These are acceptance criteria:

- `tests/vectors/` remains the only corpus copy and this repository remains its owner
  (`docs/TEST_OWNERSHIP.md:15-36`). Generated C exists only under the build directory; no committed
  `.inc` mirror becomes a second authority.
- The C runner compiles the production `shared/` source list under C99, no extensions,
  `-Wall -Wextra -Werror`, under gcc and clang (`tests/host/CMakeLists.txt:22-45`).
- No JSON parser or JSON dependency enters firmware or `shared/`. `shared/` remains plain C and may
  include only the C library and shared HAL interfaces (`CLAUDE.md:195-226`).
- The generator uses only the Python standard library, performs no network access, and makes a
  missing Python interpreter a configure failure for the corpus target rather than a silent CTest
  omission. The optional `py-opendisplay` import remains independently reported
  (`tests/host/CMakeLists.txt:336-379`).
- A target-owned expected reply is produced by production command code linked against fakes of its
  dependencies before it is counted `target-production`. A corpus fake may choose
  storage/display success or capability state; it may not include, link, receive or consult the
  generated expectation table. A historical behavior fixture is labelled as such and proves only
  dispatcher integration. Literal independence remains a code-review property; no structural
  check can prove that a copied byte literal was derived independently.
- Each H2D call is followed by exactly one `od_core_frame_done()` call, every queued frame is
  drained in order, silence means zero captured sends, and fixture state is reset between vectors.
  The production dispatch contract requires the completion call (`shared/core/od_dispatch.h:36-45`).
- Dispatch order, reservation, pre-decrypt-only deferral, explicit reply confidentiality, unknown
  silence and OD-S1 PIPE silence remain unchanged (`shared/core/od_dispatch.h:3-20`,
  `shared/core/od_dispatch.c:128-140`, `shared/core/od_dispatch.c:143-231`,
  `tests/host/gate_test.c:232-258`).
- Vector meaning is append-only. Corrections retain the old shape with `deprecated_after`; deletion
  is not used to erase deployed behavior (`docs/TEST_OWNERSHIP.md:311-330`).
- No opcode, error code, envelope byte, target capability or vendored protocol header changes.
  The vendored headers remain byte-for-byte canonical copies (`CLAUDE.md:276-279`).
- Every accepted commit leaves all targets link-complete and finishes
  `tools/check.sh --targets` with zero failures and zero skips. Build/host proof never changes a
  target's hardware status (`tools/check.sh:300-350`, `docs/MIGRATION.md:292-300`).

---

## 4. Target architecture after the software half

### 4.1 Build-time JSON generation, not a vendored parser

Add a standard-library Python generator under `tests/host/` and make CMake generate
`<binary-dir>/generated/dispatch_vectors.inc` from `tests/vectors/dispatch.json` with an explicit
dependency on both files. The generated table contains decoded frame/reply byte arrays, ids,
direction, effective origin, initial state, capability predicates, steps and provenance enums. It
contains no notes and is never checked in.

This is preferable to the alternatives:

- **Vendored C JSON parser:** adds code, license/provenance and malformed-JSON behavior to test
  infrastructure even though JSON is not a product input. It also makes the runner test a parser
  C12 does not intend to ship.
- **Runtime hand parser:** violates the strict C99 suite's purpose by adding a bespoke parser whose
  correctness can dominate failures.
- **Committed generated table:** permits stale green results and creates the corpus copy the
  ownership decision forbids.
- **CMake JSON parsing:** the project floor is CMake 3.16 (`tests/host/CMakeLists.txt:13-19`), so do
  not raise the toolchain floor merely to parse test data.

Generation at build time, rather than only at configure time, ensures editing the JSON rebuilds
the table without relying on a developer to rerun CMake. A `--check` mode validates all vector
files and emits nothing; that is the schema gate used by both the C runner build and the pinned
Python replay.

### 4.2 One runner implementation, two link-isolated proof executables

Add one common runner source plus two executables to `tests/host/CMakeLists.txt` beside the existing
dispatch and route suites (`tests/host/CMakeLists.txt:161-181`):

1. `od_dispatch_corpus_portable_test` links production `od_shared`, the existing session fake and a
   **portable dispatch profile** whose behavior-only hooks make historical and capability states
   reachable. They build replies from independent semantic inputs such as version fields or a
   driver return code. This executable runs all H2D vectors through production shared dispatch but
   is not target-handler proof.
2. `od_dispatch_corpus_nordic_test` separately links production `od_shared`, the session fake and a
   **Nordic production-command profile** compiled from target command translation units against
   fake Zephyr, storage, display, LED/buzzer and BLE seams. This follows the existing rule that
   target lifecycle tests compile production sources, not transcriptions
   (`tests/host/CMakeLists.txt:247-276`).

This separation is mandatory, not naming preference. C11's `od_cmd_app_*` seam is static link-time
composition: `od_dispatch.c` calls one fixed definition of every hook. Portable and Nordic hook
definitions cannot coexist in one binary without duplicate symbols, and C12 must not add a runtime
registry or rename production functions to evade that property. Both executables compile the same
runner source and generated table, but each resolves the seam once.

Nordic is the first production profile because its command adapters are C and already have host-compilation
precedent for device replies (`tests/host/CMakeLists.txt:296-324`). This does not certify Nordic or
ESP32 hardware and does not prove their private transfer algorithms agree. It proves shared
dispatch plus one real reply-producing target composition agrees with the common corpus. A later
ESP32 profile is warranted only when a vector expresses a deliberate target divergence that the
Nordic profile cannot own; do not add a C++ mirror merely for symmetry.

Both profiles own knobs, not generated expected-wire objects: security configured, live session,
storage success, transfer active, display return code, capability set, firmware version/SHA,
connection tag and captured radio sends. Gate/refusal/unknown replies come directly from shared
dispatch; config, direct-write and device replies counted as current-target coverage come from
production command adapters. The generated include directory and expectation object are visible
only to the common runner translation unit; fixture targets receive a narrow semantic-state header
and cannot include or link the generated table. If a vector cannot be reached without copying its
answer into a fixture, mark it unsupported and fail the software milestone until production code
is linked, independent fixture inputs are defined, or the proof classification is corrected.

### 4.3 Execution and accounting

For every vector:

1. reset session, queues, target command state, fake drivers, clock and captured sends;
2. evaluate `requires`/`forbids` against the named profile and fail on an unknown predicate;
3. apply initial state, construct `od_reply_t` from the step's explicit origin and live tag, and run
   each step in order;
4. for H2D, call `od_dispatch_frame()`, call `od_core_frame_done()` once, drain `od_txq`, and compare
   every captured wire frame byte-for-byte and in order;
5. for D2H-only steps, validate and count them as `direction-only`, never “passed through C”;
6. report totals: vectors discovered, H2D steps executed, D2H-only steps, predicate exclusions and
   failures, split by `shared`, `target-production` and `historical-fixture`. Zero discovered or
   zero H2D executed is a test failure.

The CTest passes only if all applicable H2D replies and silence match. Every H2D vector must run in
the portable profile. Every `target-production` expectation must additionally run in a registered
production profile; an exclusion is allowed only for an expectation explicitly classified
`historical-fixture`. Anything uncovered is a test failure, not a skip.

### 4.4 Hardware driver tool

Extend the existing BLE CLI with a bench-only `dispatch-gate` command rather than inventing a
second encryption implementation. The CLI already owns deterministic command sealing and raw GATT
writes (`targets/esp32-idf/tools/od-device-cli.py:101-207`,
`targets/esp32-idf/tools/od-device-cli.py:738-809`). The command must:

- log raw H2D and D2H frames with monotonic timestamps before response decryption;
- retain an already-sealed `0x0081` frame and write the identical bytes a second time without
  incrementing the client counter;
- provide a `withhold-notify` phase that disables the CCCD after authentication, writes a config
  read and then an unknown-opcode canary while notifications are disabled, waits for Nordic's
  existing consumer-side `unknown cmd 0x0060` log, then re-enables the CCCD and verifies exact
  config reassembly. RX is FIFO, so observing the canary at dispatch proves the preceding config
  read passed dispatch while notifications were disabled. This deterministically drives
  `od_hal_radio_send()` through its existing `!notify_enabled -> OD_RADIO_RETRY` arm without a
  production fault hook or a modified firmware SHA;
- classify notifications received in a bounded observation window;
- continue the transfer with fresh counters and verify completion;
- emit a provenance/result JSON record, with secrets redacted, for the evidence commit.

The ordinary read/write CLI paths must remain unchanged. Unit-test seal-once/send-twice and CCCD
withhold/re-enable against a fake BLE client before either is used on a board. The fake must show
that both command writes occur while notifications are disabled and that captured delivery begins
only after re-enable.

---

## 5. Commit sequence

**THREE PRs, not two.** C12.0-C12.1 is the corpus runner. C12.2's bench tool is its own PR: it has
no dependency on the runner, it is the one piece that cannot be validated without a board, and
folding it in makes a single PR larger than anything in C11 (schema migration of 23 vectors, a
validator, the Python adaptation, a generator, two executables, a Nordic fake set, a CLI command,
fake-BLE tests and twelve mutations). Hardware evidence is a fourth, separable PR; none of it may
hold the runner hostage to board availability.

The Nordic profile's fakes are also less new than the commit table implies: `nordic_cmd_device_test`
already fakes BLE/LED/buzzer (`tests/host/CMakeLists.txt:296-324`) and `pipe_write_test` already
fakes display/reply/kernel. Config storage is the genuinely new one.

| Commit | Content | Required proof |
|---|---|---|
| **C12.0** | Add the backward-compatible schema validator; decide `forbids`, `expect.parsed`, sequences/reply lists, explicit origin, proof classification and structured provenance; migrate the 23 existing vectors, add the current-with-patch H2D vector, correct stale notes and adapt the Python runner without changing existing wire bytes | Validator positive/negative fixtures; pinned public-API replay unchanged; all 24 discovered; historical no-patch reply retained; current-with-patch vector present; legacy captures labelled unattributed rather than invented; `git diff` shows no existing frame/reply byte changed without a documented correction; full 13/0/0 target gate |
| **C12.1** | Add the build-time generator and common corpus runner; build link-isolated portable and Nordic executables with their own hook sets; register both in CTest | gcc/clang/sanitizers execute both; 17 dispatch vectors discovered, 14 H2D vectors core-covered, 3 D2H-only accounted; production-vs-fixture totals explicit; fixture targets cannot access generated expectations; full 13/0/0 target gate |
| **C12.2** | Add the bench `dispatch-gate` replay and CCCD-withhold modes with fake-BLE tests; run the mutation list; update status/ownership/follow-up text to distinguish authored contract, legacy capture, generated test data and provenance-complete capture | Bench-tool unit tests; mutation evidence; `git diff --check`; clean-tree 13/0/0; hardware debt still explicitly ACCEPTED-UNRUN |
| **C12.H1** | At C10 `a37c04b`, run and record the Nordic C10 matrix | Board id, target/board, exact SHA, tool/host versions, raw transcript, logs and per-row PASS/FAIL; current-tree 13/0/0 before the evidence commit |
| **C12.H2** | At C10 `a37c04b`, run and record the ESP32-S3 smoke | Same provenance; two fresh auths, encrypted command/upload, disconnect/reconnect; current-tree 13/0/0 |
| **C12.H3** | On the final C12 software SHA, run the C11 exit matrix on every available promoted board, including mandatory OD-S1 injection; record final status | Per-row evidence and controls below; no ACCEPTED-UNRUN row silently converted to PASS; clean-tree 13/0/0 |

If a software step exposes a firmware defect, stop the C12 series. Reproduce it in an ordinary host
test, then write a separately reviewed fix plan/commit; do not edit an expectation to match the
current bug. C12 is test infrastructure and evidence, not a protocol-change umbrella.

---

## 6. Automated verification

### 6.1 Required coverage

- Schema: malformed root, missing/duplicate id, odd/non-hex bytes, invalid direction/origin, an H2D
  step with no effective origin, unknown state/provenance field, contradictory
  `requires`/`forbids`, mixed single/sequence form, empty steps, ambiguous `reply`+`replies`, missing
  complete-capture metadata, missing legacy-capture source/limitation and secret-shaped capture fields.
- Generator: deterministic byte-identical output, build dependency regeneration, no output in
  `--check`, and zero-vector rejection.
- Runner: plain/auth-required/decrypt-failure/oversize/unknown, two- and four-byte replies,
  explicit silence, handler NACK, ordered multi-reply, state reset, predicate/profile coverage,
  proof-classification accounting and exact discovery counts.
- Bench tool: the duplicated write is byte-identical, the counter advances once, raw capture occurs
  before decode, timeout is bounded, secrets are redacted, CCCD withholding writes while
  unsubscribed and delivers after re-enable, and normal CLI send behavior is unchanged.
- Existing dispatch/order/routing, gate, reply, TXQ, session, target-device and PIPE suites remain
  green (`tests/host/CMakeLists.txt:129-181`, `tests/host/CMakeLists.txt:278-324`).

### 6.2 Mutation checks required before C12.2

Perform each mutation locally, observe the named failure, then restore it:

1. flip one byte of `dispatch/ack-width-config-write-4byte`'s `expect.reply` — both applicable
   corpus executables fail with that id and byte offset;
2. change `dispatch/unknown-opcode-no-reply` from `null` to a frame — the silence assertion fails;
3. make the generator accept odd-length hex — the negative schema test fails;
4. remove the JSON file from the generated output dependency, edit one reply, and rebuild — the
   regeneration test fails;
5. omit fixture reset between two reordered vectors — the order-independence/state-reset case fails;
6. route one opcode to its neighbor in `od_dispatch.c` — `dispatch_route` and the corpus runner both
   fail, independently;
7. invert one `forbids` decision — predicate coverage reports the vector uncovered or executes it
   under the wrong profile and fails;
8. delete/change the oversize vector's BLE origin — schema validation or the reply assertion fails;
9. treat a D2H vector as H2D — direction accounting fails before dispatch;
10. make the bench helper seal twice instead of resending the retained bytes — its fake-BLE test
   detects different wire bytes and a counter delta of two;
11. add the generated include directory/header or expectation object to a fixture target — the
    CMake/source boundary check fails. Separately review fixture literals against their stated
    semantic inputs; the plan does not claim a grep can detect a manually copied byte literal;
12. make the CCCD-withhold helper write only after re-enable — its fake-BLE ordering test fails.

Mutation 6 retains C11's routing mutation; C12 is additive, not a replacement for the proven C11
list recorded at `docs/OD_SESSION.md:141-150`.

### 6.3 Full software gate

Run `tools/check.sh --targets` after every accepted commit and once from a clean tree for C12.2.
Required summary: 13 passed, 0 failed, 0 skipped. The new runner executes inside gcc, clang and
ASan/UBSan host suites; the existing top-level gate still covers boundary rules, fuzzing, pinned
host replay, shim ratchet, all ten ESP32 fragments/baselines and all three Nordic boards
(`docs/OD_SESSION.md:131-139`). Run `tools/check.sh --latest` before release; without CI it is
advisory only and runs only when requested (`tools/check.sh:284-298`).

---

## 7. Hardware gates

### 7.1 Evidence rules common to all three matrices

Each result records target, board/serial, firmware SHA, build fragment, protocol version, panel,
host and CLI versions, transport, date, raw frame transcript, device log, expected result and
PASS/FAIL. A build, host suite, boot log or “notification queued” log is not on-air evidence.
Photograph/video the physical refresh items and correlate them with transcript timestamps.

Run historical SHA `a37c04b` for H1/H2, then the final C12 software SHA for H3. That ordering is
what separates pre-C11 stacked debt from C11 behavior. If H1 or H2 fails, stop before H3 and bisect
the older unflashed layers; a final-tree pass cannot explain why the historical baseline failed.

### 7.2 H1 — C10 Nordic matrix at `a37c04b`

Minimum board: `xiao_nrf52840`. It last passed a real gate at C6; C9-C11 have not been flashed
(`docs/OD_SESSION.md:156-162`). Run:

| Item | Required observation | It distinguishes / it cannot distinguish |
|---|---|---|
| Authenticate, encrypted upload, disconnect/re-authenticate | image renders; new session succeeds after reconnect | Shows C9 RX + C10 dispatch/egress did not break the C6 session path / cannot isolate C9 from C10 if the first run fails |
| Config single write, chunked write, read-back, reboot/re-parse | sealed ACK precedes reload; bytes survive reboot | Separates storage/reload and multi-frame producer failures / cannot prove TX retry without the next row |
| Config read under TX backpressure | authenticate and disable the command characteristic CCCD; write a maximum-size config read followed by unknown opcode `0x0060`; require the existing consumer-side `unknown cmd 0x0060` log before re-enable; wait several pump passes, re-enable notifications, and require exact ordered config reassembly | FIFO plus the canary log proves CONFIG_READ dispatched while notify was disabled, deterministically entering Nordic HAL's existing `RETRY` arm and proving the queued head survives / cannot prove controller-pool `-ENOMEM`, which has the same HAL verdict but a different cause |
| LED, buzzer, READ_MSD, FIRMWARE_VERSION | physical actuator effects and exact replies | C10 command routing/target adapters / cannot prove encrypted transfer ordering |
| No-session and decrypt failure | `{00,cmd,FE}` and `{00,cmd,FF}` visible plaintext | Gate and explicit confidentiality / cannot prove successful sealing |
| Unknown opcode; 245-byte value | silence for unknown; application NACK for 245 bytes | Dispatcher classification and reachable ceiling / cannot prove ATT rejection above 253 |
| Direct and PIPE END | ACK observed on air before panel refresh begins | Flush barrier and target handler order / cannot be inferred from enqueue logs |
| NFC 218/219 | 218 bytes arrive whole in a 253-byte sealed value; 219 returns NFC error without truncation | Nordic response ceiling / cannot be replaced by a plain NFC read |

The transcript must show the CCCD-disabled interval, both writes, the consumer-side canary log
before re-enable, no D2H delivery while disabled, and exact config delivery after re-enable. If the
stack refuses writes while unsubscribed or the canary appears only after re-enable, record the row
FAIL/UNRUN and revise the mechanism; do not call ordinary multi-chunk delivery “induced
backpressure.” Do not add a production fault hook merely to turn this row green.

### 7.3 H2 — ESP32-S3 pre-C11 smoke at `a37c04b`

Use the actual S3 board fragment matching the board. Run two fresh authentications, one encrypted
command, an encrypted PIPE upload through refresh, then disconnect/reconnect and authenticate
again. This separates C11 adapter/context changes from the older C1/C5/C8/C9 stack. It **cannot**
separate those older layers from one another if the smoke fails; bisect their landed commits before
running H3. ESP32 has no hardware result for C1/C5 or C8-C11
(`docs/OD_SESSION.md:160-162`).

### 7.4 H3 — final C11 exit matrix on the C12 software SHA

Run every applicable row, not only rows that changed since H1/H2:

| Target | Item | It distinguishes / it cannot distinguish |
|---|---|---|
| Nordic | authenticate, encrypted command, disconnect/re-authenticate; successful key replacement | Normal PSA/session lifecycle after C11 / cannot reproduce the injected PSA-destroy fault, which remains owned by the fake-PSA test (`plans/PLAN_OD_DISPATCH_C11_2026-08-16.md:490-492`) |
| Nordic | config single/chunk write, sealed ACK, reload/read-back/reboot; repeat the CCCD-withheld read with dispatch-before-re-enable evidence | C11 module split/reset preserved state and C10 producer/RETRY behavior / cannot prove the separate controller-pool `-ENOMEM` cause |
| Nordic | direct and PIPE END ACK before refresh; unknown silent; `0x52` four-byte unsupported NACK | Handler verdict/routing and flush ordering / cannot prove OD-S1 without deliberate replay |
| Nordic | encrypted NFC 218 bytes; stored 219-byte record errors | Sealed-size cap on target / cannot establish NFC behavior on ESP32, whose NFC hook is intentionally unknown |
| ESP32-S3 | two fresh authentications | C11 PSA random success path and normal key replacement / cannot inject RNG failure on hardware |
| ESP32-S3 | BLE encrypted command, PIPE smoke, disconnect/reconnect with no stale delivery | C11 explicit `{origin,tag}` and reset path / cannot prove LAN origin |
| ESP32-S3 | plaintext LAN and TLS-LAN commands; PIPE-over-LAN refusal; 4092-byte LAN DIRECT_WRITE; long idle-plus-accepted-traffic cycle | LAN origin, TLS CCM bypass, capability refusal and owner-clock stamp | Cannot prove BLE replay handling or WiFi reliability beyond this board/session |
| ESP32-S3 | config write ACK sealed before reload | C11 session ownership/order / cannot replace reboot persistence, which must be checked separately if config changed |

The source matrix is `plans/PLAN_OD_DISPATCH_C11_2026-08-16.md:468-492`; the broader parent items
remain binding at `plans/PLAN_OD_DISPATCH_2026-08-14.md:583-602`.

### 7.5 Mandatory OD-S1 injection — every available promoted board

For each available Nordic or ESP32 board that implements PIPE:

1. wait beyond the report throttle interval, authenticate, and open an encrypted PIPE transfer with
   a small `ack_every`;
2. have `dispatch-gate` seal one valid `0x0081` DATA frame once, send it, retain the exact raw bytes,
   and observe its normal acceptance/cadence;
3. write those identical sealed bytes again while the transfer is live. The second copy has the
   same session id/counter and must reach the application as a replay;
4. require **no notification** attributable to the replay, one throttled replay/nonce telemetry
   record, no integrity-strike/session teardown, and successful continuation with fresh sealed
   frames through END and rendered refresh;
5. in a fresh transfer, send a newly counted `0x0081` with a corrupted tag and require the normal
   plaintext hard NACK. This control proves the capture window could observe a response and that
   the silent replay was not merely a disconnected notify path.

The raw duplicate must be byte-identical. Re-encrypting the same PIPE sequence number under a new
CCM counter tests PIPE duplication, not the OD-S1 replay/silence arm. Likewise, a normal successful
upload does not satisfy this gate; the tree records that exact gap
(`docs/OD_SESSION.md:164-167`).

---

## 8. Risks and stop conditions

- **A circular oracle can make every vector green.** Stop if a fake emits expected bytes, if the
  generator derives expectations from production constants/helpers, or if a handler-owned reply is
  checked without production handler code. The build boundary proves only that fixtures cannot
  consult generated expectations; literal independence remains mandatory review evidence.
- **Schema migration can silently change meaning.** Stop on any changed frame/reply byte or vector
  count not explained by a reviewed correction. Do not “fix” host/spec divergence by averaging it.
- **Generated data can go stale.** Stop if editing JSON does not rebuild the C table, or if any
  generated table is proposed for commit.
- **Proof profiles are link-time compositions.** Stop if portable and Nordic hooks appear in one
  executable, if production hook symbols are renamed for the test, or if a runtime registry is
  proposed. Shared runner source does not imply one link image.
- **Origin is part of the dispatch input.** Stop if an H2D vector reaches the runner through an
  implicit BLE default or if a TLS-LAN expectation is evaluated with BLE gate semantics.
- **One Nordic production profile is not target equivalence.** Its green result is shared/production-composition
  host evidence only. Stop any documentation that upgrades ESP32, Nordic or Silabs hardware status
  from it.
- **Capture provenance has already lost historical strength.** Do not relabel a C12 unified-firmware
  capture as pre-migration fleet truth. Capture Silabs before C13 or record that its deadline was
  consciously skipped.
- **The hardware matrices are stacked.** Stop H3 after an H1/H2 failure; bisect rather than testing
  more layers and losing attribution.
- **Backpressure and on-air ACK order need actual evidence.** For the RETRY row, a CCCD-disabled
  interval plus dispatch-before-re-enable and delivery-after-re-enable is the observation; a log
  saying “queued” or an ordinary multi-chunk read is insufficient. This proves the shared RETRY
  disposition, not the controller's `-ENOMEM` path.
- **OD-S1 is easy to test incorrectly.** Stop if the duplicate was resealed, if the transfer was no
  longer active, if telemetry was throttled away without a pre-wait, or if no bad-tag control proved
  response visibility.
- Stop and revise this plan if implementation requires a production opcode/error/envelope change,
  a vendored protocol-header edit, a Silabs command cutover, or transfer code under `shared/`.

---

## 9. What C12 does not do

- no new opcode, error code, wire shape or vendored protocol-header change;
- no Silabs dispatch/session/config migration or claim that its PURE-only build exercises dispatch;
- no direct, partial, PIPE, NFC or compression state-machine promotion;
- no second crypto backend, session-policy redesign, LAN redesign or target capability change;
- no config JSON execution adapter for `expect.parsed` (schema accepted now, execution deferred with
  the reason in § 2.3);
- no corpus copy, published artifact, new CI service or claim that `--latest` runs on a schedule;
- no hardware-verified label derived from build, host, corpus, sanitizer or mutation evidence.

---

## 10. Definition of done

C12 is complete only when:

- all 24 vectors validate under the decided schema and remain one authoritative corpus;
- the generated table is build-only, deterministic and rebuilt from JSON dependencies;
- all 17 dispatch vectors are accounted for, all 14 H2D vectors execute through
  `od_dispatch_frame()` in the portable executable, and the three D2H vectors are explicitly
  direction-only;
- every current `target-production` reply also executes through the link-isolated Nordic executable
  and production reply-producing command code; every historical fixture result is labelled and
  excluded from target-coverage claims;
- every applicable ordered reply and silence assertion is checked with no circular fake;
- the Python public-API half and the new C firmware-reply half both pass;
- every named mutation in § 6.2 has been observed failing the intended check;
- every accepted commit records `tools/check.sh --targets` at 13 passed, 0 failed, 0 skipped;
- C12.0-C12.2 can stand merged with hardware status still honestly ACCEPTED-UNRUN;
- H1, H2 and every applicable H3 row have provenance-backed PASS/FAIL results, not inference;
- OD-S1 exact sealed replay plus bad-tag control has run on every available promoted board and the
  upload recovered; and
- status documents distinguish authored contract, legacy unattributed capture,
  provenance-complete current-hardware capture, historical regression baseline, software proof and
  hardware proof.

Only after those hardware rows pass may C12 retire ACCEPTED-UNRUN. The next implementation unit is
the dedicated Silabs C13 plan; transfer architecture follows it, after the required language
decision is re-argued.
