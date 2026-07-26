# Target: Nordic (Zephyr / nRF Connect SDK)

One target, one build, **multiple boards** — nRF54L15 and nRF52840 share the Zephyr BT host,
PSA Crypto, NVS/`settings`, and the bb_epaper panel stack, so they are boards of a single
target rather than two targets. See ../../docs/TOOLCHAINS.md.

| Board | Chip | Source repo | Status |
|---|---|---|---|
| `xiao_nrf54l15` | nRF54L15 | `Firmware_NRF54` (https://github.com/davelee98/Firmware_NRF54) | imported first |
| `lm20` | nRF54L15 | `Firmware_NRF54` | variant of the above (`NRF54_BOARD_LM20`) |
| nRF52840 custom | nRF52840 | `Firmware` (https://github.com/davelee98/Firmware) | ported later, off PlatformIO/Arduino |

Not yet imported. Build (once imported): `./build.sh`, flash with `./flash.sh`.
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

> Do deployed nRF52840 units migrate to Zephyr at all, or stay on the current Arduino/Bluefruit
> firmware indefinitely while only new production ships Zephyr + MCUboot?

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
