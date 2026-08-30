# Nordic digital-button integration from the ESP32 reference

**Status:** proposed, 2026-08-30. Replaces the obsolete
[PLAN_NORDIC_BUTTONS_2026-08-22.md](PLAN_NORDIC_BUTTONS_2026-08-22.md). The replaced plan is a
historical reference only; none of its frozen decisions or staging carries forward implicitly.

## 1. Objective and authority

Replace Nordic's digital-button implementation completely with a fresh implementation whose
observable behavior comes from the live ESP32 reference in `../Firmware/src/device_control.cpp`
and `../Firmware/src/structs.h`. Before implementation, diff those files against
`targets/esp32-idf/src/device_control.cpp` and `targets/esp32-idf/src/structs.h`; the sibling is the
behavioral authority and the in-repository target is useful integration evidence, not a substitute.
The reference inspected for this plan was the clean `../Firmware` commit
`64184bbecc88a2d07332f6f28fc922f581619ffc`.

This plan is for `targets/nordic-zephyr` only. It does not promote buttons into `shared/`, does not
change ESP32, and does not integrate or refactor BG22.

The implementation must delete `targets/nordic-zephyr/src/opendisplay_button.{c,h}` and replace
them with new target-local files, provisionally `od_button_nordic.{c,h}`. Keeping a public call
shape where useful is allowed; carrying forward the old eight-entry state layout, level-polling
behavior, or implementation text is not.

## 2. Current integration facts

- Nordic currently discovers at most eight buttons and detects changes only by comparing levels in
  `opendisplay_button_process()`. A press and release between service calls is lost.
- The GPIO layer already provides `od_gpio_config_irq_arg()`, both-edge selection, detach,
  mask/unmask and a global IRQ lock. The old plan's “restore the seam” step is already obsolete.
- The GPIO IRQ registry has eight total slots. Touch can consume four, while the canonical config
  permits four `BinaryInputs` records with eight digital pins each. Full button behavior therefore
  requires capacity for 32 buttons plus four touch callbacks.
- Nordic initializes buttons only at BLE startup. Its config-reload path reloads NFC and
  advertising policy but does not reapply buttons or touch.
- `idle_delay_ms()` can sleep for one second between BLE service passes. Capturing the transition
  prevents loss, but waking the loop is required for prompt publication.

## 3. ESP32 behavior to preserve

The new implementation must pin these behaviors in tests before target integration:

1. Walk up to four `BinaryInputs` records and accept digital inputs only
   (`input_type == OD_INPUT_TYPE_BUTTON`) with `button_data_byte_index <= 10`.
2. Treat `pins_used == 0` as “all eight pin fields are candidates.” Otherwise use only set bits.
3. Skip a candidate whose pin is `0xFF` or is reserved as a configured GT911 interrupt.
4. Support the contract maximum of 32 digital buttons, not Nordic's former limit of eight.
5. Derive the advertised button ID as `(instance_number * 8 + pin_offset) & 0x07`.
6. Honor each pin's inversion, pull-up and pull-down settings. If both pull bits are set, preserve
   Nordic GPIO policy: pull-up wins, matching `od_gpio_configure_input()`.
7. After configuring inputs, allow the ESP32 reference's 10 ms input-settle interval, capture the
   initial logical level, attach a both-edge callback with the button index, then perform the
   reference's masked 50 ms re-baseline. Initialization must not count or publish a press.
8. On every accepted logical released-to-pressed transition, increment the four-bit press count
   modulo 16. A release changes state without incrementing the count. Digital buttons have no
   five-second count reset; that behavior belongs to the ESP32 ADC-ladder implementation.
9. Encode the dynamic byte as `button_id[2:0] | press_count[6:3] | state[7]`.
10. Perform BLE/dynamic-data work only on the loop thread. Request the advertising boost before
    publishing the MSD update; the order is load-bearing on Nordic.
11. Do not add ADC-ladder behavior, configured long-press power-off, or deep-sleep button wake.
    Nordic has no power latch, and those are distinct hardware capabilities rather than digital
    button reporting.

## 4. Permitted and required divergences

ESP32 is the source of behavior, not a requirement to reproduce defects. Every divergence below
must be named in `docs/DIVERGENCE_MATRIX.md` with its test evidence.

### N1 — Do not reproduce the single-global-event loss

ESP32 stores one `buttonEventPending` flag and one `lastChangedButtonIndex`. Two buttons changing
before the loop drains them overwrite the earlier index. Nordic must instead keep a per-button
atomic event word containing at least logical state, four-bit count, a pending bit and an ordering
stamp. The callback updates one word; the loop consumes coherent snapshots with Zephyr atomics.
`volatile` is not synchronization.

