# Vendored third-party code

Origin, revision, licence, and pruning for every tree under `third_party/`.

README.md § License requires this: *"Vendored third-party code keeps its own licence and its
own notice; do not relicense it on the way in. Record the origin and licence of each vendored
tree."* MIGRATION.md § "Risks to watch" adds the rule that decided the pruning below: *"Decide
what a blob's pruned form is before its first commit, not after it has been bumped twice."*

`third_party/` is deliberately **outside `shared/`** and is not scanned by the boundary check.
`bb_epaper` selects its IO backend by `#ifdef` and every backend includes vendor headers, so it
can never satisfy the `shared/` rule — but it is still *one* vendored copy for all targets, not
a per-target fork. Do not "fix" this by moving it into `shared/`.

---

## bb_epaper

| | |
|---|---|
| Author | Larry Bank (BitBank Software, Inc.) |
| Upstream | https://github.com/bitbank2/bb_epaper |
| Vendored from | https://github.com/davelee98-creator/bb_epaper — **a fork, not upstream** |
| Revision | `2ef09a1afaf4b7cec88a2821f50796a5280924ae` ("Fixed 4.26 again") |
| Licence | **GPL-3.0-or-later** |
| Vendored | 2026-07-25, at the ESP32 phase-B import |

**Pruned on the way in — 6.6 MB → 492 KB.** Kept `src/`, the four `esp_idf/*.inl` backends,
`CMakeLists.txt`, `LICENSE`, `README.md`. Dropped the example projects under `esp_idf/`
(`Spectra6`, `mini_epaper_s3`, `hello_world_c`, `hello_world_cpp`), `Fonts/`, `examples/`,
`fontconvert/`, `imageconvert/`, and the `ch32v`/`macos`/`rpi`/`rpi_pico` ports. None is
reachable from a firmware build.

### Local patches (2026-07-25) — both are upstream bugs, marked `OD-PATCH`

The tree does not compile as vendored. Two defects, each fixed with a one-line change tagged
`OD-PATCH` so a future bump can find them:

1. **`src/bb_ep.inl` defined `epd42yr_init_full` twice** (lines 1109 and 1151) while line 3831
   referenced `epd42yr2_init_full`, which existed nowhere. A copy-paste that was never renamed.
   The second definition is now `epd42yr2_init_full`, which is what the panel table expects.
2. **`src/bb_epaper.h` declared `void delay(int)` while `esp_idf/esp_generic.inl` defines
   `void delay(long)`.** Two different overloads of the library's own function, so any
   `delay(uint32_t)` call inside the library was ambiguous. The declaration now says `long`.

Both are bugs in the library itself, not incompatibilities with this repo — worth reporting
upstream, and worth checking against `bitbank2/bb_epaper` to see whether the fork introduced
them. Re-apply or re-verify on every bump.

### Third patch (2026-07-26) — `esp_generic.inl` SPI init is not re-entrant

**Symptom:** guaranteed panic on the first image upload after the boot screen.

```
E (26228) spi: spi_bus_initialize(816): SPI bus already initialized.
assert failed: bbepInitIO ... esp_generic.inl:272 (ret==ESP_OK)
```

`bbepInitIO()` runs on every **cold panel bring-up**, not once at boot — `epdSessionAcquire()`
powers the panel down when idle and re-acquires it per transfer. It calls
`spi_bus_initialize()` and asserts on the result, and there is **no `spi_bus_free()` anywhere
in this backend**, so the bus is never released. Bring-up #1 (boot screen) succeeds; #2 can
only assert.

Invisible under Arduino, because bb_epaper used `src/arduino_io.inl` there: `SPI.begin()` is
idempotent and `SPI.end()` (main.cpp, panel power-down) was its matching teardown. The IDF port
selects `esp_idf/esp_generic.inl`, which has a begin and no end.

**Fixed here rather than project-side, deliberately.** The state that has to be guarded is
`static spi_device_handle_t spi` at line 28 — file-scope static, so project code cannot see the
device it would need to remove before `spi_bus_free()` would succeed, and the library exposes
no IO teardown at all. Every project-side alternative was worse:

| Alternative | Why not |
|---|---|
| `-Wl,--wrap=spi_bus_initialize` returning OK on `INVALID_STATE` | Silences the assert but not the leak: `spi_bus_add_device` still runs per bring-up, overwriting `spi` and leaking a slot. Three devices per host — it panics on the third cold acquire instead of the first. Fixing that means wrapping `spi_bus_add_device` too, i.e. reimplementing the library's state machine through the linker, globally. |
| Define `ARDUINO` so `arduino_io.inl` is selected | Drags in bb_epaper's whole Arduino surface (`Print`, `PROGMEM`, `pgm_read_*`). The same move on FastEPD produced 117 errors. It also moves the panel *onto* the compat shim, the opposite of phase C. |
| Call `bbepInitIO()` once from `epdSessionAcquire()` and do the RST pulse in project code | Duplicates the vendor reset sequence into `display_service.cpp` — a file destined for `shared/core`, where vendor knowledge is forbidden — and drifts silently if the library's init changes. |

