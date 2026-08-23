# LED runner promotion — design

**Status:** proposed, 2026-08-23. First item of
[PLAN_DEDUP_OUTSTANDING_2026-08-22.md](PLAN_DEDUP_OUTSTANDING_2026-08-22.md) § 8 step 5.
Design only. **This promotion is not implemented here** — see § 6.

Entry conditions from that plan are met: D1's yield floor and the `repeat_forever` sentinel landed
2026-08-22 with `tests/host/silabs_led_test.c` behind them, so the promotion inherits a proven
contract instead of re-deriving one.

---

## 1. What is duplicated

| | Lines | Notes |
|---|---|---|
| `targets/nordic-zephyr/src/opendisplay_led.c` | 399 | `k_timer` scheduling |
| `targets/efr32bg22-slc/opendisplay_led.c` | 485 | `sl_sleeptimer` scheduling, superloop |
| `targets/esp32-idf/src/device_control.cpp` | ≈380 of 905 | polled from the loop task |

Three copies of: the 12-byte `reserved[]` parse, an 11-state phase machine, the group/loop
counters, the `repeat_forever` sentinel, and a software PWM (brightness 1..16, seven 100 us
slices per step) whose threshold order
(7,1,6,2,5,3,4 for R/G; 3,1,2 for B) is **character-identical** on all three. The PWM comment on
Nordic even cites the ESP32 line numbers it was transcribed from.

What genuinely differs is three lines' worth: how a pin is written, how a wait is armed, and how
the loop gets called.

## 2. Decisions

### L1 — The machine returns a deadline; it never sleeps

`uint32_t od_led_service(uint32_t now_ms)` advances at most one yield-requiring action and returns
a **relative delay in milliseconds** — how long until it wants the next call — or `OD_LED_IDLE`
when nothing is pending. Relative, not an absolute deadline, so no caller has to reason about the
32-bit millisecond wrap that `od_hal_uptime_ms()` carries.

The machine still stores its own deadline absolutely and compares wrap-safely
(`(uint32_t)(now - deadline) < HALF_RANGE`), which is what makes an **early call harmless**: a
caller that services sooner than asked gets the remaining delay back and nothing advances. A late
call advances exactly one step, not the number of steps that fit in the elapsed time — the pattern
slips rather than fast-forwarding, which is what all three runners do today.

This is the decision `PLAN_SENSOR_SEAM_2026-08-23.md` § 4 deferred to here, and it dissolves the
yielding-sleep question rather than answering it. Each target already has the scheduler it needs:

| Target | Arms the next step with |
|---|---|
| nordic-zephyr | `k_timer_start()` — as it does now |
| efr32bg22-slc | `sl_sleeptimer_start_timer_ms()` — as it does now, and the superloop can also use the deadline to decide how long to idle |
| esp32-idf | nothing; it polls and compares against `od_hal_uptime_ms()` |

**No sleep seam, no timer HAL, no new vtable.** A shared machine that called "sleep" would be
wrong on all three: two of them must return to their event loop, and BG22 must be free to sleep
the part rather than spin.

### L2 — The seam is three functions, and none of them is a GPIO HAL

```c
/* od_led_app.h */
void od_led_app_write(uint8_t pin_cfg, bool level_high);   /* one pin, one level */
uint8_t od_led_app_mode(uint8_t instance);                 /* the live reserved[0] nibble */
void od_led_app_finished(uint8_t instance);                /* clear reserved[0] on completion */
```

`pin_cfg` is the encoded pin byte the config already carries (`0xFF` = unused, and every target
already decodes port/pin from it). The busy-wait inside the PWM ramp is `od_hal_delay_us()`,
already in `shared/hal`.

There is deliberately **no** shared GPIO HAL. The survey assumed `od_hal_gpio_{config_output,write}`
existed in `shared/hal`; it does not — ESP32's is target-local and Nordic's is `od_gpio.h` with
different names. A one-function pin seam avoids needing one. Pin *configuration* stays target-side, in
each target's existing init.

### L3 — The pattern is copied in, but the mode nibble is read live

`od_led_activate()` takes the pattern bytes and parses them into shared state, so the machine never
holds a `struct LedConfig *` and a config reload cannot mutate a running pattern underneath it.

**The mode nibble is the exception, and it must stay live.** All three runners re-read
`reserved[0]` at the top of every step and stop if the nibble is no longer 1 —
`device_control.cpp:405`, `opendisplay_led.c:199` (Nordic), `opendisplay_led.c:272` (BG22). That
is externally observable: anything that clears the byte — a config reload, a `CONFIG_WRITE`, a
future writer — halts the pattern. A copy-in-only design silently drops it, and the first draft of
this plan did exactly that: `od_led_app_finished()` can report completion but cannot observe
somebody else's clear.

