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

The premise this rests on, from MEMORY_CONSTRAINTS: *"confirm both drawing APIs are not
simultaneously required on any single board first."* The `platformio.ini` comment asserts they
are not — only FastEPD's buffer/update APIs are used on those boards. **That assertion has not
been verified against the code**, and it is the thing to check before trusting this arrangement
on hardware.