The patch guards bus init **and** device add together behind one static flag, recreating the
device only if the requested clock changes. Guarding only the bus init would leave the slot
leak above.

**This is an upstream bug.** The right end state is a `bbepDeInitIO()` in bb_epaper that frees
the device and bus, or an idempotent init. Report it; until then this patch must survive every
re-vendor.

**Two things to know before bumping it.**

*It is vendored from a fork.* `Firmware`'s `platformio.ini` pulls `bitbank2/bb_epaper.git`
(upstream) while the checkout used here is `davelee98-creator/bb_epaper`. Whether the fork
carries local changes has **not** been established — establish it before the next bump, because
"vendored from a fork of unknown divergence" is how a local fix becomes permanently invisible.

*No copy is a superset, and this one is not either.* TOOLCHAINS.md § "Where bb_epaper and uzlib
live" records the problem: upstream has the `esp_idf/` backends but no `nrf54_zephyr_io.inl` or
`silabs_efr32_io.inl`, while the `Firmware_NRF54` and `Firmware_Silabs` copies have those and no
`esp_idf/`. **This vendored tree currently has only the ESP-IDF side**, which is correct for
migration step 1 and insufficient from step 2 onward. When the Nordic and Silabs targets land,
their backends must be merged in here rather than each target re-vendoring — that merge is what
makes this an assembled fork, and DESIGN_REVIEW F9 flags it as a fork with no upstream-sync
owner. Assign one before step 2.

## FastEPD

| | |
|---|---|
| Author | Larry Bank |
| Upstream | https://github.com/bitbank2/FastEPD |
| Revision | `770e168d695e693688053560d9a8d296a0dbc967` ("changed to taskYIELD") |
| Licence | **Apache-2.0** |
| Vendored | 2026-07-25, at the ESP32 phase-B import |
| Scope | **ESP32-S3 only, and PSRAM-mandatory** — parallel e-paper via the S3 LCD peripheral |

**Pruned on the way in — 13 MB → 380 KB.** Kept `src/`, `LICENSE`, `README.md`,
`library.properties`. Dropped `examples/` (8.2 MB), `Fonts/` (1.6 MB), `Linux/`, `fontconvert/`,
`imageconvert/`.

Apache-2.0 code vendored inside a GPL-3.0 work is fine; the combined work is GPL-3.0 and this
tree keeps its own licence and notice. Do not relicense it.

### The vendored bb_epaper cannot build `s3-e1004` — wrong fork

`env:esp32-s3-E1004` in the source repo pins a **different bb_epaper fork** from the one
vendored here:

| | |
|---|---|
| Vendored | `davelee98-creator/bb_epaper` @ `2ef09a1` |
| Required by E1004 | `limengdu/bb_epaper` @ `95fd94af` — limengdu's PR bitbank2#32, T133A01 support |

The vendored copy defines neither `BBEP_T133A01` nor the `EP133A_SPECTRA_1200x1600` panel
enum. `display_service.cpp:700` maps `OD_PANEL_IC_EP133A_SPECTRA_1200X1600` onto that enum,
and the whole E1004 dual-CS stream is behind `#ifdef BBEP_T133A01` — which is why the current
build compiles: the guard is false everywhere and `e1004_write_stream_bytes()` is a no-op stub.

**So `boards/s3-e1004.cmake` is deliberately absent.** Writing the fragment would produce a
board that builds and cannot drive its own panel — the exact failure mode this repo's audit
has been removing. It needs one of:

1. re-vendor bb_epaper from a revision containing the T133A01 work (upstream if bitbank2#32
   has merged since, otherwise the limengdu fork the source env pins), then add the fragment
   with `BBEP_T133A01` defined; **and**
2. fix `e1004_write_stream_bytes()`, which calls `SPI.writeBytes()` while nothing calls
   `SPI.begin()`. Under Arduino it shared the global SPI object with bb_epaper; under IDF
   bb_epaper owns its own `SPI2_HOST` bus and device, so those bytes must go through
   bb_epaper's existing device handle — the one that just sent `DTM1` — not a second handle
   on the shim.

Item 2 is a live defect today, merely unreachable. Do not add the board without it.

### FastEPD has no ESP-IDF IO backend — OD-PATCH applied

