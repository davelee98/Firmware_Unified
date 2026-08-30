# OBSOLETE — Port the ESP32 button behaviour to nordic-zephyr

**Status:** obsolete, 2026-08-30. Do not execute this plan. It is retained as reference only and
is replaced by
[PLAN_NORDIC_BUTTON_INTEGRATION_2026-08-30.md](PLAN_NORDIC_BUTTON_INTEGRATION_2026-08-30.md),
which was derived afresh from the current ESP32 reference and current target integration surfaces.

Originally proposed 2026-08-22. Successor to
[PLAN_DEDUP_OUTSTANDING_2026-08-22.md](PLAN_DEDUP_OUTSTANDING_2026-08-22.md) D10, which recorded
the defect and chose the honest half of the fix (delete the flag that did nothing). This plan is
the other half: make the missed press impossible rather than documented.

**This is a port, not a design.** `targets/esp32-idf/src/device_control.cpp` already implements
every mechanism below and is field-proven; `CLAUDE.md` makes `../Firmware` the authority when the
two disagree, and here Nordic simply dropped machinery on the way over. Diff against the sibling
before transcribing — `targets/esp32-idf/src/` is an import snapshot and drifts.

---

## 1. The defect, stated precisely

`targets/nordic-zephyr/src/opendisplay_button.c` detects a press by comparing the pin level to the
last observed state on each `opendisplay_button_process()` call. A press whose rising and falling
edge both land between two calls leaves the level where it started and is **never reported** — no
`press_count` increment, no MSD update, nothing on the wire.

The window is the caller's, not the button code's (`targets/nordic-zephyr/src/main.c`):

| Link state | `opendisplay_ble_process()` interval |
|---|---|
| Connected | 10 ms |
| Advertising, no `sleep_timeout_ms` | 500 ms |
| Advertising, `sleep_timeout_ms` configured | 1 s (`idle_delay_ms` chunk) |

So the exposure is about a second on an idle-advertising device — precisely when a user presses a
button to wake it. Recorded as `docs/FOLLOWUPS.md` § 12.

## 2. What the authority does, and what Nordic dropped

`handleButtonISR()` (`device_control.cpp:697-709`) takes the **button index** as its ISR argument
and performs the whole transition in ISR context:

```c
if (newState != btn->current_state) {
    btn->current_state = newState;
    lastChangedButtonIndex = buttonIndex;
    if (pressed) btn->press_count = (btn->press_count + 1) & 0x0F;
    buttonEventPending = true;
}
```

`processButtons()` (`:602-606`) then branches on `buttonEventPending`, takes
`lastChangedButtonIndex`, and publishes. **The edge records the event; the loop only reports it.**

Nordic's port kept a single parameterless handler that set one `volatile bool`, which nothing read.
That flag, its handler and the then-unused `od_gpio_configure_interrupt()` were deleted 2026-08-22
(dedup D10) rather than left looking like a solution. The seam is recoverable from git —
`git show f4f0aa1:targets/nordic-zephyr/src/od_gpio.c` — and step 1 below restores it deliberately.

**Three further gaps found while scoping this.** They are recorded here so the port is not
mistaken for complete when only the ISR lands:

| Gap | ESP32 | Nordic | Disposition |
|---|---|---|---|
| Press-count reset window | `press_count` resets after 5 s idle (`:214`) | never resets | **In scope** — § 4 step 3 |
| Power-off hold | `pollConfiguredPowerOffButtons()` (`:44-75`): hold `power_off_hold_sec` to trigger the latch | absent entirely | **Out of scope** — this target has no power latch, which is why `0x0052` answers `OD_ERR_POWER_OFF_UNSUPPORTED`. Do not port a trigger for hardware that does not exist |
| ADC ladder (`input_type` 3) | `AdcLadder` with debounce and thresholds | `input_type != 1` is skipped (`:50`) | **Out of scope** — no nRF board with a ladder exists, and the ESP32 comment says the scale is unvalidated on nRF |

## 3. Frozen decisions

### B1 — The ISR records the transition; the loop only publishes

The port keeps ESP32's split exactly. Anything that touches BLE, logging or I2C stays on the main
loop: `opendisplay_ble_set_dynamic_byte()`, `opendisplay_ble_update_msd()`,
`od_adv_app_boost()` and `od_log_*` are **not** ISR-safe and must not move.

### B2 — Per-button state, not a global "something happened"

The handler is registered per pin with its button index, as ESP32 does. A single flag cannot name
the pin or carry a count, which is exactly why the deleted one could not have been fixed by reading
it. `od_gpio_configure_interrupt()` returns to `targets/nordic-zephyr/src/od_gpio.{c,h}` with the
handler signature widened to carry a `void *` or `uint8_t` argument — it was parameterless before,
and that is the shape that forced the single-flag design.

### B3 — Shared state crosses ISR/thread with explicit ordering

`current_state` and `press_count` become ISR-written. On Zephyr the ordering must be stated, not
assumed: either take a spinlock around the drain (`k_spinlock`, ISR-safe) or make each button's
event a single atomic word the ISR publishes and the loop consumes. **Do not** rely on `volatile`
alone for a multi-field update — that is the bug class this port is fixing, one level down.

