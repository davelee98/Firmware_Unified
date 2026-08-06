# Target: Nordic (Zephyr / nRF Connect SDK)

One target, one build, **multiple boards** — nRF54L15 and nRF52840 share the Zephyr BT host,
PSA Crypto, NVS/`settings`, and the bb_epaper panel stack, so they are boards of a single
target rather than two targets. See ../../docs/TOOLCHAINS.md.

| Board (`-b` argument) | Chip | Source | Status |
|---|---|---|---|
| `xiao_nrf54l15/nrf54l15/cpuapp` | nRF54L15 | `Firmware_NRF54` (https://github.com/davelee98/Firmware_NRF54) | imported; **the only board built here** — `build.sh` default |
| `xiao_nrf54lm20a/nrf54lm20a/cpuapp` | **nRF54LM20A** | `Firmware_NRF54`; board definition in-tree at `boards/seeed/xiao_nrf54lm20a/` | imported, **never built here** |
| nRF52840 custom | nRF52840 | `Firmware` (https://github.com/davelee98/Firmware) | **not ported** — migration step 4 |

**`lm20` IS A DIFFERENT PART, not a variant of the L15.** This table used to say "nRF54L15 —
variant of the above", which understated it: `xiao_nrf54lm20a` is an **nRF54LM20A** with its own
DTS, pinctrl, defconfig and a second core (`cpuflpr`), and it is the only board here carrying a
full in-tree definition (15 files). `zephyr/CMakeLists.txt` gives it its own
`prj_lm20_extra.conf` and an `NRF54_BOARD_LM20` define. Treating it as a rebadged L15 is how a
board-specific defect gets debugged against the wrong datasheet.

Build with `./build.sh`, flash with `./flash.sh`; both take `BOARD=` and `BUILD_DIR=` and
**default to the same board**, so `./build.sh && ./flash.sh` works. For the LM20:

```bash
BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./build.sh
BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./flash.sh
```

`PROFILE=uart ./build.sh` for USB-serial debug; battery builds log over SEGGER RTT (J-Link).

## nRF52840 is a port, not an import

It currently builds with PlatformIO + Arduino (Adafruit Bluefruit, `Adafruit_LittleFS`,
`Adafruit_nRFCrypto`/CC310) in the `Firmware` repo. It arrives here as a **third board** on this
target — devicetree/overlay + pinctrl, an nRF52-series `prj.conf`, and reuse of the nRF54L15
drivers. It depends on this target existing first, so it is **step 4** of the migration order
(Silabs is step 3), not part of the first import.

**Do not confuse nRF52840 with the legacy nRF52.** nRF52840 is a supported board of this
target, shipped and migrating here from `Firmware`. The legacy nRF52 is a different, older
product built on the bare Nordic SDK in `Firmware_NRF`; it is also shipped but is **not**
migrated to this repo at all, which is why there is no `targets/nrf52-sdk/` (MIGRATION.md
§ "Order and rationale" item 5).

Settle **before** starting: whether deployed nRF52840 units must accept OTA from the current
UF2/Bluefruit flash layout. That constrains or blocks the move — see § Bootloader below, where
the likely answer is that it blocks it.

## Bootloader: MCUboot, both boards, going forward

**Decided 2026-07-25.** MCUboot via NCS sysbuild is the bootloader for this target — nRF54L15
now, nRF52840 when it lands. The step-4 note above about UF2 compatibility is answered by this:
new units get MCUboot; deployed nRF52840 units are a separate question, below.

**Most of this already exists.** `Firmware_NRF54` ships a complete MCUboot setup — adopt it as
the target-wide standard rather than designing one:

| Piece | Where |
|---|---|
| `CONFIG_BOOTLOADER_MCUBOOT=y` | `Firmware_NRF54/zephyr/prj.conf:78` |
| BLE SMP OTA DFU | `prj.conf:79` (`CONFIG_NCS_SAMPLE_MCUMGR_BT_OTA_DFU`) |
| sysbuild wiring | `zephyr/sysbuild.cmake`, `sysbuild.conf`, `sysbuild/mcuboot.conf` |
| Settings/bond preservation across DFU | `prj.conf:85-86` — refuses the SMP "erase application settings" command, so display config and BLE bonds survive an update |

That last one is the kind of detail worth inheriting rather than rediscovering: a naive MCUboot
adoption erases settings on update and unprovisions the device.

### The gap is host-side, and it is unbudgeted

`py-opendisplay` has **no SMP/mcumgr client** — grep for `smp`/`mcumgr`/`mcuboot` across `src/`
and `pyproject.toml` returns nothing. Its OTA module implements only legacy Nordic DFU
(`perform_nrf_dfu`) and Silabs `.gbl` (`perform_silabs_ota`). So the firmware side of MCUboot
OTA is **done on nRF54L15 and unusable**, because nothing can drive it.

One new host backend serves both boards. Until it exists, MCUboot buys signed images and
automatic revert but no field update — strictly less than the nRF52840 has today. Budget the
host work as part of adopting MCUboot, not after.

### Deployed nRF52840: the choice is not MCUboot-vs-UF2

Those units run a working BLE DFU path today — the Adafruit bootloader plus in-app
`bledfu.begin()` (`Firmware/src/ble_init.cpp:168`) and `CMD_ENTER_DFU` setting the bootloader
magic (`device_control.cpp:850-864`), with `py-opendisplay`'s `perform_nrf_dfu` as the client.

The complication is that the Adafruit bootloader ships **with the S140 SoftDevice**, and a
Zephyr application does not use SoftDevice. **Verify early whether a Zephyr image can run under
that bootloader at all.** If it cannot — the likely outcome — then porting a deployed nRF52840
to Zephyr requires replacing its bootloader, which requires physical access. The real question
becomes:

> ~~Do deployed nRF52840 units migrate to Zephyr at all, or stay on the current Arduino/Bluefruit
> firmware indefinitely while only new production ships Zephyr + MCUboot?~~

**ANSWERED 2026-08-05: they migrate.** nRF52840 becomes a board of this target
(docs/NEXT_STEPS_2026-08-05.md § Milestone 7). Two things that decision carries with it:

- **Budget physical access to deployed units** for the bootloader replacement, unless the test
  below says otherwise. That is the cost the decision accepts.
- **The host SMP/mcumgr client is a PREREQUISITE.** Migrating a fielded unit to MCUboot before
  `py-opendisplay` can drive SMP would replace a working BLE DFU path with a signed one nothing
  can use -- strictly worse than those units have today. Build the client first.

**Run the cheap test before spending the budget.** Build a Zephyr image at the Adafruit flash
layout, push it to ONE unit over the existing BLE DFU, and see whether it boots and stays up. If
it works, the physical-access cost disappears; if it does not, it is confirmed rather than
assumed. The likely answer is that it does not work -- but that is a guess, and this is
answerable in an afternoon with one board.

Leaving them is defensible — they work, they have OTA, and the fleet is finite. Decide it
before step 4 is scheduled, because it determines whether step 4 is a port or a product split.

### To verify at import

- **Flash budget on nRF52840.** 1 MB total, minus MCUboot (~32 KB) and two app slots. Confirm
  the Zephyr app fits twice; if not, `overwrite-only` (no revert) is the fallback and should be
  a recorded trade, not a surprise.
- **Swap mode.** Prefer swap-move so failed images revert — that is the main reason to adopt
  MCUboot at all.
- **Signing keys.** MCUboot signs images; decide where the private key lives and who holds it
  before the first signed build. Do not commit it.

## Toolchain

nRF Connect SDK + west. **Pin an explicit NCS version** — the source repo's `ncs-env.sh` globs
`~/ncs/v3.*` then `~/ncs/v2.*` and takes the first hit, which is not reproducible across
machines or CI. Not currently installed on the primary dev box.

## Required at import: the advertising HAL

`shared/core/od_adv_control.c` is in `shared/sources.cmake`, so **the moment this target has a
build that consumes that list, it must also supply `od_hal_adv_{program,start,stop}`** — see
[shared/hal/od_hal_adv.h](../../shared/hal/od_hal_adv.h). They are link-time C functions; a
target that lists the shared sources without implementing them fails at link with three
undefined references, which is the intended failure and needs no extra guard.

There is no compile-tested stub here on purpose. This directory holds one README and no build
system, so a fake would have nothing to compile it and would rot unnoticed. The requirement is
recorded instead, where whoever writes the first `CMakeLists.txt` will meet it.

What the Zephyr adapter owes, beyond the three functions
([docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md](../../docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md)):

- **AD packing stays here.** `od_adv_control` never sees a PDU; it hands over a 16-byte MSD
  snapshot and the target builds the advertising and scan-response records. Match the ESP32
  layout unless a divergence is deliberate and recorded: flags + complete name + MSD in ADV,
  the 128-bit service UUID in the scan response.
- **`bt_le_adv_start` / `bt_le_adv_stop` are called from the application pump, not a callback.**
  Bluetooth callbacks publish facts through a `k_msgq` or an equivalently coherent bridge and
  return.
- **A `k_work` item may bridge an API-context requirement, but must not own desired state or
  restart policy.** Keeping policy in the application pump is what preserves the same ordering
  against display refresh, transfer teardown and deep sleep that every other target has.
- **nRF52840/Bluefruit: set `restartOnDisconnect(false)`.** The SoftDevice's automatic restart
  is a second policy owner, and it is the reason the application still carries a
  `restartsAdvertisingOnDisconnect()` divergence. Once this target owns the restart explicitly,
  that divergence and its special-case branch in `serviceBleAdvertisingRestart()` are deleted.
- **DFU entry clears start intent and reaches the stop barrier first**, then disconnects and
  disables the SoftDevice. It must not depend on toggling the stack's auto-restart setting at
  the right moment to avoid a race.
