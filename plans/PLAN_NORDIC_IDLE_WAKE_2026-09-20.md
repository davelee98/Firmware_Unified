# Plan: promptly leave Nordic idle processing on BLE activity

Status: implemented on `codex/nordic-idle-wake`, including review corrections.
`tools/check.sh --targets` passed on 2026-09-21: 46 passed, 0 failed, 0 skipped.
Hardware latency and idle-power qualification remain pending.

## Outcome and scope

A BLE connection arriving during a Nordic idle wait must promptly return execution
to the normal connected loop, which processes work every 10 ms. A configured idle
timeout of seconds or minutes must not keep a live connection on the idle cadence.
Preserve idle power behavior, watchdog service, advertising retries and periodic MSD
updates. Apply to all three `nordic-zephyr` boards.

This fixes the transition from disconnected idle to connected processing. It does
not fix the reported steady-state upload throughput gap versus `../Firmware`: the
10 ms connected cadence, SACK latency, TX retry granularity, BLE PHY, connection
parameters, MTU and controller buffers are unchanged. Transfer algorithms, USB
power gating and security policy also stay outside scope. Keep this change
independent of the ESP32 WiFi plan.

## Evidence and constraints

- `targets/nordic-zephyr/src/main.c:idle_delay_ms()` processes work and sleeps in
  slices of up to 1,000 ms until the entire requested delay expires. It never checks
  for a newly connected peer. The outer loop's connected branch sleeps only 10 ms.
  A connection during a 60-second idle cycle can therefore remain on one-second
  processing intervals for the rest of that cycle. The default idle branch has a
  similar issue at its shorter 500 ms interval.
- `opendisplay_ble.c` receives asynchronous connect/disconnect callbacks;
  `opendisplay_pipe.c` queues incoming writes and defers processing/cleanup to the
  main thread. Keep that single-consumer arrangement.
- Live `../Firmware_NRF54/src/main.c` has the same uninterruptible idle loop.
  Live `../Firmware/src/main.cpp:idleDelay()` checks pending RX/transport events
  and returns early, using 100 ms slices. Preserve its early-return behavior without
  importing Arduino APIs or imposing more periodic Nordic wakeups.
- The host's `../py-opendisplay/src/opendisplay/device.py` uses a 0.5-second PIPE
  tail-flush timeout. The idle defect can delay initial command handling after a
  connection and provoke a probe. Once the outer loop reaches its connected arm,
  the remaining throughput behavior is outside this plan.
- `plans/PLAN_NORDIC_BLE_THROUGHPUT_2026-09-03.md` is superseded. Its semaphore
  design must not be implemented alongside this one. Its connection timing,
  SACK/TX retry, stack provisioning and measurement work was deliberately not
  carried into this plan and needs a fresh measurement-led plan.

## Design

Use an event-woken idle wait rather than shortening every idle sleep. Keep the
implementation target-local and small: a Nordic wake module owns a statically
defined binary Zephyr semaphore, exposes `wake` and bounded `wait` operations, and
contains no application processing. `main.c` owns the idle and MSD deadline policy.
The semaphore must exist before BLE callbacks can fire, require no heap and retain a
pending wake issued before the main thread actually blocks.

Successful connection, connection failure and disconnect
publish their state/work first, then signal the wakeup. Use the existing synchronized
queue and pending-state contracts; publish any new connection-level indicator with
proper synchronization. Do not pass raw `bt_conn` lifetimes through the wake signal
or perform command dispatch, panel cleanup or notification draining in callbacks.
In `connected()`, the `err != 0` arm returns early: give the wake after publishing
`s_adv_ended_pending` and requesting the advertising boost, immediately before that
return. On success, publish the retained connection and related state before giving.

The idle helper services watchdog/application work, checks whether it should return
to the outer loop, then waits for an event or the next existing idle service deadline
(at most one second). A latched event closes the check-before-sleep race. On an event,
return to the outer loop to service/re-evaluate state, including when a connect and
disconnect both occurred before the wake was consumed. Never clear/reset the
semaphore between checking work and blocking. Coalesced events are sufficient
because the queues and state carry the work, not the semaphore count.

