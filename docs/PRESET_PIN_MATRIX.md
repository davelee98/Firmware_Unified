# Preset pin matrix

Every display pin assignment the OpenDisplay website's config toolbox can produce, catalogued
2026-08-20 so firmware work has a concrete answer to "what pin values do real devices actually
carry". Written for the Nordic SPIM promotion
(`plans/PLAN_NORDIC_SPIM_8MHZ_2026-08-19.md` § 4.6), which needs to know whether a hardware
peripheral's reachable-pin set covers the deployed configurations.

**Source of truth:** `../opendisplay.org/httpdocs/firmware/toolbox/simple-config-presets.json`,
the `driverBoards[]` array — one hand-maintained JSON file, validated in CI by
`.github/scripts/validate-simple-config-presets.py`, fetched client-side by the toolbox page. It is
the only preset pin table in that repo; `httpdocs/js/ble-common.js` layers curated kit combinations
on top but carries no pin data, and `httpdocs/js/battery-widget.js` holds a duplicate board list
that its own header comment marks as derived. Sibling repos are read-only references; nothing here
was changed.

Regenerate this file's tables from that JSON rather than hand-editing them.

## How to read a pin value

`DisplayConfig`'s pin fields are a single `u8` each, and **the same byte means different pins on
different targets** — the host writes a number, only firmware knows what it denotes. `0xFF` is the
"not present" sentinel everywhere. The decoding is selected by `SystemConfig.ic_type`
(`shared/protocol/opendisplay_structs.h`, `enum ICType`):

| `ic_type` | Family | Encoding |
|---|---|---|
| 1 | nRF52840 | **absolute** — port = `b >> 5`, pin = `b & 0x1F` |
| 5 | nRF52811 | absolute (single port) |
| 7, 8 | nRF54L15, nRF54LM20A | **packed** — bit `0x80` set: port = `(b >> 5) & 3`, pin = `b & 0x1F`; else port = `b >> 4`, pin = `b & 0x0F` |
| 2, 3, 4 | ESP32-S3 / C3 / C6 | raw GPIO number |
| 6 | EFR32BG22 | raw port/pin, `(port << 4) \| pin` |

Nordic rows below show the decoded `Pport.pin` beside the raw byte; other families show the raw
value only.

## The matrix

