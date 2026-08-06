# Target: Nordic (Zephyr / nRF Connect SDK)

One shared Zephyr application, with **separate platform build paths** for nRF52840 and nRF54.
They share the BT host, protocol, storage, and display renderer; they do not share board init,
pin encoding, bootloader policy, flashing, or EPD rail preparation. See
[docs/PLATFORM_SEPARATION.md](docs/PLATFORM_SEPARATION.md).

| Board (`-b` argument) | Chip | Source | Status |
|---|---|---|---|
| `xiao_nrf54l15/nrf54l15/cpuapp` | nRF54L15 | `Firmware_NRF54` | supported; MCUboot |
| `xiao_nrf54lm20a/nrf54lm20a/cpuapp` | nRF54LM20A | `Firmware_NRF54` | supported; MCUboot |
| `xiao_ble/nrf52840` | nRF52840 | `Firmware` | ported; Adafruit bootloader/UF2 |

**`lm20` IS A DIFFERENT PART, not a variant of the L15.** This table used to say "nRF54L15 —
variant of the above", which understated it: `xiao_nrf54lm20a` is an **nRF54LM20A** with its own
DTS, pinctrl, defconfig and a second core (`cpuflpr`), and it is the only board here carrying a
full in-tree definition (15 files). `zephyr/CMakeLists.txt` gives it its own
board-specific configuration and source file. Treating it as a rebadged L15 is how a
board-specific defect gets debugged against the wrong datasheet.

Use the platform-specific front doors:

```bash
./build-nrf52840.sh
./flash-nrf52840.sh /path/to/ADAFRUIT_UF2_VOLUME

./build-nrf52840-debug.sh
./flash-nrf52840-debug.sh /path/to/ADAFRUIT_UF2_VOLUME

./build-nrf54.sh l15
./flash-nrf54.sh l15

./build-nrf54.sh lm20
./flash-nrf54.sh lm20
```

`./build.sh --all` remains the release-matrix command. Raw `BOARD=` builds are supported for
advanced Zephyr use, but release and bench instructions use the explicit front doors.

The normal binary compiles OpenDisplay through INFO and Zephyr subsystems through WARN. The
separate debug target enables OpenDisplay DEBUG plus Zephyr subsystem INFO, uses
`build-nrf52840-debug/`, and emits `release/opendisplay-xiao_nrf52840-debug.uf2` plus
`release/opendisplay-xiao_nrf52840-debug-merged.hex`, so it cannot overwrite the production
artifacts. Both variants use the application-owned
`[SSSS.mmm|Cn] L: message` format; debug changes verbosity, not the application log protocol.

## nRF52840 is a port, not an import

The nRF52840 implementation was ported from the Arduino/Bluefruit `Firmware` tree. It now uses
Zephyr and the shared Nordic application, but its deployed wire-level pin encoding and board
power sequence remain explicit nRF52840 platform contracts.

**Do not confuse nRF52840 with the legacy nRF52.** nRF52840 is a supported board of this
target, shipped and migrating here from `Firmware`. The legacy nRF52 is a different, older
product built on the bare Nordic SDK in `Firmware_NRF`; it is also shipped but is **not**
migrated to this repo at all, which is why there is no `targets/nrf52-sdk/` (MIGRATION.md
§ "Order and rationale" item 5).

## Bootloader policy

**nRF52840 keeps the Adafruit bootloader. nRF54L15 and nRF54LM20A use MCUboot.** The build and
flash entry points enforce this split; the generic nRF54 flasher rejects `xiao_ble/nrf52840`.

The nRF54 sysbuild configuration provides MCUboot, signed application images, and BLE SMP DFU.
The nRF52840 sysbuild configuration instead preserves the Adafruit partition layout and emits a
UF2. It is deliberately not compiled with `CONFIG_BOOTLOADER_MCUBOOT`.

The host currently has no SMP/mcumgr client, so nRF54 BLE OTA still needs corresponding host-side
support. Factory and primary-slot probe flashing are available through `flash-nrf54.sh`.

## Platform contracts

- `zephyr/Kconfig` derives `OD_PLATFORM_NRF52840` or `OD_PLATFORM_NRF54` from the selected SoC.
- `zephyr/CMakeLists.txt` compiles one board implementation and one pin decoder for that platform.
- nRF52840 configuration pins use absolute `port * 32 + pin` values; nRF54 uses its packed codec.
- The nRF52840 overlay disables UART0 because upstream UART RX on P1.12 collides with deployed
  display CS (absolute pin 44).
- nRF52840 EPD startup restores the donor firmware's P0.13 boost selection and cold rail cycle.
- Boot display success means the refresh command succeeded and BUSY asserted and released; merely
  filling the controller framebuffer is not reported as a successful physical refresh.

## Toolchain and verification

The wrappers use nRF Connect SDK + west through `ncs-env.sh`. Set `OD_NCS_VERSION` when a specific
installed NCS version is required; release manifests record the selected version.

After building, validate the platform identity and expected artifacts:

```bash
./scripts/validate-build.sh nrf52840
./scripts/validate-build.sh nrf52840 debug
./scripts/validate-build.sh nrf54l15
./scripts/validate-build.sh nrf54lm20
```

Run the portable pin-codec and shared host tests from the repository root:

```bash
cmake -S tests/host -B /tmp/od-host-tests
cmake --build /tmp/od-host-tests
ctest --test-dir /tmp/od-host-tests --output-on-failure
```

Hardware acceptance is still required before calling the nRF52840 boot-screen fault closed: flash
one board, confirm P0.13 stays low, and observe BUSY assert/release during the boot refresh. The log
line `boot display refresh complete` is the software success criterion.
