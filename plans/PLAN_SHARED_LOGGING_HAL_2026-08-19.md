# Shared logging HAL

**Date:** 2026-08-19

**Source snapshot:** reviewed against `main` at `e965dd9`.

**Authority and reconciliation:** `../Firmware/src/od_log.{h,cpp}` is the field-proven origin of
the public API and record format. The current unified ESP32 and Nordic implementations are the
authority for behavior already adapted after import. In particular, the legacy donor's TinyUSB
room polling and stall backoff are not treated as live unified-firmware requirements when neither
current target uses them.

**Verdict:** promote one shared logger for `esp32-idf` and `nordic-zephyr` over a small
complete-emission HAL. `efr32bg22-slc` deliberately keeps its bare `printf()` output and links
explicit capability-off logger stubs.

**Implementation checkpoint — 2026-08-19:** Phase 1 and the logging promotion are software-complete
on `codex/log-promotion`. `tools/check.sh --targets` passes 29/0/0, including all ESP32
configurations, all three Nordic boards and BG22; the host suite is 48/48 under its normal profile
and also passes GCC, Clang and ASan/UBSan in the gate. ESP32 and Nordic use the shared logger and
their complete-record HALs; both target-local logger copies, ESP32's logger-only FreeRTOS shims,
the ready/loop-task hooks and application drop accounting are gone. The Nordic production-adapter
fixture queues a mutable record, clobbers its source stack and proves the deferred package retained
the original bytes. BG22 states `OD_CAP_LOG=0`, defines no log HAL, and its linked image contains no
logger/HAL symbol or state. It remains 250,196 B flash and 32,284 B static RAM, exactly matching the
post-time-HAL baseline.

The time-HAL timing/known-interval rows and the normalized ESP32/Nordic hardware log captures remain
open. Implementation proceeded by project direction; the software gate does not qualify those
hardware rows. The cutover also exposed and removed one accidental include dependency: ESP32
`main.cpp` now includes `esp_heap_caps.h` directly instead of receiving its heap API transitively
through the deleted logger/FreeRTOS header.

This answers `PLAN_DEDUP_CONSOLIDATED_2026-08-17.md` Part 6 question 5 and discharges the deferral
in `targets/esp32-idf/hal/od_hal_log.h`.

---

## 1. Current state

| Target | Public surface | Current delivery |
|---|---|---|
| `esp32-idf` | Four levels, `_od_log`, raw, flush, hex renderer and dropped count | Target logger formats records, takes a FreeRTOS mutex and writes through `od_hal_log`; its port-ready check is unconditionally true |
| `nordic-zephyr` | The same four levels and functions | Target logger submits each complete record once through Zephyr `LOG_RAW`; Zephyr owns queueing, serialization and drops |
| `efr32bg22-slc` | No `od_log` API | 102 bare `printf()` calls; unchanged by this plan |

The level ladder and normal record shape are agreed:

```text
[SSSS.mmm|Cn] L: message\r\n
```

The implementations are not otherwise identical. ESP32 caps normal and raw text at 232 bytes.
Nordic permits 253 bytes before CRLF for a normal record and 255 bytes for raw output. Nordic
clamps an invalid direct `_od_log()` level to INFO; ESP32's direct call indexes the level table
without validation. Public macros only issue valid levels.

### 1.1 What this unblocks

The logging boundary is the prerequisite for later, independent promotions of:

- the two `od_log_hex_line` copies;
- `od_txq_app_dropped` logging;
- the ESP32/Nordic `od_rxq_app` pair;
- the ESP32/Nordic portions of `od_session_app`; and
- the ESP32/Nordic `od_watchdog_app` pair.

Those collapses are not part of this plan. BG22's session and TXQ logging adapters remain target
code because BG22 declines the shared logger.

---

## 2. Decisions

### 2.1 Submit one complete emission to the target

The target HAL is:

```c
void od_hal_log_open(void);
bool od_hal_log_is_open(void);
void od_hal_log_write(char *record, size_t len);
void od_hal_log_flush(void);
uint32_t od_hal_log_cycle_count(void);
```

Uptime is deliberately absent: it comes from `shared/hal/od_hal_time.h`'s `od_hal_uptime_ms()`,
which §2.3 promotes alongside `od_hal_delay_us()`. Only the cycle count stays logger-specific.

