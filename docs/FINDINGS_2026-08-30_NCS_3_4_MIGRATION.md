# NCS 3.3.1 to 3.4.0 migration findings and implementation plan

Date: 2026-08-30

## Decision

Migrate `targets/nordic-zephyr` to nRF Connect SDK 3.4.0, but do not change the
repository default until all three Nordic boards build from a clean tree and the required
hardware gates pass.

The migration is expected to be small at source level. The application-facing Zephyr and Nordic
HAL interfaces used here still exist in NCS 3.4.0. The material risks are the PSA Crypto
configuration and implementation change, preservation of the deployed flash layouts, image-size
movement, and hardware behavior after the Zephyr and SoftDevice Controller update.

NCS 3.4.0 is the appropriate long-lived destination: Nordic designates it as a five-year LTS
release, and 3.4.x is the final NCS line that supports nRF52 devices. The latter matters because
`xiao_ble/nrf52840` is a supported production target in this repository.

This document records a software migration candidate, not hardware evidence. A successful build
or merge does not close any hardware-verification row.

## Scope and evidence

The audit compared this repository with the exact upstream revisions selected by NCS 3.4.0:

- `sdk-nrf` tag `v3.4.0`, commit `99553055607b2e9885fbc80ccd11fa9da81c2df0`
- `sdk-zephyr` tag `ncs-v3.4.0`, commit `bf801e4e3d19e1ffa76164346480cb7734dd2800`
- `hal_nordic` commit `18da0cc9726f8759c627dba3180b3ba9294e433c`

The official NCS 3.4 migration guide and Zephyr 4.4 migration guide were checked against the
target's Kconfig, devicetree, sysbuild files, build wrappers, and direct API use.

No NCS 3.4 build was available during the audit. The installed SDK is NCS 3.3.1 with toolchain
checksum `0c0f19d91c`; NCS 3.4.0 requires toolchain checksum `ccc010f809`. Therefore API and Kconfig
compatibility below is source-audited but not yet compiler-proven.

## Findings

### F1. No application API removal was found

The NCS 3.4 trees retain all inspected interfaces used by the target:

- Bluetooth advertising, GATT, connection, HCI vendor command, and Channel Sounding APIs
- settings and the NVS-backed settings implementation
- GPIO, ADC, sensor, hardware-information, watchdog, and retained-memory APIs
- PSA key, AEAD, MAC, cipher, and random APIs
- direct nrfx SPIM operations
- direct Nordic GPIO, SPIM, and watchdog HAL operations

The Zephyr Channel Sounding header used by `src/opendisplay_cs.c` is unchanged between NCS 3.3.1
and NCS 3.4.0. The direct low-level calls in `src/od_epd_spi_bitbang.c`,
`src/od_epd_spi_nrfx.c`, and `src/od_hal_wdt.c` remain present in the NCS 3.4 HAL revision.

Expected action: none, unless the clean build exposes a dependency or warning not visible from
the source audit.

### F2. The application crypto configuration must move to the NCS 3.4 PSA model

`zephyr/prj.conf` currently enables:

```ini
CONFIG_MBEDTLS=y
CONFIG_MBEDTLS_BUILTIN=y
CONFIG_MBEDTLS_PSA_CRYPTO_C=y
```

The application does not call a legacy `mbedtls_*` API and does not use TLS or X.509. Its crypto
adapter calls PSA directly. NCS 3.4 moves crypto to TF-PSA-Crypto/Oberon and directs PSA-only
applications to enable `CONFIG_PSA_CRYPTO`, reserving `CONFIG_MBEDTLS` for TLS and X.509.

Recommended application configuration:

```ini
CONFIG_PSA_CRYPTO=y
CONFIG_ENTROPY_GENERATOR=y
CONFIG_ENTROPY_DEVICE_RANDOM_GENERATOR=y
CONFIG_PSA_WANT_KEY_TYPE_AES=y
CONFIG_PSA_WANT_ALG_CMAC=y
CONFIG_PSA_WANT_ALG_ECB_NO_PADDING=y
CONFIG_PSA_WANT_ALG_CCM=y
```