Return an explicit result distinguishing an elapsed deadline from an event
interruption. Re-evaluate configuration and advertising state after interruption.
The 50 ms advertising-retry branch must remain reachable when processing discovers
that advertising is inactive. In particular, when `od_adv_control` has latched a
fault and `active` can never become true until another control action, each retry
pass must still perform its bounded wait rather than busy-looping. One stale binary
semaphore token may cause one immediate pass, never an unbounded spin.

Use monotonic elapsed time/deadlines for the idle interval and service slices so
early wakes and processing time cannot be counted as an entire requested sleep.
Retain the 500 ms default idle wait, the at-most-one-second idle watchdog/service
interval and the 10 ms connected cadence. A leftover wake may cause one extra loop
iteration; it must not cause a persistent spin. If processing has already passed the
deadline, perform an interruptible wait of at least one tick before returning expiry.
This prevents a legal 1 ms interval from spinning when a pump pass takes longer.

Periodic MSD timing uses one absolute next-refresh deadline. Arm it from the last
refresh (or initial disconnected scheduling point), and preserve it across connection
interruptions. Never refresh advertising while connected. On the first
disconnected outer-loop pass at or after the deadline, publish once and advance the
deadline without replaying missed intervals. This prevents frequent short connections
from continually restarting a 60-second timer while also avoiding a catch-up burst
after a long connection. Config writes are processed synchronously on the main thread;
if `sleep_timeout_ms` changes, recompute the deadline when the next outer-loop read
observes the change. A temporary `loaded = false` during a successful reload is not
visible to that read. A completed clear or failed reload disables the periodic deadline.

Wake sources are deliberately limited to successful connection, failed connection
and disconnect. RX enqueue does not signal: connected processing sleeps with
`k_msleep(10)`, which a semaphore give cannot interrupt. LED, buzzer, touch and CCC
work remains on the existing at-most-one-second idle service tick. That preserves
current idle behavior; making those paths event-driven belongs to a separately
measured responsiveness plan.
Accepted RX frames are handled at the existing connected cadence, and the queue's
overflow handling is unchanged.

## Implementation steps

1. **Add the wake seam.** Put `K_SEM_DEFINE`, `k_sem_take()` and the wake entry
   point in one Nordic-local file such as `opendisplay_idle_wake.c/.h`, and add it
   explicitly to `targets/nordic-zephyr/zephyr/CMakeLists.txt`. Keep deadline and
   application decisions out of this seam.
2. **Replace the uninterruptible delay.** Integrate the event/timeout wait and
   explicit result into `main.c`; check state before waiting and after processing.
   Move MSD scheduling to the persistent absolute deadline described above. Keep
   all application processing on the main thread.
3. **Connect wake producers.** Signal successful and failed connection attempts and
   disconnects from `opendisplay_ble.c` after publishing their state. The existing
   `opendisplay_pipe.c` disconnect cleanup publication remains ordered before the
   disconnect wake. Do not signal on RX enqueue while the connected wait uses
   `k_msleep(10)`.
   Put the failed-connect give after `s_adv_ended_pending` and `od_adv_app_boost()`,
   before the callback's early return. Verify all callback return paths,
   initialization ordering. Do not add CCC, LED,
   buzzer or touch wakes, and do not generate a periodic wake merely to keep the
   semaphore active.
4. **Run target validation.** Use focused instrumented runs to cover the old
   failure, check-to-wait ordering and deadline behavior, then build all boards and
   run the existing full gate. Do not add a host fixture, a new Zephyr test target or
   copied scheduling model for this change. Keep the legacy `Firmware_NRF54`
   equivalent as the external follow-up recorded below; do not edit the sibling.
5. **Qualify latency and power on hardware.** Measure both under controlled
   conditions before declaring the fix complete. Keep the implementation and its
   validation evidence in an independently reviewable/revertible change.

## Required validation cases