`od_log.c` formats the final bytes, including CRLF for a normal record, in one mutable stack buffer,
stores a terminating NUL at `record[len]`, and calls `od_hal_log_write()` exactly once per
`_od_log()` or `od_log_raw()` invocation. The record contains formatted text rather than arbitrary
binary data; embedded NUL is not supported. The HAL must consume or copy the `len` bytes before
returning, must not mutate or retain the caller's buffer, and must make one call safe against
concurrent task callers. The shared logger is not ISR-safe; no target may call it from an ISR.

The return type is `void` deliberately. Zephyr's deferred logger can accept a `LOG_RAW` submission
without exposing whether a backend later drops it, so a byte-count return would invite the Nordic
adapter to fabricate delivery knowledge. Logging remains best-effort.

Target behavior:

- ESP32 uses one `uart_write_bytes()` or `fwrite()` call. The selected driver owns serialization.
- Nordic uses one `LOG_RAW("%s", record)` submission. `record` deliberately remains `char *`, which
  tells Zephyr's deferred logger that it is transient and must be copied into the log package before
  the call returns. Do not cast it to `const char *`: Zephyr treats that as read-only storage that
  may be retained. Do not use `%.*s`: Zephyr's logging package does not support width or precision
  on string conversions. Zephyr owns serialization, queueing, backend selection and its internal
  drop reporting after that copy.
- Flush and write must be mutually safe through the selected target transport. Shared code owns no
  RTOS mutex and includes no RTOS header.

### 2.2 Do not promote obsolete donor backpressure machinery

The shared interface has no `room`, `may_wait`, `is_ready`, lock or sleep operation.

This is a reconciliation, not a simplification by assumption:

- Current ESP32's `od_port_wait_ready()` returns `true` without consulting
  `od_hal_log_room()`. Its UART drains independently of a host and its stdout backend exposes no
  inspectable queue.
- No current ESP32 call installs `od_log_set_ready_hook()`.
- Current Nordic submits complete records to Zephyr and exposes neither room nor application-level
  drop accounting.
- A shared implementation of the donor algorithm would require RTOS mutex, current-task, tick and
  sleep primitives that the proposed portable boundary must not acquire.
- Applying the donor room test to ESP32's finite UART-room report would introduce drops that do not
  occur today.

The one complete-write contract removes the multipart reservation that required the ESP32 logger
mutex. `od_log_set_loop_task()` and `od_log_set_ready_hook()` retire. No production caller uses the
ready hook; the loop-task setter exists only to govern the retired wait budget.

`od_log_dropped_total()` remains for source compatibility and returns a real `0` on both adopting
targets. The `[DROP: n]` splice and the ESP32 mutex-contention counter retire. No production caller
reads the count. This is a deliberate behavioral convergence: the current Nordic target already
returns zero, and the current ESP32 target cannot generate a host-stall drop because its readiness
path is unconditional. Record it in the implementation checkpoint rather than describing the
legacy donor path as current behavior.

### 2.3 Create `od_hal_time` with the two functions that already agree; keep ambient time out of policy APIs

**Two rules, and they are separate.**

**Rule one: shared policy keeps its explicit `now_ms` parameters.** Session, watchdog and TXQ
functions are tested by passing an integer rather than installing a fake clock —
`shared/core/od_watchdog.h` says the clock "is passed in rather than taken from a time HAL so the
uptime rule is directly testable". Eight entry points follow it: `od_watchdog_boot_init`,
`od_watchdog_feed`, `od_session_alive`, `od_session_touch`, three more `od_session_*`, and
`od_txq_flush`. **None of them changes**, now or when `od_hal_time` later gains a bounded sleep.
This is the only
cost of a shared clock that makes something *worse* rather than merely costing effort, and the rule
belongs in `od_hal_time.h`'s header at creation (§2.3.1) — otherwise the first refactor to "simplify"
`od_session_alive(s, now_ms, …)` looks like an obvious cleanup.

**Rule two: create `od_hal_time` — `shared/hal/od_hal_time.h` — with the two functions the targets
already agree on.** The three time functions do not share a fate:

