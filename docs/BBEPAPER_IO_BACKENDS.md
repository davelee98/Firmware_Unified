# bb_epaper IO backends

How `bb_epaper` is ported to a platform, which backend each OpenDisplay target uses, and what
is wrong with the one the ESP32 target uses today.

Written 2026-08-03, during the investigation of a panel that renders its boot screen and then
never updates again. Every measurement below was taken from the tree and the build, not from
reading upstream docs — line numbers are `third_party/` as vendored here unless stated.

---

## 1. bb_epaper has no platform-independent IO

`bb_ep.inl` holds all the panel knowledge — init sequences, LUTs, chip quirks, BUSY polarity
for every supported panel — and calls out to a small set of byte-level functions it does not
define. Exactly one `*_io.inl` is `#include`d to supply them.

**There is no "raw bb_epaper" build.** A backend is not a wrapper layered on top of the
library; it is a required part of it. The only question is whose.

Selection, [`third_party/bb_epaper/src/bb_epaper.cpp:38`](../third_party/bb_epaper/src/bb_epaper.cpp):

```c
#ifdef __LINUX__            → rpi_io.inl
#elif ESPHOME_LOG_LEVEL     → esphome_io.inl
#elif defined(ARDUINO)      → arduino_io.inl
#elif !defined(__MACH__)    → ../esp_idf/esp_generic.inl      ← this target
```

`ARDUINO` is deliberately not defined here — defining it to fix FastEPD would also pull in
bb_epaper's Arduino font/`Print`/`PROGMEM` surface (see `targets/esp32-idf/main/CMakeLists.txt`)
— so the ESP32 target falls through to the last branch. Confirmed from the built object, not
the preprocessor: `bb_epaper.cpp.obj` defines `spi_write` and `delayCycles`, which exist only
in `esp_generic.inl`.

## 2. The backend contract is five functions

What `bb_ep.inl` + `bb_ep_gfx.inl` actually require a backend to define, with call counts
measured across both files:

| Function | Calls | Notes |
|---|---:|---|
| `bbepWriteData` | 67 | |
| `bbepWriteCmd` | 45 | |
| `bbepCMD2` | 17 | thin: cmd + one data byte |
| `bbepInitIO` | — | called by the application, not by `bb_ep.inl` |
| `bbepSetCS2` | — | dual-controller panels only |

Plus GPIO/time primitives (`digitalWrite`, `digitalRead`, `delay`) where the platform does not
already provide them, and a private byte-pusher whose name is **not** part of the contract:
`esp_generic.inl` calls it `spi_write`, `arduino_io.inl` calls it `SPI_Write`.

`bbepWakeUp` and `bbepSendCMDSequence` are **not** backend functions. Both backends
forward-declare them because `bbepInitIO` calls them, but they are defined in `bb_ep.inl`
(`:4036` and `:4246`).

## 3. Available ESP-IDF backends — only one drives hardware SPI

`third_party/bb_epaper/esp_idf/` ships four files. They are not interchangeable:

| File | Lines | `bbepInitIO` | `spi_write` | `driver/spi_master.h` | What it is |
|---|---:|:---:|:---:|:---:|---|
| **`esp_generic.inl`** | **295** | ✅ | ✅ | ✅ | **hardware SPI — the only one** |
| `esp_main_io.inl` | 347 | ✗ | ✗ | ✗ | bit-banged I²C/SPI (`SPIWriteByte`, `i2cByteOut`); no e-paper init at all |
| `s3_ulp_io.inl` | 237 | ✅ | ✗ | ✗ | ULP coprocessor, S2/S3 |
| `c6_ulp_io.inl` | 357 | ✗ | ✗ | ✗ | ULP coprocessor, C6 |

`esp_main_io.inl` is **not** a better-maintained sibling of `esp_generic.inl` — it targets
I²C-attached displays by bit-banging, does not include the SPI driver, and does not define
`bbepInitIO`. Swapping to it would not compile. The two ULP files drive a panel from the
coprocessor while the main CPU sleeps.

So for this target the options are: patch `esp_generic.inl`, or write our own backend. There is
no third choice already in the tree.

## 4. Every other OpenDisplay target already owns its backend

`Firmware_NRF54`'s copy of `bb_epaper.cpp` adds two branches upstream does not have, **above**
the `ARDUINO` branch:

```c
#elif defined(__SILABS_BG22__)                        → silabs_efr32_io.inl
#elif defined(TARGET_NRF54) || defined(CONFIG_ZEPHYR) → nrf54_zephyr_io.inl
```

| Target | Backend | Lines | Origin | `OD-PATCH` count |
|---|---|---:|---|---:|
| nRF54 / Zephyr | `nrf54_zephyr_io.inl` | 206 | written in-project | **0** |
| EFR32BG22 | `silabs_efr32_io.inl` | 315 | written in-project | 0 |
| ESP-IDF | `esp_generic.inl` | 295 → 367 | **upstream's** | **4** |

