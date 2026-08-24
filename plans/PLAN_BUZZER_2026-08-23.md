# Buzzer runner promotion — design

**Status:** software candidate implemented 2026-08-23 on `feat/buzzer-shared`; § 4 hardware rows
remain open. Second Tier B item in
[PLAN_DEDUP_OUTSTANDING_2026-08-22.md](PLAN_DEDUP_OUTSTANDING_2026-08-22.md) § 8 step 5.

The entry condition is closed here: the live `../Firmware` donor and `py-opendisplay` agree on the
frequency scale and folding rule, and the donor is authoritative for the cap where Nordic differs.

---

## 1. What is duplicated

ESP32 and Nordic each owned the complete `0x0077` stream validator, payload copy, pattern/repeat
machine, step and gap timing, enable-pin polarity, pitch mapping and total-duration cap. Only the
tone generator and scheduler are target-specific: ESP32 uses LEDC and loop polling; Nordic uses a
software square-wave `k_timer` plus a step `k_timer`. BG22 compiles buzzer config out.

## 2. Decisions

### B1 — `Firmware` and the host define pitch

The shared table is `round(100 * 13.75 * 2^(idx/24))` centi-Hz. Index 0 is silence; A4 at index
120 is exactly 44000 centi-Hz. This is `../Firmware/src/buzzer_control.cpp`'s table and the inverse
of `py-opendisplay`'s `hz_to_index()`. Nordic's linear 400–12000 Hz mapping is removed.

`tests/host/buzzer_test.c` checks all 256 inputs against an independently computed exponential
reference, so restoring the linear mapper cannot pass by preserving a few named notes.

### B2 — out-of-window indices octave-fold, they are not refused or clamped

Nonzero indices are shifted by 24 until they lie in the safe window 117..234. This preserves pitch
class: 1 maps to 121 and 255 maps to 231. The host explicitly says firmware performs this fold and
accepts raw indices 1..255, so refusing would break a payload the client considers valid.

### B3 — the total cap is 30,000 ms

`../Firmware` uses a 30,000 ms stop threshold; Nordic's 5,000 ms port is the divergence. The shared
machine clamps a tone step to the remaining budget, so a 255-unit repeating step schedules 23 full
1275 ms steps plus one 675 ms step and stops at exactly 30,000 ms. The authority does not clamp the
20 ms inter-pattern gap, so an all-gap edge can be observed at most 19 ms after the threshold, with
no tone sounding. Elapsed wall time is checked before every action.

### B4 — same deadline-returning shape as LED

`od_buzzer_service(now_ms)` returns a relative delay or `OD_BUZZER_IDLE`. It never sleeps and owns
the wrap-safe deadline. Early calls advance nothing; late calls slip rather than catch up. Nordic
arms its existing step timer from that delay, while ESP32 polls from its existing loop.

There is no timer or sleep HAL. The tone continues independently below the seam while the shared
machine waits.

### B5 — three target functions are the entire hardware seam

```c
bool od_buzzer_app_tone_start(uint8_t pin, uint32_t centihz, uint8_t duty_percent);
void od_buzzer_app_tone_stop(uint8_t pin);
void od_buzzer_app_enable_write(uint8_t pin, bool level_high);
```

ESP32 forwards tone start/stop to its existing LEDC driver. Nordic retains only the square-wave
timer and converts centi-Hz to the same rounded integer Hz the ESP32 driver uses. Config lookup,
pin initialization and reply framing stay target-side, as they do for LED instance selection.

### B6 — payload ownership and lifecycle preserve the donor

The shared machine validates the entire declared stream before preempting a run, then copies at
most 256 bytes, matching the authority rather than Nordic's private 244-byte buffer. The host's
120-step ceiling produces at most 244 bytes and target frame admission is narrower than the core,
so this only broadens non-host raw Nordic payloads. Outer repeat 0 means one. Zero-duration steps
do nothing. A valid activation preempts the current melody; session teardown does not. Deep sleep
and ESP32's power-off chirp do stop it because the scheduler will no longer run. The chirp reuses
the shared frequency function but remains an ESP32 policy.

## 3. Shape

```
shared/core/od_buzzer.c           parser, owned payload, pitch/fold, pattern machine, cap
shared/core/od_buzzer.h           od_buzzer_{activate,service,stop,index_centihz}
shared/core/od_buzzer_app.h       three-function tone/enable seam
targets/esp32-idf/...             config/replies, LEDC adapter, polling, shutdown chirp
targets/nordic-zephyr/...         config/replies, square-wave timer, step-timer arming
```

`APP_BUZZER` is a separate tier from `APP_LED`: the shapes match, but only two targets implement
the tone seam. BG22 does not consume the tier and must retain no buzzer symbol or state.

## 4. Hardware gate

The itemized open rows live in `docs/HARDWARE_VERIFICATION_CHECKLIST.md` § Shared buzzer runner.
Seven rows cover the two capable targets and BG22's zero-cost exclusion. The pitch rows need a
frequency counter or logic-analyser measurement; hearing a plausible note is not evidence for a
quarter-tone mapping.

## 5. Risks

1. Nordic's software square wave is scheduler-sensitive. The shared policy can request the right
   frequency while timer latency still produces the wrong physical pitch; only a pin measurement
   closes that row.
2. The 30-second decision is intentionally wire-visible on Nordic. A client may now hear 25 more
   seconds from a hostile or accidental repeat payload, although command processing stays
   non-blocking and a new melody still preempts it.
3. BG22's exclusion is a source-tier property, not a runtime `if`. Accidentally adding
   `APP_BUZZER` to its build would consume its narrow RAM margin even though config count is zero.