| | ESP32 | Nordic | agreement |
|---|---|---|---|
| uptime ms | `uint32_t od_hal_uptime_ms(void)` | `uint32_t od_uptime_get_32(void)` → `k_uptime_get_32()` | signature identical; name differs |
| busy wait µs | `void od_hal_delay_us(uint32_t)` → `esp_rom_delay_us` | `void od_busy_wait(uint32_t)` → `k_busy_wait` | signature **and** semantics identical — neither yields; name differs |
| sleep ms | `void od_hal_delay_ms(uint32_t)` | `void od_msleep(int32_t)` → `k_msleep` | **name and signedness differ** |

```c
/* shared/hal/od_hal_time.h */
uint32_t od_hal_uptime_ms(void);       /* monotonic ms since boot, free-running 32-bit */
void     od_hal_delay_us(uint32_t us); /* busy-wait; does NOT yield; keep the argument small */
```

The `od_hal_*` names win per `docs/SHARED_API_DESIGN.md`, matching `od_hal_nvs`/`od_hal_gpio`/
`od_hal_crypto`. Nordic renames `od_uptime_get_32` → `od_hal_uptime_ms` and `od_busy_wait` →
`od_hal_delay_us`, leaving `od_zephyr_compat.h` holding only `od_msleep`.

ESP32 is not a header-only repoint. Its current target header also declares the intentionally
unpromoted `od_hal_delay_ms(uint32_t)`, and production callers depend on that declaration. Phase 1
therefore moves the millisecond-sleep declaration and its contract into target-private
`targets/esp32-idf/hal/od_hal_sleep.h`; `od_hal_time.c` includes both the shared time header and the
private sleep header, and every ESP32 millisecond-sleep caller includes the private header. Update
the host shim at the same boundary: delete its shadow `od_hal_time.h`, include the canonical shared
header, provide the test clock as a fake `od_hal_uptime_ms()` definition, and expose any test-only
millisecond-sleep declaration through a private sleep shim. There must not be a target-local or
host-shim `od_hal_time.h` shadowing the canonical shared header through include-path order.

#### 2.3.1 Two comments `od_hal_time.h` must carry at creation

Both are one-time costs that prevent a predictable mistake, and neither is inferable from the code:

1. **The bounded wait is deferred, not forgotten.** State that `od_hal_time` deliberately declares
   no `od_hal_delay_ms()`/sleep, name the unresolved points (ESP32 `uint32_t` vs Nordic `int32_t`
   `od_msleep`, zero-delay behaviour, and the round-up-never-to-zero rule learned from two live
   ESP32 defects), and say that adding one is its own decision. Without this, the header reads like
   an incomplete port and the obvious "finish it" commit silently picks a signedness.
2. **Shared policy keeps its `now_ms` parameters.** State that `od_hal_uptime_ms()` is for ambient
   consumers only and that the eight existing policy entry points must not be "simplified" to call
   it. That refactor looks like a cleanup to anyone who has not read this plan.

**Sleep is excluded deliberately.** `od_msleep`'s `int32_t` mirrors `k_msleep`, where a non-positive
value means "already expired, return immediately"; `od_hal_delay_ms`'s `uint32_t` mirrors FreeRTOS
ticks, which have no such convention. ESP32's contract also carries a rule learned from two live
defects — round UP to whole ticks, never to zero, or a deliberate settle becomes a busy-spin.
`SHARED_API_DESIGN` said "**Decide this before the Nordic import**"; the import happened without
it, so the disagreement is locked in and needs its own decision. **Nothing in this plan needs a
shared bounded sleep.**

**Consequence for the log seam: there is no `od_hal_log_uptime_ms()`.** `od_log.c` calls
`od_hal_uptime_ms()` directly. The seam keeps only the fact that is genuinely logger-specific:

- `od_hal_log_cycle_count()` returns the target's persistent wake/deep-sleep cycle number. ESP32
  replaces its logger-only `getDeepSleepCount()` accessor with an `extern "C"` implementation in
  `main.cpp`; Nordic returns zero.

Uptime is monotonic milliseconds since the current boot, truncated modulo `2^32`. It is not wall
time and must not jump due to clock synchronization. The wrap is in the millisecond result domain,
not in a target's underlying tick domain.

**BG22 implements both, but they are not load-bearing there today.** With `OD_CAP_LOG=0` the logger
is the only shared ambient consumer and it is compiled out, so **nothing shared calls
`od_hal_uptime_ms()` on BG22**. The target implements the complete interface so a later shared
ambient consumer does not arrive to find one target missing it; this phase does not rewrite BG22's
existing direct `sl_udelay_wait()` or open-coded sleeptimer call sites. Removing those target calls
is a separate time-HAL migration with its own timing qualification, especially for the 1–2 us
hardware paths.

