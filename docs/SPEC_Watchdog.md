# Specification — OpenDisplay nRF52840 Hardware Watchdog

**Status:** descriptive specification of the implementation shipping in the canonical `Firmware`
repo (PlatformIO / Arduino / Adafruit nRF52 core + SoftDevice S140 v7.3.0). Written so a port can
be produced without opening the original source.

**Source of truth for this document**

| Artifact | Path in `Firmware` |
|---|---|
| Portable interface | `src/watchdog.h` (101 lines) |
| nRF implementation | `src/watchdog_nrf.cpp` (413 lines) |
| ESP32 stub | `src/watchdog_esp32.cpp` (82 lines) |
| Design plan | `docs/PLAN_NRF_HARDWARE_WATCHDOG_2026-08-01.md` (rev 2, plus a 2026-08-03 status update) |
| Review findings | `docs/CODE_REVIEW_2026-08-04.md` — H-2, H-3 (and H-4 for the ESP32 gap) |
| Build knob | `platformio.ini`, `[env:nrf52840custom]` |

> Note on locating the plan: at the time of writing, `docs/PLAN_NRF_HARDWARE_WATCHDOG_2026-08-01.md`
> is present only in the repo's `.claude/worktrees/*` checkouts, not at the tip of the main
> worktree. Content cited here is from
> `Firmware/.claude/worktrees/docs-mbedtls-psram-findings/docs/PLAN_NRF_HARDWARE_WATCHDOG_2026-08-01.md`.

---

## 1. Purpose and scope

The watchdog exists to recover the device when an **unbounded wait** wedges it permanently —
either in `loop()`, or below it in a vendored driver the firmware cannot instrument. Three known
fault sources motivated it:

1. `Wire_nRF52.cpp:166-181` / `:230-247` spin on TWIM events with no deadline and no yield
   (previously accepted as residual **D-L** of the bound-waits plan).
2. `nrfx_spim.c:598` blocks in `while (!nrf_spim_event_check(p_spim, NRF_SPIM_EVENT_END)){}`
   with no timeout and no yield, because `SPIClass` initialises nrfx with a NULL handler
   (`SPI.cpp:101`). On nRF the Arduino API drives the panel **one byte at a time**
   (`arduino_io.inl:214-221`), so the firmware enters that spin roughly 96,000 times per full
   refresh.
3. The scheduler-starving fault class in general: if the scheduler stalls, every `millis()`-based
   bound fails together. A hardware watchdog is what makes that class recoverable.

**The watchdog does not fix any of those waits.** It converts a permanent hang into a periodic,
observable reset. The device still drops its BLE link, loses transfer state, and pays a cold
bring-up.

**Hardware watchdog arming is nRF-only.** The *module* is portable; the ESP32 build links a stub
(§12).

---

## 2. Module shape

```
src/watchdog.h          portable. No nrf_wdt.h, no esp_task_wdt.h — any TU may include it.
src/watchdog_nrf.cpp    entire file inside #ifdef TARGET_NRF   — the real implementation
src/watchdog_esp32.cpp  entire file inside #ifdef TARGET_ESP32 — stubs
```

Each implementation file is gated whole-file on its target, so the other compiles to an **empty
translation unit** and no `build_src_filter` change is needed across CI environments. This mirrors
the `BleTransport` pattern (`src/ble_transport.h`).

The interface is **free functions, not a class**: unlike `BleTransport` there is no state worth
exposing, and every caller wants exactly one global watchdog.

### 2.1 Why the interface is portable when the watchdog is not

Only nRF arms a hardware watchdog. But **every feed site and every breadcrumb stamp lives in code
that compiles for both targets** — `loop()` and `idleDelay()` in `main.cpp` ("One loop body for
both targets"), and `waitforrefresh()` plus the `bb_epaper` entry points in `display_service.cpp`.
An nRF-only API would require `#ifdef TARGET_NRF` around roughly 20 call sites in shared files,
which is precisely what `PLAN_UNIFY_NRF_ESP32_LOOP_BLE_2026-07-27.md` worked to remove. A no-op on
ESP32 costs nothing and keeps the shared body clean.

Bonus: the change is a net `#ifdef` **reduction**. ESP32's reset-reason decode previously lived
inline in `main.cpp` under `#ifdef TARGET_ESP32`; it moved into `watchdog_esp32.cpp`.

---

## 3. Public interface

```c
void odWatchdogBootInit(void);
bool odWatchdogInSafeMode(void);
void odWatchdogArm(void);
void odWatchdogFeed(void);
void odWatchdogBreadcrumb(uint8_t phase);
```

### 3.1 Contracts

| Function | Contract |
|---|---|
| `odWatchdogBootInit()` | Decode and log why we booted, and evaluate the consecutive-reset strike counter. **Call ONCE, early in `setup()`, after `od_log_init()`** so the line is actually emitted. **Must run before `odWatchdogArm()`.** Also stamps the initial `OD_WDT_PHASE_IDLE` breadcrumb as its last act. |
| `odWatchdogInSafeMode()` | True when the strike counter tripped and this boot must skip all panel work. Pure accessor over a static bool set by `BootInit()`; safe to call any number of times. |
| `odWatchdogArm()` | Arm the hardware watchdog. **Irreversible on nRF.** Call ONCE, after `odWatchdogBootInit()` and before the boot panel path. No-op where no hardware watchdog is used. |
| `odWatchdogFeed()` | Prove forward progress. Cheap enough for a tight poll loop; a no-op when nothing is armed. Also drives the healthy-uptime strike clear (§8.3). |
| `odWatchdogBreadcrumb(uint8_t phase)` | Record the panel phase we are about to enter. Retained across reset. Repeat suppression is internal, so per-frame call sites are cheap. |

### 3.2 Ordering requirement

```
odWatchdogBootInit()          (once, early in setup(), after logging is up)
        ↓
odWatchdogArm()               (once, immediately before the boot panel path)
        ↓
odWatchdogFeed() / odWatchdogBreadcrumb()   (many, for the life of the program)
```

`odWatchdogInSafeMode()` is only meaningful after `BootInit()`; before it, the underlying static is
`false`.

### 3.3 Feed-site rule (normative)

From `watchdog.h`:

> **FEED ONLY FROM SITES WHOSE EXECUTION PROVES THE PROGRAM IS ALIVE** — `loop()`, cooperative
> waits that return, and immediately before entering a bounded library call. **NEVER from an ISR,
> timer or stack callback**: an interrupt-fed watchdog verifies that the interrupt controller is
> running, not that the program is, which is the classic way to build a watchdog that never fires.

The plan states the same rule as non-negotiable, and adds a second exclusion: the `nrfx_spim` and
`Wire` spins are **deliberately** not fed even if they were reachable — *those spins are the fault*.

The plan's corrected property statement (rev 1's was false): **every span we can reach is fed at
its boundary; spans we cannot reach must individually fit inside the timeout.**

---

## 4. What is deliberately absent from the interface

`watchdog.h` names three omissions and their reasons:

