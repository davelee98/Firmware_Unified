# F4 portable BLE advertising and lifecycle plan

**Status:** **ADOPTED 2026-08-05.** This is the accepted design for F4, and
[NEXT_STEPS_2026-08-05.md](NEXT_STEPS_2026-08-05.md) D1 schedules it as the first real source in
`shared/`. Implementation follows the merge sequence in that document's Milestone 1 —
test-first, one reviewable commit per step.

**Scope:** correctness review finding F4

**Targets:** ESP32/NimBLE, nRF52840/Bluefruit, Nordic/Zephyr, and EFR32/BGAPI

**Recorded:** 2026-08-05

## Decision

Make the application loop the sole owner of advertising policy and of the advertisement's
manufacturer-specific-data (MSD) snapshot. Stack callbacks publish facts only. A small,
plain-C, run-to-completion controller reconciles the desired state with the observed stack
state and calls a target advertising HAL.

This is deliberately not a NimBLE-host-task solution. NimBLE event queues, Zephyr work queues,
SoftDevice callbacks, and Silabs BGAPI events are target-specific ways to move facts across an
execution-context boundary; none of them should own product policy. The same controller and
tests must be usable with all four.

The intended ownership is:

```text
 stack callback / event handler             application loop
 ┌───────────────────────────┐              ┌──────────────────────────────┐
 │ copy a bounded fact       │  event bridge│ drain facts                  │
 │ publish it coherently     ├─────────────►│ update controller state      │
 │ never start/stop/rebuild  │              │ run one reconciliation step  │
 └───────────────────────────┘              └──────────────┬───────────────┘
                                                          │
                                            target-local advertising HAL
                                            (NimBLE / Bluefruit / Zephyr /
                                             BGAPI calls and AD packing)
```

## Why F4 needs an ownership change

The current ESP32 port shares ordinary C++ objects between the FreeRTOS application loop and
the NimBLE host task. In particular, `s_addr_resolved`, `s_msd`, `s_msd_len`, and
`s_adv_wanted` are written and read from different contexts in
`targets/esp32-idf/ble/od_ble_nimble.cpp`. The host callback also calls
`od_ble_advertise()` after sync, failed connection attempts, and `ADV_COMPLETE`, while the
loop calls start, stop, and MSD-update functions.

That permits three classes of defect:

- a flag can be stale or its access can be an actual C++ data race;
- the MSD byte array and its length can be observed as different revisions; and
- a late stack event can re-arm advertising after the application has committed to stop and
  tear down BLE for deep sleep.

Adding atomics to the booleans would address only the first item. It would not publish the MSD
array and length as one record, establish a single owner for start/stop ordering, or give
teardown a barrier. Moving policy to the NimBLE host task would solve the ESP32 symptom but
would encode the wrong architecture for Zephyr, Bluefruit, and the Silabs superloop.

The repository already uses the portable direction for command handling: callbacks copy and
flag, and the application loop performs substantive work. Advertising should follow the same
rule.

## Design constraints

The controller must satisfy the repository-wide architecture:

- `shared/` remains plain C and includes no RTOS, Arduino, vendor, or C++ headers;
- it is run-to-completion, non-blocking, statically allocated, and usable without a kernel;
- target code owns AD-record packing because PDU layout and update APIs differ;
- the canonical 16-byte `struct MsdAdvertisement` remains the wire payload;
- no stack callback calls advertising start, stop, payload-build, sensor, logging-policy, or
  deep-sleep code;
- an application stop request dominates late callback events; and
- ESP32 teardown can prove advertising is quiescent before the host/controller is released.

Feature parity is not required. For example, a target may support live data updates while
another requires stop/program/start. The shared state transition and externally visible
policy must still be the same.

## Shared controller

Place the controller in `shared/core/od_adv_control.c` and
`shared/core/od_adv_control.h`. It should sit beside the planned `od_advert.c`: `od_advert.c`
builds the canonical 16-byte MSD, while `od_adv_control.c` decides when that immutable snapshot
is installed and when advertising is allowed to start.

A representative state shape is:

```c
struct od_adv_control {
    bool stack_ready;
    bool desired;
    bool active;
    bool payload_valid;
    bool payload_dirty;
    bool faulted;
    uint8_t connection_count;
    uint8_t msd[16];
    uint32_t desired_revision;
    uint32_t applied_revision;
};
```

The fields are controller-owned and are accessed only by the application loop. They do not
need atomics. Revisions are compared only for equality, so wraparound does not require ordering
arithmetic.

The public input API should be small and describe facts or intent rather than a particular
stack:

```c
void od_adv_control_init(struct od_adv_control *s);
void od_adv_set_payload(struct od_adv_control *s, const uint8_t msd[16]);
void od_adv_request_start(struct od_adv_control *s);
void od_adv_request_stop(struct od_adv_control *s);

void od_adv_stack_ready(struct od_adv_control *s);
void od_adv_stack_reset(struct od_adv_control *s);
void od_adv_set_connection_count(struct od_adv_control *s, uint8_t count);
void od_adv_observe_ended(struct od_adv_control *s);

enum od_adv_process_result
od_adv_process(struct od_adv_control *s, bool start_allowed);

bool od_adv_is_quiescent(const struct od_adv_control *s);
```

`desired` is standing application intent, not a prediction of stack state. A start requested
before stack sync remains remembered. A disconnect or an advertising-ended event changes
observed state but does not erase intent. A stop request clears intent and therefore cannot be
undone by a later event.

`start_allowed` is a gate on a new start, not a command to stop an advertisement that is
already running. This preserves the current display policy: advertising need not be withdrawn
merely because an EPD refresh begins, but if a connection stops it during that refresh, the
restart waits until the refresh completes.

## Target advertising HAL

Use link-time HAL functions rather than a C++ virtual interface or a stack-specific object in
shared state:

```c
enum od_hal_adv_result {
    OD_HAL_ADV_OK,
    OD_HAL_ADV_ALREADY,
    OD_HAL_ADV_NOT_ACTIVE,
    OD_HAL_ADV_RETRY,
    OD_HAL_ADV_ERROR
};

enum od_hal_adv_result od_hal_adv_program(const uint8_t msd[16]);
enum od_hal_adv_result od_hal_adv_start(void);
enum od_hal_adv_result od_hal_adv_stop(void);
```

The contract is:

- `program` receives a complete immutable MSD snapshot and constructs both advertising and
  scan-response records using target-local rules;
- the controller calls `program` only while its observed state is inactive;
- a successful `stop` means the target accepts that it is safe to program before a delayed
  completion callback is delivered;
- `ALREADY` and `NOT_ACTIVE` are idempotent success cases, not faults;
- `RETRY` represents temporary backpressure and causes no state lie; and
- `ERROR` is returned to the application for throttled reporting and recovery policy.

The conservative cross-target update sequence is stop, program, start. A later optional live
update optimization is permissible, but must not change controller semantics and is not needed
to fix F4. Existing MSD deduplication keeps the stop/start cost bounded.

## Reconciliation algorithm

`od_adv_process()` performs at most one stack-mutating HAL call per loop pass. That makes every
transition observable, avoids blocking, and keeps error retry behavior straightforward.

In priority order:

1. If the stack is not ready, do nothing.
2. If advertising is believed active and any of the following is true, call `stop`:
   intent is stopped, a connection exists, or a newer payload must be installed.
3. If inactive and a payload is dirty, call `program`. On success, copy the desired revision
   to the applied revision and clear `payload_dirty`.
4. If inactive, intent is started, there are no connections, a payload has been applied, and
   `start_allowed` is true, call `start`.
5. Otherwise the state is reconciled and no HAL call is needed.

State updates occur only after the HAL result is known. `OK`, `ALREADY`, and `NOT_ACTIVE` move
the relevant state forward; `RETRY` leaves it unchanged. `ERROR` is surfaced and latches a
fault indication so a hot loop does not flood logs. A new stack-ready generation, explicit
recovery request, or target-defined retry policy may clear the fault.

When the bridge reports advertising ended, the controller sets `active = false` and reconciles
again. Coalescing multiple ended notifications is safe because the final fact is the same. If
a stale ended event arrives after a new start, the next start may return `ALREADY`; treating
that result as success restores the controller's observation without duplicating policy.

On stack reset, the controller clears `stack_ready`, `active`, and the applied revision, then
marks the retained payload dirty. Application intent remains intact. A later ready event thus
programs the latest payload and resumes only when all gates permit it.

## Event bridge and memory ordering

Stack callbacks must not call the shared controller directly, because that would make the
controller multi-owner. Each target instead publishes bounded facts to its application loop.
Acceptable target mechanisms include:

- an ESP-IDF FreeRTOS queue, task notification plus atomically published snapshot, or an
  existing transport event mailbox with a correct release/acquire protocol;
- a Zephyr `k_msgq` or atomically published snapshot consumed by the application thread;
- a Bluefruit/SoftDevice callback mailbox consumed by Arduino `loop()`; and
- direct fact staging in the Silabs event handler, followed by controller processing after the
  handler returns in the same superloop pass.

The bridge is target code, so it may use the target's synchronization primitives. Plain
`volatile` is not sufficient synchronization. A queue message naturally publishes all of its
fields together; a mailbox must explicitly make its payload visible before its ready flag.

