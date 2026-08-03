# bb_epaper IO backends

How `bb_epaper` is ported to a platform, what a backend must actually provide, which backend
each OpenDisplay target uses, and what was wrong with the one the ESP32 target used.

Written 2026-08-03 during the investigation of a panel that rendered its boot screen and then
never updated again; **§8 records how that resolved, and the answer contradicts what §§6-7
invite you to conclude** — the backend was replaced, and the backend was not the bug. Read §8
before acting on anything here.

Every number below was measured from this tree and the built object, not read from upstream
documentation. Call counts are comment-stripped. Paths are `third_party/` as vendored here
unless stated otherwise. §§1-7 describe the state as of the investigation and are left as
written; the ESP32 target now uses `targets/esp32-idf/panel/od_bbep_idf_io.inl`, not
`esp_generic.inl`.

---

## 1. bb_epaper has no platform-independent IO

`bb_ep.inl` holds all the panel knowledge — init sequences, LUTs, chip quirks, BUSY polarity
for every supported panel — and calls out to primitives it does not define. Exactly one
`*_io.inl` is `#include`d to supply them.

**There is no "raw bb_epaper" build.** A backend is not a wrapper layered on top of the
library; it is a required part of it. The only question is whose.

Selection, [`src/bb_epaper.cpp:38`](../third_party/bb_epaper/src/bb_epaper.cpp):

```c
#ifdef __LINUX__            → rpi_io.inl
#elif ESPHOME_LOG_LEVEL     → esphome_io.inl
#elif defined(ARDUINO)      → arduino_io.inl
#elif !defined(__MACH__)    → ../esp_idf/esp_generic.inl      ← this target
```

`ARDUINO` is deliberately not defined here — defining it to fix FastEPD would also pull in
bb_epaper's Arduino font/`Print`/`PROGMEM` surface (see `targets/esp32-idf/main/CMakeLists.txt`)
— so the ESP32 target falls through to the last branch. Confirmed from the built object rather
than the preprocessor: `bb_epaper.cpp.obj` defines `spi_write` and `delayCycles`, which exist
only in `esp_generic.inl`.

## 2. What a backend must provide: 5 functions **and 3 primitives**

The command/data path is routed through backend functions. **RST, BUSY and timing are not** —
`bb_ep.inl` calls those directly, bypassing the backend abstraction entirely.

### 2a. Routed through the backend

| Function | Calls from panel logic | |
|---|---:|---|
| `bbepWriteData` | 67 | `bb_ep.inl` 57, `bb_ep_gfx.inl` 10 |
| `bbepWriteCmd` | 45 | `bb_ep.inl` 39, `bb_ep_gfx.inl` 6 |
| `bbepCMD2` | 17 | thin: cmd + one data byte |
| `bbepInitIO` | 0 | **application-facing only** — `bb_ep.inl` never calls it |
| `bbepSetCS2` | 0 | **application-facing only**; dual-controller panels |

Plus one private byte-pusher, called **only from inside the backend** (0 external references),
whose name is therefore not contractual: `esp_generic.inl` calls it `spi_write`,
`arduino_io.inl` calls it `SPI_Write`.

`bbepWakeUp` and `bbepSendCMDSequence` are **not** backend functions. Both backends
forward-declare them because `bbepInitIO` calls them, but they are defined in `bb_ep.inl`
(`:4036`, `:4246`).

### 2b. Called directly by the panel logic — the exception that matters

```
bb_ep.inl:3995   digitalRead(pBBEP->iBUSYPin)      bbepWaitBusy       — BUSY poll
bb_ep.inl:4031   digitalRead(pBBEP->iBUSYPin)      bbepIsBusy
bb_ep.inl:4041   digitalWrite(pBBEP->iRSTPin, LOW)  bbepWakeUp        — reset pulse
bb_ep.inl:4043   digitalWrite(pBBEP->iRSTPin, HIGH)
bb_ep.inl:4281   digitalWrite(pBBEP->iRSTPin, LOW)  bbepSendCMDSequence — reset pulse
bb_ep.inl:4283   digitalWrite(pBBEP->iRSTPin, HIGH)
bb_ep.inl:4286   digitalRead(pBBEP->iBUSYPin)
```

| Primitive | Calls in `bb_ep.inl` |
|---|---:|
| `delay` | 10 |
| `digitalWrite` | 4 |
| `digitalRead` | 3 |

**So the contract is 5 + 3, not 5.** `arduino_io.inl` defines none of the three only because
the Arduino core does; `esp_generic.inl` must define them because under IDF nothing else will.