That leaves the new BG22 `od_hal_uptime_ms()` definition intentionally unused at the end of Phase 1.
This is deliberate staging, not evidence that the existing target clocks are correct. Ten current
call sites still use the 32-bit tick conversion: session, PIPE, transfer timing, six BLE/NFC/timeout
sites and the `bb_epaper` `millis()` adapter. Their pre-existing long-uptime defect is recorded in
`docs/FOLLOWUPS.md` § 7 and must be fixed as an independently reviewable target change rather than
folded silently into logging promotion.

The BG22 uptime wrapper must not use
`sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count())`. That converts a 32-bit hardware tick
count, so it wraps in the low-frequency tick domain (roughly 36 hours at 32.768 kHz) and violates the
`uint32_t` millisecond contract. It must read `sl_sleeptimer_get_tick_count64()`, convert with
`sl_sleeptimer_tick64_to_ms()`, check the status, and only then cast the 64-bit millisecond value to
`uint32_t`. A read before `sl_sleeptimer_init()` reports `SL_STATUS_INVALID_PARAMETER`; return `0`,
the boot-domain origin, on that path and on another conversion failure. Do not assert or log from
the clock: early boot logging is a planned caller, an assertion can HardFault without a debugger,
and logging the failure would recurse through timestamp acquisition.

Host tests inject uptime and cycle count and cover uptime `0`, `999`, `1000` and `UINT32_MAX`, plus
cycle zero and a non-zero value. Timestamp formatting may widen beyond four seconds digits; that is
existing behavior.

### 2.4 Flush owns its target-specific settlement

`od_hal_log_flush()` means: push the selected transport toward the host, apply its bounded drain
policy, and then perform the existing 5 ms settlement before returning.

- ESP32 UART keeps its bounded `uart_wait_tx_done()` and then calls target-local
  `od_hal_delay_ms(5)`; stdout performs `fflush()` and the same delay.
- Nordic calls `log_flush()` and then `k_msleep(5)`.

The delay lives inside the log HAL because it is part of the log transport's flush guarantee.
Shared `od_log.c` therefore needs no sleep of its own, and this is exactly why §2.3 promotes uptime
and busy-wait but not the bounded sleep. The minimum-5-ms property stays the target's
responsibility, including tick rounding.

When a bounded sleep is later added to `od_hal_time`, its contract must reconcile the current
unsigned ESP32 `od_hal_delay_ms()` with Nordic's signed `od_msleep()`, define the zero-delay
behavior and guarantee that a positive millisecond delay rounds up rather than to zero. That later
addition may replace these flush delays, but it must not remove explicit `now_ms` parameters from
shared policy modules.

### 2.5 Preserve each target's existing truncation boundary

Use one `OD_LOG_RECORD_MAX == 256` stack buffer. Target compile definitions preserve the existing
text caps:

| Profile | Normal text before CRLF | Raw text |
|---|---:|---:|
| ESP32 | 232 | 232 |
| Nordic | 253 | 255 |

`OD_LOG_TEXT_MAX` and `OD_LOG_RAW_TEXT_MAX` are compile-time constants with static assertions:

```c
_Static_assert(OD_LOG_TEXT_MAX + 2u + 1u <= OD_LOG_RECORD_MAX,
               "normal record plus CRLF and NUL must fit");
_Static_assert(OD_LOG_RAW_TEXT_MAX + 1u <= OD_LOG_RECORD_MAX,
               "raw record plus NUL must fit");
```

The target profiles are tested separately. A later decision may converge the caps, but this
promotion does not silently truncate Nordic lines earlier or lengthen ESP32 output.

Invalid direct levels are clamped to INFO in shared code, adopting Nordic's safe behavior. This is
a defensive change only for callers that bypass the four public macros; no such production caller
exists.

### 2.6 BG22 capability-off is explicit code, not optimizer behavior

`OD_CAP_LOG` defaults to 1. BG22 sets it to 0. `od_log.h` defines an effective level of `-1` when
the capability is off while keeping the existing `do { if (constant) ... } while (0)` macro shape.
Arguments therefore still compile but are not evaluated.

`od_log.c` has explicit capability branches:

- With `OD_CAP_LOG=1`, compile the real implementation and HAL references.
- With `OD_CAP_LOG=0`, compile no static logger state and no HAL references. `od_log_init()`,
  `_od_log()`, `od_log_raw()` and `od_log_flush()` are explicit no-ops;
  `od_log_dropped_total()` returns zero. The pure `od_log_hex_line()` renderer may remain available
  because it requires no logger state or HAL.

A host executable compiles this profile at `-O0`, supplies no `od_hal_log_*` fake, links
successfully and verifies with `nm` that it has no unresolved or defined log-HAL symbol. This is
the proof that BG22 does not depend on dead-code optimization for linkage. The target map separately
proves zero logger state and measures the final flash/RAM/stack delta.

Log arguments must have no side effects. That rule already applies to every compile-time-disabled
level; capability-off makes it apply to every log call on BG22. Add a ratchet for obvious nested
function calls in shared log arguments and review false positives manually.

---

## 3. Resulting ownership

```text
shared/core/od_log.h       levels, macros and public functions
shared/core/od_log.c       formatting, target-specific caps, capability-off stubs and hex renderer
shared/hal/od_hal_log.h    complete-emission transport, flush and the cycle fact
shared/hal/od_hal_time.h   od_hal_uptime_ms() and od_hal_delay_us() (§2.3)
```

`od_log.c` enters a new `OD_SHARED_SOURCES_HAL_LOG` tier. The tier explicitly depends on both the
`od_hal_log` and `od_hal_time` seams; an enabled consumer must implement both. ESP32 and Nordic take
the enabled tier. BG22 compiles the same source with `OD_CAP_LOG=0` and implements no log-seam
function; its independently compiled time implementation remains available to target code and
future shared ambient consumers.

The HAL contract is target-private behavior beneath shared policy:

- no vendor/framework types in either shared header;
- no target registry or function table;
- no dynamic allocation;
- no retention of the caller's stack buffer;
- no ISR use; and
- one target transport submission per shared emission.

---

## 4. Observable behavior

- ESP32 normal and raw bytes retain their existing caps and record formatting. Its unused
  mutex-contention drop report retires as recorded in §2.2.
- Nordic retains `LOG_RAW`, its queue/backends, its long-record limits and a real dropped total of
  zero. System logs remain visibly distinct. Its adapter also fixes the current latent deferred-log
  lifetime hazard by identifying the stack record as transient so Zephyr copies it before return.
- BG22's 102 `printf()` calls and console bytes are unchanged.
- Normal records retain CRLF. Raw records retain neither prefix nor automatic newline.
- `od_log_flush()` retains at least the existing target-specific 5 ms settlement.

Hardware boot timestamps and ESP32 cycle counts are inherently variable. “Byte-identical” gates
therefore have two layers:

1. Exact host fixtures use injected uptime/cycle values and compare every byte.
2. Hardware captures normalize only `[uptime|cycle]`; message bytes, level, ordering and CRLF must
   match the pre-cutover capture.

---

## 5. Sequence

Each phase is independently revertible.

### Phase 0 — freeze behavior

1. Capture ESP32 and Nordic boot-to-idle logs at `OD_LOG_LEVEL=DEBUG`.
2. Store a normalized fixture that replaces only uptime and cycle fields.
3. Record both targets' normal/raw truncation outputs at their exact boundaries.
4. Record the deliberate retirement of ESP32's unconsumed dropped counter and donor `[DROP]` path.

### Phase 1 — headers, fake and profiles

1. Add `od_hal_time` — `shared/hal/od_hal_time.h` with `od_hal_uptime_ms()` and
   `od_hal_delay_us()` (§2.3), carrying both §2.3.1 header comments: the bounded wait is deferred
   with its open points named, and shared policy keeps its `now_ms` parameters. On ESP32, move
   `od_hal_delay_ms()` into target-private `od_hal_sleep.h`, update its production callers and host
   shim, and delete the target-local `od_hal_time.h`. Rename Nordic's
   `od_uptime_get_32`/`od_busy_wait`. Add BG22's two wrappers without migrating its existing direct
   timing calls; implement uptime through the SDK's 64-bit tick and millisecond conversion APIs.
   This time-HAL change lands, builds and passes its adapter tests on its own before anything
   logging-specific exists. Record its flash/RAM deltas separately and make the resulting artifacts
   the baseline for the later logging-only size comparisons.