| Omitted | Why |
|---|---|
| `stop()` / `disable()` | **The nRF52840 WDT cannot be stopped once started.** Its register block has no `TASKS_STOP` and no `ENABLE` (contrast SPIM/TWI/UART, which have `ENABLE` at offset `0x500`); only a system reset clears it. Exposing a `stop()` would advertise a capability one target cannot honour. |
| runtime timeout parameter | `CRV` must be written **before** the start task and is latched thereafter. Compile-time only, via `OPENDISPLAY_NRF_WDT_S`. |
| task registration | ESP32's task watchdog is per-task subscribe/unsubscribe; nRF's reload registers are not an analogue. The contract here is **ONE LOOP TASK, ONE WATCHDOG**. |

---

## 5. Retained state — GPREGRET2

### 5.1 Why not GPREGRET (id 0)

`GPREGRET` (id 0) is **already taken**: `device_control.cpp:868-869` uses it for the DFU handshake
(writes `0xB1`). `GPREGRET2` (id 1) is free, and is retained across a watchdog or soft reset —
cleared only by power-on/brownout.

### 5.2 Bit allocation

`GPREGRET2` is 8 bits and must carry two independent things, so the layout is explicit:

```
    bit  7 6 | 5 4 | 3 2 1 0
         tag | cnt | phase
```

| Bits | Mask | Field |
|---|---|---|
| 7:6 | `0xC0` | **Validity tag, always `0b10`** (byte value `0x80`). Distinguishes a value we wrote from cold-boot garbage or another writer; a bad tag means **discard**. |
| 5:4 | `0x30` | Consecutive-DOG **strike counter**, 0–3, saturating. Shift = 4. |
| 3:0 | `0x0F` | Breadcrumb **phase**, an `OdWatchdogPhase` value. |

```c
#define OD_WDT_G2_TAG_MASK   0xC0u
#define OD_WDT_G2_TAG_VALUE  0x80u   /* 0b10 << 6 */
#define OD_WDT_G2_CNT_MASK   0x30u
#define OD_WDT_G2_CNT_SHIFT  4
#define OD_WDT_G2_PHASE_MASK 0x0Fu
```

**Without this allocation a breadcrumb write would destroy the strike counter** — that was one of
four faults in the plan's rev 1 containment design.

**Known risk (plan V6):** GPREGRET2 is unused by application and framework source, but the
**installed bootloader image was not inspected** (it is a flashed binary, absent from the sources).
If the bootloader writes it, the validity tag causes a stale value to be *discarded* rather than
misread — the counter resets, degrading containment but not causing a wrong action. Test T7 was to
settle this on hardware.

### 5.3 Read-modify-write helper

`g2UpdateField(mask, value)`:

1. Read the byte. On read failure, return failure.
2. If `(cur & 0xC0) != 0x80`, treat the byte as stale/garbage and **reinitialise** `cur = 0x80`
   (tag only, counter and phase zero).
3. Write `(cur & ~mask) | (value & mask)`.

---

## 6. The two GPREGRET2 access paths

This is the single most portability-relevant detail in the module.

### 6.1 The SVC availability rule

`sd_power_gpregret_get`, `sd_power_gpregret_set` and `sd_power_gpregret_clr` are numbered from
`SOC_SVC_BASE_NOT_AVAILABLE` (`nrf_soc.h:65,164-166`) — the block of *"SVCs that are not available
when the SoftDevice is disabled"*.

`odWatchdogBootInit()` runs early in `setup()`, whereas `ble.begin()` enables the SoftDevice much
later. **At boot those SVCs cannot be used.**

- **SoftDevice DISABLED** — `POWER` is not a protected peripheral, so `NRF_POWER->GPREGRET2` is
  directly readable and writable. This is also *cheaper and atomic*: one store, versus a
  get + clr + set SVC sequence.
- **SoftDevice ENABLED** — `POWER` is protected and the SVCs are **mandatory**.

`sd_softdevice_is_enabled()` is numbered from `SDM_SVC_BASE` (`nrf_sdm.h:89,195`), which **is**
available either way, so it is a safe discriminator.

### 6.2 Implementation

```c
static bool sdEnabled(void) {
    uint8_t on = 0;
    if (sd_softdevice_is_enabled(&on) != NRF_SUCCESS) return false;
    return on != 0;
}

static bool g2Read(uint8_t* out) {
    if (!sdEnabled()) { *out = (uint8_t)(NRF_POWER->GPREGRET2 & 0xFFu); return true; }
    uint32_t v = 0;
    if (sd_power_gpregret_get(1, &v) != NRF_SUCCESS) return false;
    *out = (uint8_t)(v & 0xFFu);
    return true;
}

static bool g2Write(uint8_t v) {
    if (!sdEnabled()) { NRF_POWER->GPREGRET2 = v; return true; }   /* single atomic store */
    /* The SoftDevice offers no store, only masked clear and set. */
    uint32_t rc = sd_power_gpregret_clr(1, 0xFFu);
    if (rc != NRF_SUCCESS) return false;
    if (v) rc = sd_power_gpregret_set(1, v);
    return rc == NRF_SUCCESS;
}
```

Note the SVC id argument is `1` (GPREGRET2), and the SVC write path is a **clear-all-then-set**,
which is not atomic. It is safe here only because **only the loop task writes it** — see the
feed-site rule (§3.3) forbidding ISR/callback writes.

**Both accessors report failure rather than swallowing it**: a silently dead breadcrumb would be
worse than no breadcrumb, because the boot log would still print a (stale or zero) phase and invite
a wrong conclusion.

---

## 7. Reset-reason decode

### 7.1 The critical constraint — do not read the peripheral

> **CRITICAL: do NOT read `NRF_POWER->RESETREAS`, and do NOT call `sd_power_reset_reason_get()`.**

The Adafruit nRF52 Arduino core has **already read and cleared** the register before `setup()` runs
(`cores/nRF5/wiring.c:37-40`):

```c
_reset_reason = NRF_POWER->RESETREAS;
NRF_POWER->RESETREAS |= NRF_POWER->RESETREAS;   /* write-1-to-clear */
```

and exposes the saved word through `readResetReason()` (`wiring.h:32`). **Reading the peripheral
ourselves returns zero on every boot**, which would silently report every watchdog reset as a
power-on — defeating the entire point of the module. (This was blocking error #1 found in the
plan's adversarial review; rev 1's SoftDevice-API design would have read zero forever.)

A second consequence: the bits **do not accumulate across boots** — the core clears them each time,
so no clearing is needed or possible on our side.

### 7.2 The decode

`RESETREAS` is a **bitfield, not an enum**: several causes can be latched at once. The
implementation prints **every** set bit, not the first match.

All **nine** nRF52840 causes are listed, in this order:

| Mask symbol | Printed name |
|---|---|
| `POWER_RESETREAS_RESETPIN_Msk` | `RESETPIN` |
| `POWER_RESETREAS_DOG_Msk` | `DOG` |
| `POWER_RESETREAS_SREQ_Msk` | `SREQ` |
| `POWER_RESETREAS_LOCKUP_Msk` | `LOCKUP` |
| `POWER_RESETREAS_OFF_Msk` | `OFF` |
| `POWER_RESETREAS_LPCOMP_Msk` | `LPCOMP` |
| `POWER_RESETREAS_DIF_Msk` | `DIF` |
| `POWER_RESETREAS_NFC_Msk` | `NFC` |
| `POWER_RESETREAS_VBUS_Msk` | `VBUS` |