**The consequence is the single most important fact in this document: the reset pulse and the
BUSY polling live in `bb_ep.inl`, which is shared by every platform.** They are panel logic, not
backend logic. Replacing the ESP-IDF backend would therefore not change reset timing or BUSY
behaviour *at all*. Any defect in those is a bb_epaper-wide or hardware issue, and a new
backend cannot fix it.

### 2c. Dead weight

Six of `esp_generic.inl`'s functions are called by **nothing** in any compiled bb_epaper source
(`bb_ep.inl`, `bb_ep_gfx.inl`, `bb_epaper.cpp`, `g5enc.inl`, `g5dec.inl`):

`delayMicroseconds`, `delayCycles`, `mymemset`, `mymemcpy`, `i2str`, `i2strf`

Two more, `pinMode` (13 calls) and `millis` (6), are used **only by `bb_epaper.cpp`** — the C++
`BBEPAPER` class wrapper, which this project does not use at all (zero references in
`targets/esp32-idf/`). It is still compiled, so they must still link.

Net accounting of the 17 definitions in `esp_generic.inl`:

| | Count | |
|---|---:|---|
| Contractual `bbep*` functions | 5 | required |
| Private write helper | 1 | required |
| Primitives the panel logic calls | 3 | `digitalWrite`, `digitalRead`, `delay` |
| Primitives only the unused C++ class calls | 2 | `pinMode`, `millis` |
| **Dead in all compiled code** | **6** | |

## 3. Available ESP-IDF backends — only one drives hardware SPI

`esp_idf/` ships four files. They are not interchangeable:

| File | Lines | `bbepInitIO` | `spi_write` | `driver/spi_master.h` | What it is |
|---|---:|:---:|:---:|:---:|---|
| **`esp_generic.inl`** | **295** | ✅ | ✅ | ✅ | **hardware SPI — the only one** |
| `esp_main_io.inl` | 347 | ✗ | ✗ | ✗ | bit-banged I²C/SPI (`SPIWriteByte`, `i2cByteOut`); no e-paper init |
| `s3_ulp_io.inl` | 237 | ✅ | ✗ | ✗ | ULP coprocessor, S2/S3 |
| `c6_ulp_io.inl` | 357 | ✗ | ✗ | ✗ | ULP coprocessor, C6 |

`esp_main_io.inl` is **not** a better-maintained sibling — it targets I²C-attached displays by
bit-banging, does not include the SPI driver, and does not define `bbepInitIO`. Swapping to it
would not compile. The ULP files drive a panel from the coprocessor while the main CPU sleeps.

For this target the options were: patch `esp_generic.inl`, or write our own. There was no third
choice in the tree. §8 records which was taken.

## 4. Every other OpenDisplay target already owns its backend

`Firmware_NRF54`'s copy of `bb_epaper.cpp` adds two branches upstream does not have, **above**
the `ARDUINO` branch:

```c
#elif defined(__SILABS_BG22__)                        → silabs_efr32_io.inl
#elif defined(TARGET_NRF54) || defined(CONFIG_ZEPHYR) → nrf54_zephyr_io.inl
```

| Target | Backend | Lines | Origin | `OD-PATCH` sites |
|---|---|---:|---|---:|
| nRF54 / Zephyr | `nrf54_zephyr_io.inl` | 206 | written in-project | **0** |
| EFR32BG22 | `silabs_efr32_io.inl` | 315 | written in-project | 0 |
| ESP-IDF | `esp_generic.inl` | 295 → 367 | **upstream's** | **4** |

Three for three: every target that owned its backend was patch-free; the one that borrowed
upstream's carried the patches. (This target now owns one too — §8.) `nrf54_zephyr_io.inl` also bit-bangs SPI
(`bb_spi_bitbang`), so it has no peripheral lifecycle to get wrong — part of why Nordic never
hit the defect class in §7.

This matters for the migration end state: `Firmware_Unified` will vendor **one** `bb_epaper` for
all three targets, at which point `nrf54_zephyr_io.inl` and `silabs_efr32_io.inl` arrive anyway
and sit beside whatever ESP-IDF uses. An `od_bbep_idf_io.inl` alongside them is the consistent
shape; a patched upstream file as the odd one out is not.

## 5. Function inventory: `esp_generic.inl` vs `arduino_io.inl`

Baseline is the **as-vendored** `esp_generic.inl` at commit `cef6148`, 295 lines, verified
byte-identical to the upstream checkout at `~/bb_epaper/esp_idf/esp_generic.inl`. Compared
against `arduino_io.inl`, 250 lines, unmodified. Line numbers are from those two files.

