# De-duplication sweep 6 — everything not already claimed

**Date:** 2026-08-17. **Branch:** `codex/silabs-c13`. **Status:** analysis only; no source file was
modified.

**Scope.** Five parallel sweeps own boot screen, qrcode, LED runner, the sensor/input family
(touch, buttons, SHT40, BQ27220, nPM1300, buzzer, battery) and uzlib/compression. The transfer
state machines (direct, `0x76`, PIPE `0x80`-`0x82`, NFC `0x83`) are on the roadmap at
`plans/PLAN_MIGRATION_ENDGAME_2026-08-17.md` § 2.3. This report is everything else: config
storage and provisioning, panel geometry and colour tables, pin decoding, panel power policy, the
crypto and radio HALs, the per-target seam implementations, host-test fixtures, and the build
surface.

**Method.** Same-name file diffs across all three targets, then symbol-level searches for the same
logic under different names, then a line-count pass. Every claim below cites a file and line I
opened. Two areas — host tests and the build/config surface — were measured by dedicated sweeps
and the load-bearing claims in each were re-verified here by hand; those are marked
**(re-verified)**.

**Ranking.** `(duplication removed + divergence risk closed) ÷ (seam cost + hardware burden)`.
Findings that close a divergence rank above findings that only remove lines, and findings that
need no new seam rank above findings that need one.

**Two things this sweep did not find.** `shared/` itself is close to duplication-free (§ 20), and
the corpus-runner/profile split in `tests/host/` is a correct design that must not be collapsed
(§ 21).

---

## 1. The Silabs host tests compile against a different `struct od_config` than the firmware

**Rank 1.** Not the biggest line count in this report — it is the only finding that makes a
currently-passing test assert the wrong thing about unmerged code.

`targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:381-392` sets **eleven** definitions on
the BG22 build:

```
OD_CONFIG_MAX_SIZE=2048u  OD_TXQ_SLOTS=3u  OD_CAP_PIPE=0  OD_CAP_PARTIAL=0  OD_CAP_RXQ=0
OD_CONFIG_WITH_TOUCH=0  OD_CONFIG_WITH_BUZZER=0  OD_CONFIG_WITH_WIFI=0  OD_CONFIG_WITH_DATA_EXTENDED=0
OPENDISPLAY_ZLIB_WINDOW_BITS=9  OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=0
```

`tests/host/CMakeLists.txt:49-54` sets **five**:

```
OD_CONFIG_MAX_SIZE=2048u  OD_TXQ_SLOTS=3u  OD_CAP_PIPE=0  OD_CAP_PARTIAL=0  OD_CAP_RXQ=0
```

The four missing `OD_CONFIG_WITH_*=0` flags `#if`-gate five members of the parsed-config aggregate
— `shared/core/od_config.h:125-147` (`touch_controllers`+count, `passive_buzzers`+count,
`data_extended`+flag, `wifi_config`+flag). So `od_shared_silabs` and everything built on it
compiles `shared/core/od_config.c` and `targets/efr32bg22-slc/od_cmd_silabs.c` against a **larger
aggregate with different member offsets** than the firmware does.

It is not only a layout difference, it is a **behaviour** difference. `shared/core/od_config.c:147-155`
returns `OD_CONFIG_APPLY_NOT_BUILT` for config packet `0x28` when `OD_CONFIG_WITH_TOUCH` is 0, and
`:243` counts that into `report->dropped_not_built`. On the real BG22 a touch/buzzer/wifi/
`data_extended` packet is **dropped and counted**; in every Silabs host test it is **stored**.

Five executables are affected — `tests/host/CMakeLists.txt:107` (`od_config_asm_cap_test`), `:122`
(`od_silabs_storage_test`), `:207` (`od_dispatch_silabs_test`), `:470`
(`od_dispatch_corpus_silabs_test`), `:490` (`od_silabs_fault_test`) — including the
`target-production` corpus profile whose whole claim is that it runs the real BG22 hooks.

The comment two lines above the omission asserts the opposite: `tests/host/CMakeLists.txt:45-46`
says "BG22 changes the ABI of cap-sensitive shared structs … compile a real second variant", and
`:457` says the corpus runs "with the same 2 KiB/three-slot/no-PIPE ABI as the target". Both are
true of the five flags that are there and false of the four that are not.

- **Drift or adaptation:** drift, introduced by C13 on this branch. No donor repo has this split.
- **Divergence:** yes — silently wrong, in the direction that makes tests pass.
- **Caught anywhere?** No. `tools/check.sh` has no `OD_CONFIG_WITH_` / `OD_CAP_` /
  `OD_CONFIG_MAX_SIZE` gate; I read all of `silabs_c13_config` (`tools/check.sh:198-224`) and it
  ratchets PSA slots, BGAPI payload and the kernel-absence assumption only.
- **Seam:** one `targets/efr32bg22-slc/profile.cmake` exporting `set(OD_SILABS_PROFILE_DEFS …)`,
  included by both consumers. ~11 lines added, ~5 removed.
- **Lines removable:** 5. **Divergence closed:** one class of false PASS on the whole C13 test
  surface.
- **Hardware burden:** none.

---

## 2. `opendisplay_display_color.c` — the same four functions, one target computing different byte counts

**Rank 2.** 138 lines duplicated across two targets, with a **wire-visible** disagreement, and no
seam needed at all: the file is already pure C with no vendor header.

| copy | lines |
|---|---|
| `targets/nordic-zephyr/src/opendisplay_display_color.c` | 73 |
| `targets/efr32bg22-slc/opendisplay_display_color.c` | 65 |
| `…/opendisplay_display_color.h` (both) | 25 each, **byte-identical** |

The authority is `../Firmware` via `targets/esp32-idf/src/display_service.cpp:1877-1901`
(`directWriteComputeGeometry`) and `:1549-1561` (`getBitsPerPixel`). Three disagreements:

1. **Row padding.** ESP32 sizes every plane row-padded — `((w+7)/8)*h`, `((w+3)/4)*h`,
   `((w+1)/2)*h` (`display_service.cpp:1885,1893-1895`), with a comment saying flat sizing
   "under-counts on width-not-divisible-by-8 panels (e.g. 122-wide EP213), auto-completing before
   the bottom rows are written". Nordic matches
   (`opendisplay_display_color.c:19-20,55-63`). **Silabs is flat**:
   `opendisplay_display_color.c:22-24` returns `(w*h + 7)/8` and `:55-62` divides the pixel count.
   On a 122×250 EP213 the two answers differ by `(16-15.25)*250 = 187` bytes.
2. **GRAY4 is not treated as two planes on Silabs.** `opendisplay_color_is_bitplanes()` includes
   `OD_COLOR_SCHEME_GRAY4` on Nordic (`:33-35`) and excludes it on Silabs (`:31-33`). ESP32's
   direct-write path forces the two-plane total for gray4 explicitly
   (`display_service.cpp:1900-1901`).
3. **GRAY4 start plane.** Nordic returns PLANE_0 (`:41-47`, with a comment that ESP32's
   `getplane()` returns PLANE_1 but its direct-write path starts at PLANE_0 regardless — verified
   at `display_service.cpp:1544` vs `:1918`). Silabs returns **1** (`:47-49`).

There is also a fourth, smaller one: ESP32's `getBitsPerPixel()` returns 4 for
`OD_COLOR_SCHEME_BWGBRY_SPLIT` (scheme 8, `display_service.cpp:1556`); neither Nordic nor Silabs
handles that value, so both fall through to 1 bpp.

Call sites confirm this is live, not dead: `targets/efr32bg22-slc/opendisplay_display.cpp:614,
765-771, 815-817` and `targets/nordic-zephyr/src/opendisplay_display.cpp:596, 796, 997-1003`.