> The source comment says "ALL eight nRF52840 causes are listed" while the table actually contains
> nine entries. The table is the authority; the count in the comment is stale.

**The `r == 0` rule.** A cold start latches nothing, so:

```c
if (r == 0) { od_log_info("[WDT] reset reason: POWERON (0x00000000)"); return; }
```

This is the **only** power-on signature. An earlier revision omitted `VBUS`, `NFC` and `LPCOMP`,
which made any of them print as `POWERON` because the fallback keyed on *"nothing matched"* rather
than on `r == 0`. The current code additionally appends `UNKNOWN` when `r` has bits set that no
listed mask covers — *nonzero with no recognised bit is a real anomaly, not a power-on.*

Formatting details worth reproducing: names are joined with `|` into a 96-byte buffer; `snprintf`
returns the length it *would* have written, so `n` is clamped (`if (n >= sizeof(buf) - 1) { n = sizeof(buf) - 1; break; }`)
so a truncating write cannot push the offset past the buffer. Output line:

```
[WDT] reset reason: DOG|SREQ (0x00000006)
```

---

## 8. Boot init, strike counter and safe mode

### 8.1 Why the strike counter exists

> A watchdog that resets a device which wedges during **BOOT** turns one hang into an endless reset
> cycle: the device never advertises, is unreachable over BLE/DFU, and flattens its battery faster
> than if it had simply hung. That is strictly worse than the bug it is meant to fix.

Since `odWatchdogArm()` is called **before** the boot panel path, boot wedges *are* covered — which
is exactly what makes strikes able to accumulate, and therefore what makes the escape hatch
meaningful.

### 8.2 Constants

```c
#define OD_WDT_SAFE_MODE_STRIKES 3u
#define OD_WDT_HEALTHY_MS        (10UL * 60UL * 1000UL)   /* 600000 ms; >= 2x the timeout */
```

Module statics: `s_safeMode`, `s_strikesToClear`, `s_bootMs`, `s_resetReason`.

### 8.3 `odWatchdogBootInit()` algorithm

1. `s_bootMs = millis();`
2. `s_resetReason = readResetReason();` then `logResetReason(s_resetReason)`.
3. `wasDog = (s_resetReason & POWER_RESETREAS_DOG_Msk) != 0;` `strikes = 0`.
4. Read GPREGRET2:
   - read failure → `od_log_warn("[WDT] GPREGRET2 unreadable - breadcrumb and strike count unavailable")`;
   - tag valid → extract `strikes` and `phase`, log
     `"[WDT] breadcrumb from previous run: phase=%s (%u) strikes=%u"`;
   - tag invalid → log `"[WDT] no retained breadcrumb (cold start or first boot)"` and write the
     bare tag `0x80`; if that write fails,
     `od_log_warn("[WDT] GPREGRET2 unwritable - breadcrumbs disabled")`.
5. Strike update:
   - `wasDog` → saturating increment (`if (strikes < 3) strikes++`), persist, and
     `od_log_warn("[WDT] previous boot ended in a watchdog reset (strike %u/%u)")`.
   - else if `strikes != 0` → **reset to 0** and persist. *"Any non-DOG reset means the fast-reset
     cycle was broken by something else (power cycle, pin reset, deliberate reboot). Start clean."*
6. `s_safeMode = (strikes >= OD_WDT_SAFE_MODE_STRIKES)`. On entry, log at error level:
   `"[WDT] SAFE MODE: %u consecutive watchdog resets - skipping ALL panel work this boot so the
   device stays reachable over BLE/DFU. Clears after %lu s of healthy uptime."`
7. `s_strikesToClear = (strikes != 0);`
8. `odWatchdogBreadcrumb(OD_WDT_PHASE_IDLE);` — establishes a known phase and keeps the breadcrumb
   cache honest (§10).

`strikesSet(n)` clamps `n` to 3 and writes bits 5:4 via `g2UpdateField`.

### 8.4 The uptime-based clear rule, and why it is not tied to a refresh

`strikesClearIfHealthy()` runs from **every** `odWatchdogFeed()`:

```c
if (!s_strikesToClear) return;
if ((uint32_t)(millis() - s_bootMs) < OD_WDT_HEALTHY_MS) return;
s_strikesToClear = false;
strikesSet(0);
od_log_info("[WDT] %lu s of healthy uptime - strike counter cleared", ...);
```

The reasoning, stated in the source:

- **Clearing on "first successful refresh" would never accumulate**, because every ordinary boot
  performs a successful boot refresh and would wipe the previous strike. The count could never
  reach 3.
- **It would also make safe mode permanent**, because safe mode performs no refresh and so could
  never satisfy the clear condition.

An uptime rule solves both at once and is **panel-independent**, so it behaves identically in safe
mode. *A device that survives ten minutes between wedges is not boot-looping and should keep
retrying the panel.*

**Safe mode is therefore self-exiting.** After 10 healthy minutes the counter clears, so the next
reset boots normally and retries the panel. Worst case is a bounded oscillation — 3 fast resets, a
long safe-mode period, one retry — rather than a permanent brick.

### 8.5 What safe mode actually skips

Three gates, all reading `odWatchdogInSafeMode()`:

1. **`main.cpp:230`** — `setup()` skips `initDisplay()` entirely; logs
   `"[WDT] safe mode - skipping initDisplay()"` and sets `rebootFlag = 1`. BLE still comes up
   below, *"which is the whole point — a device in safe mode stays reachable for a config change or
   a DFU instead of being bricked until someone pulls the battery."*
2. **`display_service.cpp:387`** — `epdSessionAcquire()` refuses and returns `false`. Skipping
   `initDisplay()` at boot is **not sufficient on its own**: a client can connect and push an image,
   and that reaches `epdSessionAcquire()` directly. Refusing leaves the session in `PWR_OFF`, so the
   transfer fails through the existing refresh-failure path rather than through new error handling.
   It is reported **late and generically**; a clean immediate NACK would need a "device in safe mode"
   code originating in `../opendisplay-protocol`, which was out of scope. **See defect H-2 (§13.1).**
3. **`main.cpp:1278`** — `pwrmgm()` refuses rail **power-up** only:
   `if (onoff && odWatchdogInSafeMode()) { warn; return; }`. Powering *down* is still allowed, so a
   rail left on by a pre-safe-mode boot can still be shut off.

**Not surfaced on the wire.** Safe mode is reported via the boot log only. Status bit 3 in
`include/opendisplay_structs.h` is reserved and "must be 0", and that header is vendored
byte-for-byte from `opendisplay-protocol`, so surfacing safe mode over BLE must originate in the
canonical protocol repo. Explicitly out of scope.

---

## 9. Arming the watchdog

### 9.1 The build knob

```c
#ifndef OPENDISPLAY_NRF_WDT_S
#define OPENDISPLAY_NRF_WDT_S 300
#endif

#if OPENDISPLAY_NRF_WDT_S != 0
static_assert(OPENDISPLAY_NRF_WDT_S >= 60 && OPENDISPLAY_NRF_WDT_S <= 3600,
              "OPENDISPLAY_NRF_WDT_S must be 0 (disabled) or 60..3600 seconds");
#endif
```

Bounds, as documented in source:

- **Lower bound** — must exceed every legitimate blocking span in the firmware; the largest is a
  **~240 s `bbepRefresh()`** on a 7-colour split-buffer panel (plan **V12**).