When multiple pending buttons target different MSD bytes, retain the newest event for each byte.
When they target the same byte, retain the event with the newest ordering stamp, which preserves
the wire format's “most recently changed button” meaning. Publish at most once per service pass.
A press and release completed before the pass must still leave the incremented count visible,
even if the published state is already released.

Sequence comparison must be wrap-safe, and the host suite must force a wrap. The representation
must not silently limit the press count, state or ordering fields below their stated widths.

### N2 — IRQ attachment failure is observable and has a fallback

ESP32 ignores the attach result. Nordic must check it. A failed attach logs the pin and error once
and leaves that button active in level-poll fallback mode. A held transition must still publish;
a complete pulse between polls remains an explicitly logged degraded mode, not a false claim of
full behavior.

The GPIO registry capacity must be derived from 32 buttons plus `OD_CONFIG_MAX_TOUCH`, or replaced
with an equally bounded representation that admits that combination. Exhaustion and duplicate-pin
replacement need host tests. Do not lower the 32-button contract to fit the current eight slots.
Touch owns a configured GT911 interrupt pin. Among digital buttons, the first config-order claimant
of a duplicate pin wins and later claimants are logged and refused; allowing the GPIO registry's
replace semantics to redirect the first button's callback silently would create two runtime records
for one interrupt and is a reference defect Nordic must not copy.

### N3 — Config reload owns teardown before reuse

Before clearing runtime state, detach callbacks by walking the old runtime records. Reapplying a
configuration must use this order:

1. detach old button callbacks and clear button pending/wake state;
2. reinitialize touch, which detaches its old recorded pins and claims the new touch pins;
3. initialize buttons from the new config, skipping the new touch interrupt pins.

Use one target-local input-reconfiguration coordinator from both BLE startup and
`opendisplay_ble_reload_config_from_nvm()`. This prevents a new button callback from being removed
by touch's old-pin cleanup, and prevents old button cleanup from removing a new touch callback.
A failed/empty config must leave no old button callback armed.

### N4 — Bounce is a hardware-gated divergence, not an assumption

Start with ESP32's rule: read the pin in the callback and accept a transition only when the logical
level differs from the stored state. This requires narrowing `od_gpio.h`'s current “set a flag
only” callback comment to the operations actually allowed here: `od_gpio_read()`, Zephyr atomic
operations and `k_sem_give()`; BLE, logging, allocation, I2C and blocking remain forbidden.

If the hardware gate shows any count greater than the number of physical presses, add a bounded
per-button debounce/lockout and record it as a wire-visible divergence. Its interval must come from
measured bounce traces, not an arbitrary constant. Repeat every functional and current-consumption
row after adding it. The implementation is not done while this choice remains unresolved.

## 5. Loop wake and timing

Give a dedicated binary semaphore from every accepted button transition. Both connected and idle
waits consume it so an event reaches `od_button_nordic_process()` promptly.

Changing `k_msleep(step)` to a semaphore wait must not shorten configured timing. Convert
`idle_delay_ms()` to a wrap-safe deadline loop: after an early button wake, service BLE/buttons,
then wait only for the remaining wall-clock duration while preserving the one-second watchdog
service ceiling. Apply the same wakeable wait to the connected 10 ms cadence without busy-looping.

Initialization, deinitialization and config reload drain the semaphore and clear pending events
while callbacks are detached or masked. A stale give from the previous configuration must not
publish through a reused button slot.

## 6. BG22 cost/benefit decision

**Decision: exclude BG22 completely.** It already has a target-specific 32-button implementation
coupled to Silicon Labs external-interrupt allocation, EM4 wake arming, `app_proceed()` and direct
advertising restart. Replacing it offers only prospective deduplication while adding hardware
risk to a target whose button path is not hardware-qualified in this workspace.

The Nordic implementation remains target-local and is not added to `shared/sources.cmake` or any
BG22 source list. No file under `targets/efr32bg22-slc/` changes. Therefore the expected BG22 cost
is exactly zero bytes of flash and zero bytes of RAM. Before merge, compare the BG22 size/map
summary from the parent and candidate revisions. Any positive flash, data or BSS delta attributable
to this work is a failure: remove the dependency or linkage rather than accepting the increase.

A later shared-button proposal may reconsider BG22 only if it separately proves no flash or RAM
increase and carries a BG22 hardware gate. That work is outside this plan.

## 7. Implementation stages

Each software stage must build and pass its focused host tests before the next stage.

### Stage 1 — Characterize the reference

Add a host behavior table derived from the live ESP32 source for config admission, ID mapping,
initialization, transitions, count wrap and byte packing. Record the exact sibling commit inspected
in the implementation commit message. Do not compile sibling code or edit the sibling repository.

### Stage 2 — Replace the Nordic module