| Function | `esp_generic.inl` | `arduino_io.inl` | |
|---|---|---|---|
| **Contractual — command/data path** | | | |
| `bbepInitIO` | ✅ `:235` | ✅ `:59` | both |
| `bbepWriteCmd` | ✅ `:197` | ✅ `:148` | both |
| `bbepWriteData` | ✅ `:221` | ✅ `:171` | both |
| `bbepCMD2` | ✅ `:213` | ✅ `:232` | both |
| `bbepSetCS2` | ✅ `:187` | ✅ `:33` | both |
| **Private byte-pusher** (name not contractual) | | | |
| `spi_write` | ✅ `:162` | — | |
| `SPI_Write` | — | ✅ `:41` | same role |
| **IT8951 / parallel panels** | | | |
| `bbepWriteIT8951Cmd` | — | ✅ `:108` | **Arduino only** |
| `bbepWriteIT8951Data` | — | ✅ `:124` | **Arduino only** |
| `bbepWriteIT8951CmdArgs` | — | ✅ `:137` | **Arduino only** |
| **Primitives the panel logic calls directly (§2b)** | | | |
| `digitalWrite` | ✅ `:39` | — | Arduino core supplies |
| `digitalRead` | ✅ `:60` | — | Arduino core supplies |
| `delay` | ✅ `:78` | — | Arduino core supplies |
| **Primitives only the unused C++ class calls** | | | |
| `pinMode` | ✅ `:42` | — | Arduino core supplies |
| `millis` | ✅ `:65` | — | Arduino core supplies |
| **Dead in all compiled bb_epaper code** | | | |
| `delayMicroseconds` | ✅ `:70` | — | 0 calls |
| `delayCycles` | ✅ `:155` | — | 0 calls |
| `mymemset` | ✅ `:82` | — | 0 calls |
| `mymemcpy` | ✅ `:101` | — | 0 calls |
| `i2str` | ✅ `:128` | — | 0 calls |
| `i2strf` | ✅ `:113` | — | 0 calls |
| **Forward declarations** (defined in `bb_ep.inl`) | | | |
| `bbepWakeUp` | `:36` | `:28` | both |
| `bbepSendCMDSequence` | `:37` | `:29` | both |

**Definitions: 17 vs 9. Common: 5.**

Three conclusions:

1. **Over half of `esp_generic.inl` is not IO.** Eleven of seventeen functions are an
   Arduino-core reimplementation plus libc substitutes — and **six of those eleven are dead**
   (§2c). `arduino_io.inl` has none of them because the core provides them. That block is
   exactly what had to be `#if 0`'d out here (`OD-PATCH`, current copy lines 71–159) because it
   collided with `compat/arduino_compat.h`, and it is the source of the duplicate `pinMode` /
   `digitalWrite` / `digitalRead` / `millis` symbols in `bb_epaper.cpp.obj`.
2. **The transport function is renamed between backends**, confirming it is private rather than
   contractual.
3. **`esp_generic.inl` cannot drive an IT8951 panel at all** — it lacks all three IT8951
   functions. A capability gap, not a style difference, and consistent with FastEPD existing
   separately in this tree for the parallel/large-panel path.

A replacement backend needs **5 contractual functions + 1 private helper + 3 primitives**, and
`pinMode`/`millis` only while `bb_epaper.cpp`'s unused C++ class is compiled. Six functions get
deleted rather than ported. `nrf54_zephyr_io.inl` at 206 lines — which likewise supplies no
libc layer — is the best available size estimate.

## 6. Behavioural differences that changed when the backend changed

The shipped fleet runs `arduino_io.inl`. These are the ways the IDF backend behaves differently
on the same hardware — the suspect list for any regression seen only here.

**SPI object ownership — the structural one.** Under Arduino, bb_epaper and the application
shared **one global `SPI` object**: `arduino_io.inl:85` calls `SPI.begin(...)` on every
`bbepInitIO`, and `SPI.end()` in `main.cpp` tore down that same bus. Teardown and setup were
symmetric across the library/application boundary — pads were re-attached on every cold
bring-up as a side effect of sharing the object, which nobody designed.

Under IDF they are two separate buses: `esp_generic.inl` owns its own
`spi_bus_config_t`/`spi_device_handle_t` file-statics, while `compat/SPI.h` owns a different one
on the same host. `main.cpp`'s `SPI.end()` now acts on the compat object, which never held the
panel's bus, so it is a silent no-op for the panel while the pad parking in
`configureDisplayPinsLowPower()` still happens. **The pairing broke invisibly when the backend
changed.**

**Reset assertion in `bbepInitIO`.**