- **Upper bound** — `CRV` is 32 bits at 32768 Hz, so ~131072 s is representable; **3600 s** keeps a
  wide margin and rejects a mistyped value that would silently disable recovery for hours.

**The header default is 300 s; the shipped build sets 120 s.** `platformio.ini`
`[env:nrf52840custom]` carries `-DOPENDISPLAY_NRF_WDT_S=120`, and the three other nRF envs inherit
it via `${env:nrf52840custom.build_flags}`. The flag's comment is explicit that this is a known gap:

> KNOWN GAP: 120 s is BELOW the single longest span the firmware cannot instrument — a
> `REFRESH_FULL` on a 7-colour split-buffer panel sends the init sequence to BOTH controllers, and
> each of its 4 `BUSY_WAIT` entries can take 30 s, i.e. ~240 s inside one `bbepRefresh()` call. The
> per-entry-point feed in `display_service.cpp` still bounds the dog to that one call rather than
> that call plus everything preceding it, but at 120 s a HEALTHY refresh on that panel class will
> trip the watchdog mid-refresh. Chosen anyway on 2026-08-03; re-check before shipping to any
> 7-colour split-buffer panel.

### 9.2 The 300 s vs 120 s derivation

The plan's blocking-span inventory (§3.1):

| Span | Worst case | Feed possible inside? |
|---|---|---|
| `bbepRefresh(REFRESH_FULL)` on a 7-colour split-buffer panel | **~240 s** | **No** — one libdep call |
| `waitforrefresh(60)` | ~126 s — 6000 iterations × ~21 ms (`delay(10)` + `bbepIsBusy`'s own `delay(10)`+`delay(1)`). The argument is **not** seconds | **Yes** |
| `bbepSendCMDSequence` (init only) | N × 5 s (B/W) or N × 30 s (multicolour) | No — libdep |
| `pwrmgm(true)` | 900 ms | Yes |
| `nrfx_spim` / `Wire` spins | **unbounded** (the fault) | No — and deliberately not |

The 240 s figure (plan **V12**) is derived: the 8.1" Spectra is `BBEP_SPLIT_BUFFER | BBEP_7COLOR`;
its init list holds **4** `BUSY_WAIT` entries (`bb_ep.inl:3704-3726`), `REFRESH_FULL` sends the
whole sequence to CS1 **and again** to CS2 (`:4373-4380`), and the per-wait cap for 3/4/7-colour is
**30 s** (`:3967-3969`). 4 × 2 × 30 s = **240 s inside one `bbepRefresh()` call**.

**Decision D-1 confirmed 300 s**, made safe not by the timeout alone but by the feed policy: feed
immediately before every call that enters `bb_epaper`'s blocking region. Because the 240 s is a
single uninterruptible call, feeding on entry means the watchdog faces exactly that span and
nothing else — longest uncovered span 240 s, margin 60 s (**1.25×**). The plan flags that margin as
thin and records it as a residual.

**The 2026-08-03 status update** dropped the shipped value to **120 s** as a deliberate choice made
outside the plan's analysis. Its consequence is stated plainly: *"the margin this plan relies on is
gone. At 120 s, a healthy `REFRESH_FULL` on the 7-colour split-buffer panel will trip the watchdog
mid-refresh on a device that isn't wedged."* Do not ship 120 s to a 7-colour split-buffer panel
without re-deriving it. **See defect H-3 (§13.2).**

### 9.3 `odWatchdogArm()` — the inherit-detection branch

**Inherit detection runs on EVERY build, including `OPENDISPLAY_NRF_WDT_S=0`.**

```c
if (nrf_wdt_started(NRF_WDT)) {
    od_log_warn("[WDT] ALREADY RUNNING at boot (not started by this call). "
                "CRV=%lu (%lus) RREN=0x%lX CONFIG=0x%lX - cannot be stopped or "
                "reconfigured; feeding it as-is.",
                NRF_WDT->CRV, (NRF_WDT->CRV + 1UL) / 32768UL, NRF_WDT->RREN, NRF_WDT->CONFIG);
    s_inherited = true;
    odWatchdogFeed();
    return;
}
```

Why this is unconditional, quoting the source:

> Whether a running nRF52840 WDT survives a non-power-on reset (soft reset, DOG reset, pin reset)
> is **NOT established** by any source available in this workspace, and the two possibilities have
> very different consequences:
>
> - If it does **NOT** survive, this branch simply never fires and costs a single register read at
>   boot.
> - If it **DOES** survive, then a build with the watchdog disabled — reached by DFU, a reflash, or
>   `NVIC_SystemReset` from a build that had it enabled — would inherit a live watchdog it never
>   feeds, and reset forever until someone physically removes power. **That is a brick.**
>
> Feeding whatever we find is correct under both, so do that rather than pick.

The `RUNSTATUS` log at boot was intended to settle the question empirically (test T7). **This
document does not claim it has been settled.**

A related consequence, also unverified: if the *bootloader* left the WDT running, our configuration
is silently discarded and we inherit its timeout — possibly far shorter than ours — while believing
we set our own. Hence "log loudly and skip configuration" rather than pretend.

### 9.4 The arm sequence

`OPENDISPLAY_NRF_WDT_S == 0` path: log `"[WDT] disabled at build time (OPENDISPLAY_NRF_WDT_S=0)"`
and return (inherit detection above still ran).

Otherwise:

```c
/* Order is load-bearing: CRV, RREN and CONFIG all latch at TASKS_START. */
nrf_wdt_reload_value_set(NRF_WDT, ((uint32_t)OPENDISPLAY_NRF_WDT_S * 32768UL) - 1UL);
nrf_wdt_reload_request_enable(NRF_WDT, NRF_WDT_RR0);          /* one reload register, one feeder, one task */
nrf_wdt_behaviour_set(NRF_WDT, NRF_WDT_BEHAVIOUR_RUN_SLEEP);

s_armed = true;
odWatchdogFeed();                                             /* start the first period from a known state */
nrf_wdt_task_trigger(NRF_WDT, NRF_WDT_TASK_START);            /* irreversible */
od_log_info("[WDT] armed: %us", OPENDISPLAY_NRF_WDT_S);
```

| Step | Value / effect |
|---|---|
| `CRV` | `(OPENDISPLAY_NRF_WDT_S × 32768) − 1`. At 300 s: `9,830,399` (`0x0095FFFF`). At the shipped 120 s: `3,932,159` (`0x003BFFFF`). |
| `RREN` | `RR0` only. |
| `CONFIG.SLEEP` | **1** (`NRF_WDT_BEHAVIOUR_RUN_SLEEP`). `WDT_CONFIG_SLEEP_Pos = 0`. |
| `CONFIG.HALT` | **0** — implied by choosing `RUN_SLEEP` rather than `RUN_SLEEP_HALT`. `WDT_CONFIG_HALT_Pos = 3`. |

**`RUN_SLEEP` reasoning, verbatim from source:** *"keep counting while the CPU sleeps (idleDelay's
`delay()` chunks), but pause while halted by a debugger, so a breakpoint is not a reset."* Without
`SLEEP=1`, a device that hangs while idle is never recovered; with `HALT=0`, the watchdog is safe in
the debug envs (decision D-4) and a >5 min breakpoint does not reset.