FastEPD picks its IO backend with `#ifdef ARDUINO` / `#elif defined(__LINUX__)`. There is no
third arm, and under ESP-IDF **neither macro is defined**. That is not a compile error: every
`it8951*` transport function in `FastEPD.inl` simply collapsed to an *empty body between two
`gpio_set_level()` calls*.

The consequences were invisible to the build and to the boot log:

- `it8951WriteCmdCode` / `it8951WriteData` / `it8951WriteNData` transmitted nothing
- `it8951ReadData` / `it8951ReadNData` returned 0, so the controller probe read all zeros
- the GPIO reset/busy/power handshake still ran, so the panel *looked* alive

`bbepInitIT8951` therefore "succeeded" against a panel that had never received a byte. Only an
IT8951-over-SPI panel is affected; the parallel ED103 path drives the S3 LCD peripheral and
never went through this code.

**The patch.** Every affected guard becomes
`#if defined(ARDUINO) || defined(OD_FASTEPD_IDF_SPI)`, and
`targets/esp32-idf/main/CMakeLists.txt` sets `OD_FASTEPD_IDF_SPI=1` on `FastEPD.cpp` alone.
The Arduino branch is reused verbatim because `targets/esp32-idf/compat/SPI.h` provides exactly
that API over IDF's `spi_master`. Patched sites are marked `OD-PATCH` and all point at the
explanatory note on `it8951WriteData()`.

**Why not just define `ARDUINO`.** Tried, and rejected: it also selects FastEPD's Arduino
font/`Print`/`PROGMEM` surface (`pgm_read_*`, `memcpy_P`, `ledcAttach`, `TwoWire::setTimeout`)
— an entire Arduino core rather than an IO backend — and `bb_epaper` reads the same macro to
choose *its* backend and must keep the `esp_idf` one.

**Retiring it.** The upstream-shaped fix is an `esp_idf_io.inl` in FastEPD selected by
`ESP_PLATFORM`. When that exists, delete both the patch and the CMake define. Until then, note
that `compat/` cannot be removed at the end of phase C while FastEPD still depends on it —
`compat/ratchet.sh` reports these vendored users separately for exactly that reason.

---

## The Group5 collision — the same code, twice, under two licences

`bb_epaper` and `FastEPD` **both ship `Group5.cpp`, `Group5.h`, `bb_ep_gfx.inl`, `g5dec.inl`,
`g5enc.inl` and `arduino_io.inl`.** Same author, overlapping history — and the copies are *not*
identical. The licence headers differ:

```
bb_epaper/src/Group5.cpp:  SPDX-License-Identifier: GPL-3.0-or-later
FastEPD/src/Group5.cpp:    (no SPDX header; FastEPD as a whole is Apache-2.0)
```

`Group5.cpp` is the only overlapping file that is *compiled* (the `.inl` files are includes), so
it is the only one that produces duplicate symbols at link time.

**The source repo's answer was `-Wl,--allow-multiple-definition`**, with the comment *"FastEPD
and bb_epaper both ship bb_ep_gfx/Group5; we only use FastEPD buffer+update APIs."* That flag
tells the linker to silently keep whichever definition it saw first — which means the effective
G5 codec depends on link order, and a divergence between the two copies becomes a bug nobody can
see in the source.

**This repo does not carry that flag.** MEMORY_CONSTRAINTS.md § "Open questions" recommends
option 1 — *compile the duplicated files from one library only* — and DESIGN_REVIEW § "Likely
pitfalls" #4 warns specifically that the flag must not ship "temporarily", because temporary
linker workarounds are exactly the class of thing that becomes permanent. It says to adopt
option 1 **before** phase B, not during. That is what `targets/esp32-idf/main/CMakeLists.txt`
does: on a FastEPD board it compiles `Group5.cpp` from FastEPD and excludes bb_epaper's; on a
non-FastEPD board only bb_epaper's exists and the question does not arise.

**Resolved 2026-07-26, and the collision was wider than Group5.cpp.** FastEPD and bb_epaper
also both carry `bb_ep_gfx.inl`, defining `bbepUnicodeTo1252`, `bbepUnicodeString`,
`RotateCharBox`, `bbepStretchAndSmooth` and `millis`. Excluding one copy does not work:
each library's own wrappers call into its copy, even though the application calls none of
them. **FastEPD's copy is therefore given internal linkage** (an anonymous namespace,
`OD-PATCH` in `FastEPD.cpp`) so both libraries keep a private copy and there is no
arbitrary linker winner. The duplicated text is mostly discarded as unreferenced.

The premise this rests on, from MEMORY_CONSTRAINTS: *"confirm both drawing APIs are not
simultaneously required on any single board first."* The `platformio.ini` comment asserts they
are not — only FastEPD's buffer/update APIs are used on those boards. **That assertion has not
been verified against the code**, and it is the thing to check before trusting this arrangement
on hardware.