2. Add `shared/hal/od_hal_log.h` with the five-function contract in §2.1.
3. Add `shared/core/od_log.h`, including `OD_CAP_LOG` and effective-level handling.
4. Add a capture fake with configurable open state, uptime and cycle values.
5. Add ESP32-cap, Nordic-cap and capability-off host profiles.
6. Do not link production targets yet.

### Phase 2 — dormant shared implementation

1. Implement one-buffer/one-write formatting, raw output, safe level clamping and hex rendering.
2. Add explicit `OD_CAP_LOG=0` stubs.
3. Add `OD_SHARED_SOURCES_HAL_LOG` without adding it to production target lists.
4. Prove the complete host matrix, including the `-O0` no-HAL capability-off executable.

### Phase 3 — target cutovers

One target per commit:

1. **ESP32:** extend `hal/od_hal_log.c` with complete-write and settled-flush semantics;
   replace `getDeepSleepCount()` in `main.cpp` with the C-linkage cycle seam. Delete
   `src/od_log.{h,cpp}`, remove the loop-task setter from `main.cpp`, and remove its FreeRTOS host
   shims if orphaned. Preserve the existing port-open ordering.
2. **Nordic:** add `src/od_hal_log.c` over `LOG_RAW`, `log_flush`, `k_msleep` and cycle zero.
   Call `od_hal_log_open()` before `od_log_init()` in `main.c`, then delete
   `src/od_log.{h,c}`.
3. **BG22:** set `OD_CAP_LOG=0`, link the stub profile, implement no log-HAL function and prove the
   target-size/output gate against the post-time-HAL Phase 1 baseline.

### Phase 4 — later deduplication

Promote the blocked duplicate seams separately, smallest to largest: hex rendering, TXQ drop
wording, RXQ app, session app, watchdog app. Each retains its own target and hardware gate. This
logging plan is complete before Phase 4 starts.

---

## 6. Required tests and gates

### 6.1 Host tests

- exact level characters and prefixes;
- uptime `0`, `999`, `1000`, `UINT32_MAX` and cycle zero/non-zero;
- the BG22 time adapter converts across the underlying 32-bit tick rollover without an uptime jump,
  narrows modulo `2^32` only after 64-bit millisecond conversion, and returns `0` when conversion is
  unavailable before sleeptimer initialization;
- valid levels plus invalid direct-level clamping;
- normal and raw caps for both target profiles, including cap−1, cap and cap+1;
- CRLF on normal records and no automatic suffix on raw records;
- one and only one HAL write call per emission;
- zero-length raw output behavior pinned to the current targets' result;
- hex rendering at lengths 0, 1, 32 and 33;
- closed/uninitialized logger behavior;
- flush calls the HAL exactly once; target tests own the 5 ms implementation proof;
- interleaved host threads cannot split one shared record into multiple HAL calls;
- `OD_CAP_LOG=0` at `-O0` links without a HAL and has no logger state or HAL symbols.

Run under gcc, clang and ASan/UBSan.

### 6.2 Target gates

- The standalone Phase 1 time-HAL commit runs `tools/check.sh --targets` with no failure or skip;
  all ESP32 configurations, all three Nordic boards and BG22 must compile before logging work begins.
- Phase 1 records source-body equivalence for the existing ESP32 and Nordic microsecond-delay
  implementations. On available hardware, exercise ESP32's D-FF 50 µs pulse path and Nordic's
  `bb_epaper` delay path after the header/name migration; an open row is hardware debt, not a pass.
- BG22 verifies `od_hal_uptime_ms()` against a known hardware interval, in addition to the fake-
  sleeptimer rollover test. Because production currently has no caller and section GC removes the
  adapter, this row requires a temporary instrumented build or waits for the first linked consumer;
  a normal image cannot exercise it. This qualifies the new wrapper only; it does not qualify the
  ten direct call sites tracked separately in `docs/FOLLOWUPS.md` § 7.
- `tools/check.sh --targets` reports no failure and no skip after every cutover.
- ESP32 and Nordic normalized hardware fixtures match message bytes, levels, ordering and CRLF.
- ESP32 UART and stdout builds both compile; Nordic builds all three boards.
- A Nordic deferred-mode test queues one application record, clobbers its source stack before the
  backend drains, and compares the exact output, proving the transient record was copied during the
  `LOG_RAW` submission.