Most inputs are level state and may be coalesced:

- connection count should come from the authoritative instance table, not from counting a
  potentially lossy sequence of callback edges;
- advertising-ended notifications may be represented by a monotonically changing sequence or
  a consume-once bit; and
- stack readiness and identity must be published as one generation-stamped snapshot.

Every event that can survive teardown should carry a stack generation, or the bridge should be
disabled and drained before deinitialization. Events from a prior stack generation are ignored.

## Identity and the rest of the lifecycle state

F4 is broader than the `s_adv_wanted` flag. The same refactor must remove the plain cross-task
handoff for BLE identity and lifecycle state.

The stack-ready event should carry a coherent identity snapshot:

```c
struct od_ble_identity_snapshot {
    uint32_t stack_generation;
    uint8_t address[6];
    uint8_t address_type;
};
```

The host callback resolves and copies the identity before publishing the ready event. The loop
stores that snapshot and serves address queries from its own copy. It must not poll
`s_addr_resolved` or read address type while the host may be updating them.

Other shared fields need an explicit disposition:

- initialization/lifecycle state is loop-owned and changes only after init/deinit results;
- device name is immutable from before host start until the deinit barrier completes, or is
  delivered through a host command if runtime changes are ever required;
- preferred MTU is likewise fixed before host start or handed to the host through a coherent
  command/event mechanism;
- connection state comes from the already synchronized transport instance table; and
- characteristic storage remains host-owned if NimBLE serializes all GATT access to it.

This field-by-field audit is required. Fixing only the advertising flag would leave other
undefined cross-context accesses in place.

## Target mappings

### ESP32-IDF / NimBLE

- `od_on_sync()` publishes `STACK_READY(identity, generation)`; it does not advertise.
- `BLE_GAP_EVENT_ADV_COMPLETE` and genuinely failed connect attempts publish an ended fact; they
  do not call `od_ble_advertise()`.
- connect/disconnect callbacks retain their bounded instance-table and transport-event work,
  but advertising reconciliation runs later in the loop.
- `od_ble_set_manufacturer_data()`, `od_ble_restart_advertising()`, and
  `od_ble_stop_advertising()` become loop-side intent/snapshot operations.
- `ble_gap_adv_set_fields`, `ble_gap_adv_rsp_set_fields`, `ble_gap_adv_start`, and
  `ble_gap_adv_stop` are reachable only through the loop-owned target HAL.

The NimBLE host still owns its internal state. The claim is only that OpenDisplay advertising
policy has one owner. If an individual NimBLE API is documented as host-context-only, its HAL
adapter may submit a command to the host and report `RETRY` until completion; that submission is
mechanism, not policy.

### nRF52840 / Bluefruit

Set `Bluefruit.Advertising.restartOnDisconnect(false)`. The automatic restart currently makes
the SoftDevice a second policy owner and is the reason the application needs
`restartsAdvertisingOnDisconnect()` as a target divergence.

Callbacks publish connection facts. Arduino `loop()` alone calls Bluefruit advertising
start/stop/data methods through the target HAL. Once converted, remove
`restartsAdvertisingOnDisconnect()` and its special branch: every target has the same explicit
restart contract.

DFU entry first clears start intent and reaches the advertising stop barrier, then disconnects
and disables the SoftDevice. It must not rely on separately toggling Bluefruit's auto-restart
setting as a race-prevention mechanism.

### Nordic / Zephyr

Bluetooth callbacks publish facts through `k_msgq` or an equivalent coherent bridge. The main
application processing context calls `bt_le_adv_start`, `bt_le_adv_stop`, and, if selected by
the adapter, `bt_le_adv_update_data`.

A Zephyr `k_work` item may implement the bridge when an API requires a system-workqueue context,
but it must not become the owner of desired state or restart policy. Keeping policy in the
application pump preserves the same ordering with display refresh, transfer teardown, and deep
sleep as every other target.

### EFR32 / Silabs BGAPI

BGAPI events already arrive in a superloop. Record the facts during the event callback and run
the controller at the tail of the pass, after returning from the handler. AD-record creation and
`sl_bt_legacy_advertiser_set_data` calls stay in the target HAL. No RTOS queue is needed, which
is an important proof that the shared controller does not depend on a kernel.

## Teardown barrier

Teardown is an application lifecycle operation, not an advertising callback side effect:

1. Enter a loop-owned `STOPPING` lifecycle state; reject any later start request.
2. Call `od_adv_request_stop()`.
3. Pump events and `od_adv_process()` until `od_adv_is_quiescent()` is true, or until a bounded
   target timeout reports failure.
4. Disable/drain the event bridge or advance the stack generation.
5. Stop and deinitialize the BLE host/controller.
6. Mark lifecycle `DOWN` only if deinitialization succeeds.

