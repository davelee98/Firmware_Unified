# Commit record correction

## `797ccc0` also carried an unrelated change

`797ccc0` ("fix(session): close the eight findings from the landed-code review") also contains
`targets/nordic-zephyr/src/opendisplay_ble.c`, which is **not part of the review fixes** and is
not mentioned in that commit message.

What it is: a boot-display / advertising race fix. `opendisplay_ble_init()` now blocks on a new
`s_boot_display_done` semaphore before `start_advertising()`, bounded at
`OD_BOOT_DISPLAY_WAIT_MS` (30 s). A connected central can push a direct-write immediately, and
`opendisplay_display_direct_write_start()` touches the same `BBEPDISP s_epd` and the same panel
GPIO/SPI device the boot-display work-queue thread is mid-sequence on, with no lock between them.
The bound is deliberately shorter than the worst legitimate render (~125 s) so a stuck panel does
not also take BLE/DFU reachability down.

Why it is recorded here rather than split out: the change was authored in the working tree during
the review-fix work and swept in by a `git add -A`. It was already pushed by the time that was
noticed, and separating it would mean force-pushing `feat/od-session` — rewriting history already
on the remote. Correcting the record was judged the better trade.

**Verification status: HARDWARE-VERIFIED 2026-08-15.** It was present in the tree for the full
`tools/check.sh --targets` run that `797ccc0` reports, and it was on the `xiao_nrf52840` that
subsequently passed MIGRATION.md's Gate 2 — so the gate it introduces did not prevent the board
from advertising, connecting, authenticating, or completing an encrypted upload. What that pass
does not tell you is how the 30 s bound behaves against a genuinely stuck panel, since the render
on that board completed normally; the timeout arm is still untested.