| Preset | `id` | `ic_type` | data | clk | cs | dc | reset | busy | pwr |
|---|---|---|---|---|---|---|---|---|---|
| Seeed EN04 (NRF52840) | `en04` | nRF52840 | `47` P1.15 | `45` P1.13 | `44` P1.12 | `31` P0.31 | `15` P0.15 | `29` P0.29 | `43` P1.11 |
| Seeed EE02 (ESP32-S3) | `ee02` | ESP32-S3 | `9` | `7` | `44` + `cs2`=`41` | `10` | `38` | `4` | `43` |
| Seeed EE03 (ESP32-S3) | `ee03` | ESP32-S3 | `9` | `7` | `44` | `8` | `38` | `4` | `43` |
| Seeed EE04 (ESP32-S3) | `ee04` | ESP32-S3 | `9` | `7` | `44` | `10` | `38` | `4` | `43` |
| Seeed EE05 (ESP32-S3) | `ee05` | ESP32-S3 | `9` | `7` | `44` | `10` | `38` | `4` | `43` |
| Seeed EN05 (NRF52840) | `en05` | nRF52840 | `47` P1.15 | `45` P1.13 | `44` P1.12 | `31` P0.31 | `15` P0.15 | `29` P0.29 | `43` P1.11 |
| Seeed reTerminal E1001 (ESP32-S3) | `reterminal-e1001` | ESP32-S3 | `9` | `7` | `10` | `11` | `12` | `13` | `11` |
| Seeed reTerminal E1002 (ESP32-S3) | `reterminal-e1002` | ESP32-S3 | `9` | `7` | `10` | `11` | `12` | `13` | `11` |
| Seeed reTerminal E1004 (ESP32-S3) | `reterminal-e1004` | ESP32-S3 | `9` | `7` | `10` + `cs2`=`2` | `11` | `38` | `13` | `12` |
| Seeed reTerminal E1003 (ESP32-S3) | `reterminal-e1003` | ESP32-S3 | `9` | `7` | `10` | `8` | `12` | `13` | `0xFF` |
| Seeed reTerminal Sticky (ESP32-S3) | `reterminal-sticky` | ESP32-S3 | `14` | `13` | `15` | `16` | `17` | `18` | `47` |
| Seeed XIAO ePaper Breakout NRF52840 | `nrf52840-xiao` | nRF52840 | `47` P1.15 | `45` P1.13 | `3` P0.03 | `29` P0.29 | `2` P0.02 | `5` P0.05 | `0xFF` |
| Seeed XIAO ePaper Breakout ESP32-S3 | `esp32s3-xiao` | ESP32-S3 | `9` | `7` | `2` | `4` | `1` | `6` | `0xFF` |
| Seeed XIAO ePaper Breakout ESP32-C3 | `esp32c3-xiao` | ESP32-C3 | `10` | `8` | `3` | `5` | `2` | `7` | `0xFF` |
| Seeed XIAO ePaper Breakout 7.5 C3 | `xiao-75-c3` | ESP32-C3 | `10` | `8` | `3` | `5` | `2` | `4` | `0xFF` |
| Seeed XIAO ePaper Breakout ESP32-C6 | `esp32c6-xiao` | ESP32-C6 | `18` | `19` | `1` | `21` | `0` | `23` | `0xFF` |
| M3 NRF52811 | `m3-nrf` | nRF52811 | `20` P0.20 | `19` P0.19 | `6` P0.06 | `5` P0.05 | `4` P0.04 | `3` P0.03 | `7` P0.07 |
| M3 Lite NRF52811 | `m3-lite-nrf` | nRF52811 | `20` P0.20 | `19` P0.19 | `6` P0.06 | `5` P0.05 | `4` P0.04 | `3` P0.03 | `7` P0.07 |
| M3 Pro base (EFR32BG22) | `m3-pro-silabs` | EFR32BG22 | `3` | `4` | `16` | `6` | `7` | `8` | `0` |
| M3 Core (EFR32BG22) | `m3-core-silabs` | EFR32BG22 | `3` | `4` | `16` | `6` | `7` | `8` | `0` |
| Waveshare ESP32-S3-PhotoPainter | `esp32-s3-wspp` | ESP32-S3 | `11` | `10` | `9` | `8` | `12` | `13` | `0` |
| Waveshare E-Paper ESP32 Driver Board | `esp32-epd-driver` | *9 (undefined)* | `14` | `13` | `15` | `27` | `26` | `25` | `0xFF` |
| Inkplate 5 V2 | `inkplate-5v2` | *9 (undefined)* | `0xFF` | `0xFF` | `0xFF` + `cs2`=`0xFF` | `0xFF` | `0xFF` | `0xFF` | `0xFF` |
| Inkplate 10 | `inkplate-10` | *9 (undefined)* | `0xFF` | `0xFF` | `0xFF` + `cs2`=`0xFF` | `0xFF` | `0xFF` | `0xFF` | `0xFF` |
| XTEINK X4 (ESP32-C3) | `x4` | ESP32-C3 | `10` | `8` | `21` | `4` | `5` | `6` | `0xFF` |
| OpenDisplay 4.26" Mono Kit | `opendisplay-426-mono-kit` | nRF52840 | `47` P1.15 | `45` P1.13 | `44` P1.12 | `31` P0.31 | `15` P0.15 | `29` P0.29 | `43` P1.11 |
| OpenDisplay 7.3" Color Kit | `opendisplay-73-color-kit` | nRF52840 | `47` P1.15 | `45` P1.13 | `44` P1.12 | `31` P0.31 | `15` P0.15 | `29` P0.29 | `43` P1.11 |
| Seeed XIAO ePaper Breakout — nRF54L15 | `nrf54l15-xiao` | nRF54L15 | `34` P2.02 | `33` P2.01 | `21` P1.05 | `23` P1.07 | `20` P1.04 | `27` P1.11 | `0xFF` |
| Seeed XIAO ePaper Breakout nRF54LM20A | `nrf54lm20-xiao` | nRF54LM20A | `22` P1.06 | `20` P1.04 | `191` P1.31 | `189` P1.29 | `16` P1.00 | `23` P1.07 | `0xFF` |
29 presets. `cs2` appears only on dual-controller boards and is listed inline beside `cs`.

## Distinct (data, clk) pairs