Remove the three explicit `CONFIG_MBEDTLS*` assignments above unless an NCS 3.4 configuration
trace proves that a target dependency still needs one. MCUboot is a sysbuild child image with its
own configuration; its requirements are not a reason to enable TLS in the application image.

The current shortened-tag policy is intentional:

```c
PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 12)
```

Do not change it to plain `PSA_ALG_CCM`. The 12-byte tag is part of the deployed wire contract.
Both CC310 on nRF52840 and CRACEN on nRF54 must accept key import and complete authenticated
traffic after the migration.

### F3. The XIAO nRF52840 Adafruit bootloader layout remains supported

The complete `boards/seeed/xiao_ble` configuration is identical between NCS 3.3.1 and the Zephyr
revision selected by NCS 3.4.0, except for a documentation typo. NCS 3.4 preserves UF2 generation
and this flash map:

| Region | Start | Size | End (exclusive) |
| --- | ---: | ---: | ---: |
| SoftDevice/reserved | `0x000000` | `0x027000` | `0x027000` |
| Application | `0x027000` | `0x0C5000` | `0x0EC000` |
| Settings | `0x0EC000` | `0x008000` | `0x0F4000` |
| Adafruit UF2 bootloader | `0x0F4000` | `0x00C000` | `0x100000` |

`zephyr/sysbuild_adafruit.conf` correctly sets `SB_CONFIG_BOOTLOADER_MCUBOOT=n`, and `build.sh`
selects that file for `xiao_ble`. Preserve both decisions.

Only `zephyr.uf2` is a valid field-update artifact for this target. Do not deliver an nRF54
MCUboot artifact, erase the full chip, or flash a merged image over the reserved or bootloader
regions. The NCS 3.4 candidate must prove that every UF2 payload address lies in
`0x027000..0x0EBFFF` and that the linked application fits within `0x0C5000` bytes.

### F4. Partition Manager is not an immediate NCS 3.4 blocker

The nRF54 build path uses MCUboot with Partition Manager. NCS 3.4 deprecates Partition Manager but
still includes it. This migration must not combine the SDK update with a partition-system
conversion.

Preserve the current MCUboot slot, settings, and storage addresses so artifacts remain compatible
with already-fielded bootloaders and update flows. Plan a separate DTS-partition migration with
its own compatibility analysis before moving beyond the NCS 3.4 LTS line.

The custom XIAO nRF54LM20A board contains explicit DTS partitions as well as a
`PARTITION_MANAGER_ENABLED` branch. The NCS 3.4 build must record which layout is selected and
compare it byte-for-byte with the NCS 3.3.1 baseline.

### F5. Zephyr 4.4 NVS movement does not affect the target

Zephyr 4.4 moves direct NVS and ZMS headers from `zephyr/fs/` to `zephyr/kvss/` without changing
their Kconfig symbols. This target uses `zephyr/settings/settings.h` rather than the direct NVS
header, so no source change is expected.

Settings persistence remains a hardware gate because layout or backend behavior can still change
without an API change.

### F6. RTT SRAM discovery is deprecated but still functional

`rtt.sh` reads `CONFIG_SRAM_BASE_ADDRESS` and `CONFIG_SRAM_SIZE` from the generated application
configuration. Zephyr 4.4 deprecates these Kconfig values in favor of the `zephyr,sram` chosen
devicetree node, but continues to generate them. This is not an NCS 3.4 blocker.

Leave the helper unchanged for the first migration candidate. Replace it in a follow-up before a
later Zephyr release removes the compatibility values. Do not combine that diagnostic-tool change
with the SDK migration unless NCS 3.4 stops emitting either value in an actual build.

### F7. The repository pin must change only after qualification

`ncs-env.sh` currently defaults `OD_NCS_VERSION` to `v3.3.1`. That reproducibility pin is correct
and must remain in place during exploration. Use an explicit environment override for candidate
builds. Change the default, build manifest fallback, README, and `docs/TOOLCHAINS.md` together only
after the migration gates pass.

