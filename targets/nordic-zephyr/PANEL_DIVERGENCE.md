# Panel-traffic divergence from `Firmware_NRF54` — 2026-08-05

**Status: CLOSED 2026-08-05 — the newer vendored `bb_epaper` is ACCEPTED.** Option 1 of the
three below is the decision. This document is now a record of what that accepts, not an open
question.

**Still not hardware-verified.** Closing the decision does not verify the outcome, and the
verification list further down is unchanged. Read it before flashing a shipped panel.

This target was imported from `Firmware_NRF54@0f19c0c` and now builds against this repo's
**newer** vendored `bb_epaper` (76 panel enums vs 69, plus three documented `OD-PATCH`es).
The class-to-raw-`BBEPDISP` conversion in `5068c29` was validated call-by-call and introduces
no divergence of its own.

**The divergence comes from the library version, and it is not cosmetic.** Four panels this
target maps receive *different initialisation traffic* than the shipped firmware sends. Every
one of these compiles cleanly and is invisible to every gate in this repo.

## The four panels

| Protocol | Panel | What changed |
|---|---|---|
| `0x0014` | `EP75_800x480` | fast and partial LUT/init sequences differ substantially |
| `0x0015` | `EP75_800x480_4GRAY` | full grayscale LUT **replaced**; the old sequence survives as the *fast* sequence, so fast refresh looks preserved and full init does not |
| `0x0027` | `EP426_800x480` | partial init command `0x21` changed from two data bytes `00 00` to one byte `40` |
| `0x003A` | `EP42YR_400x300` | full init sequence substantially different; `BBEP_4COLOR`, so `od_bbep_wake()` and `od_bbep_send_panel_init_full()` both send it |

Mappings are in `src/opendisplay_epd_map.c`.

## The one that worries me most: `EP42YR_400x300` BUSY handling

`Firmware_NRF54` gave this panel a **`BBEP_SKIP_BUSY_WAIT`** flag, which turns every
init/wake/sleep `BUSY_WAIT` marker into a fixed **200 ms delay**.

**That flag does not exist upstream.** It is a *fourth* undocumented local edit to the vendored
tree in the source repo (alongside the `#elif` backend arms, the `bbepWaitBusy` guard, and the
added `sendPanelInitFull()` method). It reads like a deliberate workaround for a panel whose
BUSY line does not behave.

On this target the panel now performs a **real busy poll, up to 30 seconds**. If that BUSY line
is the reason the flag exists, every operation on this panel stalls for 30 s instead of 200 ms.
Nothing here can tell the difference; a board can, immediately.

## Verification still owed

Do **not** read "the target builds" or "the decision is closed" as "the target is safe on these
panels". Before this target is flashed to anything shipped, on real hardware, old firmware
versus new:

- `EP42YR_400x300` first — confirm BUSY actually reaches idle, and time one wake/refresh cycle
  against the 200 ms the old build spent;
- the other three panels: full, fast and partial updates, and both bitplanes;
- an SPI/CS trace around wake, full init, plane start, refresh and sleep, checking the number
  of full-init sequences matches the old build exactly.

## The decision — taken 2026-08-05

**Option 1: accept the newer library.** One vendored copy for every target, no re-vendor, no
re-opened vendored edits. The three options as originally framed:

1. **Accept the newer library.** — **CHOSEN.**
2. ~~Re-vendor `bb_epaper` at the source repo's older revision~~ — rejected; it trades this
   problem for the reverse one on the ESP32, which is the target that has actually run.
3. ~~Port the `BBEP_SKIP_BUSY_WAIT` workaround forward as a documented `OD-PATCH`~~ — not taken.

### What accepting this accepts

Stated explicitly, because option 1 does not make the differences go away — it decides to live
with them:

- The four panels above now receive the newer library's initialisation sequences. Where the
  shipped firmware and this build disagree, **this build is now the intended behaviour**, not a
  regression to be reverted.
- **`EP42YR_400x300` keeps the real busy poll.** The `BBEP_SKIP_BUSY_WAIT` flag is not coming
  back. If that panel's BUSY line does not reach idle, operations on it wait up to 30 s instead
  of the source's fixed 200 ms. This is the sharpest consequence of the decision and it is
  accepted along with the rest.

### The one thing that would reopen this

Hardware showing `EP42YR_400x300`'s BUSY line does not reach idle. That is a fact about the
panel, not about the library version, and it would make option 3 necessary irrespective of which
library is vendored. Reopen on that evidence and nothing else — a slow refresh that still
completes is the new normal, not a trigger.