| (data, clk) | n | `ic_type` | presets |
|---|---|---|---|
| (9, 7) | 9 | ESP32-S3 | ee02, ee03, ee04, ee05, reterminal-e1001, reterminal-e1002, reterminal-e1004, reterminal-e1003, esp32s3-xiao |
| (47, 45) | 5 | nRF52840 | en04, en05, nrf52840-xiao, opendisplay-426-mono-kit, opendisplay-73-color-kit |
| (10, 8) | 3 | ESP32-C3 | esp32c3-xiao, xiao-75-c3, x4 |
| (3, 4) | 2 | EFR32BG22 | m3-pro-silabs, m3-core-silabs |
| (14, 13) | 2 | *9 (undefined)*, ESP32-S3 | reterminal-sticky, esp32-epd-driver |
| (20, 19) | 2 | nRF52811 | m3-nrf, m3-lite-nrf |
| (255, 255) | 2 | *9 (undefined)* | inkplate-5v2, inkplate-10 |
| (11, 10) | 1 | ESP32-S3 | esp32-s3-wspp |
| (18, 19) | 1 | ESP32-C6 | esp32c6-xiao |
| (22, 20) | 1 | nRF54LM20A | nrf54lm20-xiao |
| (34, 33) | 1 | nRF54L15 | nrf54l15-xiao |
## Nordic: do the presets fit the SPIM reachable-pin sets?

**Yes — every Nordic preset lands exactly on its board's qualified pair, with no exceptions.**

nRF54L GPIO ports belong to power domains, and the datasheets state the rule twice: *"Peripherals
must use pins in their own domain"* and *"Peripherals cannot mix pins from different ports. All
pins must be on the same port."* nRF52840 has no such rule — `PSEL.SCK` is `PIN [0..31]` +
`PORT [0..1]` + `CONNECT`, so any of its 48 GPIOs works with any SPIM instance.

| Target | Panel SPIM | Reachable set for that instance | Preset data / clk | Fits |
|---|---|---|---|---|
| nRF52840 | SPIM2 | **any pin** (no domain rule) | P1.15 / P1.13 | yes, trivially |
| nRF54L15 | SPIM00 | **dedicated pins on P2 only** | P2.02 / P2.01 | yes — and these *are* the dedicated pins |
| nRF54LM20A | SPIM23 | any pin on **P1 or P3** (both PERI) | P1.06 / P1.04 | yes |

The constraint is severely asymmetric, which matters when deciding how hard to defend against an
out-of-domain pin:

- **nRF52840 — nothing to reject.** The runtime pin contract is honoured completely.
- **nRF54LM20A — 45 legal pins** (P1.00-P1.31 plus P3.00-P3.12). A variant board could move the
  panel and still work. `od_pin_decode()` already accepts ports 0-3 gated on
  `od_gpio_port_ready()`, so P3 is expressible today.
- **nRF54L15 — effectively zero configurability.** SPIM00 uses *dedicated* P2 pins from the pin
  assignment table, not any P2 pin. P2.01 SCK / P2.02 MOSI / P2.04 MISO / P2.00 CSN is the whole
  set. Any other value is unreachable.

Two consequences for panel-bus work, both recorded in the plan:

1. **8 MHz on P1 is the pad's ceiling as well as the peripheral's.** P1 and P0 are rated 8 MHz
   max I/O; only P2 reaches 64 MHz. So `nrf54lm20-xiao` at 8 MHz runs SPIM23 at prescaler 2 (its
   floor) *and* P1.04/P1.06 at their rated maximum, with no margin on either.
2. **SPIM20/21 on nRF54L15 can also reach the dedicated P2 pins**, cross-domain, at prescaler 2
   with no errata 8/212 workaround — but only in Constant Latency sub-power mode, a standing power
   cost on a battery tag. SPIM00 remains the right choice; this is why, not an oversight.

## Adding a Nordic board

A new Nordic board has to answer two questions this repo currently conflates: **which pins carry
the panel bus**, and **which SPIM instance can drive them**. Pins arrive at runtime in the config
packet; the instance is compiled in. Both must agree, and on nRF54L the silicon decides whether
they *can*.

### Step 1 — pick the panel pins, then read the instance off this table

Legal (SCK, MOSI) placements, by SoC. `n/a` means the silicon cannot route it, not that it is
merely inconvenient.

**nRF52840** — no domain rule at all. `PSEL.SCK` is `PIN [0..31]` + `PORT [0..1]` + `CONNECT`, so
any of the 48 GPIOs works with any instance. Pick the instance by what else the board uses:

| Instance | Blocked by | Max | Note |
|---|---|---|---|
| `spi2` | nothing | 8 MHz | the deployed choice; no shared block |
| `spi3` | nothing | 32 MHz | carries anomalies 195 and 198 — avoid unless >8 MHz is required |
| `spi0` / `spi1` | `i2c0` / `i2c1` share the register block | 8 MHz | only if the board uses no TWI on that block |