Three for three: every target that owns its backend is patch-free; the one that borrowed
upstream's is the one generating patches and carrying a live bug. `nrf54_zephyr_io.inl` also
bit-bangs SPI (`bb_spi_bitbang`), so it has no peripheral lifecycle to get wrong — which is
part of why Nordic never hit the class of defect described in §6.

This matters for the migration end state: `Firmware_Unified` will vendor **one** `bb_epaper`
for all three targets, at which point `nrf54_zephyr_io.inl` and `silabs_efr32_io.inl` arrive
anyway and sit beside whatever ESP-IDF uses. An `od_bbep_idf_io.inl` alongside them is the
consistent shape; keeping a patched upstream file as the odd one out is not.

## 5. Function inventory: `esp_generic.inl` vs `arduino_io.inl`

Baseline is the **as-vendored** `esp_generic.inl` at commit `cef6148`, 295 lines, verified
byte-identical to the upstream checkout at `~/bb_epaper/esp_idf/esp_generic.inl`. Compared
against `arduino_io.inl`, 250 lines, unmodified. Line numbers are from those two files.

| Function | `esp_generic.inl` | `arduino_io.inl` | |
|---|---|---|---|
| **Panel byte-level interface** | | | |
| `bbepInitIO` | ✅ `:235` | ✅ `:59` | both |
| `bbepWriteCmd` | ✅ `:197` | ✅ `:148` | both |
| `bbepWriteData` | ✅ `:221` | ✅ `:171` | both |
| `bbepCMD2` | ✅ `:213` | ✅ `:232` | both |
| `bbepSetCS2` | ✅ `:187` | ✅ `:33` | both |
| **SPI transport** | | | |
| `spi_write` | ✅ `:162` | — | name is backend-private |
| `SPI_Write` | — | ✅ `:41` | same role, different name |
| **IT8951 / parallel panels** | | | |
| `bbepWriteIT8951Cmd` | — | ✅ `:108` | **Arduino only** |
| `bbepWriteIT8951Data` | — | ✅ `:124` | **Arduino only** |
| `bbepWriteIT8951CmdArgs` | — | ✅ `:137` | **Arduino only** |
| **GPIO layer** | | | |
| `pinMode` | ✅ `:42` | — | Arduino core supplies |
| `digitalWrite` | ✅ `:39` | — | Arduino core supplies |
| `digitalRead` | ✅ `:60` | — | Arduino core supplies |
| **Time** | | | |
| `millis` | ✅ `:65` | — | Arduino core supplies |
| `delay` | ✅ `:78` | — | Arduino core supplies |
| `delayMicroseconds` | ✅ `:70` | — | Arduino core supplies |
| `delayCycles` | ✅ `:155` | — | IDF only |
| **libc / string helpers** | | | |
| `mymemset` | ✅ `:82` | — | IDF only |
| `mymemcpy` | ✅ `:101` | — | IDF only |
| `i2str` | ✅ `:128` | — | IDF only |
| `i2strf` | ✅ `:113` | — | IDF only |
| **Forward declarations** (defined in `bb_ep.inl`) | | | |
| `bbepWakeUp` | `:36` | `:28` | both |
| `bbepSendCMDSequence` | `:37` | `:29` | both |

**Definitions: 17 vs 9. Common: 5.**

Three conclusions from the table:

1. **Two-thirds of `esp_generic.inl` is not an IO backend.** Eleven of its seventeen functions
   are an Arduino-core reimplementation (`pinMode`, `digitalWrite`, `digitalRead`, `millis`,
   `delay`, `delayMicroseconds`) plus libc substitutes (`mymemset`, `mymemcpy`, `i2str`,
   `i2strf`). `arduino_io.inl` has none of them because the core provides them. That block is
   exactly what had to be `#if 0`'d out here (`OD-PATCH`, current copy lines 71–159) because it
   collided with `compat/arduino_compat.h`, and it is the source of the duplicate `pinMode` /
   `digitalWrite` / `digitalRead` / `millis` symbols in `bb_epaper.cpp.obj`.
2. **The transport function is renamed between backends**, confirming it is private rather than
   contractual.
3. **`esp_generic.inl` cannot drive an IT8951 panel at all** — it lacks all three IT8951
   functions. A capability gap, not a style difference, and consistent with FastEPD existing
   separately in this tree for the parallel/large-panel path.

A replacement backend therefore needs **five functions plus a private write helper**, not
seventeen. The GPIO/time/libc block is deleted rather than rewritten: the compat shim (or
direct IDF calls) already supplies all of it, and two providers is an active defect today.
206 lines for `nrf54_zephyr_io.inl`, which likewise supplies no GPIO or time layer, is the best
available estimate.

## 6. Behavioural differences that changed when the backend changed

The shipped fleet runs `arduino_io.inl`. These are the ways the IDF backend behaves
differently on the same hardware — i.e. the suspect list for any regression seen only here.

**SPI object ownership — the structural one.** Under Arduino, bb_epaper and the application
shared **one global `SPI` object**: `arduino_io.inl:85` calls `SPI.begin(...)` on every
`bbepInitIO`, and `SPI.end()` in `main.cpp` tore down that same bus. The teardown and setup were
symmetric across the library/application boundary — pads got re-attached on every cold
bring-up as a side effect of sharing the object, which nobody designed.

