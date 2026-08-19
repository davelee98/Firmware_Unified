# Shared logging HAL

**Date:** 2026-08-19

**Source snapshot:** `main` at `5a169f2`, plus Phase 2 step 11 on `feat/transfer-phase2-step11`

**Authority:** `../Firmware/src/od_log.{h,cpp}` (71 + 401 lines). Per the migration constraint,
`Firmware` is the field-proven original; this plan promotes its shape rather than re-deriving one.

**Answers:** `PLAN_DEDUP_CONSOLIDATED_2026-08-17.md` Part 6 open question 5 ("Shared logging HAL:
yes or no?") and Part 3 prerequisite 2. It also discharges the deferral written into
`targets/esp32-idf/hal/od_hal_log.h` — "a decision to make when the logger is promoted and both
targets are in front of you".

**Verdict: yes, for `esp32-idf` and `nordic-zephyr`.** The API is already agreed between them;
only the implementation is duplicated.

**`efr32bg22-slc` does not adopt it** (project decision, 2026-08-19). It keeps its 102 bare
`printf()` calls and gains no record format. That is not a gap to close later — §3.3 states how
BG22 still links shared code that logs, and §5 has no BG22 cutover step.

---

## 1. What is actually there today

This is not a design-from-scratch task. Two of three targets already ship the authority's public
surface verbatim.

| | public macro surface | implementation | port seam |
|---|---|---|---|
| `../Firmware` (authority) | 4 levels, compile-time filter, `_od_log`/`od_log_raw`/`od_log_flush`/`od_log_hex_line` | 401 lines: drop accounting, `[DROP: n]` splice, ready hook, loop-task budget, stall backoff | `Stream*` / `tud_cdc_write()` by `#ifdef` |
| `esp32-idf` | **identical** (`src/od_log.h`, 83 lines) | 362 lines, near-verbatim port | `hal/od_hal_log.{h,c}` — `open`/`is_open`/`room`/`write`/`flush` |
| `nordic-zephyr` | **identical** (`src/od_log.h`, 68 lines) | 126 lines; Zephyr owns queueing, `LOG_RAW` delivers | none — Zephyr backend |
| `efr32bg22-slc` | **absent, and staying absent** | none | 102 bare `printf()` calls |

The level ladder, the four macros, the `do { if (OD_LOG_LEVEL >= …) } while (0)` construction and
its rationale, `_od_log`'s `__attribute__((format(printf, 2, 3)))`, and the
`[SSSS.mmm|Cn] L: message` record format are already byte-identical between ESP32 and Nordic.
Nordic's header even documents *why* the level test is inside the macro body rather than around it
— the same reason the authority has it there.

**So the promotion is: move the agreed policy into `shared/core/od_log.c`, and define the port
seam beneath it.** The disagreement is entirely below the API.

### 1.1 What is blocked on it

`DEDUP_6_SWEEP_2026-08-17.md` §7, re-counted against the current tree:

| Duplicate | Lines | What differs |
|---|---|---|
| `od_watchdog_app.{c,h}` | 164+48 esp32, 196+64 nordic = **472** | the clock, a Zephyr spinlock, four log strings |
| `od_session_app.*` | 136 + 136 + 63 = **335** | the clock, device-id source, log wording, rate-limit budget |
| `od_rxq_app.*` | 69 + 43 = **112** | log wording, ESP32's hex line and encrypted/plaintext token |
| `od_txq_app_dropped` | 3 sites, ~**20** | log wording only |
| `od_log_hex_line` | 2 copies, ~**40** | nothing — identical algorithm, 32-byte cap, `" ..."` suffix |

`targets/esp32-idf/src/od_watchdog_app.cpp` states the dependency in its own header: *"They are two
copies because `shared/` has no logging HAL and `od_watchdog` therefore cannot emit the boot report
itself; the day one lands, both collapse into it."*

Collapsing those is **not** part of this plan. This plan lands the keystone; each collapse is its
own promotion with its own hardware gate, sequenced afterwards.

**With BG22 out, three of the five collapse fully and two collapse partially.**
`od_watchdog_app` (472) and `od_rxq_app` (112) are ESP32+Nordic only already — BG22 declines
`HAL_WDT` and `APP_RXQ` — and `od_log_hex_line` (~40) has only two copies. Those 624 lines are
unaffected by the decision. `od_session_app` collapses its ESP32+Nordic pair (272 lines) while
BG22's 63-line copy stays, and one of `od_txq_app_dropped`'s three sites stays. **~900 of the ~979
lines still come back**; the residue is BG22's own and is not duplication, because after this there
is nothing left for it to be duplicated *with*.

---

## 2. The decision `od_hal_log.h` deferred

That header refused to narrow to `docs/SHARED_API_DESIGN.md`'s one-line sink because
`od_hal_log_room()` is load-bearing on nRF, and it recorded the question rather than guessing.
Here is the answer, and it needs no new machinery.

**Keep a port seam wide enough to express backpressure. "Unbounded" is a legitimate answer to
`room()`, and ESP32's own stdout backend already gives it.**

From `hal/od_hal_log.h`:

> The stdout backend reports `INT_MAX`. That is the honest answer for it and not a placeholder: the
> IDF console driver exposes no inspectable queue … Reporting a made-up finite number would turn a
> caller's backoff into either a permanent stall or a permanent no-op.

That is exactly Nordic's situation: Zephyr owns its own queue and its own drop accounting, so
`INT_MAX` is the honest answer there too. Shared policy's `room() >= need` test then passes
immediately, no wait occurs, and no drop is ever counted — precisely today's Nordic behaviour. The
backpressure machinery costs Nordic nothing and stays available on ESP32, where it is load-bearing.

**Rejected alternatives, and why:**

- *One-line sink (`void od_hal_log(const char *line)`).* Cannot express `room()`. ESP32's budget,
  stall backoff and drop counting exist for a real defect — a USB host that holds the port open and
  stops draining wedges `loop()` — and deleting them to fit a narrower contract trades a live
  protection for symmetry.
- *A capability flag / two-tier HAL.* Unnecessary. `INT_MAX` already encodes "no inspectable queue"
  without a second concept, and a flag would need every policy branch to be written twice.

### 2.1 The two FreeRTOS-typed hooks

The authority's `od_log_set_loop_task(TaskHandle_t)` and `od_log_set_ready_hook(bool (*)(void))`
cannot cross the boundary — the first names a vendor type, and CLAUDE.md's one rule forbids vendor
headers in `shared/`. Both become port-seam predicates the target answers:

- `bool od_hal_log_may_wait(void)` — replaces the loop-task capture. ESP32 answers "am I on the
  task that owns the budget"; Nordic answers `true` unconditionally, which is inert because its
  `room()` never forces a wait. The authority's load-bearing NULL default ("not captured yet ⇒ full
  budget") becomes the target's problem, stated in its adapter.
- `bool od_hal_log_is_ready(void)` — replaces the ready hook. Distinct from `is_open()`: a port can
  be open with nobody listening, and the authority's comment explains why conflating them hands the
  first terminal to attach a `[DROP: 4102931]` that says nothing. Nordic returns `is_open()`.

Two implementers, not three. A seam with one honest implementation per target is the bar; a third
that only exists to satisfy a linker is what §3.3 avoids.

---

## 3. Resulting shape

```
shared/core/od_log.h        the four levels, the four macros, _od_log, od_log_raw,
                            od_log_flush, od_log_hex_line, od_log_dropped_total
shared/core/od_log.c        POLICY: record format, compile-time level filter, drop
                            accounting and the [DROP: n] splice, the wait budget and
                            stall backoff, hex-line rendering
shared/hal/od_hal_log.h     PORT: open, is_open, is_ready, may_wait, room, write, flush,
                            now_ms
```

`od_log.c` goes in a new **`OD_SHARED_SOURCES_HAL_LOG`** tier — named for the HAL it needs, like
`HAL_CRYPTO` and `HAL_WDT`, not for a seam. Every target takes it; nothing declines it.

`od_log_cycle_count()` stays a seam (Nordic already declares it `__weak`): the deep-sleep cycle
count in the record header is a target fact.

### 3.1 The clock, and the time HAL this plan deliberately does not create

**There is no shared time HAL.** `shared/hal/` holds four headers — `od_hal_adv`, `od_hal_crypto`,
`od_hal_radio`, `od_hal_wdt` — and `od_hal_uptime_ms()` is target-local to
`targets/esp32-idf/hal/od_hal_time.h`.

**That absence is a design choice, not an omission, and the reason is testability.**
`shared/core/od_watchdog.h` states it outright: the clock "is passed in rather than taken from a
time HAL so the uptime rule is directly testable and `shared/` needs no clock interface of its
own." Eight shared entry points across three modules follow that rule today —
`od_watchdog_boot_init`, `od_watchdog_feed`, `od_session_alive`, `od_session_touch`, three more
`od_session_*`, and `od_txq_flush` — and every one of them is tested by passing an integer rather
than by installing a fake clock. That is why the session, watchdog and txq suites are as direct as
they are.

A logger cannot follow the rule: threading `now_ms` through ~500 call sites would destroy the API
the promotion exists to preserve. So **the port seam carries the clock**:
`uint32_t od_hal_log_now_ms(void)`. That is consistent with what the seam already is — the DEDUP
table lists "the clock" as the first of the things that differ between every duplicated seam file,
so it is a target fact by the same argument as `room()` and `may_wait()`.

**Do not create `shared/hal/od_hal_time.h` here.** Three reasons, in order of weight:

1. **It would put the eight existing call sites at risk.** A shared clock is a standing invitation
   for the next refactor to "simplify" `od_session_alive(s, now_ms, …)` into
   `od_session_alive(s, …)`. Nothing about the timeout logic gets better; the host suite gets
   materially worse. This is the only cost of a time HAL that makes something *worse* rather than
   merely costing time.
2. **It is not a one-function promotion.** `docs/SHARED_API_DESIGN.md` § `od_hal_time` carries an
   unresolved naming dispute (`od_msleep` signed vs `od_hal_delay_ms`) flagged as "Decide this
   before the Nordic import" — which did not happen — plus a contract point learned from two live
   ESP32 defects: `delay_ms` must round up to whole ticks and never to zero, or a deliberate settle
   becomes a busy-spin.
3. **One subsystem per swap.** The DEDUP plan schedules it as a Phase 2 prerequisite alongside the
   I2C seam, with its own gate. Landing it inside the logging plan couples two independently
   revertable pieces of work.

**When it does land, the reconciling rule is: shared modules that already take `now_ms` keep taking
it. The HAL serves ambient consumers only** — the logger, and whatever later module genuinely
cannot thread a parameter. Write that rule into `od_hal_time.h`'s header at creation, because the
refactor in reason 1 will otherwise look like an obvious cleanup to someone who has not read this.

With that rule, promotion costs the logger one line per target: `od_hal_log_now_ms()` is deleted
and `od_log.c` calls the shared clock directly. Keeping the dependency inside the log seam now is
what makes that a deletion rather than a rewrite, and note that
`targets/esp32-idf/hal/od_hal_time.h` was already written to the shared contract verbatim on
exactly that bet — "the same bet `od_hal_nvs` took". Promotion will not retire ESP32's `delay` and
`millis`, though: both are pinned by name from `third_party/` (bb_epaper declares `void delay(long)`
unmangled, FastEPD's `arduino_io.inl` has 19 `millis()` call sites), so a time HAL consolidates
less on that target than it appears to.

### 3.2 Record buffer sizing

The authority uses a 256-byte stack buffer in `_od_log` plus a 24-byte tag. Size it behind a
constant with a documented floor, per CLAUDE.md's memory rule:

```c
#ifndef OD_LOG_RECORD_MAX
#define OD_LOG_RECORD_MAX 256u          /* ESP32, Nordic */
#endif
_Static_assert(OD_LOG_RECORD_MAX >= 96u, "a record must hold the header plus a useful message");
```

`OD_LOG_MAX_TEXT` (the authority's 232) derives from `OD_LOG_RECORD_MAX`, not a second constant.

**BG22's decision removes this plan's only memory risk.** A 256-byte record frame against 480 bytes
of heap-inclusive headroom and a ~2.7 KB main stack was the one place the logger could cost more
than it saved. With `OD_CAP_LOG=0` there is no frame, no tag and no static state on that target —
see §3.3. Both remaining targets have room for the authority's numbers unchanged.

### 3.3 How BG22 links shared code that logs

BG22 takes `PURE + HAL_CRYPTO + HAL_RADIO + APP_SESSION + APP_INFLATE + APP_XFER`, so it compiles
`od_session.c`, `od_xfer*.c` and every other shared module. Once those modules log, BG22 has to
resolve the calls without implementing the seam.

**`OD_CAP_LOG`, defaulting to 1; BG22 sets 0 in `target_compile_definitions`.** Two properties make
this work:

- The existing macro body is `if (OD_LOG_LEVEL >= OD_LOG_ERROR)` with `OD_LOG_ERROR == 0`, so
  `OD_LOG_LEVEL` can never disable ERROR on its own. `OD_CAP_LOG=0` forces the level negative, and
  `-1 >= 0` is false for every level. **No macro body changes.** Arguments still compile, so a typo
  in a log call is still a build error on BG22 — the property Nordic's header calls out as the
  reason the test is inside the macro rather than around it.
- **`od_log.c` is still compiled on BG22, not omitted.** With `OD_CAP_LOG=0` its bodies reduce to
  empty and its static state to nothing, but the translation unit exists and `_od_log` resolves.
  Relying on the optimizer to delete a call in a folded-away branch would make linkage depend on
  `-O` level; that is not a bet worth taking on the one target with no headroom to debug it.

So BG22 implements **no** `od_hal_log_*` function, gains no buffer, no record format and no RAM,
and its console output is byte-for-byte what it is today.

**The rule this creates, and it is a trap:** a log call in shared code does nothing on BG22, so
**log arguments must have no side effects**. `od_log_info("n=%u", consume_next())` would skip
`consume_next()` on BG22 and nowhere else. This was already true for any level below the compiled
threshold; BG22 makes it true for *every* level on a whole target, which is what turns a latent
style rule into a ratchet (§6.1).

---

## 4. Behaviour that must not change

The record format is observable — Nordic's header calls it "the stable
`[SSSS.mmm|Cn] L: message` record format", and log scrapers and the hardware checklist's
"confirmed directly in device log" evidence both depend on it.

- **ESP32 output stays byte-identical.** The authority is explicit that ESP32 never counts a drop
  and never takes the tagged path, because doing so would break that guarantee. Preserve it.
- **Nordic keeps Zephyr as the transport.** `LOG_RAW` delivery, Zephyr's queue, its backend
  selection, and system logs staying visibly distinct. Shared policy formats the record; Zephyr
  still carries it.
- **Nordic's `od_log_dropped_total()` keeps returning a real `0`,** not a fabricated count — its
  own comment argues this and it is right.
- **BG22's output does not change at all.** Its 102 `printf()` calls stay exactly as they are.
  This is the strongest form of the guarantee: the target with the least headroom and the least
  hardware evidence is not touched by the promotion.

---

## 5. Sequence

Each phase is independently revertable and separately reviewable. Phases 1–3 change no target's
observable output.

**Phase 0 — freeze the current output.** Capture a boot-to-idle log from ESP32 and Nordic at
`OD_LOG_LEVEL=DEBUG` as fixtures. Without these, "byte-identical" in §4 is an assertion rather than
a test.

**Phase 1 — `shared/hal/od_hal_log.h` and the host fake.** Header only, plus a capture fake for
`tests/host/`, plus the first host test: record format, level filtering, `[DROP: n]` splice
position, hex-line rendering against the authority's exact output. No production file moves. This
is also the moment the logger becomes testable at all, which it has never been on any target.

**Phase 2 — `shared/core/od_log.c`.** Port the authority's policy. Dormant: no target links it yet,
so it is proven only against the host fake. Add the `HAL_LOG` tier to `shared/sources.cmake`
without adding it to any target's list.

**Phase 3 — cut over, one target per commit, in repository order.**
- *ESP32 first.* Its `hal/od_hal_log.c` is already the right shape — it gains `is_ready` and
  `may_wait` and loses nothing. `src/od_log.{h,cpp}` (445 lines) is deleted. Diff the Phase 0
  fixture; it must match byte-for-byte.
- *Nordic second.* New `src/od_hal_log.c` wrapping `LOG_RAW`, reporting `INT_MAX` room and
  `true` for `may_wait`/`is_ready`. `src/od_log.{h,c}` (194 lines) is deleted. Fixture must match.
- *BG22 third — capability-off, not a cutover.* Set `OD_CAP_LOG=0`, link `od_log.c`, implement no
  seam. The gate is a **zero** delta: same flash, same static RAM, same stack high-water, same
  console bytes. This step exists only to prove the shared logger costs the declining target
  nothing.

**Phase 4 — collapse the blocked duplicates.** `od_log_hex_line` first (identical, ~40 lines, zero
risk), then `od_txq_app_dropped`, `od_rxq_app`, `od_session_app`, `od_watchdog_app` — smallest to
largest, each its own promotion with its own gate. This is where the ~900 lines come back.
`od_session_app` and `od_txq_app_dropped` collapse their ESP32+Nordic copies only; BG22's stay.

---

## 6. Gates

Every phase runs `tools/check.sh --targets` and reads the summary. Additionally:

- **Phase 1–2:** host tests pass under gcc, clang and ASan/UBSan. The shared-boundary greps must
  pass — `od_log.c` may include `<stdarg.h>`, `<stdio.h>` and `<string.h>` and nothing else.
- **Phase 3, ESP32 and Nordic:** the Phase 0 fixture matches byte-for-byte at the same level, and
  the target builds.
- **Phase 3, BG22:** flash, static RAM, stack high-water and console output are all **unchanged**.
  A nonzero delta on any of the four means `OD_CAP_LOG=0` is not compiling out what it claims to.
- **Hardware, per target:** one boot-to-idle capture compared against the fixture, plus — on ESP32
  only, because it is the only target with the machinery — a deliberate host-stall test confirming
  the budget, the stall backoff and a `[DROP: n]` splice on recovery. That path has never had a
  regression test on any target and is the single most defect-prone thing being moved.

### 6.1 Ratchets

- **One logger.** No `od_log.{c,cpp,h}` under `targets/` after phase 3; the only `_od_log`
  definition is `shared/core/od_log.c`.
- **One hex renderer.** `od_log_hex_line` defined once. Its two current copies each carry a comment
  saying the RX and TX sides must render identically — that requirement becomes structural.
- **No vendor types in the seam.** `shared/hal/od_hal_log.h` names no `TaskHandle_t`, no `Stream`,
  no `k_*`.
- **Level ladder pinned.** `ERROR=0 WARN=1 INFO=2 DEBUG=3` and the `"EWID"` character table
  `_Static_assert`ed, so a reordering that silently relabels every historical log line is a build
  error.
- **BG22 implements no `od_hal_log_*`,** and `OD_CAP_LOG=0` stays set for it. A seam implementation
  appearing there is the decision being reversed by accident.
- **No side effects in log arguments.** Grep shared `od_log_*` call sites for `(` inside an
  argument other than a cast or a member access. Imperfect, but it catches the shape that silently
  changes behaviour on the one target where every log call is dead.

---

## 7. Stop conditions

Stop the current phase when:

- a target's Phase 0 fixture does not match after its cutover, and the difference is not a
  deliberate recorded change;
- BG22's flash, static RAM, stack or console output changes at all;
- BG22 acquires an `od_hal_log_*` implementation, or shared code starts assuming a logger exists;
- the seam acquires a vendor type, or a fifth port function that only one target can implement;
- Nordic's logger stops being carried by Zephyr's transport, or its system logs stop being visibly
  distinct;
- `od_log_dropped_total()` returns a fabricated number on any target;
- shared code starts logging inside an ISR or a lock held across a write; or
- a phase-5 collapse changes a log string that a plan, a doc or a hardware-checklist row quotes.

---

## 8. Definition of done

1. `shared/core/od_log.c` is the only logger; `shared/hal/od_hal_log.h` is the only port seam.
2. ESP32 and Nordic implement the seam; no target defines `_od_log` or `od_log_hex_line`; BG22
   implements none of it and builds with `OD_CAP_LOG=0`.
3. ESP32 and Nordic output is byte-identical to their Phase 0 fixtures; BG22's output is unchanged.
4. The drop/budget/backoff path has a host test and one ESP32 hardware observation.
5. `OD_LOG_RECORD_MAX` has its floor asserted, and BG22's flash/RAM/stack deltas are recorded as
   zero.
6. No `shared/hal/od_hal_time.h` was created as a side effect; the clock stays inside the log seam.
7. The four ratchets in §6.1 are in `tools/check.sh`.
8. `tools/check.sh --targets` reports no failure and no skip.

Phase 4's ~900 lines are tracked separately; this plan is done when the keystone is in and the
duplicates are *unblocked*, not when they are gone.