### B4 — The loop is woken, not just eventually reached

Capturing the edge stops the press being lost; it does not stop it being reported up to a second
late. `idle_delay_ms()` (`main.c:16-33`) becomes a wait on a `k_sem` the ISR gives, with the same
1 s chunk as its timeout, so cadence is unchanged when nothing happens and immediate when something
does. Feeding the watchdog stays where it is — the chunking exists for that.

### B5 — Debounce is the ISR's problem now

Level-polling was its own debounce: a bouncing edge settled before the next poll. An edge-triggered
handler sees every bounce, and each one that flips `current_state` increments `press_count` — a
single press could report as several. ESP32 re-reads the pin inside the ISR and compares against
`current_state`, which absorbs bounces that resolve before the read but not a slow noisy contact.
**Decide this explicitly with a measurement on hardware**, not by assumption: either a per-button
lockout (ignore edges within N ms of the last accepted one) or ESP32's re-read alone if the boards
in this fleet prove clean. This is the one place the port may need to exceed the authority.

### B6 — `MAX_BUTTONS` stays 8, and stays loud

The contract allows 4 blocks × 8 pins = 32; this target tracks 8. Raising it costs RAM on a part
that has some to spare, but that is a separate decision with its own justification. D10 already
replaced the silent drop with a logged refusal; this plan does not change the cap.

## 4. Staging

Each step builds and is independently revertable. Only step 5 needs hardware.

**Step 1 — restore the seam, widened.** Bring back `od_gpio_configure_interrupt()` from
`f4f0aa1` with a handler that receives its button index. No caller yet; the build proves it links
and costs nothing (`--targets`, all three boards).

**Step 2 — ISR capture.** Register per-button handlers; move the `current_state`/`press_count`
transition into the handler per B1/B2/B3. `opendisplay_button_process()` drains and publishes.
Level polling stays as the fallback ESP32 also keeps — a missed or coalesced edge must still be
recovered, and on this target the poll is what covers a pin whose interrupt failed to attach.

**Step 3 — press-count reset window.** Port ESP32's 5 s idle reset (`:214`). Wire-visible: the
4-bit count in the MSD byte is what a host reads, so a device that never resets diverges from one
that does after 16 presses.

**Step 4 — wake the loop (B4).** `k_sem` given by the ISR, `idle_delay_ms()` waits on it.

**Step 5 — hardware gate.** Mandatory, on `xiao_nrf52840` with buttons fitted. Rows in § 5.

**Step 6 — records.** `docs/HARDWARE_VERIFICATION_CHECKLIST.md` rows, `docs/DIVERGENCE_MATRIX.md`
entry if step 3 or B5 changes what the wire shows, and close `docs/FOLLOWUPS.md` § 12.

## 5. Hardware gate — the rows that decide it

No host test can qualify these: the defect is a timing window against physical contacts.

- [ ] **The defect, reproduced first.** On the pre-change image, idle-advertising with
      `sleep_timeout_ms` set, tap a button briefly. Confirm no MSD change. *Without this the fix
      has nothing to be measured against.*
- [ ] **The same tap, post-change**, reports: `press_count` increments once and the dynamic byte
      updates.
- [ ] **Latency:** the update is observed promptly rather than at the next 1 s boundary (B4).
- [ ] **One press is one count** across 20 presses on each configured pin — the bounce row (B5).
      A count that runs ahead of the presses is a FAIL, not a tuning note.
- [ ] **Connected state unregressed:** press/release at 10 ms cadence behaves as before.
- [ ] **A pin whose interrupt fails to attach** still reports via the poll fallback.
- [ ] **Count wrap:** the 4-bit counter wraps 15 → 0 and the host reads it correctly.
- [ ] **Reset window** (step 3): the count returns to 0 after the idle period and a later press
      starts from 1.
- [ ] **Sleep/current unregressed:** the idle current with buttons configured is unchanged, or the
      change is measured and accepted. Edge interrupts on pulled pins are a standing battery risk.

## 6. Out of scope, deliberately

- **Deep-sleep wake arming.** ESP32 has `targets/esp32-idf/src/wake_button.cpp` (EXT1/GPIO wake);
  Nordic has no equivalent and cannot wake on a button from its low-power path at all. That is a
  missing capability, not this defect, and the Zephyr mechanism is different enough that porting is
  a design rather than a transcription.
- **Power-off hold and the ADC ladder** — § 2, with reasons.
- **Raising `MAX_BUTTONS`** — B6.

## 7. Definition of done

1. A press shorter than the idle poll interval is reported, and this was demonstrated to fail
   first on the pre-change image.
2. The ISR does no BLE, logging or I2C work.
3. ISR/thread sharing has stated ordering, not incidental `volatile`.
4. One physical press produces exactly one count on hardware.
5. Every § 5 row is passed or explicitly open as named release debt — implementation is not a pass.
6. `tools/check.sh --targets` reports no failure and no skip.
7. `docs/FOLLOWUPS.md` § 12 is closed, and any wire-visible change from step 3 or B5 is in
   `docs/DIVERGENCE_MATRIX.md`.