- Target-specific tests or instrumentation confirm one transport submission per emission under
  concurrent task logging.
- ESP32 flush retains bounded UART drain plus at least 5 ms settlement; Nordic flush retains
  `log_flush()` plus at least 5 ms settlement.
- BG22 console output is unchanged, no `od_hal_log_*` symbol exists, no logger static state appears
  in the map, and logging-only flash/RAM/stack deltas are recorded against the post-time-HAL Phase 1
  baseline. A nonzero logging-only delta stops the cutover for review.

### 6.3 Ratchets

- one `_od_log` and one `od_log_hex_line` definition, both in `shared/core/od_log.c`;
- no target-local `od_log.{c,cpp,h}` after all three cutovers;
- no vendor type or header in either shared logging file;
- no target-local or host-shim `od_hal_time.h`; ESP32's unpromoted millisecond sleep is declared only
  by its target-private `od_hal_sleep.h` and, where required, its corresponding test shim;
- `ERROR=0`, `WARN=1`, `INFO=2`, `DEBUG=3`; host output tests pin `"EWID"` mapping;
- BG22 sets `OD_CAP_LOG=0` and defines no `od_hal_log_*` function;
- shared session/watchdog/TXQ function signatures retain their explicit `now_ms` parameters, and
  no shared policy source calls `od_hal_uptime_ms()` — the logger is its only shared caller;
- `shared/hal/od_hal_time.h` declares exactly two functions; a bounded sleep appearing there is
  the §2.3 exclusion being reversed without its own decision;
- no target defines `od_uptime_get_32` or `od_busy_wait` after Phase 1;
- the Nordic adapter contains neither a `%.*s` log submission nor a `const char *` cast of the
  transient shared record;
- BG22's time adapter uses `sl_sleeptimer_get_tick_count64()` and converts before narrowing, while
  direct BG22 timing call sites remain explicitly outside this phase;
- no `room`, `may_wait`, ready-hook, loop-task setter, `[DROP:` or shared logging lock returns;
- no obvious side-effecting function call in a shared log argument.

---

## 7. Stop conditions

Stop and revise the current phase if:

- the standalone Phase 1 target build or BG22 known-interval clock gate does not pass;
- a target needs more than one HAL write to carry one shared emission;
- Nordic stops using Zephyr's logging transport or system logs become indistinguishable;
- a HAL write retains the shared stack pointer after returning;
- Nordic cannot prove that deferred logging copied the transient record synchronously;
- target serialization requires a shared RTOS lock or vendor header;
- flush loses the bounded 5 ms settlement or rounds a positive delay to zero;
- a bounded sleep is added to `od_hal_time` without separately resolving delay naming, signedness,
  zero and tick-rounding semantics;
- an existing explicit `now_ms` policy parameter is removed;
- BG22 uptime wraps in the hardware tick domain, narrows before conversion, or asserts/logs when
  conversion is unavailable before sleeptimer initialization;
- BG22 gains a log-HAL implementation, logger state, output change or unexplained logging-only size
  delta relative to the post-time-HAL baseline;
- a target's truncation boundary changes without a separate recorded decision; or
- logging is introduced in an ISR or while holding a lock across the target write.

---

## 8. Definition of done

1. `shared/core/od_log.c` and `shared/core/od_log.h` are the only logger implementation/API.
2. ESP32 and Nordic implement the five-function log HAL; BG22 implements none of it.
   All three implement `od_hal_time.h`'s two functions.
3. Every shared emission reaches its target transport in one call.
4. Uptime, cycle, truncation, CRLF and raw behavior are pinned by deterministic host fixtures.
5. Hardware captures match after normalizing only uptime/cycle fields.
6. Flush settlement remains target-owned and measured. `od_hal_time` declares exactly
   `od_hal_uptime_ms()` and `od_hal_delay_us()`, and its header carries both §2.3.1 comments.
7. The standalone time-HAL target build, BG22 known-interval clock check and available-target timing
   rows are recorded before logging promotion begins.
8. Existing shared policy APIs retain explicit `now_ms` arguments.
9. BG22 passes the `-O0` no-HAL host proof and target zero-state/size/output gate.
10. `tools/check.sh --targets` reports no failure and no skip.

The later ~900-line seam collapse remains separate. This plan lands the logging keystone and its
falsifiable boundaries; it does not bundle the consumers that become promotable afterwards.