Under IDF they are two separate buses: `esp_generic.inl` owns its own
`spi_bus_config_t`/`spi_device_handle_t` file-statics, while `compat/SPI.h` owns a different
one on the same host. `main.cpp`'s `SPI.end()` now acts on the compat object, which never held
the panel's bus, so it is a silent no-op for the panel while the pad parking in
`configureDisplayPinsLowPower()` still happens. **The pairing broke invisibly when the backend
changed.**

**Reset sequence.**

| | |
|---|---|
| `arduino_io.inl` | `pinMode(RST, OUTPUT); digitalWrite(RST, HIGH);` — no pulse, guarded on `!= 0xff` |
| `esp_generic.inl` | `LOW → delay(100) → HIGH → delay(100)` — a real reset pulse, unguarded |

IDF asserts a hardware reset on every bring-up that Arduino never did, immediately after
`pwrmgm(true)` has powered the rail and waited 800 ms.

**Bus init is conditional under Arduino, unconditional under IDF.** `arduino_io.inl` has a
documented shared-SPI escape (`u8MOSI == 0xff` → do not touch the bus) and a bit-bang mode
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
2. **Duplicate utility functions** — the GPIO/time/libc block of §5, `#if 0`'d out.
3. **SPI init is not re-entrant** — `spi_bus_initialize()` on every cold bring-up with no
   `spi_bus_free()` anywhere in the file, so bring-up #2 could only `assert()`.
4. **The first fix for (3) was wrong**, and the failure is instructive: guarding the block
   behind an "already initialised" flag stranded SCLK/MOSI, because project code
   (`configureDisplayPinsLowPower()` → `pinMode()` → `gpio_config()`) revokes the pad routing
   between bring-ups and `spi_bus_initialize()` is the only call that restores it. An
   idempotent-init guard is wrong *here specifically*. Now teardown-then-reinit.
5. **No `esp_log.h`** — the file could not report anything.

### Open

6. **Three `assert()`s on ordinary runtime errors**, one of them per transfer. `assert` in
   shipped firmware is a reboot; and it is config-dependent — with
   `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE` all three become *silent* and SPI write
   failures vanish entirely. An IO backend must return status. (This target currently builds
   with `ASSERTION_LEVEL=2`, i.e. enabled and verbose.)
7. **`static spi_transaction_t trans;` shared by all callers** — works only because everything
   is single-task polling transmit, while `devcfg.queue_size = 2` advertises queued transactions
   the single struct cannot support.
8. **No teardown API.** There is no `bbepDeInitIO()`. This is the root cause of (3) and (4):
   the library never releases the bus and project code has no legitimate way to ask it to.
   Every fix in this area is a workaround for a missing function.
9. **`pinMode()` calls `gpio_reset_pin()` first; the compat shim's does not.** Two functions of
   the same name in one binary with materially different behaviour.
10. **Duplicate global symbols** — `pinMode`, `digitalWrite`, `digitalRead`, `millis` exported
    from `bb_epaper.cpp.obj` alongside the shim's `static inline` versions.
11. **No `0xFF` pin guard** in `bbepInitIO` for DC/RST/CS (BUSY is guarded). An unset pin gives
    `1ULL << 255`, undefined behaviour. The compat shim has `od_pin_valid()`; bb_epaper's
    `pinMode` has nothing.
12. **`spi_write()` chunks at 4000 bytes "full duplex mode"** while the device is registered
    `SPI_DEVICE_HALFDUPLEX` and `max_transfer_sz` is 4096 — nobody reconciled the three numbers.

Items 6 and 8 are why defects in this layer are hard to diagnose: it either panics or says
nothing, so "commands are not reaching the panel" is indistinguishable from "the panel is
slow". Items 7, 9, 10, 11 are latent and independent of any current symptom.

## 8. Open decision

**Replace `esp_generic.inl` with an in-project `od_bbep_idf_io.inl`, or keep patching it?**

For replacing: five functions plus a write helper (§2, §5); deletes the duplicate GPIO/time
layer rather than porting it; removes four patch sites and the NOTICE.md "re-verify on every
bump" burden; matches what both other targets already did (§4); and gives us a place to make
BUSY waits return status instead of expiring silently. Panel logic in `bb_ep.inl` is untouched.

Against replacing *now*: the live bug in §7(4)'s neighbourhood is not yet diagnosed. If it
turns out to be panel rail or reset/settle timing, a new backend fixes nothing and we would
have rewritten the one layer that was not at fault, losing the diagnosis. Rewriting the IO
layer mid-investigation risks relocating the bug rather than fixing it.

**Not on the table:** dropping bb_epaper and writing the panel driver against ESP-IDF directly.
That means owning init sequences, LUTs, chip quirks and BUSY polarity for the whole fleet — the
"Rewriting working drivers" non-goal in `CLAUDE.md`, and the boot screen proves that code works.

Status: deferred pending one instrumented hardware run. See `docs/FOLLOWUPS.md`.
