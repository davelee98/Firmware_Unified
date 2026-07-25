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
drivers. It depends on this target existing first, so it is step 3 of the migration order, not
part of the first import.

Settle **before** starting: whether deployed nRF52840 units must accept OTA from the current
UF2/Bluefruit flash layout. That constrains or blocks the move.

## Toolchain

nRF Connect SDK + west. **Pin an explicit NCS version** — the source repo's `ncs-env.sh` globs
`~/ncs/v3.*` then `~/ncs/v2.*` and takes the first hit, which is not reproducible across
machines or CI. Not currently installed on the primary dev box.