**nRF54L15** — pins are bound to power domains. *"Peripherals must use pins in their own domain."*

| Panel pins on | Legal instances | Constant Latency | Max | Note |
|---|---|---|---|---|
| P2.01 / P2.02 (**dedicated pins only**) | `spi00` | no | 32 MHz | errata 8/212 armed at 8 MHz (prescaler 16) |
| P2 dedicated, alternative route | `spi20` / `spi21` | **yes** | 8 MHz | ~200x idle current — see below |
| any P1.00-P1.14 | `spi20` / `spi21` / `spi22` | no | 8 MHz | pick one whose block the board does not use for UARTE/TWIM |
| any P0.00-P0.04 | `spi30` | no | 8 MHz | only 5 LP-domain pins; usually wasteful |
| **mixed ports** | none | — | — | **illegal** |

**nRF54LM20A** — same rule, more room. P1 has 32 pins and P3 adds 13, both PERI domain.

| Panel pins on | Legal instances | Constant Latency | Max | Note |
|---|---|---|---|---|
| P2 dedicated pins | `spi00` | no | 32 MHz | |
| any P1 or P3 pin | `spi20`/`21`/`22`/`23`/`24` | no | 8 MHz | `spi23` is the XIAO route |
| any P0.00-P0.09 | `spi30` | no | 8 MHz | |
| **mixed ports** | none | — | — | **illegal** |

### Step 2 — check the four rules that are easy to violate

1. **Both pins must be on the same port.** The datasheet is explicit: *"Peripherals cannot mix pins
   from different ports. All pins must be on the same port."* SCK on P1 with MOSI on P2 decodes
   cleanly, passes a per-pin reachability check, and cannot work. This must be its own validation.
2. **`SPIM00` wants *dedicated* P2 pins, not any P2 pin.** P2.01 SCK / P2.02 MOSI / P2.04 MISO /
   P2.00 CSN is the entire set on nRF54L15. Another P2 pin is as unreachable as a P1 one.
3. **The instance's register block is shared.** `spi2x` shares with `uart2x` and `i2c2x`; `spi00`
   with `uart00`; on nRF52840 `spi0`/`spi1` with `i2c0`/`i2c1`. Only one can be enabled. On
   `xiao_nrf54l15` this already costs two candidates: `uart20` is the console
   (`zephyr,console = &uart20`) and `i2c22` is the D4/D5 bus.
4. **Cross-domain P2 access costs about 200x idle current.** Reaching P2 from a PERI-domain
   instance requires Constant Latency sub-power mode (`CONFIG_NRF_SYS_EVENT` plus
   `nrf_sys_event_request_global_constlat()`). The nRF54L15 tables give `ION_IDLE11` (Constant
   Latency) as **0.55 mA** against `ION_IDLE5` (System ON, 256 KB retained) at **2.7 uA**. The API
   is reference counted, so it can be scoped to a transfer — but a leaked reference on any error
   path costs 0.55 mA permanently, silently, with no wire-visible symptom. Treat this route as
   closed unless something forces it.

### Step 3 — board-design guidance

If the pins are still yours to choose, **put the panel on the P2 dedicated pins**. P2 is rated
64 MHz against P1/P0's 8 MHz, so at this project's 8 MHz the pad has 8x margin and the instance has
a path to 16 or 32 MHz later. A P1 board runs the pad at its rating on day one and caps the bus at
8 MHz permanently. Pin freedom and headroom are opposed on nRF54L: P1 gives fifteen usable pins and
no headroom; P2 gives two fixed pins and all of it.

Going to P1 is not otherwise a downgrade — it drops the Constant Latency question entirely and,
at prescaler 2, does not arm the errata 8/212 workaround that `spi00` needs at 8 MHz.

### Step 4 — the board axis has to exist first

**This repo does not currently distinguish board from SoC, and that breaks before SPIM does.**
`zephyr/CMakeLists.txt` derives `OD_BOARD_NRF54L15` from `CONFIG_SOC_NRF54L15`, and
`src/platform/nrf54/od_board_nrf54l15.c` hard-codes P2.03 / P2.05 / P2.10 — XIAO antenna-switch and
boost-select pins — behind `BUILD_ASSERT(IS_ENABLED(CONFIG_SOC_NRF54L15))`, with `od_board_name()`
returning the literal `"XIAO nRF54L15"`. A second nRF54L15 board inherits every one of those pin
writes. The comment above that CMake block records this exact failure happening once already, on
pins that did not exist and so no-opped silently; on a second real board they would not no-op.

