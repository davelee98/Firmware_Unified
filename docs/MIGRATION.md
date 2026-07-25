# Migration plan

Incremental, one target at a time, keeping the original repos buildable throughout. Nothing
is deleted from the source repos until the unified target has been flashed and verified on
hardware.

## Order and rationale

1. **`esp32-nrf52840-pio`** (from `Firmware`) — first, because it is the most actively
   developed, carries both an ESP32 and an nRF52840 target already (so it proves the
   two-chip-one-tree case), and its protocol implementation is the most complete. It becomes
   the reference for what `shared/core` must expose.
2. **`nrf54l15-zephyr`** (from `Firmware_NRF54`) — second: Zephyr's build system is the most
   opinionated, so it stresses the `shared/` boundary hardest. If `shared/` survives being
   consumed by west/CMake with no vendor includes, the boundary is real.
3. **`efr32bg22-slc`** (from `Firmware_Silabs`) — third. Constrained RAM (32 KB) validates the
   memory-sensitivity rules in ARCHITECTURE.md. Note its build config is SLC-generated and
   `slc generate` needs a full Simplicity SDK, which is not available on every dev box.
4. **`nrf52-sdk`** (from `Firmware_NRF`) — last, or possibly never. Legacy; evaluate whether
   it is still shipped before spending effort. It is also the only one owned by the
   `OpenDisplay` org rather than a personal fork.

## Per-target procedure

1. Import the target's sources **unchanged** into `targets/<t>/` in one commit, so the diff
   afterwards is reviewable and blame stays meaningful.
2. Build it from the new location without touching logic. Fix only include paths and build
   files. Flash and verify on hardware. Commit.
3. Identify duplicated logic against `shared/` (or, for the first target, promote its logic
   *into* `shared/` behind HAL interfaces).
4. Replace the target's copy with the shared implementation **one subsystem at a time** —
   config parsing, then dispatch, then each transfer path. Build + flash + verify after each.
5. Only then delete the corresponding code from the original repo, and point its README at
   this one.

Do not batch step 4. Each subsystem swap is independently revertable; a combined swap is not.

## Verification bar per subsystem

A subsystem is "migrated" only when, on real hardware for that target:

- a full image push renders correctly (compressed and uncompressed),
- a config read/write round-trips,
- the encrypted/authenticated path works if the target supports it, and
- an interrupted transfer recovers (disconnect mid-stream, then retry).

## Risks to watch

- **Silent behavioural divergence.** The four repos do not implement identical semantics
  today. When promoting logic to `shared/`, differences must be resolved deliberately and
  written down — not settled by whichever repo was copied first.
- **Protocol header drift.** Until the sync tool's copy map includes
  `Firmware_Unified/shared/protocol/`, the headers here are unpoliced. Fix that early.
- **Memory regressions on small targets.** Shared code sized for the ESP32 will not fit the
  EFR32BG22. Keep buffer sizes target-parameterised.
- **Losing history.** Prefer imports that preserve provenance where practical; if a plain
  copy is used, record the source repo and commit SHA in the import commit message.