- **Drift or adaptation:** drift. The Silabs file's own header comment (`:5-9`) cites
  `encodeCanvasToByteData (ble-common.js)` as its reference while Nordic's cites
  `py-opendisplay encoding/images.py`; the Nordic form is the one the host actually sends.
- **Divergence:** yes, wire-visible. A direct write to a 122-wide panel on BG22 will complete
  short, or overrun, depending on the scheme.
- **Seam:** none. No vendor header, no target state — the file already only needs
  `opendisplay_structs.h`. Straight promotion to `shared/core/od_color.c` + `od_color.h`, `PURE`
  tier. It is a table, not a state machine, so it does **not** collide with roadmap § 2.3; it is
  an *input* to direct write and would make that promotion smaller.
- **Lines removable:** ~90 of 188 (one implementation plus one header instead of two of each).
- **Hardware burden:** none new — BG22 Gate 2 already has to run direct write. Nordic and ESP32
  keep their present behaviour bit-for-bit, so no re-verification there.

---

## 3. The stored-config record is implemented three times, and `MAX_CONFIG_SIZE` is stated twice per target

**Rank 3.** The largest genuinely-shareable protocol logic left outside the roadmap, and the seam
already exists and was written for exactly this.

All three targets implement the same five-function contract over the same on-flash record —
`[magic:4][version:4][crc:4][data_len:4][data…]`, magic `0xDEADBEEF`, version `1`, and **the same
CRC-32** (reflected, poly `0xEDB88320`, init and final invert), transcribed three times:

| target | file | lines | CRC-32 at |
|---|---|---|---|
| esp32-idf | `src/config_parser.cpp:92-215` + `src/config_parser.h:6-60` | ~124 + 60 | `config_parser.cpp:209-220` |
| nordic-zephyr | `src/opendisplay_config_storage.c` | 106 | `:14-29` |
| efr32bg22-slc | `opendisplay_config_storage.c` | 165 | `:55-70` |