| | |
|---|---|
| `arduino_io.inl` | `pinMode(RST, OUTPUT); digitalWrite(RST, HIGH);` — no pulse, guarded on `!= 0xff` |
| `esp_generic.inl` | `LOW → delay(100) → HIGH → delay(100)` — a pulse, unguarded |

IDF asserts a hardware reset in `bbepInitIO` that Arduino never did, immediately after
`pwrmgm(true)` has powered the rail and waited 800 ms. Note this is *in addition to* the pulses
`bbepWakeUp` and `bbepSendCMDSequence` do from `bb_ep.inl` on both platforms (§2b).

**Bus init is conditional under Arduino, unconditional under IDF.** `arduino_io.inl` has a
shared-SPI escape (`u8MOSI == 0xff` → do not touch the bus) and a bit-bang mode
(`u32Speed == 0`). `esp_generic.inl` has neither and always seizes `SPI2_HOST` — which is why
bb_epaper and the E1004 dual-CS path (through `compat/SPI.h`) can collide on one host here.

**Error handling.**

| | `arduino_io.inl` | `esp_generic.inl` |
|---|---|---|
| `assert()` calls | **0** | **3** (`:178` per transfer, `:268`, `:281`) |
| Shared static transaction struct | no | yes — one `static spi_transaction_t trans` |
| `0xFF` pin guards | RST and BUSY guarded | BUSY only |
| Transaction discipline | `beginTransaction`/`endTransaction` once at init, with an in-file note that re-entering **hangs on ESP32** | `spi_device_polling_transmit` per write, no acquire/release |

The older, Arduino-era file is the more defensive one — because it is the one that gets used.

## 7. Known problems with `esp_generic.inl`

### Fixed here, and therefore carried as patches forever

1. **`delay(int)` vs `delay(long)`** — `bb_epaper.h` declared one, `esp_generic.inl` defines the
   other, making every `delay(uint32_t)` *inside the library* an ambiguous overload. Does not
   compile as vendored.
2. **Duplicate utility functions** — the primitive/libc block of §2c, `#if 0`'d out.
3. **SPI init is not re-entrant** — `spi_bus_initialize()` on every cold bring-up with no
   `spi_bus_free()` anywhere in the file, so bring-up #2 could only `assert()`.
4. **The first fix for (3) was wrong**, and the failure is instructive: guarding the block behind
   an "already initialised" flag stranded SCLK/MOSI, because project code
   (`configureDisplayPinsLowPower()` → `pinMode()` → `gpio_config()`) revokes the pad routing
   between bring-ups and `spi_bus_initialize()` is the only call that restores it. An
   idempotent-init guard is wrong *here specifically*. Now teardown-then-reinit.
5. **No `esp_log.h`** — the file could not report anything.

### Open

6. **Three `assert()`s on ordinary runtime errors**, one per transfer. `assert` in shipped
   firmware is a reboot; and it is config-dependent — with
   `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE` all three become *silent* and SPI write
   failures vanish entirely. An IO backend must return status. (This target currently builds
   with `ASSERTION_LEVEL=2`: enabled and verbose.)
7. **`static spi_transaction_t trans;` shared by all callers** — works only because everything is
   single-task polling transmit, while `devcfg.queue_size = 2` advertises queued transactions the
   single struct cannot support.
8. **No teardown API.** There is no `bbepDeInitIO()`. This is the root cause of (3) and (4): the
   library never releases the bus and project code has no legitimate way to ask it to. Every fix
   in this area is a workaround for a missing function.
9. **`pinMode()` calls `gpio_reset_pin()` first; the compat shim's does not.** Two functions of
   the same name in one binary with materially different behaviour. Worse than a naming
   collision: `bb_ep.inl`'s RST/BUSY calls (§2b) bind to **bb_epaper's** `digitalWrite`/
   `digitalRead`, not the shim's, so panel pin handling and application pin handling go through
   different GPIO implementations in the same firmware.
10. **Duplicate global symbols** — `pinMode`, `digitalWrite`, `digitalRead`, `millis` exported
    from `bb_epaper.cpp.obj` alongside the shim's `static inline` versions.
11. **No `0xFF` pin guard** in `bbepInitIO` for DC/RST/CS (BUSY is guarded). An unset pin gives
    `1ULL << 255`, undefined behaviour. The compat shim has `od_pin_valid()`; bb_epaper's
    `pinMode` has nothing.
12. **`spi_write()` chunks at 4000 bytes "full duplex mode"** while the device is registered
    `SPI_DEVICE_HALFDUPLEX` and `max_transfer_sz` is 4096 — nobody reconciled the three numbers.
