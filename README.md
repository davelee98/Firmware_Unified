# Firmware_Unified

One repository for **all OpenDisplay firmware targets**, replacing four independently
versioned repos that had drifted apart while implementing the same wire protocol.

> **Status: scaffold.** Nothing is imported yet. The layout, the `shared/` boundary rules,
> and the migration order are defined; the per-target code lands incrementally (see
> [docs/MIGRATION.md](docs/MIGRATION.md)). Until a target's directory contains a build
> file, build it from its original repo.

## Why unify

The four firmware repos each implement the same BLE/LAN wire protocol, config-packet
parsing, chunked-transfer state machines, compression, and session encryption — separately.
That has real costs:

- **Fixes land once, per repo.** A bug in the config parser or a transfer state machine has
  to be found and fixed up to four times.
- **The protocol header is copied byte-for-byte into every repo**, kept in sync only by a
  script and a CI check.
- **Behaviour silently diverges.** Targets differ in which transmission modes, refresh
  paths, and error responses they actually implement, with no single place to compare.

Unifying puts the target-agnostic ~80% in `shared/` and leaves each target only its
chip-specific drivers and build system.

## Layout

```
shared/          target-agnostic. MUST NOT include any vendor SDK header.
  protocol/      canonical wire contract (see "Protocol header" below)
  core/          command dispatch, config parsing, transfer state machines, session auth
  compress/      inflate engines (uzlib bit-serial, ROM tinfl adapter) behind one API
  hal/           abstract interfaces that targets implement (SPI, GPIO, timers, NVS, radio)
targets/         one directory per target: chip drivers + build system + HAL implementation
tools/           protocol-header sync, provisioning, device CLI
docs/            architecture and migration notes
```

## Targets

| Directory | Chips | Build system | From |
|---|---|---|---|
| `targets/esp32-nrf52840-pio/` | ESP32-S3 / C3 / C6, nRF52840 | PlatformIO (Arduino) | `Firmware` |
| `targets/nrf54l15-zephyr/` | XIAO nRF54L15 | Zephyr / nRF Connect SDK + west | `Firmware_NRF54` |
| `targets/efr32bg22-slc/` | EFR32BG22 | Simplicity SDK + CMake (SLC-managed) | `Firmware_Silabs` |
| `targets/nrf52-sdk/` | nRF52 (legacy) | Nordic SDK (bare C) | `Firmware_NRF` |

Four toolchains stay four toolchains — unification is about **shared source**, not a single
build system. Do not attempt to make one build system drive all four.

## The `shared/` rule

`shared/` compiles for every target, so it may use only the C/C++ standard library and
`shared/hal` interfaces. **No `esp_*`, `nrf_*`, `sl_*`, `zephyr/*`, or `Arduino.h` includes.**
Anything needing a peripheral goes through `shared/hal`, implemented per target.

This is the invariant that makes the repo worth having. Code that reaches for a vendor
header belongs in `targets/`.

## Protocol header

`shared/protocol/` holds the canonical wire contract, copied from the
[`opendisplay-protocol`](../opendisplay-protocol) repo. It is **pure `#define` constants and
config structs** — no functions, no per-target types.

**Never hand-edit these files.** Change the canonical source, then propagate:

```bash
cd ../opendisplay-protocol
tools/sync_protocol_header.py --push     # canonical -> all copies
tools/sync_protocol_header.py --check    # fail if any copy drifted (CI/pre-commit)
```

> **Required follow-up:** the sync tool's copy map does not know about this repo yet, so the
> headers here are currently a one-time copy and `--check` will not police them. Add
> `Firmware_Unified/shared/protocol/` as a destination in
> `opendisplay-protocol/tools/sync_protocol_header.py` before relying on them.

## Getting started

Per-target builds are documented in each `targets/*/README.md` once that target is imported.
Migration order and the rationale are in [docs/MIGRATION.md](docs/MIGRATION.md); the
`shared`/`hal` boundary is specified in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