Because callbacks never start advertising, an event arriving during steps 2-4 can at most
report a fact. It cannot defeat the stop intent. A stop or deinit failure remains visible to the
caller; lifecycle state must continue to describe reality. This complements, but does not
replace, correctness-review finding F7 about deinit failure reporting.

## Test plan

The core controller should be tested on the host with a scripted fake HAL. At minimum cover:

- start requested before stack ready;
- ready before and after the initial MSD snapshot;
- repeated start and stop requests;
- stop followed by a late advertising-ended event;
- failed connection with no live link, followed by restart;
- connect/disconnect entirely between two application-loop passes;
- disconnect during an EPD refresh, with restart deferred until `start_allowed` becomes true;
- MSD updates while inactive, advertising, connected, and refresh-gated;
- several MSD updates coalescing to the latest complete 16-byte snapshot;
- `RETRY`, `ALREADY`, `NOT_ACTIVE`, and hard-error HAL results at every transition;
- stack reset while active, followed by a new generation and automatic reconciliation;
- teardown racing ready, ended, connect, disconnect, and payload-update events; and
- revision-counter wrap, verified using equality-only semantics.

Target adapter tests or hardware tests must additionally verify:

- exact ADV and scan-response bytes remain compatible with each target's current layout;
- a passive scanner still sees the expected name/MSD behavior;
- no callback stack contains an advertising start/stop call;
- no restart occurs after deep-sleep/DFU teardown commits;
- an advertisement resumes after a genuinely failed connection and after an ordinary
  disconnect when allowed;
- identity address/type is coherent and remains the value exposed to LAN discovery; and
- repeated MSD updates never broadcast mixed bytes from two revisions.

TSan can validate the host controller and a pthread-backed event-bridge model, but it cannot
prove the ESP-IDF or radio-stack adapters race-free. The target bridge requires code review plus
stress tests on dual-core ESP32-S3 hardware and the nRF/Zephyr boards.

## Implementation sequence

1. Add failing host tests for the pure state machine and fake HAL.
2. Add `od_adv_control.c/.h` to `shared/sources.cmake` and make every current consumer compile
   it. This is a small, explicit exception to the documented config-first migration ordering;
   it should not arrive hidden inside an ESP-only patch.
3. Add the ESP32 event bridge and identity snapshot, then remove all advertising calls from
   NimBLE callbacks.
4. Route ESP32 advertising and MSD APIs through the controller and target HAL.
5. Add the teardown barrier and verify deep-sleep wake/connect/re-sleep cycles on hardware.
6. Convert Bluefruit by disabling automatic restart and remove the
   `restartsAdvertisingOnDisconnect()` divergence.
7. Implement the Zephyr and Silabs adapters against the same HAL contract as their migrations
   land; run the shared host vectors unchanged.
8. Re-audit every file-static BLE object for cross-context reads and writes, then update the F4
   finding with the evidence that each object has one owner or a documented handoff.

`shared/sources.cmake` is currently intentionally empty. That makes step 2 visible and
test-first: the new source should not be listed until the host fake HAL and tests link with it,
and all scaffolded target consumers must either provide their HAL adapter or an explicit
build-time test stub.

## Acceptance criteria

F4 is resolved only when all of the following are true:

- every OpenDisplay advertising-policy object has exactly one execution-context owner;
- the 16-byte MSD and its revision are published as one loop-owned snapshot;
- stack callbacks contain no advertising start, stop, rebuild, or restart-policy call;
- start-before-ready and disconnect-during-refresh work without target-specific policy branches;
- a stop request cannot be reversed by a late event;
- BLE identity is handed off as a coherent snapshot;
- teardown has a bounded, observable quiescence barrier and reports failure truthfully;
- the same host state-machine tests pass for ESP32, Bluefruit, Zephyr, and Silabs HAL fakes; and
- target hardware tests preserve current discovery bytes and restart behavior.

## Alternatives considered

**Run all advertising policy on the NimBLE host task.** This can fix the immediate ESP32 race,
but makes the portable architecture depend on one stack's execution model and separates radio
policy from display/deep-sleep policy.

**Make shared booleans atomic and protect the MSD with a mutex.** This can make individual
accesses defined, but leaves two owners issuing start/stop operations and makes the Silabs
no-kernel target pay for an unnecessary synchronization model.

**Let each stack auto-restart advertising.** This cannot express the application rule that a
disconnect during an EPD refresh defers restart, and it makes teardown dependent on disabling
stack-specific hidden policy at exactly the right time.

The loop-owned reconciler is therefore the smallest solution that fixes the observed ESP32
race while remaining native to nRF/Zephyr and usable on the kernel-free Silabs target.
