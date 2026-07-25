# Target: EFR32BG22 (Simplicity SDK + CMake)

Source repo: `Firmware_Silabs` (https://github.com/davelee98/Firmware_Silabs)

Not yet imported. Build config is SLC-managed via the `.slcp`; `slc generate` requires a
FULL Simplicity SDK (a source-only copy is not enough). The CMake build itself does not.

Tightest RAM budget of all targets (32 KB) — the reference case for the memory rules in
../../docs/ARCHITECTURE.md.