Delete `opendisplay_button.{c,h}` and add the new `od_button_nordic` module. Implement all § 3
behavior, the per-button atomic mailbox, IRQ-error fallback, deterministic coalescing per MSD byte,
and boost-before-publish. Update Nordic's CMake source list and callers in one buildable change.

### Stage 3 — Make IRQ ownership sufficient and testable

Raise or redesign the Nordic GPIO IRQ registry for 32 buttons plus four touch controllers. Update
its stale comments and callback-context contract. Add attach, detach, replacement, exhaustion and
callback-argument tests against the production GPIO registry where practical.

Record flash, data and BSS deltas for all three Nordic boards. The expected RAM cost is the extra
24 button records plus IRQ callback storage. If the increase exceeds 2 KiB on any Nordic image,
stop and reduce callback/runtime storage without reducing the 32-button contract before proceeding.

### Stage 4 — Integrate lifecycle and wake

Add the ordered input reconfiguration coordinator from N3 and call it at boot and after every
successful or failed config reload. Replace the relevant sleeps with deadline-preserving semaphore
waits. Verify that watchdog service remains no farther than one second apart and that repeated
button wakes do not collapse a configured idle interval into a busy loop.

### Stage 5 — Software gates

Add a production-source host suite covering at least:

- four records × eight pins, the `pins_used == 0` rule, sparse masks and `0xFF` pins;
- invalid input types and byte indices, touch-pin conflicts, pulls and inversion;
- initialization quiescence and the 10 ms/50 ms baseline sequence;
- press, release, count wrap, ordering-stamp wrap and exact encoded bytes;
- press plus release before one drain, and simultaneous events on same and different MSD bytes;
- attach failure with polling fallback, slot exhaustion and duplicate pins;
- config reload from buttons to none, pin reassignment, and button/touch ownership swaps;
- an IRQ racing a drain and an IRQ racing teardown, under ASan/UBSan where supported;
- semaphore coalescing, stale-give clearing, prompt wake and preservation of wall-clock deadlines;
- the absence of a five-second reset for ordinary digital buttons.

Add a ratchet that Nordic has no second button parser/state machine and no reference to the deleted
`opendisplay_button` module. Run `tools/check.sh`; then run `tools/check.sh --targets` and require
no failure and no skip. Read all three Nordic image/map outputs and the BG22 size result rather than
assuming successful builds prove the footprint claims.

### Stage 6 — Nordic hardware gate

Run on `xiao_nrf52840` with every test pin physically fitted. Implementation is not hardware
qualification. Required rows:

- reproduce the pre-change missed short tap while idle-advertising;
- post-change short press and release increments once and publishes promptly, both connected and
  idle-advertising with `sleep_timeout_ms` configured;
- 20 presses on every configured pin produce exactly 20 increments, resolving N4;
- inversion, pull-up and pull-down configurations each report the correct logical state;
- two buttons pressed before one service pass both survive when assigned different MSD bytes;
- two buttons sharing one MSD byte publish the most recently changed button;
- count wraps 15 to 0 with no time-based reset;
- a deliberately failed IRQ attachment logs the degradation and a held transition is recovered by
  polling;
- config reload removes old pins, arms new pins and survives a button/touch pin ownership swap;
- idle latency is prompt, watchdog service remains healthy, and configured idle timing is not
  shortened by repeated presses;
- idle and active current with buttons armed are measured against the parent image and accepted.

If practical on available fixtures, repeat the basic press/release, reload and current rows on one
nRF54 board. Absence of nRF54 hardware keeps those rows open; it does not invalidate the explicitly
scoped nRF52840 result.

### Stage 7 — Records and closeout

Update `docs/HARDWARE_VERIFICATION_CHECKLIST.md`, close `docs/FOLLOWUPS.md` § 12 only after the
short-tap hardware row passes, and record N1-N4's actual dispositions in
`docs/DIVERGENCE_MATRIX.md`. Record exact Nordic and BG22 size deltas. Do not describe an open,
waived or unrun row as passed.

## 8. Definition of done

1. Nordic's former button source is deleted, and the new implementation is traceable to the live
   ESP32 behavior table rather than to the obsolete Nordic plan.
2. All 32 contract-valid digital buttons can be represented alongside four touch IRQs.
3. A complete press/release between loop passes increments the count and causes prompt publication.
4. Concurrent buttons, ISR/thread ordering, callback failure and config reload are host-tested.
5. One physical press produces one count; bounce disposition is closed with evidence.
6. Advertising boost precedes MSD publication, waits preserve deadlines, and watchdog cadence is
   unchanged.
7. `tools/check.sh --targets` reports no failure and no skip, and all produced images are inspected.
8. BG22 source/linkage is unchanged and its flash, data and BSS increases are all zero.
9. Hardware evidence and remaining open debt are recorded without upgrading software evidence into
   a hardware pass.