## Recommended implementation plan

### Step 1: Install and identify NCS 3.4.0

Install the official NCS 3.4.0 SDK and its matching toolchain without replacing the NCS 3.3.1
installation. Confirm:

```sh
export OD_NCS_VERSION=v3.4.0
source targets/nordic-zephyr/ncs-env.sh
west list nrf zephyr hal_nordic
```

Record the resolved SDK roots, revisions, compiler version, and toolchain checksum in the build
evidence. A candidate built with mixed 3.3 and 3.4 modules is invalid.

Rollback point: none; both SDK installations coexist and the repository still defaults to 3.3.1.

### Step 2: Capture a clean NCS 3.3.1 baseline

From the migration branch, build all three boards at the same source commit and profile intended
for the 3.4 comparison:

```sh
OD_NCS_VERSION=v3.3.1 PROFILE=debug \
  targets/nordic-zephyr/build.sh --all
```

Archive or record, per board:

- final Kconfig `.config`
- devicetree output
- partition report and addresses
- ELF and map file
- flash, RAM, and static-object totals
- ELF build ID and release-manifest entry
- `zephyr.uf2` for nRF52840
- signed application binary, DFU zip, and merged hex for each nRF54 board

The source commit and configuration must be identical for the 3.3.1 and 3.4.0 comparisons.

### Step 3: Make the minimal PSA configuration patch

In `targets/nordic-zephyr/zephyr/prj.conf`:

1. add `CONFIG_PSA_CRYPTO=y`;
2. remove the three explicit application `CONFIG_MBEDTLS*` assignments;
3. retain the four existing `CONFIG_PSA_WANT_*` selections and entropy selections;
4. make no source, partition, bootloader, or protocol change in this step.

Run a pristine single-board NCS 3.4 build first for `xiao_ble/nrf52840`, because it exercises the
final-supported nRF52 family, CC310, and the Adafruit layout. Treat any Kconfig warning as a
failure until explained; do not silence warnings by restoring broad Mbed TLS configuration.

Rollback point: revert only the small Kconfig patch and return to the explicit 3.3.1 build.

### Step 4: Build all targets and inspect configuration

Run:

```sh
OD_NCS_VERSION=v3.4.0 PROFILE=debug \
  targets/nordic-zephyr/build.sh --all
```

Required software results:

- all three boards build from pristine directories;
- the summary contains no skip and no unexplained Kconfig warning;
- `CONFIG_PSA_CRYPTO=y` and all required AES algorithms resolve in every application image;
- no application image unexpectedly enables TLS or X.509;
- no promoted protocol or transfer implementation is discarded unexpectedly;
- release artifacts exist with the correct board-specific names;
- every image is opened and inspected, not merely observed in the output directory.

Run the repository gate with targets after the standalone Nordic builds:

```sh
tools/check.sh --targets
```

A skip is not a pass. Read the gate summary and require zero skips.

### Step 5: Prove boot and partition compatibility before flashing

Compare NCS 3.3.1 and 3.4.0 layouts for each board.

For `xiao_ble/nrf52840`:

- confirm the application begins at `0x027000`;
- confirm it ends before `0x0EC000`;
- confirm settings remain `0x0EC000..0x0F3FFF`;
- confirm the UF2 bootloader remains `0x0F4000..0x0FFFFF`;
- decode or inspect every UF2 block address;
- reject any artifact that writes a reserved region.

For both nRF54 boards:

- compare MCUboot, slot 0, slot 1, settings, and storage addresses;
- require equal upgrade-slot sizes where the current swap mode requires them;
- verify signing parameters and image header load addresses;
- compare DFU zip metadata with the 3.3.1 baseline;
- investigate size changes against existing headroom before flashing.

No flash is authorized by a successful linker result alone.

### Step 6: Hardware-qualify the nRF52840 candidate

Enter the existing Adafruit bootloader by double reset and copy only the inspected UF2. Verify:

1. boot and advertising;
2. plaintext PIPE upload;
3. encrypted authentication and PIPE upload;
4. config read;
5. config write, immediate reload, and reboot persistence;
6. interrupted-transfer recovery after a mid-PIPE BLE disconnect;
7. runtime TX-power command behavior;
8. watchdog boot/feed/reset behavior appropriate to the available test setup;
9. NFC read/write behavior or explicitly retain the existing open hardware-debt status;
10. return to the Adafruit bootloader by double reset after the application has run.

Include a wrong-tag authentication control so successful encrypted traffic cannot conceal an
authentication bypass. Where practical, repeat the retained-frame dispatch-gate stimulus that
tests the nonce-rejection silence path.

### Step 7: Hardware-qualify both nRF54 candidates

For `xiao_nrf54l15/nrf54l15/cpuapp` and
`xiao_nrf54lm20a/nrf54lm20a/cpuapp`, verify the same protocol and persistence paths, plus:

- CRACEN acceptance of the shortened-tag CCM key policy;
- MCUboot boot confirmation and rollback behavior;
- OTA through the produced signed artifact and DFU package;
- settings preservation across OTA;
- watchdog and retained GPREGRET behavior;
- display SPI operation on the nrfx path and its fallback/error handling;
- the configured SRAM boundary, especially the nRF54L15 CPUAPP reclaim and LM20A scan limit.

Do not infer one nRF54 board's result for the other. They have different SoCs, board definitions,
memory layouts, and hardware ownership details.

### Step 8: Promote the repository pin

Only after Steps 4-7 pass:

1. change the default in `targets/nordic-zephyr/ncs-env.sh` to `v3.4.0`;
2. change the manifest fallback in `targets/nordic-zephyr/build.sh` to `v3.4.0`;
3. update `targets/nordic-zephyr/README_SOURCE.md` and `docs/TOOLCHAINS.md`;
4. record exact software and hardware evidence in
   `docs/HARDWARE_VERIFICATION_CHECKLIST.md`;
5. run `tools/check.sh --targets` once more at the final commit;
6. inspect all final release images and manifests again.

Do not rewrite earlier hardware evidence as though it were produced under NCS 3.4. Add new,
dated evidence and leave any unexercised row open.

## Acceptance criteria

The migration is complete only when all of the following are true:

- the repository defaults to the exact NCS 3.4.0 SDK and matching toolchain;
- all three Nordic board configurations build pristine;
- `tools/check.sh --targets` reports no failure and no skip;
- application crypto is PSA-only and performs the deployed CCM/CMAC/ECB operations;
- flash and settings layouts are unchanged or an explicitly reviewed compatible change exists;
- the nRF52840 UF2 neither overwrites nor disables the Adafruit bootloader;
- both nRF54 MCUboot/OTA paths accept the new artifacts and preserve settings;
- required on-device protocol, persistence, recovery, and watchdog checks have dated evidence;
- image-size changes remain within each target's measured headroom;
- rollback artifacts built with NCS 3.3.1 remain available until field-update compatibility is
  demonstrated.

## Expected changed files

The minimal migration should need only:

- `targets/nordic-zephyr/zephyr/prj.conf`
- `targets/nordic-zephyr/ncs-env.sh`
- `targets/nordic-zephyr/build.sh`
- `targets/nordic-zephyr/README_SOURCE.md`
- `docs/TOOLCHAINS.md`
- `docs/HARDWARE_VERIFICATION_CHECKLIST.md`

Any required change to protocol code, flash layout, bootloader selection, or target HAL code is a
new finding. Stop and review it independently instead of expanding the migration patch silently.

## External references

- NCS 3.4.0 release notes:
  <https://github.com/nrfconnect/sdk-nrf/blob/v3.4.0/doc/nrf/releases_and_maturity/releases/release-notes-3.4.0.rst>
- NCS 3.4 migration guide:
  <https://github.com/nrfconnect/sdk-nrf/blob/v3.4.0/doc/nrf/releases_and_maturity/migration/migration_guide_3.4.rst>
- Zephyr XIAO BLE board documentation:
  <https://docs.zephyrproject.org/latest/boards/seeed/xiao_ble/doc/index.html>