13. **Six dead functions** (§2c), carried and compiled for no consumer.

Items 6 and 8 are why defects in this layer are hard to diagnose: it either panics or says
nothing, so "commands are not reaching the panel" is indistinguishable from "the panel is slow".
Items 7, 9, 10, 11, 13 are latent and independent of any current symptom.

## 8. Decision: taken, and the hardware result that framed it

**Resolved 2026-08-03. The backend was replaced — and it was NOT the fix for the panel bug.**
Both halves matter, because the temptation to remember only the first is obvious.

### What the instrumented run measured

The stall this document was written during was 16.3 s and 21.4 s per cold panel bring-up, with
the panel dark after the boot screen. Per-step timing on a working build:

```
[EPD cold] pwrmgm(on)      898 ms
[EPD cold] bbepInitIO      201 ms
[EPD cold] bbepWakeUp       49 ms
[EPD cold] initSeq (full)  250 ms
[EPD cold] alignRamMode      0 ms
acquire done: COLD,       1398 ms total      (898+201+49+250+0 = 1398, so nothing is hidden)
```

`bbepInitIO` is **201 ms, essentially all of it the two `delay(100)` reset pulses.** The SPI
lifecycle — the entire subject of §7's fixed items — costs approximately zero. The old 16 s was
**entirely BUSY-wait expiry** inside `bb_ep.inl` (5 s a time, §6), and no `BUSY wait TIMED OUT`
warning appears now.

So §2b's warning held exactly: the reset pulses and BUSY polling are panel logic in
`bb_ep.inl`, shared by every platform, and a backend cannot touch them. **A backend rewrite was
never capable of fixing this bug.** Two wrong diagnoses were published before the log settled
it, both from static reading; the fix that mattered was releasing the SPI bus at power-down
(`bbepDeInitIO()` from `epdSessionForceOffLocked()`) rather than at the following bring-up.

### The replacement, and why it was still right

`targets/esp32-idf/panel/od_bbep_idf_io.inl` replaces `esp_generic.inl`, selected without
touching vendored code: `panel/od_bbep.cpp` includes `bb_epaper.h` -> our backend ->
`bb_ep.inl` -> `bb_ep_gfx.inl`, and the build excludes `bb_epaper.cpp`. That replaces its 52
lines of glue instead of patching an `#elif` into its `#ifdef` chain (Firmware_NRF54's method),
so no fifth `OD-PATCH` joins NOTICE.md's re-verify list — and it drops the other 775 lines, the
unused `BBEPAPER` C++ class, which was the only consumer of `pinMode()` and `millis()`.

Justified on maintenance grounds, which the measurement does not undermine: three `assert()`s
on ordinary runtime errors (one per transfer) became return values; a real teardown exists for
the first time; the shared transaction struct became a local; DC/RST/CS gained pin-validity
guards; GPIO semantics now match the compat shim; six dead functions are gone, verified absent
from the linked image.

`esp_generic.inl` is now **dead code in the tree** — zero symbols in the linked image, no
compiled includer — while still carrying its four patches. Deleting it, and NOTICE.md's "Third
patch" section describing it, is outstanding.

One coupling bug surfaced during the swap and is worth recording: FastEPD's `arduino_io.inl`
declared `millis()` extern and deferred the definition **to bb_epaper's backend**, so removing
that backend turned FastEPD's 19 calls into undefined references — a link error naming FastEPD
for a cause in bb_epaper. `millis()` now lives in `compat/arduino_compat.cpp` beside `delay()`,
so neither vendored library depends on the other for it.

### What was NOT done, and stays not done

**Dropping bb_epaper and writing the panel driver against ESP-IDF directly.** That means owning
init sequences, LUTs, chip quirks and BUSY polarity for the whole fleet — the "Rewriting working
drivers" non-goal in `CLAUDE.md`. The boot screen worked throughout, which is the proof that
code is fine.

### Still open in this area

1. **FastEPD's nine unpatched guards** (§7 is bb_epaper's list; this is FastEPD's). All three
   `it8951WriteFramebuffer{1,2,4}Bit` functions fall through to nothing under IDF, so an IT8951
   panel receives no pixel data while every call reports success. `NOTICE.md`'s "every affected
   guard" claim is false.
2. **`bbepWaitBusy` blocks the loop task** with a bare `delay(20)`, so `serviceBleTx()` stops
   for the duration of a refresh and queued BLE responses stall. Independent of the panel bug
   and unaffected by fixing it. `bbepLightSleep()` is where a pump belongs.
3. **Delete `esp_generic.inl`** and its NOTICE.md section, or state why a patched, uncompiled
   backend is being kept.