`targets/esp32-idf/hal/od_hal_nvs.h:1-17` already states the split — "The HAL stores an OPAQUE
BLOB. Record framing — magic, version, inner CRC32, length — is the core's business" — and
`:5-6` says the signatures are `docs/SHARED_API_DESIGN.md § od_hal_nvs` verbatim "so promoting the
config subsystem into shared/core later is a repoint rather than a rewrite". The ESP32 code even
anticipates the BG22 constraint in-line (`config_parser.cpp:110-117`: "it is NOT affordable on the
EFR32BG22 … that target will need either a two-key record or a streaming write; do not carry this
buffer across as if it were free"), and BG22's C13 answer is already written
(`opendisplay_config_storage.c:32-41` overlays the assembler and the record in one union).

**The constant is stated twice on every target, and nothing checks the pair.**

| | record cap | assembler cap |
|---|---|---|
| esp32-idf | `src/config_parser.h:7` `MAX_CONFIG_SIZE 4096` | `od_config_asm.h:69-70` default `4096u` |
| nordic-zephyr | `src/opendisplay_config_storage.h:18` `MAX_CONFIG_SIZE 4096` | default `4096u` |
| efr32bg22-slc | `opendisplay_config_storage.h:9` `MAX_CONFIG_SIZE 2048` | `opendisplay-bg22.cmake:382` `OD_CONFIG_MAX_SIZE=2048u` |

On BG22 the two are the *same buffer* by union (`opendisplay_config_storage.c:37-41` asserts
matching offsets and sizes), so a change to one and not the other is a static-assert failure
there. On ESP32 and Nordic nothing ties them. Related staleness:
`targets/esp32-idf/hal/od_hal_nvs.h:34-37` still hardcodes
`OD_HAL_NVS_MAX_RECORD 4160u` and its comment still says "4096 on every target since 2026-07-25",
which decision 12 reversed on 2026-08-17.

Separate memory note, ESP32 only: `config_parser.cpp:86` `configScratch[4096]`, plus two more
`static uint8_t blob[4112]` at `:119` and `:155` — **12.3 KB of `.bss`** for one config subsystem,
where the second `blob` exists only because save and load each wanted a contiguous staging area.

- **Drift or adaptation:** deliberate per-target backends (LittleFS→NVS, Zephyr settings, NVM3)
  around an accidentally-triplicated core.
- **Divergence:** two real ones. Nordic's `loadConfig` serves from a RAM cache
  (`opendisplay_config_storage.c:74-81`) that the other two do not have, and Nordic's `saveConfig`
  commits that cache only after the write succeeds (`:53-58`) — a correctness property neither
  other target states. ESP32's `clearStoredConfig` also resets the in-RAM config and WiFi
  credentials (`config_parser.cpp:131-144`); the other two only delete the record.
- **Seam:** `od_hal_nvs.h` promoted to `shared/hal/`, plus a BG22-shaped extension for the
  partial-read path (`nvm3_readPartialData`, `opendisplay_config_storage.c:124,140`) so the 32 KB
  target is not forced to stage a whole record. `MAX_CONFIG_SIZE` collapses into
  `OD_CONFIG_MAX_SIZE`.
- **Lines removable:** ~180 of ~455, plus 8 KB of ESP32 `.bss` if the staging blobs collapse into
  `configScratch`.
- **Hardware burden:** high — config persistence is a Gate 2 row on all three targets. Do it as
  its own unit, after C13's hardware gate, not folded into anything else.

---

## 4. The config pin byte is decoded four different ways

**Rank 4.** Small line count, pure logic, no seam required, and a real behavioural split on a wire
field.

`targets/nordic-zephyr/src/od_pin_codec.h:7-8` says it outright: *"Pure wire-format decoders. They
intentionally do not inspect devicetree or GPIO readiness, which keeps the encoding contract
host-testable."* The implementation (`od_pin_codec.c`, 46 lines) includes nothing but `<stddef.h>`.
It is already `shared/core`-shaped and is not there.

Four decoders of the same config-supplied `uint8_t`:

| site | form |
|---|---|
| `nordic-zephyr/src/od_pin_codec.c:7-22` `od_pin_decode_absolute` | port `= cfg>>5`, pin `= cfg&0x1F` |
| `nordic-zephyr/src/od_pin_codec.c:24-46` `od_pin_decode_packed` | bit7 set → port `(cfg>>5)&3`, pin `cfg&0x1F`; else port `cfg>>4`, pin `cfg&0x0F` |
| `efr32bg22-slc/panel/od_bbep_efr32_io.inl:99-114` `silabs_bb_pin_decode` | port `cfg>>4`, pin `cfg&0x0F`, **no bit-7 form** |
| `efr32bg22-slc/opendisplay_display.cpp:61-77` `decode_pin` | byte-for-byte the same as the line above — a second copy inside the same target |

Consequence: a config byte with bit 7 set decodes to `(port, pin)` on an nRF54 and is **rejected**
on BG22 (`pr` lands ≥ 8, above `GPIO_PORT_MAX`). Whether BG22 should accept the packed form is a
product question; that the two disagree silently is not.

The sentinel is stated three more times: canonical has it
(`shared/protocol/opendisplay_structs.h:299` `OD_PIN_UNUSED 0xFFu`, and `:298` says explicitly it
"was `GPIO_PIN_UNUSED` in the repos"), yet `nordic-zephyr/src/od_pin_codec.c:5` defines a private
`OD_PIN_UNUSED 0xFFu` and `efr32bg22-slc/opendisplay_constants.h:24` defines
`GPIO_PIN_UNUSED 0xFF`. The Nordic copy of that same constants header
(`nordic-zephyr/src/opendisplay_constants.h:3-15`) carries a rule against exactly this.

- **Drift or adaptation:** drift. ESP32 needs none of it (its pin field is a raw GPIO number —
  `hal/od_hal_gpio.c:18-21`), so this is a two-target contract that got written twice and a half.
- **Divergence:** yes, behaviour-visible on a config a host can send today.
- **Seam:** none. `shared/core/od_pin_codec.{c,h}`, `PURE` tier; targets keep the
  `port → device`/`port → GPIO_Port_TypeDef` mapping, which is the only part that is chip-specific.
- **Lines removable:** ~35 (Silabs's second copy plus one of the two identical decoders), and the
  encoding gains one host test instead of zero.
- **Hardware burden:** low on Nordic (no behaviour change); a BG22 pin-decode change is a Gate 2
  panel-bring-up item, so gate it behind an explicit decision about the packed form.

---

## 5. Panel rail policy: BG22 drives a guessed pin and waits zero milliseconds

**Rank 5.** Almost no lines to remove; a first-flash failure mode on the one target that has never
been flashed.

`targets/nordic-zephyr/src/opendisplay_display.cpp:132-220` is a faithful port of the reference
`pwrmgm(true)` and says so (`:98-118`, citing `Firmware/src/main.cpp:1341-1382`, and ESP32's own
`display_service.cpp:342-348` confirms "asserts the panel rail, waits 800 ms for it to settle").
`targets/efr32bg22-slc/opendisplay_display.cpp:105-126` is the same function, and differs in three
ways:

1. **No rail settle.** Nordic waits `OD_PANEL_RAIL_SETTLE_MS` 800 ms then
   `OD_PANEL_PIN_SETTLE_MS` 100 ms (`nordic …:200,214`). Silabs sets the pin and returns. The
   Nordic comment (`:100-113`) describes precisely the symptom this produces: init runs against an
   unpowered controller, `bbepIsBusy()` reads idle, the whole frame is clocked into a dead panel,
   and the only visible failure is a 60 s "BUSY NEVER ASSERTED" at refresh time.
2. **Fallback pin.** Silabs `:44-46` defines `OD_FALLBACK_DISPLAY_PWR_PIN 0x00` and `:118-120`
   substitutes it when `pwr_pin == 0xFF`. Nordic `:154-162` handles `0xFF` as "rail is permanently
   powered, there is no pin", and states the rule: *"What must never happen is substituting a
   fallback pin number; driving a guessed pin is the unsafe case."* `0x00` decodes to port A pin 0.
3. **No safe-mode gate.** Nordic refuses power-up in watchdog safe mode (`:141-145`). BG22
   declines `HAL_WDT` entirely, so this one is consistent with its build, not a defect.

The same file pair carries a third copy of the same shape: `wait_for_refresh`
(`nordic …:226-269`, 44 lines; `silabs …:562-578`, 17 lines). Identical algorithm, identical
`saw_busy` exit condition, 50 ms poll on both. Silabs has **none** of the four diagnostic lines
Nordic added — and Nordic's comment (`:229-239`) says those lines exist specifically to
distinguish "the panel is talking to us" from "we are bit-banging into the void", which is the
question a first BG22 flash will ask. ESP32's equivalent (`display_service.cpp:727-751`) polls at
10 ms and reports the elapsed time.

- **Drift or adaptation:** drift, inherited from `../Firmware_Silabs` and never reconciled.
- **Divergence:** behaviour-visible; two of the three are plausible first-flash failures.
- **Seam:** the settle policy could become a shared `od_panel_power_policy` taking an
  `od_hal_time`-style clock, but on a superloop target an 800 ms *blocking* wait is the wrong
  shape — BG22 would need a state, not a delay. Recommend recording the divergence and fixing
  BG22 in place rather than promoting.
- **Lines removable:** ~10. **Divergence closed:** the guessed-pin write and the missing settle.
- **Hardware burden:** this *is* the BG22 hardware gate.

---

## 6. `od_hal_crypto` is the same PSA implementation twice

**Rank 6.**

| target | file | lines | backend |
|---|---|---|---|
| nordic-zephyr | `src/od_hal_crypto.c` | 260 | `psa_aead_*`, shortened-tag CCM |
| efr32bg22-slc | `od_hal_crypto.c` | 195 | `psa_aead_*`, shortened-tag CCM (CRYPTOACC driver) |
| esp32-idf | `hal/od_hal_crypto.c` + `hal/od_hal_crypto_random.c` | 181 + 72 | mbedTLS CCM; PSA for RNG only |

Nordic and Silabs are the same code: same `OD_PSA_CCM_ALG` macro
(`nordic:11` / `silabs:11`), same slot table, same `psa_import_key` with the
`PSA_KEY_USAGE_ENCRYPT|DECRYPT` policy, same `psa_aead_encrypt`/`psa_aead_decrypt` calls, same
`PSA_ERROR_INVALID_SIGNATURE → OD_HAL_CRYPTO_AUTH_FAILED` mapping. The differences are identifier
spelling (`s_slots`/`s_slot_ready` vs `s_keys`/`s_ready`, `crypto_init_once` vs `init_once`), the
log call (`od_log_error` vs `printf`), and comment volume.

Two small semantic splits worth recording either way: Silabs treats `PSA_ERROR_INVALID_HANDLE` from
`psa_destroy_key` as success (`silabs:29-35`) and Nordic does not
(`nordic:66-86`); and Nordic's `slot_release` returns a status that `key_set` propagates, where
Silabs folds it into the same guard expression — same outcome, different failure attribution in
the log.

The third target is deliberately not on PSA, and the reason is written down:
`targets/esp32-idf/hal/od_hal_crypto_random.c:13-16` — *"THE AEAD STAYS ON CLASSIC MBEDTLS …
that is a second crypto-backend migration with its own hardware gate."* Respect that; this is a
two-target merge, not three.

- **Drift or adaptation:** independent transcription of the same PSA sequence, two weeks apart.
- **Divergence:** none wire-visible. The `INVALID_HANDLE` split can only change a log line.
- **Seam:** `psa/crypto.h` is an Arm standard header, not a vendor one, but `tools/check.sh`'s
  boundary greps would still have to be told that — and a `shared/hal/` directory holding an
  *implementation* is a new category. The cheaper form is a shared `.c` under
  `targets/_common/` or an `.inl` both targets include, plus one `od_hal_crypto_log(const char*)`
  seam. Blocked on the same decision as § 7.
- **Lines removable:** ~180 of 455.
- **Hardware burden:** medium. Nordic's PSA arm is silicon-proven (nRF52840, 2026-08-15); BG22's
  is not. Merging before BG22's Gate 2 means a first-flash failure has two candidate causes.

---

## 7. There is no shared logging HAL, and it is the keystone under five separate duplications

**Rank 7.** The largest *aggregate* line count in this report, all of it blocked on one decision.

`targets/esp32-idf/src/od_watchdog_app.cpp:2-6` states the dependency explicitly:

> *"this file is deliberately the same module line for line as
> `targets/nordic-zephyr/src/od_watchdog_app.c` apart from the clock and the absence of a lock.
> KEEP THE PAIR IN STEP. They are two copies because `shared/` has no logging HAL and
> `od_watchdog` therefore cannot emit the boot report itself; the day one lands, both collapse
> into it."*

What is waiting on it:

| duplicate | copies | lines | what actually differs |
|---|---|---|---|
| `od_watchdog_app.{c,h}` | esp32 164+48, nordic 196+64 | **472** | the clock, a Zephyr spinlock, four log strings |
| `od_session_app.*` | esp32 136, nordic 136, silabs 63 | **335** | the clock, the device-id source, the log wording, and the rate-limit budget count (2 vs 1) |
| `od_rxq_app.*` | esp32 69, nordic 43 | **112** | log wording, plus ESP32's hex line and encrypted/plaintext token |
| `od_txq_app_dropped` | 3 (`esp32-idf/src/od_session_app.cpp:128`, `nordic-zephyr/src/od_hal_radio.c:66`, `efr32bg22-slc/od_hal_radio.c:50`) | ~20 | log wording only |
| `od_log_hex_line` | 2 (`esp32-idf/src/od_log.cpp:305-323`, `nordic-zephyr/src/od_log.c:96-117`) | ~40 | nothing — same algorithm, same 32-byte cap, same `" ..."` suffix |
| `od_log.h` level ladder | 2 (`esp32-idf/src/od_log.h:17-38`, `nordic-zephyr/src/od_log.h:12-38`) | ~35 identical | the transport half genuinely differs |

`od_log_hex_line` deserves a line of its own: **both** copies carry a comment saying the two copies
must render identically (`nordic:92-95`: "the RX and TX sides of a link must dump a frame
identically or the two directions quietly drift apart"; `esp32:298-303`: "two copies of this loop
is how the two directions drift apart"). It is a pure `snprintf` loop with no target dependency at
all and should be in `shared/core` today, log HAL or not.

The third logging idiom is bare `printf` — BG22 has no `od_log` (`grep` finds none), it writes
`printf("[OD] …\r\n")` from ~40 sites through `od_rtt.c:10-24`'s `_write` override into SEGGER
RTT. So the shared logger has to accommodate three back-ends, one of which is a `newlib` stdio
hook.

`targets/esp32-idf/hal/od_hal_log.h:23-33` has already worked out the hard part and says why it
stopped: the `docs/SHARED_API_DESIGN.md` contract is a one-line sink `void od_hal_log(const char*)`,
which *"cannot express the free-space query, and the free-space query is load-bearing on the nRF
target"* — so the narrowing is *"a decision to make when the logger is promoted and both targets
are in front of you"*. That decision is now three targets wide and is the gate on ~1,000 lines.

- **Drift or adaptation:** deliberate and documented, but the documentation is now the argument
  for closing it.
- **Divergence:** two behavioural ones hiding in the duplication. Nordic serialises
  `od_watchdog` on a spinlock because `main()` feeds while the display work queue stamps
  breadcrumbs (`od_watchdog_app.c:26-37`); ESP32 does not, and its header pins the reason
  (`od_watchdog_app.h:11-15`). And Silabs's `od_session_app` has **one** rate-limit budget
  (`od_session_app.c:12`) where both others have **two** (`nordic:22-23`,
  `esp32:79-81`), with a comment on each saying why one budget is wrong: *"a stale client
  spamming session-id mismatches must not be able to silence the out-of-window line, which is the
  one that reports real transfer loss."* That is a diagnostic regression on BG22, in code that has
  never been flashed.
- **Seam:** `shared/hal/od_hal_log.h`. Not a small decision — it is a fourth shared HAL and it has
  to satisfy a Zephyr deferred logger, an IDF UART/stdout port with a drop budget, and a newlib
  `_write` on a superloop.
- **Lines removable:** ~450-500 across the five, conservatively.
- **Hardware burden:** low per-target (log wording is not a wire contract), but broad — every
  target's boot log changes at once, which is exactly the artefact a Gate 2 transcript is read
  from.

---

## 8. `targets/esp32-idf/lib/uzlib/src/` is a byte-identical second copy of `third_party/uzlib/src/`

**Rank 8.** 947 lines, four path edits. **(re-verified: `diff -rq` between the two directories
returns nothing.)**

Nordic and Silabs compile from `third_party/uzlib/src/`
(`nordic-zephyr/zephyr/CMakeLists.txt:105,251`; `efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake:272,315`).
ESP32 compiles from its own copy (`targets/esp32-idf/main/CMakeLists.txt:28-32,156`), whose
comment (`:25-27`) says `od_zlib_stream.c` "is compiled from `lib/` for now so that the promotion
is a separate, revertable commit". Two thirds of that promotion has since happened.

`targets/nordic-zephyr/zephyr/CMakeLists.txt:7-9` argues against exactly this state: importing a
per-target copy *"would have made this repo's 'one vendored copy for all targets' rule a fiction"*.

I am flagging this rather than analysing it: the compression subsystem belongs to the uzlib sweep,
and the 723-line `od_zlib_stream.c` inside both copies is its call. What is squarely in scope here
is that **a divergence between the copies would be silently wrong** — one target's inflater would
differ from the other two with no build error and no `tools/check.sh` gate — and the fix is four
path edits in one file.

- **Lines removable:** 947. **Seam:** none. **Hardware burden:** the ESP32 build must be rerun;
  the bytes compiled are identical today, so a diff of the two `.o` sets is the whole gate.

---

## 9. Host-test fixtures: one link seam written nine times, one assertion header written 25 times

**Rank 9.** ~625 removable lines, no production code touched, no hardware. **(re-verified: the
seam symbol counts and the 25 `#define CHECK(` copies were confirmed by grep; `gate_test.c:34-45`
read directly.)**

**9a. The `od_hal_radio` / `od_session_app` / `od_txq_app_dropped` link seam — 456 lines across
nine files.** Every executable that links `od_session` or `od_dispatch` re-writes it:
`gate_test.c:34-90` (the reference), `reply_test.c:34-97`, `dispatch_test.c:42-95`,
`config_read_test.c:34-86`, `core_reset_test.c:42-97`, `corpus_runner.c:47-98`,
`dispatch_route_test.c:41-84`, `txq_test.c:33-95`, `silabs_fault_test.c:36-48`. Between 39 % and
78 % of each block is byte-identical to `gate_test.c`'s, comments included. The variation is a
handful of knobs (`g_tag_live`, `g_radio_stalled`, a scripted result array) that each file already
declares as file statics. Folding a superset into the existing `od_session_fake` library
(`tests/host/CMakeLists.txt:243`) removes ~300; `txq_test.c` and `rxq_test.c` keep their own
scripted radio because they *are* the drain suite.

**9b. `CHECK` / `CASE` / `g_checks` — 341 lines across 25 files**, 22 of which normalise to the
same 10-line block. The three variants are `rxq_test.c:16-27` (names the counter `g_fails`, so a
copy-paste from any other suite into it does not compile), and the two Silabs suites. A 15-line
`tests/host/od_check.h` removes ~300 including the identical two-line `main()` tail.

**9c. `nordic_cmd_device_test.c:80-141` re-fakes 12 symbols that `fake_nordic/fake_nordic.c`
already defines** (`opendisplay_ble_get_app_version` at `:93` vs `fake_nordic.c:228`, and eleven
more; `:126-131` is verbatim `fake_nordic.c:286-288` down to the `fake_nvic_resets` name). The
CMake target (`CMakeLists.txt:355-362`) deliberately omits the fake. Worse than duplication: the
two disagree on defaults (`0x0102` here vs `0x0105` in the fake). ~55 lines.

**9d. `_od_log` fake ×4** — `fake_nordic.c:290`, `nordic_cmd_device_test.c:135`,
`nordic_session_app_test.c:63` are the same body; `nordic_crypto_slot_test.c:44` earns its variant
(it counts errors for an assertion). ~25 lines.

**9e. `tests/host/CMakeLists.txt`** — 581 lines, 262 of code, of which 28 executables follow the
identical `add_executable` / `target_link_libraries` / `add_test` triplet (~90 lines). The
`foreach()` pattern that fixes this already exists in the repo at
`tests/fuzz/CMakeLists.txt:20-36`. ~80 removable, with all 319 comment lines preserved verbatim —
they are the file's actual value.

- **Divergence:** 9c is a live one (two Nordic fakes, two app versions).
- **Hardware burden:** none.

---

## 10. `opendisplay_epd_map.c` — the panel-ID table, three copies, divergent tail

**Rank 10.**

| copy | lines | cases |
|---|---|---|
| `targets/esp32-idf/src/display_service.cpp:624-706` (`mapEpd`) | 83 | `0x0000`-`0x004C` |
| `targets/nordic-zephyr/src/opendisplay_epd_map.c` | 94 | `0x0000`-`0x004C` |
| `targets/efr32bg22-slc/opendisplay_epd_map.c` | 75 | `0x0000`-`0x0041` |

The two `opendisplay_epd_map.h` files are byte-identical (14 lines each). The `.c` bodies agree on
all 66 entries up to `0x0041`; the diff is entirely the tail. ESP32 maps `0x0043`-`0x004C` to real
panels; Nordic maps four of them to `EP_PANEL_UNDEFINED` and substitutes two with near-neighbours,
with a comment explaining that the definitions are not in the vendored `bb_epaper`; Silabs stops
at `0x0041` and returns `EP_PANEL_UNDEFINED` for everything above.

- **Drift or adaptation:** the Nordic tail is documented adaptation (missing vendored panels). The
  Silabs tail is drift — its map predates the entries.
- **Divergence:** behaviour-visible but benign in direction — a device with an unmapped panel ID
  refuses rather than mis-drives.
- **Seam:** the mapping is protocol-ID → `bb_epaper` panel enum, so the table needs
  `bb_epaper.h` and cannot live in `shared/` under the one rule. The right home is
  `third_party/bb_epaper/`-adjacent, or a target-side table generated from one list. Decision 13
  keeps `third_party/` one copy; that argues *for* the table living beside it, not against.
- **Lines removable:** ~150 of 252 if one table is shared; 0 if the panel-availability differences
  must stay expressible per target — in which case make the differences a `#if`, not three files.
- **Hardware burden:** low (a table lookup), but every panel that changes mapping needs a board.

---

## 11. `od_hal_time` never became a shared HAL, and three targets have three clocks

**Rank 11.** Prerequisite for several items above; cheap on its own.

`targets/esp32-idf/hal/od_hal_time.h:3-6` says it was written to be promoted: *"Signatures are
`docs/SHARED_API_DESIGN.md § od_hal_time` verbatim … Written to that contract now so the eventual
promotion to `shared/hal` is a repoint rather than a rewrite — the same bet `od_hal_nvs` took."*
`shared/hal/` holds four headers and this is not one of them.

| target | clock | delay |
|---|---|---|
| esp32-idf | `od_hal_uptime_ms()` (`hal/od_hal_time.h:30`), 24 call sites | `od_hal_delay_ms/us` |
| nordic-zephyr | `od_uptime_get_32()` (`src/od_zephyr_compat.h:12`, impl `od_zephyr_time.c:11`) | `od_msleep`, `od_busy_wait` |
| efr32bg22-slc | `sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count())`, open-coded at `od_session_app.c:26` and `opendisplay_display.cpp:48` | `sl_sleeptimer_delay_millisecond` / `sl_udelay_wait`, superloop |

The three functions are the same three functions under three names. **Promote only the clock.**
`od_hal_delay_ms` is the wrong shape for a superloop target — BG22's whole design is
`sl_power_manager` plus a cooperative loop, and a shared blocking delay in `shared/` would be an
invitation to write one into shared code. A one-function `od_hal_uptime_ms()` is universally
implementable and is what `od_cmd_reply`, `od_session_app`, `od_watchdog_app` and `od_log` all
actually need.

- **Lines removable:** ~30 directly (the Nordic compat shim collapses), but it unblocks § 7 and
  § 17.
- **Seam cost:** one header, three one-line implementations. **Hardware burden:** none.

---

## 12. `logo_bitmap.h` — 1,176 lines, 111,510 bytes, byte-identical on two targets

**Rank 12.** The single largest exact duplicate outside vendored trees.

`targets/esp32-idf/src/logo_bitmap.h` and `targets/nordic-zephyr/src/logo_bitmap.h` are identical
(`cmp` reports no difference; both 111,510 bytes). Both are generated —
`:2` "Auto-generated by `tools/convert_logo.py` — do not edit by hand" — and hold five scaled
`static const uint8_t BOOT_LOGO_BITMAP_S*` tables. BG22 has none.

The *rendering* is the boot-screen sweep's; the **asset** is a table, which this brief asks about.
Being `static const` in a header, each translation unit that includes it gets its own copy unless
the linker folds it — worth checking the ESP32 and Nordic maps.

- **Drift or adaptation:** neither. It is one generated artefact checked in twice.
- **Divergence:** none today, and the generator makes recurrence likely rather than certain.
- **Seam:** `third_party/`-style shared asset directory, or generate at build time into the build
  tree. Decision 13's "one vendored copy for all targets" reasoning applies verbatim.
- **Lines removable:** 1,176. **Hardware burden:** none — identical bytes.
- **Coordinate with the boot-screen sweep before acting.**

---

## 13. `factory_config` — the same provisioning check, and a fourth CRC-16

**Rank 13.**

`targets/nordic-zephyr/src/factory_config.{c,h}` (80+37) and
`targets/esp32-idf/src/factory_config.{cpp,h}` (60+23) are the same three functions: the toolbox
outer CRC-16 with the two length bytes forced to zero, a packet validity check, and
`tryProvisionFactoryEmbed()`. Silabs has no factory provisioning at all.

The CRC is the fourth in-tree implementation of CRC-16/CCITT:

| site | variant |
|---|---|
| `shared/core/od_config_tlv.c:119-158` `od_config_tlv_crc16` | **the promoted one**, length bytes zeroed |
| `nordic-zephyr/src/factory_config.c:14-31` | identical algorithm, local copy |
| `esp32-idf/src/factory_config.cpp:5-18` | identical algorithm, local copy |
| `esp32-idf/src/communication.cpp:275-288` `calculateCRC16CCITT` | plain CRC-16, **no** zeroed length bytes; declared twice (`communication.h:9`, `main.h:202`) |

`targets/esp32-idf/src/config_parser.cpp:205-208` records that the toolbox CRC-16 *was* deleted
from that file when it was promoted, "rather than left beside it: a local copy of a promoted
function is dead code that still compiles". The same argument applies to the two in
`factory_config`, and the Nordic copy says so in its own comment (`:6-12`: "Kept local so this
file has no dependency on the parser's static helper") — a dependency that no longer needs
avoiding, since `od_config_tlv.h:95` exports it.

- **Divergence:** the log strings differ ("saved to settings" vs "saved to filesystem"), and the
  ESP32 failure line is `od_log_error` where Nordic's is `od_log_info`. Cosmetic.
- **Seam:** `factory_flash_cfg_t` embeds `data[MAX_CONFIG_SIZE]` (`factory_config.h:11-15` on
  both), so this rides on § 3's `OD_CONFIG_MAX_SIZE` unification. Otherwise the logic is pure.
- **Lines removable:** ~90 of 200, plus the two orphan CRC copies (~30).
- **Hardware burden:** factory provisioning is a first-boot path; low risk, needs one board.

---

## 14. `od_cmd_reply` and the reply-shape helpers

**Rank 14.**

`targets/nordic-zephyr/src/od_cmd_reply.c` (48) and `targets/esp32-idf/src/od_cmd_reply.cpp` (49)
are semantically identical: two null-guarded forwards to `od_reply`/`od_reply_plain`, plus
`od_cmd_flush_before_refresh()` with the same 250 ms deadline, the same 5 ms poll, the same
watchdog service inside the loop, and the same wrap-safety comment. The only difference is
`k_uptime_get_32()`/`k_msleep` vs `od_hal_uptime_ms()`/`od_hal_delay_ms`. Silabs has neither file —
`targets/efr32bg22-slc/od_cmd_silabs.c:54` uses local `reply`/`reply_plain` helpers instead.

The two headers (57 and 68 lines) say the same thing at different lengths and disagree about
history: `nordic …:9-14` attributes the byte-inference defect to "this target", `esp32 …:9-13`
attributes it to both, correctly.

- **Seam:** `od_hal_uptime_ms` (§ 11) plus a bounded-wait decision for the superloop target — BG22
  cannot block for 250 ms, so `od_cmd_flush_before_refresh` is the one part that genuinely does not
  generalise. Split it: the two forwards go shared, the barrier stays per-target.
- **Lines removable:** ~60 of 222.
- **Hardware burden:** the barrier is what stops a host aborting a completed transfer; changing it
  is a Gate 2 item on both targets that have one.

---

## 15. The bb_epaper IO backend: the top half is identical, the bottom half is the driver

**Rank 15.**

Three backends implement the same twelve-function contract:
`targets/esp32-idf/panel/od_bbep_idf_io.inl` (535), `targets/nordic-zephyr/panel/od_bbep_zephyr_io.inl`
(329), `targets/efr32bg22-slc/panel/od_bbep_efr32_io.inl` (372).

The **top** half is chip-specific and must stay: SPI setup, GPIO primitives, `delay`, `millis`.
The **bottom** half is not. `bbepWriteCmd`, `bbepWriteData`, `bbepWriteCmdData`, `bbepCMD2`,
`bbepSetCS2`, `od_bbep_cs`, `bbepWriteIT8951Cmd/Data/CmdArgs` are DC/CS sequencing over one SPI
primitive, and the Nordic and Silabs versions are the same code:

- `nordic …:271-329` vs `silabs …:314-372` — 59 lines each, and the *only* semantic difference is
  `bb_spi_write(...)` vs `SPI_Write(...)`. Everything else is indentation.
- `nordic …:134-152, 243-270` vs `silabs …:187-205, 284-313` — the CS helper and the three IT8951
  entry points, likewise identical modulo the same rename and one `uint8_t ucTemp[4]` initialiser
  style.

The same applies to the wrapper pair. After stripping comments,
`nordic-zephyr/panel/od_bbep_zephyr.h` is **19 code lines** and `efr32bg22-slc/panel/od_bbep_efr32.h`
is **20**, differing in 5 (the include guard, plus one extra `bbepSetCS2` declaration); the two
`.cpp` files are **19 code lines each**, differing in 2 (the IO-backend and header filenames). Both
files' own comments argue for this: `od_bbep_efr32.h:4-7` — *"THIRD TARGET ON THIS PATTERN, and
deliberately not a third variation of it … all three now integrate bb_epaper the same way, so a
re-vendor has one integration to re-check rather than three."* Three copies of the integration is
three things to re-check.

- **Drift or adaptation:** the split is adaptation; the identical bottom half is duplication that
  followed it.
- **Divergence:** none found. The IT8951 block may be dead on both Nordic and BG22 — no IT8951
  panel ID appears in either `opendisplay_epd_map.c` — **unverified**.
- **Seam:** one `od_bbep_io_common.inl` beside the vendored library, taking the SPI primitive as a
  macro; plus `#define OD_BBEP_IO_BACKEND "…"` for the wrapper pair. Decision 13 exempts
  `third_party/` from the one rule and requires one vendored copy — a shared *integration* file is
  consistent with both.
- **Lines removable:** ~120 from the `.inl` files, ~38 code lines from the wrappers.
- **Hardware burden:** high in kind, low in extent — this is the byte path to the panel. Any change
  needs a panel refresh on each affected board, but the change is a rename.

---

## 16. Wire encoders written once per target: `FIRMWARE_VERSION`, `READ_MSD`, and the 4-byte NACK

**Rank 16.**

`od_cmd_app_firmware_version` builds `[ACK][0x43][major][minor][shaLen][sha…][patch]` in three
places: `targets/esp32-idf/src/communication.cpp:341-359`,
`targets/nordic-zephyr/src/od_cmd_device.c:58-87`, `targets/efr32bg22-slc/od_cmd_silabs.c:69-86`.
All three cap the SHA at 40, all three put the patch byte last for the documented
old-host reason, and two of them carry the same comment about it verbatim. `od_cmd_app_read_msd`
is nine lines, three times (`communication.cpp:263-271`, `od_cmd_device.c:89-100`,
`od_cmd_silabs.c:88-95`).

The 4-byte hard NACK `{RESP_NACK, opcode, err, 0x00}` is hand-built at roughly 15 sites across the
three targets (`grep` finds 58 `RESP_NACK` references overall; the 4-byte shape dominates). There
is no `od_reply_nack()` in `shared/core/od_reply.h` — `od_gate.c:16-20` and `od_dispatch.c:95-106`
each build their own 3-byte control frame instead.

Local redefinitions of canonical constants, found while counting:

- `targets/nordic-zephyr/src/opendisplay_pipe_write.cpp:22-23` defines `RESP_ACK 0x00u` and
  `RESP_NACK 0xFFu` — while including `opendisplay_protocol.h` at `:6`, which defines both at
  `:702-703`. Same values today. The file's own comment at `:25-29` warns against precisely this
  pattern for the two structs it stopped shadowing.
- `targets/efr32bg22-slc/opendisplay_runtime.h:30-35` defines six `SECURITY_FLAG_*` macros that
  canonical carries as `OD_SECURITY_FLAG_*` at `shared/protocol/opendisplay_structs.h:902-907`,
  with the same values. ESP32 (`config_parser.cpp:335-338`, `boot_screen.cpp:156`) and Nordic
  (`boot_screen.cpp:222`) both use the canonical spelling; Silabs
  (`opendisplay_display.cpp:397`) uses the local one.
- `targets/efr32bg22-slc/opendisplay_constants.h:24` `GPIO_PIN_UNUSED` and
  `targets/nordic-zephyr/src/od_pin_codec.c:5` `OD_PIN_UNUSED` — both shadow
  `opendisplay_structs.h:299`. See § 4.
- `targets/efr32bg22-slc/opendisplay_constants.h:21` names bit `0x29` `CONFIG_PKT_BUZZER` where
  Nordic names it `CONFIG_PKT_PASSIVE_BUZZER` (`:21`), and `:34` names transmission-mode bit 0
  `TRANSMISSION_MODE_ZIPXL` where Nordic names it
  `TRANSMISSION_MODE_STREAMING_DECOMPRESSION` (`:41`). Same bits, different names, and the Silabs
  header is missing bits 2, 4 and 7 that Nordic defines.

`targets/nordic-zephyr/src/opendisplay_constants.h:3-15` is the rule these all break, and it is
worth quoting because it explains why this class is not cosmetic: *"`OD_BUS_TYPE_I2C` is why this
is a rule and not a tidy-up. Canonical declares it as an ENUMERATOR; a macro of the same spelling
textually rewrites the enumerator and the header fails to compile … So: do not add a definition
here for anything `shared/protocol/` names, even with a matching value."*

- **Divergence:** none in values today. Every one of these is a fuse, not a fire.
- **Seam:** none for the redefinitions — delete and repoint. For the encoders: `od_cmd_app.h` hooks
  would each gain a shared body plus a target accessor for the version/SHA/MSD bytes.
- **Lines removable:** ~60 for the encoders, ~20 for the redefinitions.
- **Hardware burden:** the `0x43` response is the one command a client can issue before it can
  authenticate; C11 already found it silently broken on ESP32 once. Any change here needs a wire
  test, not just a build.
- **A `tools/check.sh` grep would prevent recurrence** — "no target file defines a macro whose
  name, with or without the `OD_` prefix, is defined in `shared/protocol/`".

---

## 17. `od_hal_radio` — three implementations, and one returns the wrong verdict class

**Rank 17.** Low line count, one real defect.

`shared/hal/od_hal_radio.h:42-44` defines the three failure verdicts, and `shared/core/od_txq.c`
treats them very differently: `OD_RADIO_GONE` at `:191-192` calls `report_dropped_tag`, which drops
**every queued entry for that tag** (`:148`); `OD_RADIO_ERROR` at `:196-200` drops **only the
current entry**.

`targets/efr32bg22-slc/od_hal_radio.c:20-24` returns `OD_RADIO_GONE` for
`frame == NULL || len == 0u`. Nordic (`od_hal_radio.c:14-16`) and ESP32
(`hal/od_hal_radio.cpp:49-51`) both return `OD_RADIO_ERROR` for the same condition. A malformed
call on BG22 therefore flushes the entire outbound queue for a live connection; on the other two it
drops one frame.

Second split, this one documented on both sides. When the peer is connected but not subscribed,
Nordic (`:29-33`) and ESP32 (`:78-82`) return `OD_RADIO_RETRY` with the same comment —
*"the CCCD write may still arrive, and dropping here would lose the first response of every
session"*. Silabs returns `OD_RADIO_ERROR` (`:26-29`) with its own reason: retrying would deadlock
the BGAPI event that would change the subscription state. Both arguments are sound; the outcomes
differ and only one target loses the first response of a session.

- **Drift or adaptation:** the subscription arm is adaptation; the `GONE`-for-malformed is drift.
- **Seam:** none — this is a per-target HAL and should stay one. Fix the verdict; add a
  `tools/check.sh` grep or a host test asserting the mapping table.
- **Lines removable:** ~20 (the three `od_txq_app_dropped` copies, which belong to § 7).

---

## 18. Build surface **(re-verified where load-bearing)**

Three toolchains stay three (decision 3). What follows is duplication *inside* one toolchain, or a
statement of one fact in two files.

**18a. `OD_FASTEPD_BOARDS` states the FastEPD board set a second time.**
`targets/esp32-idf/main/CMakeLists.txt:131-136` lists five board names that
`boards/s3-n16r8.cmake:14`, `s3-n8r8.cmake:11`, `s3-n32r8.cmake:11`,
`s3-n16r8-extuart.cmake:15` and `s3-n16r8-extuart-debug.cmake:19` already state by setting
`OPENDISPLAY_FASTEPD`. `main/CMakeLists.txt:110-121` explains why the second copy exists — IDF
expands component `CMakeLists` in a separate early pass with none of the parent's variables — which
is a real constraint, not an excuse for the copy being unchecked. **A mismatch is silently wrong:**
it flips which `Group5.cpp` wins the collision resolved at `:75-107`, with no error. The file
records this having gone wrong twice already (`:127-131`: `s3-e1004` wrongly present, `s3-n8r8` and
two extuart boards missing). ~8 lines of configure-time cross-check fixes it permanently.

**18b. Nordic has no `OD_TXQ_SLOTS` static assert.** ESP32 asserts both queues against
`PIPE_MAX_W + 2` (`targets/esp32-idf/src/structs.h:65-66` and `:73-74`), with `:67-72` describing
the deadlock the TXQ one prevents. Nordic asserts only the RX ring
(`targets/nordic-zephyr/src/opendisplay_pipe_write.cpp:17`). Two lines.

**18c. ESP32 board fragments — 12 files, 244 lines, 115 of code.**
`OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=1` appears in all 11 boards (unanimous → belongs in
`main/CMakeLists.txt`); `OPENDISPLAY_LOG_UART`+`_RX=44`+`_TX=43` in 4; `OPENDISPLAY_ENABLE_WIFI`
+ `BOARD_HAS_PSRAM` always together in 7. One 11-line comment block is byte-identical in
`c3-n16.cmake:15-25`, `c3-n4.cmake:15-25`, `c6-n4.cmake:20-30`. ~55-70 removable.

**18d. `sdkconfig.defaults.esp32{,c3,c6,s3}`** — 204 lines, 12 of actual `CONFIG_`. Thirty lines
are the byte-identical "Universal MAC address count — A WIRE-VISIBLE FLEET CONTRACT" essay,
repeated four times. The symbol is legitimately chip-prefixed; the essay is not. ~90 removable to
a doc pointer.

**18e. Nordic `boards/xiao_nrf54l15_*.conf` vs `xiao_nrf54lm20a_*.conf`** — only lines 1-4 differ;
the 12-line Channel-Sounding rationale and 12 `CONFIG_BT_*` lines are identical. A dead
`zephyr/prj_cs.conf` (14 lines) already exists for this and `zephyr/CMakeLists.txt:17-27` explains
why it was abandoned. ~38 removable.

**18f. Dead build artefacts.** `targets/esp32-idf/boards/s3-n16r8.panel.cmake` (4 lines) sets
`OD_USE_FASTEPD`, whose only occurrence in the tree is its own definition; nothing includes
`*.panel.cmake`, and `targets/esp32-idf/build.sh:41-48` spends 7 lines excluding it.
`third_party/bb_epaper/CMakeLists.txt` (9 lines) is an `idf_component_register` block with no
`EXTRA_COMPONENT_DIRS` anywhere in the repo, registering the exact file
`main/CMakeLists.txt:107` filters out. `zephyr/CMakeLists.txt:29-35` is self-documented as a dead
branch, with `zephyr/prj_lm20_extra.conf` (5 lines) behind it. ~32 removable.

**18g. The release-manifest header, four times.** `targets/esp32-idf/build.sh:203-221`,
`targets/nordic-zephyr/build.sh:20-37`, `targets/efr32bg22-slc/build-and-flash.sh:386-410`,
`build-release.sh:111-126` each re-implement `git rev-parse --short HEAD || echo unknown` +
`git diff --quiet HEAD || dirty=…` + `date -u` + `mkdir -p release` + `stat -c %s` + the same
`printf '%-Ns %10s'` row form, and each restates the `MANIFEST-<target>.txt` naming rationale. The
columns genuinely differ per target; the header does not. ~35-40 removable via
`tools/release_manifest.sh`.

**18h. Nordic's board roster, three times inside one target.**
`scripts/target-registry.sh:5-32` is the registry; `build.sh:14-18` re-enumerates the same three
board strings as `OD_ALL_BOARDS`; `flash.sh:29-30` hardcodes one of them as a default, and its own
comment at `:18-28` records a past bug caused by exactly that. Plus a byte-identical 5-line
profile→suffix block in `build-nrf52840.sh:10-14` and `build-nrf54.sh:18-22`. ~17 removable.

**18i. `tools/check.sh`** — the three target gates (`:440-448`, `:455-462`, `:465-472`) are the
same `if $DO_X / check / else skip` shape three times, and the target set is stated a fourth time
at `:37` (`--targets`) and a fifth at `build-release.sh:51`. ~15 removable; low priority, and
arguably clearer flat.

**Also noted, not duplication:** `targets/nordic-zephyr/scripts/validate-build.sh` (82 lines) is a
real ratchet that `tools/check.sh` never calls. And of the three toolchain version pins, only
ESP32 has an enforcement ladder (`build.sh:129-145`); Silabs has no version check at all —
`opendisplay-bg22.cmake:38-44` only tests that one SDK file exists, so a different SDK builds
silently under a message naming 2025.12.2.

**`shared/sources.cmake` itself is not duplicated.** All four consumers take the variables rather
than restating them (`esp32-idf/main/CMakeLists.txt:17,63,165`;
`nordic-zephyr/zephyr/CMakeLists.txt:6,269-272,101`; `efr32bg22-slc/…/opendisplay-bg22.cmake:62,289-292,311`;
`tests/host/CMakeLists.txt:40,43,47`). The four tier selections are genuinely four different sets.
Only the 20-line preamble comment is restated four times.

---

## 19. Divergence by absence: idle-session teardown exists on one target

Not a duplication finding, recorded because the sweep looked for the duplicate and there is none.

`targets/esp32-idf/src/session_guard.{h,cpp}` (54+186) is the single teardown routine that
`docs/CONNECTION_POLICY.md` R6 requires, with five documented callers including a transfer timeout
and an idle timeout. Neither `nordic-zephyr` nor `efr32bg22-slc` has an equivalent — both rely on
`od_core_reset()` at disconnect (`nordic-zephyr/src/opendisplay_pipe.c:126`,
`efr32bg22-slc/opendisplay_pipe.c:111`) and have no idle or transfer-timeout teardown at all.
`od_session` owns session expiry; the *link* and *transfer* halves of the policy are ESP32-only.

Worth a decision — it is either a deliberate capability difference or a gap — but it is not a
de-duplication opportunity.

---

## 20. `shared/` itself

5,923 lines across 33 files. I found no meaningful internal duplication.

The only repetition is three small local builders of the same `[b0][cmd_lo][code]` control frame:
`shared/core/od_gate.c:16-20`, `shared/core/od_dispatch.c:95-106`, `shared/core/od_reply.c:18`.
Five lines each, three different semantics (`[00][cmd][FE]`, `[FF][cmd_lo][FE]`,
`[00][cmd][FF]`), and C12 found that collapsing two of those shapes into one was itself the
regression (`docs/DIVERGENCE_MATRIX.md` and the C12 note in CLAUDE.md). **Leave them separate.** If
anything, add a named constructor per shape rather than one generic one.

`od_span.h` and `od_nonce_window.h` correctly have no `sources.cmake` entry (all-inline); the two
pure seam headers `od_cmd_app.h` and `od_session_app.h` likewise. `od_caps.h` (34 lines, new in
C13) is the right shape and its `#error` range checks are the pattern the missing
`OD_CONFIG_WITH_*` checks in § 1 should copy.

---

## What should NOT be de-duplicated

- **The three toolchains** (decision 3). ESP-IDF's `export.sh`, nRF Connect's
  `environment.json` + python resolver, and `slt where` are three genuinely different mechanisms.
  `build-release.sh` is already the correct thin driver over them and needs no change.
- **`od_hal_radio` implementations.** NimBLE, Zephyr BT and BGAPI are three different stacks with
  three different backpressure signals. § 17 is a bug in one mapping, not an argument for merging.
- **Panel drivers, board fragments, devicetree overlays, pinctrl, `sl_*` config headers.** Per-board
  by construction. Nordic's two nRF54 overlays differ in 55 lines of real pin maps.
- **`tests/host/session_ccm_reference.inc`** (140 lines). `:1-13` says it is a frozen verbatim copy
  of the pre-C1 soft CCM kept precisely so it can disagree with `shared/core/od_session.c`.
  Removing the duplicate removes the test.
- **`tests/host/config_tlv_test.c:279-302`** (`ref_feed`/`ref_crc`). An independent transcription
  of CRC-16 written from its description rather than by calling the function under test
  (`:9`, `:279-281`; `CMakeLists.txt:126-129`). Same rule.
- **`tests/host/aes128.{c,h}`** and `session_fake.c`'s `host_cmac` (`:23-79`). Differential
  references validated against FIPS-197 and RFC 4493 vectors, not against implementation output.
- **`tests/host/advert_test.c`.** CLAUDE.md already says it holds the two shipped encoders as the
  differential reference and that they must not be "updated" to match `od_advert`.
- **The corpus runner / three profiles split.** `corpus_runner.c` + `.h` (384 lines) are already
  shared verbatim by all three executables (`CMakeLists.txt:419,431,460`); the profiles
  (`portable` 316, `nordic` 59, `silabs` 33) share only the four-function seam signature and have
  zero body overlap. `corpus_runner.h:1-13` explains why two hook sets cannot share a binary. This
  is the correct design; the only removable thing in it is the § 9a seam copy at
  `corpus_runner.c:47-98`.
- **`tests/host/fake_silabs/` vs `fake_nordic/`.** ~22 overlapping symbol names, genuinely
  different bodies, different knobs. `nvm3_fake.c` and `fake_silabs_bgapi.c` fake vendor APIs
  nothing else touches. Leave them.
- **`third_party/bb_epaper`** (decision 13). One vendored copy, `#ifdef`-selected backend. § 15
  proposes a shared *integration* file beside it, which is consistent with that rule, not an
  exception to it.
- **The three `#if`-gated capability differences** (decision 9). BG22 not paying for PIPE, partial
  write or an RX ring is the point of `od_caps.h`, not duplication.

---

## Open questions, ranked

1. **Does the `od_shared_silabs` layout mismatch (§ 1) invalidate any assertion that has already
   been recorded as evidence for C13?** The C13 note in CLAUDE.md cites "the target-production
   Silabs corpus runs the real BG22 command hooks against fake drivers and passes" and a
   "232-assertion" fault suite. Both were built against the wrong `struct od_config`. Someone has
   to say which of those assertions actually depend on the gated members before the claim stands.

2. **Which byte count is correct for BG22 direct write (§ 2) — and has any real client ever pushed
   an image to one?** If a deployed BG22 fleet exists and works, the flat count is what its hosts
   were validated against and "fixing" it is the wire break. If BG22 has never shipped a display
   push, the row-padded form is simply right. `../Firmware_Silabs` and `py-opendisplay` together
   should settle it; I did not open them for this.

3. **Should `OD_PIN_UNUSED` bit-7 packing be accepted on BG22 (§ 4)?** Promoting `od_pin_codec`
   forces the question: keep two decode policies behind a target macro, or converge. A config
   written for an nRF54 and loaded onto a BG22 currently silently loses pins.

4. **What is the shared logging HAL's contract (§ 7)?** `od_hal_log.h:23-33` deferred it pending
   "both targets in front of you"; there are now three, one of which routes stdio through
   `_write` into RTT and has no room for a drop budget. Until this is answered, ~500 lines stay
   duplicated and the BG22 single-budget rate limiter (§ 7) stays a latent diagnostic regression.

5. **Is a shared PSA implementation of `od_hal_crypto` (§ 6) allowed under the one rule?**
   `psa/crypto.h` is an Arm standard header, not a vendor one, and is not on CLAUDE.md's
   prohibition list — but `shared/hal/` has only ever held interfaces. Either extend the rule
   explicitly or put the shared body under `targets/_common/`. Do not decide this implicitly.

6. **Does the config-storage promotion (§ 3) wait for the transfer units, or precede them?** It is
   protocol logic outside roadmap § 2.3, its HAL was written for it, and BG22's C13 union already
   proves the 32 KB shape. But it is also a Gate 2 row on all three targets, which argues for
   sequencing it with them rather than alongside.

7. **Should `logo_bitmap.h` (§ 12) be generated at build time rather than checked in twice?**
   `tools/convert_logo.py` exists; 223 KB of identical generated bytes in two target trees is
   the alternative. Needs the boot-screen sweep's view before anyone moves it.

8. **Is the IT8951 block in the Nordic and BG22 bb_epaper backends (§ 15) dead code?** No IT8951
   panel ID appears in either target's `opendisplay_epd_map.c`. **Unverified** — I did not trace
   whether `bb_ep.inl` reaches those entry points by another route. If dead, ~60 lines go without
   a seam at all.

9. **Is `OD_APP_VERSION` divergence intentional?** Nordic ships `0x0100`
   (`zephyr/CMakeLists.txt:110`, with a dead identical `#ifndef` fallback at
   `src/opendisplay_ble.c:74-75`); Silabs ships `0x0019`
   (`cmake_gcc/CMakeLists.txt:17`, overriding an identical `0x0100` fallback at
   `opendisplay_ble.c:28-29`); ESP32 defines it nowhere. Both are returned over the same GATT
   characteristic. This is a product question, not a refactor, but the value is stated in four
   places with two answers and no single source. **I did not verify what a host does with it.**

10. **Should the "no target may redefine a canonical constant" rule (§ 16) become a
    `tools/check.sh` grep?** Four instances survive today, all currently harmless. The
    `OD_BUS_TYPE_I2C` precedent in `nordic-zephyr/src/opendisplay_constants.h:11-15` shows the
    failure mode is a compile error pointing at the wrong file — cheap to prevent, expensive to
    diagnose.
