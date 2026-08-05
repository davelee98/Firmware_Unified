# Seeed XIAO nRF54LM20A on NCS

The Seeed `xiao_nrf54lm20a` board definition is vendored under
`Firmware_NRF54/boards/seeed/xiao_nrf54lm20a/` (from
[platform-seeedboards](https://github.com/Seeed-Studio/platform-seeedboards)).
CMake sets `BOARD_ROOT` so west finds it without an external Board Roots path.

## Build / flash

```bash
cd Firmware_NRF54
BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./build.sh
BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./flash.sh
```

`flash.sh` uses **pyocd chip-erase + flash** of `merged.hex` (target
`nrf54lm20a`). Do not rely on plain `west flash` on the Seeed CMSIS-DAP probe:
it can leave the last bytes unwritten, which faults inside `net_buf` during
`bt_enable` so the device never advertises. Chip erase also clears NVS config.

RTT console (reset + dump):

```bash
BUILD_DIR=build-lm20 DURATION=20 ./rtt.sh
```

Board-specific `zephyr/boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.conf` is
auto-picked (disables `CONFIG_BT_CTLR_ASSERT_OPTIMIZE_FOR_SIZE`).

Default `./build.sh` remains **XIAO nRF54L15** (`xiao_nrf54l15/nrf54l15/cpuapp`).

## Power / nPM1300 notes

`power_en` is **P1.12** (`regulator-fixed` in DTS), not the **SHPHLD** pad.
SHPHLD is a separate board pad for nPM1300 ship/hibernate mode. Early init
drives P1.12 high via GPIO (Seeed samples use `regulator_enable(power_en)`;
enabling Zephyr `CONFIG_REGULATOR` breaks USB-HS on this board, so we keep the
GPIO path).

Hardware nPM1300 I2C is **P1.17 SCL / P1.18 SDA**. The board DT `pmic_i2c`
node is **disabled** in the LM20 overlay so those GPIOs stay free for
OpenDisplay bit-bang I2C from config.

## nPM1300 in OpenDisplay

App code talks to the PMIC over the configured `data_bus` (bit-bang I2C), same
pattern as BQ27220/SHT40:

- Requires sensor type **6** (`npm1300`) + I2C `data_bus` (SCL=`pin_1`, SDA=`pin_2`)
- Toolbox preset `nrf54lm20-xiao`: bus pins `0xb1`/`0xb2`, addr `0x6b`, MSD byte 10
- VBAT via ADC TASK registers; SOC estimated from voltage; charging from CHG_STAT
- Deep-sleep command → SHIP hibernate register write over the same bus
- `bus_id` / `i2c_addr_7bit` are used (default addr 0x6B)

A collapsed **3.3 V rail when seated in an ePaper breakout** is a hardware short
(extra LM20 pads vs classic socket) — PMIC software cannot fix that.

## Onboard RGB LED

Sense RGB is **config-driven** via LED TLV `0x21` (software PWM in
`opendisplay_led.c`). Preset `nrf54lm20-xiao` pins (DTS active-high):

| Channel | SoC | Config |
|---------|-----|--------|
| R | P1.22 | `0xB6` |
| G | P1.24 | `0xB8` |
| B | P1.23 | `0xB7` |

`led_flags=0x7` (active-low / inverted). Early init does **not** park RGB —
`opendisplay_led_init()` claims the pins after config load and drives them off.
Mic CLK/DIN are still parked in early init.

## Pin note / ePaper breakout

XIAO D-pin → SoC mapping differs from L15. Pins above 15 need the extended
config byte (`0x80 | (port << 5) | pin`).

Seeed ePaper breakout (BUSY on **D5**, same as L15 `nrf54l15-xiao` preset):

| Signal | XIAO | LM20 SoC | Config |
|--------|------|----------|--------|
| RST | D0 | P1.0 | `0x10` |
| CS | D1 | P1.31 | `0xBF` |
| DC | D3 | P1.29 | `0xBD` |
| BUSY | D5 | P1.7 | `0x17` |
| CLK | D8 | P1.4 | `0x14` |
| DATA | D10 | P1.6 | `0x16` |

Driver Board V2 uses BUSY on **D2** (`P1.30` → `0xBE`) instead of D5.

Use toolbox preset `nrf54lm20-xiao` — do not reuse L15 pin bytes.

## Advertising / low power

Steady advertising is fixed at **1000 ms** (SoftDevice was sticking to the old
160 ms floor when given a 160–1000 ms window). Button/touch still boosts to
20–30 ms for 3 s. That matches legacy `Firmware_NRF` for idle current; discovery
after interaction stays fast.

`transmission_modes` does not select advertising interval. Preset value **25**
(`9|16`) advertises streaming + direct_write + **pipe_write** so clients can use
PIPE_WRITE (`0x0080`–`0x0082`).

Deep-sleep CMD on LM20 enters **nPM1300 hibernate** (wake via SHPHLD / VBUS).
That is not the same as `Firmware_NRF` SoftDevice System OFF — do not compare
hibernate current 1:1 with System OFF.

Unused LM20 blocks are disabled in the board overlay (`usbhs`, `pwm20`,
`i2c30`, `pdm20`, …). Early init parks mic GPIOs only; RGB comes from
the LED config packet.

### NFC (SoC NFCT)

`&nfct` is enabled. Antenna pads are fixed **N1/N2 = P1.01/P1.02** (solder an
external coil). Runtime enable is via config packet **0x2A** (`nfc_config`) with
`flags` bit0 set and `nfc_ic_type` = `2` (`soc_nfct`) or `0` (`auto` on LM20).
`bus_instance` / `power_pin` / `field_detect_pin` are ignored; field presence is
reported via NFCT events into `adv_button_byte_index` in the MSD. NDEF is
read/written over BLE `CMD_NFC_ENDPOINT` (same as Silabs toolbox UI). Payload
lives in RAM until rewritten (not stored on an external tag IC).

### Current testing

```bash
# Quieter build for advertising-current (LOG off, no heartbeat / MSD dump prints)
PROFILE=quiet BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./build.sh
BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./flash.sh
```

1. Write config from `nrf54lm20-xiao` (defaults to power preset `battery-2000`;
   board `powerDefaults` set `sleep_timeout_ms=40000`, `tx_power=0`).
2. Disconnect the debugger for steady-state µA readings (RTT/probe adds load).
3. Measure advertising idle separately from hibernate.

Runtime TX power needs `CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL` (enabled in
`prj.conf`). Without it, HCI VS Write_Tx_Power_Level fails with `-5`.
