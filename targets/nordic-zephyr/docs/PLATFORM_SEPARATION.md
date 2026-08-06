# Nordic platform separation

The Nordic target is one shared Zephyr application with two explicit platform families.
Application behavior stays shared; hardware and delivery policy do not.

## Boundaries

Shared application sources under `src/` own BLE, protocol dispatch, configuration, storage,
rendering, and device features. They call neutral `od_board`, `od_gpio`, and `od_zephyr_compat`
interfaces.

Platform sources live under `src/platform/`:

- `nrf52840/` owns absolute `port * 32 + pin` decoding, P0.13 EPD boost selection, and the
  donor firmware's cold rail cycle.
- `nrf54/` owns packed pin decoding and separate L15/LM20 board initialization.

`zephyr/Kconfig` derives exactly one platform from the selected SoC. CMake compiles exactly one
board implementation and one pin-codec wrapper. nRF52840 is never identified as `TARGET_NRF54`.

## Build and delivery paths

| Platform | Build | Bootloader | Primary artifact | Flash |
|---|---|---|---|---|
| nRF52840 | `./build-nrf52840.sh` | Adafruit | `zephyr.uf2` | `./flash-nrf52840.sh` |
| nRF54L15 | `./build-nrf54.sh l15` | MCUboot | signed image/DFU zip | `./flash-nrf54.sh l15` |
| nRF54LM20A | `./build-nrf54.sh lm20` | MCUboot | signed image/DFU zip | `./flash-nrf54.sh lm20` |

Thin front doors consume `scripts/target-registry.sh`; the west implementation remains shared.
`scripts/validate-build.sh <target>` checks the selected SoC/platform, build identity, expected
artifact, forbidden cross-platform definitions, and the nRF52840 UART0 disable.

## EPD contract

The generic display layer validates the configured power pin and refuses an unset value. It
never falls back to P0.00. Before the first nRF52840 display power-up it executes:

1. P0.13 low;
2. rail on for 50 ms;
3. rail off for 50 ms;
4. P0.13 low again;
5. rail on, followed by normal rail and signal settling.

Boot rendering is not considered successful until the refresh command succeeds and BUSY has
asserted and released. A failed physical refresh receives one bounded retry and remains
observable as a failure.

## Verification

Run the host codec contract:

```bash
cmake -S tests/host -B /tmp/od-host-tests
cmake --build /tmp/od-host-tests
ctest --test-dir /tmp/od-host-tests --output-on-failure
```

Then clean-build and validate each target:

```bash
./build-nrf52840.sh
./scripts/validate-build.sh nrf52840
./build-nrf54.sh l15
./scripts/validate-build.sh nrf54l15
./build-nrf54.sh lm20
./scripts/validate-build.sh nrf54lm20
```