The clock is LFCLK, which the Arduino core has **already started** before `setup()`
(`wiring.c:45-57`, `TASKS_LFCLKSTART = 1`). Arm ordering is therefore a policy choice, not a clock
constraint. The HAL used is `hal/nrf_wdt.h` — header-only inline functions already on the include
path; the nrfx *driver* (`nrfx_wdt.c`) is **not compiled** in this framework package, and
`NRFX_WDT_ENABLED` is absent from `nrfx_config.h`.

---

## 10. `odWatchdogFeed()`

```c
void odWatchdogFeed(void) {
    strikesClearIfHealthy();
#if OPENDISPLAY_NRF_WDT_S != 0
    if (!s_armed && !s_inherited) return;
#else
    if (!s_inherited) return;   /* nothing of ours is armed; only feed an inherited dog */
#endif
    for (uint8_t i = 0; i <= (uint8_t)NRF_WDT_RR7; i++) {
        nrf_wdt_rr_register_t rr = (nrf_wdt_rr_register_t)i;
        if (nrf_wdt_reload_request_is_enabled(NRF_WDT, rr)) {
            nrf_wdt_reload_request_set(NRF_WDT, rr);
        }
    }
}
```

Two points a port must preserve:

**Why it walks all eight reload registers instead of assuming RR0.** *"Every ENABLED reload register
must be written before the counter reloads — RREN is an **AND**, not an **OR**. Our own arm path
enables RR0 alone, but an **INHERITED** watchdog (one the bootloader started) may have any subset
enabled, and feeding only RR0 would then never reload it. Walk RREN instead of assuming."*

**The magic value.** `nrf_wdt_reload_request_set()` writes **`NRF_WDT_RR_VALUE = 0x6E524635`** into
the selected `RR[i]` register. *"the magic value is why a wild pointer or stray memset cannot
accidentally pet the dog."*

Note also that `strikesClearIfHealthy()` runs **before** the early-return guards, so the strike
counter clears on healthy uptime even in a build with `OPENDISPLAY_NRF_WDT_S=0` and no inherited
watchdog.

---

## 11. `odWatchdogBreadcrumb()`

### 11.1 The phase enum — all 16 values

Values must fit **4 bits (0–15)**, matching the GPREGRET2 field. All 16 are in use;
`OD_WDT_PHASE__MAX = 15`. **There is no spare phase value.**

| # | Name | Meaning |
|---|---|---|
| 0 | `OD_WDT_PHASE_IDLE` | Pre-session: stamped once at boot, before the first `epdSessionAcquire`/`Release`/`ForceOff`. |
| 1 | `OD_WDT_PHASE_ACQUIRE_COLD` | Cold panel bring-up entered (rail was off). |
| 2 | `OD_WDT_PHASE_ACQUIRE_WARM` | Warm re-acquire (or defensive already-ACTIVE re-entry). |
| 3 | `OD_WDT_PHASE_INIT_SEQ` | Controller init: `bbepInitIO` + `bbepWakeUp` + `bbepSendCMDSequence`. |
| 4 | `OD_WDT_PHASE_FILL` | `bbepFill` of both planes (sub-rect partial prep). |
| 5 | `OD_WDT_PHASE_STREAM` | Image bytes being pushed to the controller. |
| 6 | `OD_WDT_PHASE_REFRESH_WAIT` | Inside `waitforrefresh()`'s BUSY poll. |
| 7 | `OD_WDT_PHASE_RELEASE` | `epdSessionRelease()` entered. |
| 8 | `OD_WDT_PHASE_FORCE_OFF` | `epdSessionForceOffLocked()` teardown entered. |
| 9 | `OD_WDT_PHASE_BOOT_REFRESH` | Boot-screen `bbepRefresh(REFRESH_FULL)`. |
| 10 | `OD_WDT_PHASE_IDLE_OFF` | Session fully powered down (`PWR_OFF`). |
| 11 | `OD_WDT_PHASE_IDLE_WARM` | Panel kept awake for keep-alive (`PWR_WARM`). |
| 12 | `OD_WDT_PHASE_PWRMGM_AXP2101` | Before `initAXP2101()` (I2C PMIC bring-up). |
| 13 | `OD_WDT_PHASE_PWRMGM_RAIL` | Before `pwr_pin` HIGH + `delay(800)`. |
| 14 | `OD_WDT_PHASE_PWRMGM_PINS` | Before panel GPIO setup + `delay(100/200)`. |
| 15 | `OD_WDT_PHASE_PWRMGM_WIRE` | Before `initOrRestoreWireForOpenDisplay()`. |

**Why 10 and 11 are distinct from 0.** `pwrmgmState` is plain RAM, not retained across a reset, so
without a distinct phase per idle sub-state a freeze during either one reports the same generic
`OD_WDT_PHASE_IDLE` and the two are indistinguishable after the fact.

**Why 12–15 exist.** `pwrmgm(true)`'s rail bring-up was uninstrumented until the **2026-08-03
freeze**: a reset landed ~120 s after `ACQUIRE_COLD` and never reached `INIT_SEQ` — the wedge was
somewhere inside `pwrmgm()` itself. These four name which of its sub-steps was entered last, and
they consume the last 4 of the 16 available values.

A parallel name table `phaseName()` in `watchdog_nrf.cpp` maps each value to a string for the boot
log (`"UNKNOWN"` in `default:`). It is kept as a plain table, the same pattern as
`logResetReason()`'s `kinds[]`, so a new phase is one line here rather than a guess at the call
site.

### 11.2 The write path

```c
void odWatchdogBreadcrumb(uint8_t phase) {
    static uint8_t s_lastPhase = 0xFF;
    static bool s_failLogged = false;
    phase &= OD_WDT_G2_PHASE_MASK;
    if (phase == s_lastPhase) return;
    if (!g2UpdateField(OD_WDT_G2_PHASE_MASK, phase)) {
        if (!s_failLogged) {
            s_failLogged = true;
            od_log_warn("[WDT] GPREGRET2 write failed - breadcrumb may be stale");
        }
        return;
    }
    s_lastPhase = phase;
}
```

**The phase cache (`s_lastPhase`).** Skip the register work when the phase has not actually
changed. *"A stamp costs up to three SVCs once the SoftDevice is enabled (get + clr + set), so
without this the streaming stamps — which sit on per-frame and per-row paths — would be far too
expensive to place where they are most useful."* Initial value `0xFF` can never equal a masked
phase, so the first call always writes.

**The cache is kept in sync with the register** by `odWatchdogBootInit()`, which stamps `IDLE`
explicitly after establishing the tag, *"so the cache never starts out lying."*

**The cache advances ONLY on success.** *"Updating it first would suppress the retry after a failed
write, leaving the cache claiming a phase the register never received — and a failed clr+set can
even leave the byte cleared."*

**The failure log is latched** (`s_failLogged`) because this function sits on per-frame paths; an
unlatched warn would flood the log.

### 11.3 Not implemented: the TIMEOUT ISR

Decision **D-3** is **deferred and NOT implemented**. The WDT `TIMEOUT` event fires ~61 µs
(2 LFCLK cycles) before the reset — enough to stamp a final breadcrumb, not enough to write flash or
drain a UART. It was dropped because phase-transition breadcrumbs are stamped **eagerly** at every
panel-phase entry, so the retained value is already correct when the reset lands; the ISR would add
only ~61 µs of redundancy. Revisit if a real failure shows a phase gap.