| Scenario | Required result |
| --- | --- |
| Connection during a 60-second idle interval | Immediate wake/return; no remaining one-second processing regime |
| Connection before wait, between check and wait, during wait | No lost wake in any ordering |
| Connection created while process hook runs | No subsequent long idle sleep |
| RX queued immediately after connection | Processed on main thread at connected cadence |
| Rapid connect/disconnect before main resumes | Deferred cleanup runs; advertising retries are prompt |
| Failed connection attempt while idle | Pending advertising state is serviced without waiting a full second |
| Connection during 50 ms retry or 500 ms default wait | Same event wake behavior |
| Advertising becomes inactive during a long idle cycle | Outer loop reaches bounded retry branch without spinning |
| Advertising fault is latched and `active` stays false | Repeated 50 ms path performs timed waits; at most one immediate stale-token pass |
| No activity for a long configured interval | Existing service/watchdog cadence; one MSD refresh at the absolute deadline |
| Repeated short connections before MSD deadline | Deadline survives interruptions; refresh occurs once when disconnected and due |
| Connection spans the MSD deadline | No refresh while connected; one refresh after disconnect, with no catch-up burst |
| Timeout changes in a config handler | Next outer config read recomputes the deadline from the newly observed value |
| Zero/default timeout, short timeout, clock boundary | No unsigned underflow, premature expiry or infinite wait |
| Pump pass takes longer than a 1 ms configured interval | Expiry still performs a timed wait; no sustained CPU spin |
| Coalesced/stale connection wake tokens | Bounded extra work; no persistent busy loop |
| LED, buzzer, touch or CCC work while idle | Remains serviced on the existing at-most-one-second periodic tick |

Do not add committed test targets for this change. Cover these cases with focused
target instrumentation and hardware runs, then remove temporary instrumentation.
The existing host suite still runs as a regression gate, but
`tests/host/CMakeLists.txt` gains no idle-wake fixture and no Zephyr compatibility
layer.

Build all supported boards with `targets/nordic-zephyr/build.sh --all` using the
pinned toolchain: XIAO nRF54L15, XIAO nRF54LM20A and XIAO nRF52840 (`xiao_ble`).
Before merge run `tools/check.sh --targets` and inspect the complete summary;
skipped checks are not passes.

## Hardware acceptance

On XIAO nRF52840, configure a 60-second idle timeout and connect at several offsets
within the idle cycle. Under no display refresh or other blocking command, measure
connection-event-to-main-service and received-command-to-dispatch latency. Require
less than 100 ms for the initial idle exit and verify return to the normal 10 ms
connected scheduling cadence. Measure firmware dispatch separately from BLE host
and connection-interval latency. Repeat with the default 500 ms idle behavior.

Exercise config read/write, immediate PIPE upload after connection, raw and compressed
transfers, disconnect during transfer, rapid reconnect, and advertising recovery.
Verify that the first command after an idle connection is no longer delayed by the
remaining idle interval. Record steady-state transfer rate as an observation only:
this plan does not claim to improve the existing approximately 10x throughput gap,
the 10 ms connected cadence, SACK latency or TX retry granularity.

Compare battery idle current before/after on the same board, firmware profile and
advertising settings with VBUS absent. The recorded nRF52840 reference is about
40 microamps; establish a same-session baseline and measurement tolerance before
testing. Acceptance requires no sustained additional periodic wakeups or material
current increase beyond that tolerance. Check USB enumeration with VBUS present and
watchdog stability over several long idle cycles. Repeat latency/transfer checks on
each available nRF54 board; unavailable board rows remain explicitly open.

Record board, build, timing method, transfer mode, current conditions and results in
`docs/HARDWARE_VERIFICATION_CHECKLIST.md`. Update the root status only with a concise
pointer if qualification changes. Host tests and successful builds do not substitute
for the latency and battery measurements.

## External follow-up

`../Firmware_NRF54/src/main.c` has the same uninterruptible idle loop. After this
repository's fix is implemented and qualified, prepare an equivalent, independently
reviewed change in that repository if it is still maintained. The sibling is a
read-only reference for this task and is not modified here.

## Completion criteria

All target builds and existing required gate checks pass; focused instrumentation
and hardware demonstrate prompt idle exit without an idle-power regression. Preserve
any unperformed hardware qualification as open work rather than marking it passed.
No new committed test target, host timeout or wire-format change is needed.
