# OpenDisplay — nRF54 (XIAO nRF54L15)

Bluetooth LE ePaper firmware built with **nRF Connect SDK (NCS) + west** and **bb_epaper**.

Channel Sounding / Android ranging is compiled in and **enabled at runtime** when `system_config.device_flags` bit 5 (`DEVICE_FLAG_CHANNEL_SOUNDING`, `0x20`) is set in the config packet.

## Prerequisites

- [nRF Connect SDK](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/installation.html) **v3.0.1+** (tested with v3.3.1)
- Matching toolchain installed via nRF Connect (see `~/ncs/toolchains/`)

## Build

```bash
export NCS_ROOT=~/ncs/v3.3.1   # optional — auto-detected
cd Firmware_NRF54
./build.sh
```

**USB bench debugging** (UART serial monitor):

```bash
PROFILE=uart ./build.sh
```

**Factory config embed:**

```bash
FACTORY_CONFIG_HEX='...' ./build.sh
```

**Release versioning:**

```bash
BUILD_VERSION=v1.2.3 SHA=$(git rev-parse --short HEAD) ./build.sh
```

`build.sh` passes `-Dzephyr_OD_FW_VERSION=...` (sysbuild image prefix). Do not pass bare `-DBUILD_VERSION` — that is Zephyr’s cmake version variable and does not reach the app. Boot screen and BLE `CMD_FIRMWARE_VERSION` both report `major.minor.patch` (e.g. `v2.2.3` → `2.2.3`). The patch byte is appended after the SHA on the wire so older hosts that stop after SHA keep working.

## Flash

```bash
./flash.sh              # factory: chip erase + merged.hex (wipes config)
./flash.sh update       # primary slot only: keeps display config
# Defaults to XIAO nRF54LM20A (build-lm20). For L15:
BUILD_DIR=build BOARD=xiao_nrf54l15/nrf54l15/cpuapp ./flash.sh update
```

| Mode | Image | Erase | Keeps display config? |
|------|-------|-------|------------------------|
| `factory` (default) | `merged.hex` (MCUboot + app) | chip | No |
| `update` | `zephyr.signed.hex` (primary slot) | sector | Yes |

Display config (and BLE bonds) live in `settings_storage` **after** both MCUboot slots (`pm_static_*.yml`). BLE OTA only programs the secondary slot; the partition map keeps settings out of that range.

**Important:** nRF Connect Device Manager / the nRF Connect app default to **Erase application settings** on DFU. That uses SMP `zephyr/basic` storage-erase and wipes config. This firmware builds with `CONFIG_MCUMGR_GRP_ZBASIC_STORAGE_ERASE=n` so that command is refused. Still turn the option off in the client if present. Probe `./flash.sh update` keeps settings; only chip-erase **factory** flash clears them.

**Note:** MCUboot is built with UART console disabled (`zephyr/sysbuild/mcuboot.conf`). Leaving UART on caused flat battery current via back-feed into the XIAO USB-UART bridge. After updating MCUboot, use a **factory** flash once (or at least reflash `merged.hex`); `./flash.sh update` alone does not replace MCUboot. The app also parks uart20 pins at boot so an older MCUboot image is mitigated.

Battery builds log via SEGGER RTT when a J-Link probe is attached.

## OTA (MCUboot + MCUmgr SMP)

Factory/probe flash uses `merged.hex` (MCUboot + app). Over-the-air updates use the signed `zephyr.signed.bin` (or `dfu_application.zip`) printed by `./build.sh` after a successful build.

Images are signed with the NCS default **ED25519** development key (`bootloader/mcuboot/root-ed25519.pem`). Replace that key for production.

| Encryption in config | SMP GATT at boot | How to open OTA |
|----------------------|------------------|-----------------|
| Off | Registered | Upload with nRF Connect Device Manager / `mcumgr` / `nrfutil` |
| On | Hidden | Authenticate over the OpenDisplay pipe, send `CMD_ENTER_DFU` (`0x0051`), reconnect, then upload |

After `0x0051` the device registers the SMP service, disconnects, and re-advertises so the client rediscovers GATT. On the next reboot with encryption on, SMP is hidden again.

## Channel Sounding

| `device_flags` bit | Name | Effect |
|--------------------|------|--------|
| `0x20` (bit 5) | `DEVICE_FLAG_CHANNEL_SOUNDING` | Advertise RAS UUID `0x185B`, run CS reflector on connect (pair/bond required for Android ranging) |

Default presets leave this bit clear — normal OpenDisplay `0x2446` pipe works without bonding.

For the Seeed XIAO preset (`nrf54l15-xiao`), set `device_flags` to **`0x21`** (`0x1 | 0x20`).

### Android ranging test (e.g. Pixel 10 Pro)

1. Build/flash main firmware with `device_flags` bit 5 set in the config packet.
2. Pair and bond with the phone when prompted.
3. Expected UART/RTT logs when ranging works:
   - `CS capability exchange completed`
   - `CS config creation complete`
   - `CS security enabled`
   - `CS procedures enabled`

Default XIAO board DTS uses the **onboard ceramic antenna** (`rfsw_ctl` LOW at boot). External IPEX needs P2.05 HIGH — see Seeed `zephyr-rfsw` example.

References: [NCS RAS reflector sample](https://github.com/nrfconnect/sdk-nrf/tree/main/samples/bluetooth/channel_sounding/ras_reflector), [Nordic Channel Sounding](https://www.nordicsemi.com/Products/Wireless/Bluetooth-Low-Energy/Channel-Sounding).

## nRF54LM20A

Seeed XIAO nRF54LM20A board DTS is vendored under `boards/seeed/xiao_nrf54lm20a/`.

```bash
BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./build.sh
BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./flash.sh
# advertising-current tests:
PROFILE=quiet BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./build.sh
```

See [docs/LM20_NCS.md](docs/LM20_NCS.md). Default `./build.sh` remains L15.

## Pin encoding

OpenDisplay configs use a compact GPIO byte:

| Pins | Encoding | Example |
|------|----------|---------|
| 0–15 | `(port << 4) \| pin` | `P2.02` → `0x22` |
| 16–31 | `0x80 \| (port << 5) \| pin` | `P1.31` → `0xBF` |

LM20 XIAO D1/D2/D3 are P1.31/30/29 and need the high-pin form. Use the nRF54 toolbox presets — do not reuse L15 or nRF52840 pin bytes.

## Layout

- `src/` — OpenDisplay application (BLE, protocol, display, optional CS)
- `third_party/bb_epaper/` — vendored library with `nrf54_zephyr_io.inl`
- `zephyr/` — CMakeLists.txt, prj.conf, overlays
- `build.sh` / `flash.sh` / `ncs-env.sh` — NCS build helpers