A second board therefore needs, before any SPIM work:

- `CONFIG_OD_BOARD_*` set by the board fragment rather than defaulted from `SOC_*`, with the
  XIAO-specific `od_board_*.c` gated on the board;
- the panel instance declared as a board-level Kconfig choice with **no default**, so an unstated
  board is a build error rather than a silent inheritance — the same enforcement property the
  dispatch hooks get from a link error;
- a runtime validator applying the four rules above and refusing loudly, because a config packet
  can still name a pin the compiled-in instance cannot reach.

This is tracked as its own work item; see `docs/FOLLOWUPS.md`.

### Step 5 — where board files live, and what to vendor

`build.sh` passes `-DBOARD_ROOT="${SCRIPT_DIR}"`, so in-repo boards under
`targets/nordic-zephyr/boards/<vendor>/<board>/` are found by Zephyr. `xiao_ble` and
`xiao_nrf54l15` come straight from NCS and are **not** vendored; `xiao_nrf54lm20a` is vendored
because NCS 3.3.1's Zephyr has no such board — its eleven `seeed/xiao_*` boards do not include it.

That vendored copy came from Seeed's
[platform-seeedboards](https://github.com/Seeed-Studio/platform-seeedboards) via
`Firmware_NRF54` (`dc2c90b`, "add nrf54lm20a target"), then into this repo unchanged as `379b19e`.
Note it was **reflowed on the way** — upstream Zephyr uses tabs, the vendored copy uses 4-space
indent — so a future diff against a Seeed update will show whitespace that is not substance.

`seeed_xiao_connector.dtsi` is per-board, not shared: eleven independent copies exist in NCS alone,
each just that board's D-pin map behind the same `seeed,xiao-gpio` compatible. Its `xiao_spi:`
alias is what picks the panel bus — `&spi23` on LM20A, `&spi00` on nRF54L15, `&spi2` on `xiao_ble`
— and on LM20A that one inherited line is the entire reason the bus tops out at 8 MHz.

### Step 6 — the preset is external work

Firmware support is not device support. A new board also needs a `driverBoards[]` entry in
`../opendisplay.org/httpdocs/firmware/toolbox/simple-config-presets.json` with its pin bytes in the
right encoding for its `ic_type`, or no one can configure it from the toolbox. That repo is a
read-only reference here — record the need as external follow-up rather than editing it.

## Findings

- **All five nRF52840 presets share P1.15 / P1.13**, including the custom EN04/EN05 and OpenDisplay
  kit boards, which differ from the XIAO breakout on every *other* pin (`cs`, `dc`, `reset`, `busy`,
  `pwr`). The SPI pair is effectively a fleet-wide constant on that part.
- **`ic_type = 9` is used by three presets and is not defined in `enum ICType`**, which stops at
  8. `esp32-epd-driver`, `inkplate-5v2` and `inkplate-10` all carry it — classic-ESP32 boards,
  apparently. The website ships a value the canonical protocol header does not name. The header is
  frozen; this is cross-repo follow-up work, not something to fix from here.
- **The two Inkplate presets set every display pin to `0xFF`.** Their panels are driven by an
  onboard controller, so there is no SPI bus to configure — the sentinel is correct, not missing
  data.
- **`(14, 13)` is used by two presets in different families** (`reterminal-sticky`, ESP32-S3, and
  `esp32-epd-driver`, `ic_type` 9). Identical bytes, unrelated hardware — a reminder that a pin
  value is meaningless without its `ic_type`.
- **The nRF52811 presets** (`m3-nrf`, `m3-lite-nrf`) decode to P0.20 / P0.19 under the absolute
  scheme. Legacy `Firmware_NRF` territory per CLAUDE.md decision 8; listed for completeness, not in
  scope for this repo.

## Caveats

**Presets are not deployed configurations.** This catalogue bounds what the toolbox *offers*, not
what devices in the field *carry*. A hand-edited or third-party config can name any byte the
firmware's decoder accepts, so a clean preset survey lowers the probability of an out-of-domain
panel pin without eliminating it. Firmware that narrows the reachable set still needs to refuse an
unreachable pin loudly rather than assume this table is exhaustive.

**No firmware-side corroboration was done.** The board headers in `../Firmware`,
`../Firmware_NRF54`, `../Firmware_NRF` and `../Firmware_Silabs` were not diffed against these
values. If a mismatch matters to a decision, that is a separate check.