So `od_led_app_mode(instance)` is part of the seam and `od_led_service()` consults it first, as
the three runners do now. **If a target ever wants the copy-in semantics instead, that is a
divergence with a matrix row** — not a silent consequence of promotion.

Activation inputs, stated so the seam is unambiguous: the **instance tag**, the **three encoded
pin bytes** (`led_1_r`, `led_2_g`, `led_3_b`), the **inversion flags** (`led_flags` bits 0-2), and
the **12 pattern bytes**. The machine holds all five; it asks the target only for the mode nibble.

### L4 — Instance selection stays target-side

ESP32 resolves an "active instance" by scanning `globalConfig.leds[]` for `led_type == 1`
(`device_control.cpp:639-647`); Nordic and BG22 index directly. That is config-shaped work on
three different config layouts. The target passes the resolved instance and its pin set in;
the machine treats the instance as an opaque tag it echoes back on `od_led_app_finished()`.

### L5 — The yield floor and the sentinel are the machine's, and are already specified

`LED_MIN_STEP_DELAY_MS` on every flash and at the group-closing edge, and raw `0xFE`/`0xFF` both
meaning indefinite (`DIVERGENCE_MATRIX` § 11). `tests/host/silabs_led_test.c` becomes the shared
machine's suite by repointing it; its 146 checks were written against exactly this contract, and
the exact-emission-count assertions transfer unchanged.

### L6 — ESP32 is the authority for the PWM ramp

All three agree today, but where they ever disagree, `../Firmware`'s ramp is the reference — it is
what the fleet has been calibrated against, and Nordic's own comment records that it was
transcribed from there. Diff against the sibling before transcribing, per the standing rule.

## 3. Shape

```
shared/core/od_led.c          the phase machine, the parse, the PWM ramp
shared/core/od_led.h          od_led_{reset,activate,stop,service}
                              (no od_led_active: step 2 deleted that query as uncalled --
                               do not reintroduce it without a caller)
shared/core/od_led_app.h      od_led_app_{write,mode,finished}      <- the whole seam
targets/*/opendisplay_led.c   pin config, instance resolution, timer arming   (~80 lines each)
```

New tier `APP_LED` in `shared/sources.cmake`, named for the seam rather than a driver, as
`APP_NFC`/`APP_XFER` are. BG22, Nordic and ESP32 all take it — there is no capability-off arm,
because every target has LEDs.

Expected: ~1,260 lines removed, ~420 added, and the D1 hang becomes unrepresentable rather than
fixed per target.

## 4. Hardware gate

Per target, and this is user-visible output — a host cannot verify it.

- [ ] **Each target:** a pattern with three loops, non-zero delays and a finite group count plays
      the same colours, in the same order, for the same duration as the pre-change image.
- [ ] **Each target:** brightness across its full range looks unchanged (the PWM ramp is the
      risk). The range is **1..16**, not 1..8: brightness is `((reserved[0] >> 4) & 0x0F) + 1`.
- [ ] **Each target:** an endless pattern (raw `0xFE`) runs until `LED_STOP`.
- [ ] **Each target:** clearing the mode nibble out of band (a config write) stops a running
      pattern, as it does today (L3).
- [ ] **BG22:** the D1 all-zero-delay case still yields — this is the regression the promotion
      could silently undo, and the host suite only covers the machine, not the scheduling.
- [ ] **BG22:** RAM unchanged (`heap_size`, `data + bss`).
- [ ] **Nordic:** no 1-step-per-second stall (the survey reported one; confirm it is gone rather
      than assuming the promotion fixed it).
- [ ] **ESP32:** the LED path does not regress the loop-task cadence.

## 5. Risks

1. **The PWM ramp is timing-sensitive and shared code makes it uniform.** If the three have
   drifted in a way the eye can see, promotion changes appearance on at least one target. The gate
   rows above exist for this; a "looks the same" judgement is the pass criterion, deliberately.
2. **BG22 has no watchdog.** A defect in shared scheduling reaches a superloop with no recovery.
   D1's suite guards the machine; it does not guard the target's timer arming.
3. **ESP32's polled model has no timer at all**, so its `od_led_app` must compare deadlines
   against `od_hal_uptime_ms()`. That is new code on the one target whose LED path is currently
   entangled with `device_control.cpp`.

## 6. Why this is not implemented in the same change as steps 3 and 4

`docs/MIGRATION.md`'s standing rule is **one subsystem per swap, build + flash + hardware verify
between each, independently revertable**. Steps 3 and 4 are written designs and carry no code, so
they share a branch safely. This promotion is a three-target code change whose gate is visual
output on three boards — batching it with the storage and sensor promotions would produce exactly
the un-bisectable change that rule exists to prevent.

It should land on its own branch, with § 4's rows attached, and before the config-storage and
sensor promotions, since it creates the `APP_LED` tier and settles L1 for both of them.
