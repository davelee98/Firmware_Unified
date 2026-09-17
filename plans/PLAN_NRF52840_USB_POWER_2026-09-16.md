# Plan — nRF52840 USB power management: enable USB only while VBUS is present

Date: 2026-09-16
Target: `nordic-zephyr`, board `xiao_ble/nrf52840` (build dir `build-nrf52840`). NCS v3.3.1.
Reference: `../Firmware` (Adafruit nRF52 core + TinyUSB) on the same board, which idles at ~55 µA
while advertising. This target idles at ~2 mA.
Nothing under `shared/` changes. The nRF54 boards log over RTT and do not build the USB stack; they
are out of scope.

## Objectives and change budget

1. On the XIAO nRF52840, do not enable USBD or hold its HFXO request while VBUS is absent.
2. Preserve CDC enumeration for cable-at-boot, hot-plug and re-plug, including hand-off from the
   Adafruit UF2 bootloader.
3. Stop the unplugged CDC log backend from waking at 1 kHz.
4. **Minimize new code and touch the fewest possible source files.** Prefer the documented Zephyr
   control flow and the board file already selected by the build. Do not add a generic USB power
   abstraction, a new source file, target-wide hook, nRF54 stubs, CMake wiring, fault-injection
   framework or speculative driver recovery for a failure that hardware has not demonstrated.

The intended implementation changes exactly three firmware inputs:

- `targets/nordic-zephyr/zephyr/boards/xiao_ble_nrf52840.conf`
- `targets/nordic-zephyr/src/platform/nrf52840/od_board_nrf52840.c`
- `targets/nordic-zephyr/zephyr/boards/xiao_ble_nrf52840.overlay`

The hardware checklist is the only documentation file updated with results. If implementation
requires another production source file, stop and justify it in this plan before adding it.

## Why this plan exists

On battery, with no USB cable, this firmware keeps the USB peripheral enabled and the 32 MHz
crystal (HFXO) running for the life of the boot. The console's logging then turns the log thread
into a 1 kHz sleep loop once the CDC TX buffer fills. The reference firmware does neither: TinyUSB
enables USBD and HFXO only while VBUS is present.

Evidence, all read from the installed trees:

| # | Fact | Where |
|---|---|---|
| E1 | The board's CDC console turns USB on at boot: `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` and `CONFIG_CDC_ACM_SERIAL_ENABLE_AT_BOOT` are `=y` in the resolved `.config`. | `zephyr/boards/common/usb/Kconfig.cdc_acm_serial.defconfig`, `build-nrf52840/zephyr/zephyr/.config` |
| E2 | `cdc_acm_serial_init_device()` calls `usbd_enable()` at `APPLICATION` init without checking VBUS. | `zephyr/subsys/usb/device_next/app/cdc_acm_serial.c:203-209` |
| E3 | `udc_nrf_enable()` requests HFXO and enables USBD. Only `udc_nrf_disable()` releases HFXO. Nothing in this build calls disable. | `zephyr/drivers/usb/udc/udc_nrf.c:1704-1712`, `:1735` |
| E4 | The driver's only low-power path, `LOWPOWER=LowPower`, runs on a bus SUSPEND event, which needs a connected, enumerated host. It never releases HFXO. | `udc_nrf.c:516-520`, `:971-997`, `:1434-1436` |
| E5 | `cdc_acm_poll_out()` with `hw-flow-control` loops on `k_msleep(1)` for as long as the TX ring is full. With no host the ring never drains. | `zephyr/subsys/usb/device_next/class/usbd_cdc_acm.c:991-1012`, `:639-642`; `targets/nordic-zephyr/zephyr/boards/xiao_ble_nrf52840.overlay` (`&board_cdc_acm_uart { hw-flow-control; }`) |
| E6 | The UART log backend writes one byte per `poll_out` and has no readiness or DTR check. | `zephyr/subsys/logging/backends/log_backend_uart.c:73-104`; `CONFIG_LOG_BACKEND_UART_BUFFER_SIZE=1` |
| E7 | Nordic's guidance is to enable USBD only after VBUS is detected, and says HFXO may be off when VBUS is absent. | nRF52840 PS, USBD "power-up sequence" and "suspend and resume" |
| E8 | Zephyr's documented pattern for battery devices is to enable on `USBD_MSG_VBUS_READY` and disable on `USBD_MSG_VBUS_REMOVED`. | Zephyr USB device_next docs; `zephyr/samples/subsys/usb/cdc_acm/src/main.c:49-93` |
| E9 | The `ENABLE_AT_BOOT` help text says: "When disabled, the application is responsible for enabling/disabling the USB device." It has a prompt, so a `.conf` may set it. | `zephyr/subsys/usb/device_next/app/Kconfig.cdc_acm_serial:19-24` |
| E10 | With a USB-using bootloader (the XIAO ships Adafruit's UF2 bootloader), the application may not get `USBDETECTED` for a cable that was already attached. The legacy driver re-checks `nrfx_power_usbstatus_get()` at attach for this reason. The device_next driver does not. | `zephyr/drivers/usb/device/usb_dc_nrfx.c:1299-1307` |
| E11 | TinyUSB does the same boot check, reading `USBREGSTATUS` before it enables power events, and Bluefruit repeats it once the SoftDevice owns the POWER events. | `~/.platformio/packages/framework-arduinoadafruitnrf52-seeed/libraries/Adafruit_TinyUSB_Arduino/src/arduino/ports/nrf/Adafruit_TinyUSB_nrf.cpp` (`usb_hardware_init`); `.../Bluefruit52Lib/src/bluefruit.cpp:78-90` |
| E12 | Message callbacks run on the system workqueue (`CONFIG_USBD_MSG_DEFERRED_MODE=y`). `usbd_enable()` and `usbd_disable()` return `-EALREADY` when already in the requested state. Only one callback can be registered per context (`-EALREADY` otherwise). | `zephyr/subsys/usb/device_next/usbd_msg.c:18-86`, `usbd_device.c:310-318, 361-363` |
| E13 | The console's `usbd_context` is `static` in `cdc_acm_serial.c`, but `USBD_DEVICE_DEFINE` places it in an iterable section named `cdc_acm_serial`. | `zephyr/include/zephyr/usb/usbd.h:474-520` |

Expected savings are **not measured**. Datasheet reasoning puts HFXO at roughly 100–250 µA (PS:
70–143 µA crystal core standby; 646 vs 418 µA TIMER on HFXO vs HFINT). The enabled USBD without VBUS
and the 1 kHz log loop have no spec figure. § 5 settles the numbers. Do not record anything in
`docs/HARDWARE_VERIFICATION_CHECKLIST.md` on the strength of this table.

Prior art for the handler shape: `nrf/applications/nrf_desktop/src/modules/usb_state.c:1189-1240`
(an `usb_enabled` flag, and ignoring `-ETIMEDOUT` from `usbd_enable()`). Note that `nrf_desktop`
only VBUS-gates on DWC2 controllers and still enables at boot on nRF52840, so it is a pattern
reference, not evidence that Nordic ships this on nRF52840.

---

## 0. Baseline measurement first

Measure before changing anything, so each step below is an A/B pair.

- Supply: PPK2 (or equivalent) in source mode on the XIAO **battery pads**, 3.7 V, USB cable
  **unplugged**. A USB-powered measurement is meaningless for this plan.
- Configuration: the deployed device config, panel attached, not connected over BLE, at least 60 s
  after boot (past the 4 s log-thread startup delay and the first MSD cycle).
- Record the average and a trace screenshot for the current HEAD `battery` build. A 1 kHz ripple
  in the trace is E5 made visible.
- Optional isolating builds, not committed:
  - A: `CONFIG_CDC_ACM_SERIAL_ENABLE_AT_BOOT=n` only. This removes E3 and should also remove E5
    (see § 3 for why it does not fully).
  - B: A plus no `hw-flow-control`.

## 1. Kconfig — stop enabling USB at boot

`targets/nordic-zephyr/zephyr/boards/xiao_ble_nrf52840.conf`:

```
# The board's CDC console registers and initialises USB at boot; the application enables it
# only while VBUS is present (src/platform/nrf52840/od_board_nrf52840.c). Enabling at boot
# requests HFXO and holds it for the life of the boot with no cable attached.
CONFIG_CDC_ACM_SERIAL_ENABLE_AT_BOOT=n
```

- Leave `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=y` (board default). It registers the class,
  runs `usbd_init()` (which arms the nrfx_power USB events, `udc_nrf.c:1762-1765`), and costs no
  current.
- Applies to every profile (`battery`, `debug`, `uart`, `quiet`), because all of them go through
  this board `.conf`. That is intended: a debug build run on battery should not pay for HFXO either.
- Check that `build.sh`'s warnings-as-errors accepts it: the symbol has a prompt (E9), so no
  "assigned but got" warning is expected.

## 2. Application — the smallest VBUS gate

### 2.1 Extend the existing nRF52840 board file

Put the complete implementation in
`targets/nordic-zephyr/src/platform/nrf52840/od_board_nrf52840.c`. That file is already selected by
the existing `CONFIG_OD_PLATFORM_NRF52840` CMake block, and `main()` already calls its
`od_board_early_init()` after `od_log_init()`. Add a private `usb_power_init()` call inside
`od_board_early_init()`; do not add a public hook.

Guard the USB includes, private functions and call with
`CONFIG_USB_DEVICE_STACK_NEXT && CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT`. A future nRF52840 build
without the CDC helper then retains the existing board initialization without a stub or build-file
change.

This choice deliberately avoids:

- a new `od_usb_power_nrf52840.c`;
- edits to `CMakeLists.txt`, `main.c` or `od_board.h`;
- empty nRF54 definitions; and
- a cross-target abstraction for behavior that only this board builds.

### 2.2 Control flow

Use one `k_work_delayable` and no multi-state recovery machine. The work handler reads
`nrfx_power_usbstatus_get()` and the public `udc_is_enabled(s_usbd->dev)`, then converges once:

Define USB power as present whenever `nrfx_power_usbstatus_get()` is anything other than
`NRFX_POWER_USB_STATE_DISCONNECTED`. Both `CONNECTED` and `READY` therefore request enable.
Waiting for `READY` would deadlock: the USB regulator only becomes ready after USBD is enabled.

| Desired/current | Action |
|---|---|
| VBUS present, disabled | call `usbd_enable()` |
| VBUS absent, enabled | call `usbd_disable()` |
| already converged | no API call |

Treat `0` and `-EALREADY` as convergence. On an enable error, log the first error and retry up to
three times at 250 ms while VBUS remains present. Reset the enable-attempt counter whenever the
sampled USB power state is `DISCONNECTED`, so a later replug starts a fresh attempt set. On a
disable error, log once and do not retry: Zephyr clears its enabled state even when the lower driver
fails, so a second call only returns `-EALREADY` and cannot complete skipped cleanup.

A failed disable is a logged, non-rebooting fault for the rest of that cable session. Its HFXO
request may remain held until reset; the next cable connection may exhaust its three enable attempts
because endpoint cleanup was incomplete. Do not add an automatic reboot or another cleanup path in
this board-local change.

After successful enable, reschedule the same work every `OD_USB_SUPERVISE_MS` (1000 ms) while the
controller remains enabled. This bounds the cost of a dropped `VBUS_REMOVED` message to one second.
Do not poll while disabled: a dropped `VBUS_READY` then requires a replug, which is an acceptable
console failure and preserves the battery objective.

The message callback handles only `USBD_MSG_VBUS_READY` and `USBD_MSG_VBUS_REMOVED` and reschedules
the work immediately. With deferred messages it runs on the same system workqueue; if that Kconfig
setting changes, rescheduling remains safe from the device-stack context.

### 2.3 Boot level and context lookup

`cdc_acm_serial.c` completes `usbd_init()` at `APPLICATION` priority before `main()`, so
`od_board_early_init()` may register the callback. Find the context in the iterable section:

1. use the exact `"cdc_acm_serial"` match;
2. if there is no match and exactly one context, use it and log WARN;
3. for zero or ambiguous contexts, log ERROR and return without touching USB.

If `usbd_can_detect_vbus()` is false or callback registration fails, log ERROR and call
`usbd_enable()` once to preserve the pre-plan always-on console. Do not add a force-on mode or
retry framework for this configuration-error fallback.

After successful registration, reschedule the work immediately. This boot-level read covers a
cable already attached before the application starts, including the UF2 hand-off case where the
edge may have been consumed. Early USB messages are safe to miss: `usbd_msg_pub_simple()` publishes
only when `ctx->msg_cb` is non-NULL, so an event before registration is dropped rather than calling
through a null callback (`subsys/usb/device_next/usbd_msg.c:118-124`).

### 2.4 No speculative pull-up recovery

The installed driver handles `NRFX_POWER_USB_EVT_READY` internally and sets `USBPULLUP`; the
application cannot call that static handler. Do not inspect `NRF_USBD->USBPULLUP`, cycle the
regulator, write peripheral registers or add debug fault hooks in this change.

Cable-at-boot and bootloader hand-off are hard hardware gates in § 5. If either fails, stop. Record
the device_next missed-READY behavior as an upstream Zephyr/NCS issue and make the choice between a
target-owned driver patch and an upstream fix in a separate plan. Do not grow an unproven recovery
state machine inside the board file.

## 3. Devicetree — the CDC flow-control spin (E5)

Gating USB does **not** by itself stop E5. With USB disabled the ring still fills from the log
backend and never drains, and with `hw-flow-control` the log thread still spins at 1 kHz. The same
happens after an unplug if the thread was mid-line. This needs its own fix.

Remove `hw-flow-control` from `&board_cdc_acm_uart` in `xiao_ble_nrf52840.overlay`. A full ring then
discards instead of sleeping at 1 kHz. When a host is attached, a burst that outruns it may lose
characters; `CONFIG_LOG_BUFFER_SIZE=4096` and deferred logging absorb most bursts, and § 5 records
whether the trade-off is acceptable.

Do not add a debug-only overlay, DTR tracking or runtime log-backend control in this plan. Those
options touch more files and add lifecycle code to preserve lossless diagnostics, while the product
requirement here is battery behavior. If hardware testing shows unacceptable log loss, address it
separately with measured evidence.

Removing flow control also makes `PROFILE=uart`'s immediate logging non-blocking when no host drains
the ring.

While unplugged, log output fills the CDC ACM TX FIFO (default 1024 bytes) and later characters are
dropped. On the next plug the host receives that oldest buffered KiB first, followed by current
output; a gap in the unplugged interval is therefore expected during the replug tests.

## 4. Documentation touch-ups (same change)

- `xiao_ble_nrf52840.conf`: the console comment block gains one line pointing at § 1's rationale.
- `xiao_ble_nrf52840.overlay`: replace the `hw-flow-control` block per § 3.
- `docs/HARDWARE_VERIFICATION_CHECKLIST.md`: add rows under the nRF52840 section for the § 5
  cases, left **open** until run.
- No `CLAUDE.md` status change until § 5 has run on hardware.
- Do not add a design document, source-list entry or test-only configuration file. This plan and the
  checklist are sufficient for a board-local change with three firmware inputs.

## 5. Verification

### 5.1 Build (required before any hardware step)

```
cd targets/nordic-zephyr
source ncs-env.sh
./build-nrf52840.sh                 # battery
PROFILE=debug ./build-nrf52840.sh    # -> build-nrf52840-debug
PROFILE=uart  BUILD_DIR="$PWD/build-nrf52840-uart"  ./build-nrf52840.sh
```

- Never use bare `./build.sh` here: its `BOARD` defaults to `xiao_nrf54l15/nrf54l15/cpuapp`
  (`build.sh:8`), so it would verify the wrong board and none of this board's `.conf`/overlay.
- `build-nrf52840.sh` only gives `debug` its own directory. `uart` and `quiet` would otherwise
  overwrite `build-nrf52840/`, hence the explicit `BUILD_DIR`.
- Confirm every nRF52840 build's `.config` has `CONFIG_BOARD_TARGET="xiao_ble/nrf52840"`.
- Confirm in the resolved `.config`: `# CONFIG_CDC_ACM_SERIAL_ENABLE_AT_BOOT is not set`,
  `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=y`.
- Confirm in `zephyr.dts`: no `hw-flow-control` on `board_cdc_acm_uart`.
- Run `tools/check.sh` (host gate), plus `--targets` before merge.
- Record the application flash/RAM delta against the baseline battery build. The board-file addition
  should remain small (roughly 150 non-comment lines or less); exceeding that is a signal that the
  implementation has recreated the rejected recovery framework and must be simplified.
- Do not add a host fake framework for three convergence branches. The battery, bootloader and
  hot-plug behavior is hardware-dependent and § 5.2–5.3 is the authoritative gate. If the source
  can no longer be reviewed as those three branches plus lookup/retry plumbing, stop and split the
  newly discovered behavior into a separately tested change.

`PROFILE=quiet` is explicitly **non-gating** for this change. It already combines `CONFIG_LOG=n`
with the board's `CONFIG_LOG_BACKEND_UART=y` and may fail before reaching this code. Do not expand a
USB power change into profile-convergence work. If desired, record a diagnostic result with:

```
PROFILE=quiet BUILD_DIR="$PWD/build-nrf52840-quiet" ./build-nrf52840.sh
```

### 5.2 Hardware — current (PPK2 on battery pads, as § 0)

| Case | Expectation |
|---|---|
| Battery build, no cable, 60 s+ after boot | Average well below baseline. No 1 kHz ripple. Report the number, not a pass/fail against a guess. |
| Same, after a plug → unplug cycle | Returns to the no-cable average (HFXO released, E3). |
| Debug build, no cable | Same as the battery build within ordinary logging variance. |

### 5.3 Hardware — function (USB powered)

| Case | Expectation |
|---|---|
| Cable attached **before** power-on (cold boot from USB) | Enumerates as the CDC console. Boot log visible. This is a release gate for the boot-level check. |
| Double-tap reset into the UF2 bootloader, then reset to the app with the cable held | Enumerates. This is the E10 case the legacy driver comment warns about. |
| Battery boot, then hot-plug | Enumerates within about 1 s. Subsequent logs visible. |
| Hot-unplug, then re-plug, ×20 | Enumerates every time. No `usb: enable/disable failed` logs. No watchdog reset. |
| Fast plug/unplug bounce (under 100 ms, by hand or with a switched hub) | Settles to the final level. After final unplug, PPK2 returns to the no-cable average within about 1 s. |
| Plug/unplug during a BLE upload | Upload completes. USB state follows the cable. |
| Serial monitor attached through a log burst (config write, refresh) | Note dropped characters from the no-flow-control trade-off; they do not block firmware work. |
| UF2 drag-and-drop flashing | Unchanged: the bootloader owns USB there. |

The first two rows are hard gates. If either cable-at-boot case fails, stop rather than adding
register peeks, synthetic READY events or recovery states. The next step is a separate driver-level
investigation, because the smallest correct fix may belong in Zephyr rather than this application.

### 5.4 Risks to watch during § 5

- **MPSL and POWER USB events.** NCS's MPSL owns parts of the POWER peripheral. The current build
  relies on the READY event reaching `udc_nrf` after the at-boot enable, since that is what sets the
  pull-up. The hot-plug DETECTED/REMOVED edges have never been exercised. If hot-plug does not
  enumerate, investigate here first. Temporarily log each `usbd_msg` type at DEBUG in the existing
  board file; do not retain that instrumentation after the test.
- **Context lookup by name** can change on an NCS upgrade. The single-context fallback and WARN line
  are the tripwire without adding a public accessor or local SDK patch.
- **`usbd_disable()` while the log thread is inside `poll_out`.** With flow control removed, the
  write discards rather than parking the logger, so disable does not depend on a host draining it.
- **Dropped VBUS_READY while disabled.** There is deliberately no battery-side polling. Replugging
  is the recovery; if hardware shows this is common, revisit the message path separately.

## 6. Out of scope — separate follow-ups from the same investigation

These showed up in the idle-current audit, but they are not USB and each needs its own change and
measurement:

1. **On-board QSPI flash (P25Q16H) never enters deep power-down.** `nrf_qspi_nor` wakes it at init
   and only suspends it under `CONFIG_PM_DEVICE`. The app's bit-banged `0xB9` cannot reach
   P0.20–25 while `&qspi` owns them. Tens of µA.
2. **`DEVICE_FLAG_XIAOINIT` (bit 1) is not implemented.** `opendisplay_device_flags.h` calls bits
   0–4 ESP32-only, but in `../Firmware` bit 1 runs `xiaoinit()` →
   `powerDownExternalFlash(20,24,21,25,22,23)` (`../Firmware/src/main.cpp:1419-1426, 1515-1598`).
   The comment is wrong. This overlaps with item 1.
3. **A configured TX power of 0 dBm is treated as unset and becomes +8 dBm**
   (`opendisplay_ble.c:696-697`). `../Firmware` honours 0.
4. **The advertising-fault latch** makes `main.c:69-75` poll at 20 Hz forever.
5. Config-dependent pin states worth checking against the deployed config: active-low XIAO LEDs
   without the invert flag, internal I2C pull-ups into unpowered sensors, and button pull
   direction.