---

## 12. The ESP32 stub — what it does NOT do

`watchdog_esp32.cpp` implements the same five symbols under `#ifdef TARGET_ESP32`.

| Function | Behaviour |
|---|---|
| `odWatchdogBootInit()` | **Real work.** `esp_reset_reason()` decoded through `resetReasonName()` and logged as `"Reset reason: %s (%d)"`. Names handled: `ESP_RST_POWERON/EXT/SW/PANIC/INT_WDT/TASK_WDT/WDT/DEEPSLEEP/BROWNOUT/SDIO`, else `UNKNOWN`. This is the decode that used to live inline in `main.cpp`. |
| `odWatchdogInSafeMode()` | Returns `false`, unconditionally. |
| `odWatchdogArm()` | **Arms nothing.** Logs one accurate line (below). |
| `odWatchdogFeed()` | Empty body. |
| `odWatchdogBreadcrumb()` | Empty body; `(void)phase`. |

There is **no** retained breadcrumb, **no** strike counter, **no** safe mode, and **no** hardware
watchdog on ESP32.

The `Arm()` comment is deliberately precise, because the obvious wording ("no watchdog on this
target") is **false** and would mislead:

- The IDF task watchdog **is** enabled and initialised in the shipped sdkconfigs —
  `CONFIG_ESP_TASK_WDT_EN=y`, `CONFIG_ESP_TASK_WDT_INIT=y`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`.
- But nothing it watches would catch a wedged `loop()`:
  - S3 and classic ESP32 set `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y`, while
    `CONFIG_ARDUINO_RUNNING_CORE=1` puts `loopTask` on **CPU1**. A spin in `loop()` starves IDLE1,
    which nothing is subscribed to.
  - C3 and C6 initialise the task watchdog but subscribe **no idle task at all**.

Logged line:

```
[WDT] no firmware watchdog on ESP32; IDF TWDT is enabled but no task that would catch a wedged loop() is subscribed
```

Closing the gap is described as a one-liner — `esp_task_wdt_add(NULL)` on the loop task — and *the
feed sites it would need are already wired by this module*. It was deliberately not done: the
timeout would be bounded by different spans than nRF's (FastEPD refresh, WiFi/TLS handshakes) and
needs its own analysis. `CODE_REVIEW_2026-08-04.md` records this as finding **H-4**.

---

## 13. Known defects and limitations

### 13.1 H-2 (High) — safe mode can still enter panel operations

**Locations:** `src/display_service.cpp:488-525`, `:2291-2319`, `:3512-3540`.

`epdSessionAcquire()` returns `bool`, but `false` has **two meanings**: a successful *warm*
acquisition, and *refusal* because the watchdog safe-mode gate is active. Consumers cannot tell
them apart:

- `directWriteActivatePanel()` **ignores the return value** entirely.
- `partial_prepare_panel_ram()` uses it only to choose the word `cold` or `warm` in a log message,
  then proceeds into panel setup/fill calls regardless.

Consequence: after the watchdog reset threshold is reached, a peer can issue a new image `START`
and drive the same panel code safe mode intended to suppress — **potentially recreating the reset
loop indefinitely**.

**Recommended fix (from the review):** return a three-state result — `ACQUIRED_COLD`,
`ACQUIRED_WARM`, `REFUSED`. Every direct, partial and PIPE caller must propagate `REFUSED` into a
deterministic NACK and complete session cleanup without touching the panel. Add a host-testable
state-machine test covering the safe-mode branch.

Note the partial mitigation already present: `pwrmgm()`'s own safe-mode gate (`main.cpp:1278`)
refuses to raise the rail, so a caller that ignores the `false` still cannot power the panel up.
This bounds but does not close the finding.

### 13.2 H-3 (High) — the shipping timeout is below a documented healthy operation

**Locations:** `src/watchdog_nrf.cpp:303-310`, `src/display_service.cpp:3540-3544`,
`platformio.ini` nRF flags.

The shipping nRF timeout is **120 s**. Source comments document that a healthy seven-colour
`bbepRefresh()`/fill operation may block for **~240 s**. The code feeds immediately before the
blocking call, but **cannot feed while inside the third-party driver**. A healthy supported panel
can consequently trigger the watchdog.

Additionally: the compile-time assertion only requires **≥ 60 s**, contradicting the source comment
that the lower bound must exceed the longest legitimate operation.

**Recommended fix (from the review):** preferably make panel work interruptible, or add a driver
progress callback that feeds the watchdog only on demonstrated forward progress. Until that is
available, set the timeout above the measured worst-case operation with margin and make the
assertion **enforce** that value. Capture per-panel timing on cold batteries and low temperatures
before selecting the margin.

### 13.3 Other limitations recorded in the plan (§5)

- **No unbounded wait is fixed.** `nrfx_spim.c:598` and the six `Wire_nRF52` spins are untouched. A
  permanent hang becomes a periodic reset — better and observable, but the device still drops its
  link, loses transfer state, and pays a cold bring-up.
- **Recovery is slow, by choice.** Up to one timeout period dead before reset; with a threshold of
  3, up to ~3 timeout periods to reach safe mode. The plan judges this the right trade for e-paper,
  where five minutes late is invisible but resetting a healthy device mid-refresh is a visible
  regression.
- **A stuck BUSY pin does NOT trip the watchdog, and is not meant to.** `waitforrefresh` keeps
  feeding for its 6,000 iterations, returns `false`, and `loop()` resumes. *A failed refresh is an
  error to report, not a wedge to reset through.* Any test must distinguish "BUSY stuck" (no reset
  expected) from a non-returning call inside one iteration (reset expected).
- **DFU can take a `DOG` reset — accepted, out of scope.** `enterDFUMode()`
  (`device_control.cpp:868-884`) does `sd_softdevice_disable()`, moves the vector table, and calls
  `bootloader_util_app_start()` — it **jumps to the bootloader without a system reset**. Combined
  with "cannot be stopped", an armed WDT keeps counting into a bootloader that may not feed it. The
  expected outcome of a reset there is an interrupted transfer the host must retry (GPREGRET id 0
  still holds `0xB1`, so the reset re-enters the bootloader), **not** a brick — *unless* the reset
  lands mid-flash-write, which was not analysed. The plan also notes the residual **may not exist at
  all**, since many Nordic/Adafruit bootloaders feed the WDT in their main loop; the installed
  bootloader is a flashed binary absent from the sources, so this was never verified.
- **Says nothing about brownout.** POFCON is not enabled (`grep POFCON src/` → nothing).
- **Does not verify the panel rail power-cycles on reset** (descoped, was D-6).
- **The `bootdiag` env's `while (!Serial)` gate** sits far above the arm point and stays
  deliberately outside watchdog coverage.
- **Debug builds** additionally wait up to 2 s for a USB CDC host before `odWatchdogBootInit()`
  (`main.cpp:105-110`, `#if OD_LOG_LEVEL >= OD_LOG_DEBUG`), because USB re-enumerates from scratch
  on reset and the reset-reason/breadcrumb lines would otherwise race the host's reconnect and be
  silently discarded.

### 13.4 Explicitly unestablished

State these as unknown rather than assuming either way:

1. **Whether a running nRF52840 WDT survives a non-power-on reset** (soft / `DOG` / pin). Not
   established by any source in the original workspace. The implementation is written to be correct
   under both possibilities (§9.3).
2. **Whether the installed bootloader writes GPREGRET2 or leaves a WDT running.** The bootloader is
   a flashed binary, not present in the sources. The validity tag makes bootloader interference
   degrade containment rather than cause a wrong action.
3. **The real measured worst-case refresh span on a 7-colour split-buffer panel.** 240 s is derived
   from source constants, not measured. The plan's test T2 — measuring it — was called "the single
   most important test" and, since the 120 s change, a prerequisite for shipping to that panel class.

---

## 14. Call sites — the complete hook inventory

**47 call sites in application code**, across five files, plus 3 internal calls inside
`watchdog_nrf.cpp` itself. This section is what a port must reproduce.

### 14.1 Summary

| Function | App call sites |
|---|---|
| `odWatchdogBootInit` | 1 |
| `odWatchdogArm` | 1 |
| `odWatchdogInSafeMode` | 3 |
| `odWatchdogFeed` | 16 |
| `odWatchdogBreadcrumb` | 26 |
| **Total** | **47** |

| File | Sites |
|---|---|
| `src/main.cpp` | 10 |
| `src/display_service.cpp` | 31 |
| `src/boot_screen.cpp` | 4 |
| `src/split_panel.cpp` | 2 |

### 14.2 Lifecycle sites — `src/main.cpp`

| File:line | Enclosing function | Call | Phase | Why |
|---|---|---|---|---|
| `main.cpp:141` | `setup()` | `odWatchdogBootInit()` | — | Decode why we booted, on **both** targets. On nRF this also reads the retained breadcrumb, so a watchdog reset can name the panel phase that wedged. Must run **after** `od_log_init()` (or the line is emitted into a dark port) and **before** `odWatchdogArm()`. |
| `main.cpp:229` | `setup()` | `odWatchdogArm()` | — | Armed **immediately before the boot panel path**, so a wedge inside `initDisplay()` is itself covered — that is what makes the strike counter meaningful (W-3). Nothing earlier may block longer than the timeout. |
| `main.cpp:230` | `setup()` | `odWatchdogInSafeMode()` | — | Gate: three consecutive watchdog resets ⇒ skip `initDisplay()` entirely and set `rebootFlag = 1`. BLE still comes up below, so the device stays reachable for a config change or a DFU. |

### 14.3 Feed sites — the liveness proofs

| File:line | Enclosing function | Why this site feeds |
|---|---|---|
| `main.cpp:933` | `loop()` | **The primary liveness proof.** *"Reaching the top of `loop()` is what 'the program is still making progress' means. Every other feed site exists only to keep a LEGITIMATE long wait from looking like a wedge."* |
| `main.cpp:1075` | `idleDelay()` | Per ≤100 ms chunk. *"A long idle wait is healthy, not a wedge."* This is also **what makes `CONFIG.SLEEP=1` safe**: the CPU sleeps inside `delay()` and the watchdog keeps counting through it. |
| `display_service.cpp:863` | `waitforrefresh()` | Per poll iteration. The loop bound is **~126 s**, not `timeout` seconds, because each iteration costs `delay(10)` plus `bbepIsBusy()`'s own `delay(10)+delay(1)`. Feeding per iteration is what keeps that healthy wait from being mistaken for a wedge. Explicitly **not** a blind spot for a stuck BUSY line: that case exhausts the bound, returns `false`, and `loop()` resumes. |

### 14.4 Feed sites — immediately before a `bb_epaper` entry point

Every one of these carries the comment *"reload before entering bb_epaper (may block ~240 s)"* (or,
for `bbepInitIO`, *"bbepInitIO sends pInitFull internally (~240 s worst case)"*). The policy: reload
on entry so the watchdog faces **exactly the single uninstrumentable call**, not that call plus
everything preceding it in the same handler invocation.

| File:line | Enclosing function | Guards this call |
|---|---|---|
| `display_service.cpp:263` | `initBbepPanelSession()` | `bbepInitIO()` |
| `display_service.cpp:265` | `initBbepPanelSession()` | `bbepWakeUp()` |
| `display_service.cpp:267` | `initBbepPanelSession()` | `bbepSendCMDSequence(pInitFull)` |
| `display_service.cpp:357` | `epdSessionForceOffLocked()` | `bbepSleep(&bbep, 1)` |
| `display_service.cpp:411` | `epdSessionAcquire()` — cold branch | `bbepInitIO()` |
| `display_service.cpp:413` | `epdSessionAcquire()` — cold branch | `bbepWakeUp()` |
| `display_service.cpp:417` | `epdSessionAcquire()` — cold branch | `bbepSendCMDSequence(initSeq)` |
| `display_service.cpp:447` | `epdSessionAcquire()` — warm branch | `bbepWakeUp()` |
| `display_service.cpp:451` | `epdSessionAcquire()` — warm branch | `bbepSendCMDSequence(initSeq)` |
| `display_service.cpp:531` | `refreshBootScreenFull()` | `bbepRefresh(&bbep, REFRESH_FULL)` |
| `display_service.cpp:2505` | `directWriteFinishAndRefresh()` | `bbepRefresh(&bbep, refreshMode)` |
| `display_service.cpp:3367` | `partial_trigger_refresh()` | `bbepRefresh(&bbep, refreshMode)` |
| `display_service.cpp:3397` | `partial_prepare_panel_ram()` | `bbepFill(&bbep, BBEP_WHITE, PLANE_1)` |
| `display_service.cpp:3399` | `partial_prepare_panel_ram()` | `bbepFill(&bbep, BBEP_WHITE, PLANE_0)` |
| `split_panel.cpp:103` | `splitPanelInitIo()` | `bbepInitIO()` on the dual-controller path |

> Two counting errors were caught in the plan's review and are reflected above: `bbepFill` appears
> **twice** (consecutive), and `bbepInitIO` was originally omitted entirely even though it sends
> `pInitFull` internally — twice on a split-buffer panel — making it a ~240 s span in its own right.

### 14.5 Breadcrumb sites

| File:line | Enclosing function | Phase stamped | Why |
|---|---|---|---|
| `display_service.cpp:259` | `initBbepPanelSession()` | `INIT_SEQ` | About to run `bbepInitIO` + `bbepWakeUp` + `bbepSendCMDSequence`. |
| `display_service.cpp:344` | `epdSessionForceOffLocked()` | `FORCE_OFF` | Teardown entered (`pwrmgmState != PWR_OFF`). |
| `display_service.cpp:368` | `epdSessionForceOffLocked()` | `IDLE_OFF` | Panel work **finished**. Without re-stamping, a later wedge in BLE/WiFi/command handling would boot reporting `breadcrumb=FORCE_OFF` and point the next investigation at a teardown that had already completed. `IDLE_OFF` rather than plain `IDLE` so a freeze here is distinguishable from one during `PWR_WARM` keep-alive. |
| `display_service.cpp:394` | `epdSessionAcquire()` | `ACQUIRE_COLD` | Rail was `PWR_OFF`; full bring-up begins (`pwrmgm(true)` follows). |
| `display_service.cpp:407` | `epdSessionAcquire()` — cold | `INIT_SEQ` | Non-split cold controller init. |
| `display_service.cpp:426` | `epdSessionAcquire()` | `ACQUIRE_WARM` | Warm re-acquire, or defensive already-`PWR_ACTIVE` re-entry. |
| `display_service.cpp:443` | `epdSessionAcquire()` — warm | `INIT_SEQ` | Non-split warm re-init (`bbepWakeUp` + resend). |
| `display_service.cpp:469` | `epdSessionRelease()` | `RELEASE` | Release entered. (No stamp is added in the power-off branch: `epdSessionForceOffLocked()` stamps `IDLE_OFF` itself, and adding one here would clobber it.) |
| `display_service.cpp:487` | `epdSessionRelease()` | `IDLE_WARM` | Keep-alive path: `PWR_WARM`, deadline armed, controller stays awake. Distinguishes a freeze during keep-alive from one during `PWR_OFF`. |
| `display_service.cpp:521` | `refreshBootScreenFull()` | `BOOT_REFRESH` | About to enter `bbepRefresh(REFRESH_FULL)` for the boot screen. |
| `display_service.cpp:849` | `waitforrefresh()` | `REFRESH_WAIT` | Entering the BUSY poll. (Stamped after the FastEPD early return, so it marks the bb_epaper path only.) |
| `display_service.cpp:2039` | `streamGray4Bytes()` | `STREAM` | Panel data path. *"Repeats are filtered inside `odWatchdogBreadcrumb()`, so a per-call stamp here costs one comparison."* |
| `display_service.cpp:2062` | `directWriteSinkBytes()` | `STREAM` | Same; the generic direct-write byte sink. |
| `display_service.cpp:3239` | `partial_consume_bytes()` | `STREAM` | Per-frame on the partial-update path; repeats filtered internally. |
| `display_service.cpp:3396` | `partial_prepare_panel_ram()` | `FILL` | About to run the two white plane fills (sub-rect case only). |
| `boot_screen.cpp:515` | `writeGray4PlaneRow()` | `STREAM` | Per-row 4-gray plane de-interleave + stream. |
| `boot_screen.cpp:1035` | `writeBootScreenWithQr()` | `STREAM` | Plain `bbepWriteData(row)` row push — **FastEPD-enabled build** branch. |
| `boot_screen.cpp:1044` | `writeBootScreenWithQr()` | `STREAM` | Plain `bbepWriteData(row)` row push — **non-FastEPD build** branch (the `#else` twin of the above). |
| `boot_screen.cpp:1066` | `writeBootScreenWithQr()` | `STREAM` | Blanking `PLANE_1` with `h` rows of zeros when bitplanes are used without a colour swatch plane. |
| `split_panel.cpp:102` | `splitPanelInitIo()` | `INIT_SEQ` | Dual-controller init; stamped after `bbepSetCS2()` and before `bbepInitIO()`. |
| `main.cpp:1314` | `pwrmgm(true)` | `PWRMGM_AXP2101` | About to call `initAXP2101()` (I2C PMIC bring-up). |
| `main.cpp:1342` | `pwrmgm(true)` | `PWRMGM_RAIL` | About to drive `pwr_pin` HIGH and `delay(800)`. |
| `main.cpp:1351` | `pwrmgm(true)` | `PWRMGM_PINS` | About to do panel GPIO setup and `delay(100)` (or `delay(200)` on the FastEPD-SPI path). |
| `main.cpp:1386` | `pwrmgm(true)` | `PWRMGM_WIRE` | About to call `initOrRestoreWireForOpenDisplay()`. |
| `watchdog_nrf.cpp:273` | `odWatchdogBootInit()` | `IDLE` | *(internal)* Establishes a known phase after the tag is valid, and keeps the phase cache from starting out lying. |

**Breadcrumb-before-log ordering is normative in `pwrmgm()`.** Each of the four `PWRMGM_*` stamps is
placed **before** its `od_log_debug()` line, because *"the log call itself reaches
`tud_cdc_write_flush()` → `usbd_edpt_claim()` → a `WAIT_FOREVER` mutex, so it is not guaranteed to
return either. Stamping first means the phase survives even if the log line is what hangs."*

**The `pwrmgm(false)` power-down path has no breadcrumb** — *"No spare breadcrumb phase values
remain (all 16 are used), so this path is debug-log-only."*

### 14.6 Safe-mode gates

| File:line | Enclosing function | Effect |
|---|---|---|
| `main.cpp:230` | `setup()` | Skip `initDisplay()`; `rebootFlag = 1`. |
| `main.cpp:1278` | `pwrmgm(bool onoff)` | Refuse rail **power-up** (`onoff == true`) with `"Panel power-up refused - watchdog safe mode"`. Power-**down** still permitted, so a rail left on by a pre-safe-mode boot can still be shut off. |
| `display_service.cpp:387` | `epdSessionAcquire()` | Refuse acquisition; log `"[EPD session] acquire REFUSED - watchdog safe mode"`; return `false`. **This is the return value H-2 says is ambiguous.** |

### 14.7 Internal calls inside `watchdog_nrf.cpp`

| Line | Context | Call |
|---|---|---|
| `:273` | end of `odWatchdogBootInit()` | `odWatchdogBreadcrumb(OD_WDT_PHASE_IDLE)` |
| `:343` | inherit-detection branch of `odWatchdogArm()` | `odWatchdogFeed()` — feed the found-running dog immediately |
| `:358` | arm path of `odWatchdogArm()` | `odWatchdogFeed()` — *"start the first period from a known state"*, before `TASKS_START` |

### 14.8 Debug instrumentation that accompanies the hooks

Many breadcrumb sites are paired with `od_log_debug("[EPD session][WDT] …")` or
`od_log_debug("[pwrmgm][WDT] …")` lines, each marked in-source as
*"WDT-DEBUG: … safe to delete this block if it's no longer needed."* These are **not** part of the
watchdog contract — a port may omit them — but they are the intended way to correlate a live serial
log with the retained breadcrumb.

---

## 15. Porting checklist

1. Portable header with the five free functions and the 16-value phase enum; **no vendor headers**.
2. One implementation per target, whole-file gated, so untargeted ones are empty TUs.
3. Retained byte with the `7:6 tag = 0b10 / 5:4 strikes / 3:0 phase` layout, and a validity tag that
   causes garbage to be discarded rather than misread.
4. Two access paths if the platform has a protected-peripheral/SVC split; pick per a discriminator
   that is itself available in both states.
5. Reset reason read from wherever the framework saved it, never from a register the framework has
   already cleared. Zero means power-on; unknown nonzero bits are an anomaly, not a power-on.
6. Arm: write timeout, reload-enable, behaviour, **feed**, then start — in that order, because all
   configuration latches at start. Detect and feed an already-running watchdog unconditionally.
7. Feed: walk all enabled reload registers; use the platform's magic reload value.
8. Breadcrumb: cache the last phase, advance the cache only on a successful write, latch the failure
   log.
9. Strike counter incremented only on watchdog-caused resets, zeroed on any other reset cause, and
   cleared on **uptime**, never on a successful refresh.
10. Feed only from sites whose execution proves the program is alive. Never from an ISR, timer or
    stack callback.
