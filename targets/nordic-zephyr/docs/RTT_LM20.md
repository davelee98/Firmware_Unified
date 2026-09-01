# Live RTT debugging — XIAO nRF54LM20A

SEGGER RTT is the **only** console on this board. `zephyr/prj.conf` selects
`CONFIG_USE_SEGGER_RTT` / `CONFIG_RTT_CONSOLE` / `CONFIG_LOG_BACKEND_RTT`, and the `battery`
profile builds no UART console at all. Debugging means attaching the probe, not rebuilding.

The LM20A carries an on-board **CMSIS-DAP** probe on its debug USB port — no external J-Link.

This document describes RTT with **pyocd directly**. `./rtt.sh` is a convenience wrapper around
the same mechanism; everything here works without it, and when the wrapper's build-directory
resolution gets in the way, this is the fallback that always works.

## The three things you need

1. **pyocd, installed by you** — `pipx install pyocd`. Do *not* `source ncs-env.sh` first: the NCS
   toolchain ships an older pyocd that frequently lacks the `nrf54lm20a` target. Verify:
   ```bash
   pyocd list --targets | grep nrf54      # want: nrf54lm20a ... builtin
   pyocd list                             # want: Seeed Studio XIAO nRF54LM20A CMSIS-DAP
   ```
2. **The RTT control block address**, from the ELF of the image that is *actually flashed*:
   ```bash
   nm build-nrf54lm20-debug/zephyr/zephyr/zephyr.elf | awk '$3=="_SEGGER_RTT"{print "0x"$1}'
   ```
   Host `/usr/bin/nm` reads ARM ELF symbol tables fine. The toolchain's own copy is at
   `/opt/nordic/ncs/toolchains/*/opt/zephyr-sdk/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm`.
   `CONFIG_SEGGER_RTT_SECTION_CUSTOM` places the block low and stably — currently `0x20000470`.
3. **A TTY.** See "pyocd rtt needs a terminal" below; this bites immediately if you pipe it.

## Live attach — the command

```bash
pyocd rtt -t nrf54lm20a -M attach -a 0x20000470 -s 0x100
```

- **`-M attach` is the whole point.** pyocd's default connect mode **halts** the core. On nRF54L
  the SoftDevice Controller and MPSL share that core with the application, so a halted CPU is a
  silent one — advertising stops and the device vanishes from scanners. `attach` leaves the core
  running; RTT is plain memory reads against live SRAM.
- **`-a` is the start of a *search range*, not the block address.** Passing the exact
  `_SEGGER_RTT` address with a small `-s` turns the search into a direct hit. Do not omit these:
  unbounded, pyocd searches its declared RAM region, and its `nrf54lm20a` map claims 512 KB
  (`0x20000000 + 0x80000`) while cpuapp SRAM is **511 KB**, ending at `0x2007FC00`
  (`CONFIG_SRAM_SIZE=511` in the build's `.config`). The read past the end faults the probe:
  ```
  Memory transfer fault (FAULT ACK) @ 0x2007fc00-0x2007ffff
  ```
  If you genuinely must scan, bound it: `-a 0x20000000 -s 0x20000`.

A successful attach reports the discovered channels before streaming:

```
I 3 up channels and 3 down channels found [rtt_cmd]
I Reading from up channel 0 ("Terminal") [rtt_cmd]
```

Select another with `--up-channel-id` / `--down-channel-id`.

### pyocd rtt needs a terminal

`pyocd rtt` puts the local terminal in raw mode for its interactive session. Redirect or pipe it
and it dies *after* a successful connect, which reads like a board fault but is not:

```
I Reading from up channel 0 ("Terminal") [rtt_cmd]
C Error: (19, 'Operation not supported by device') [__main__]
```

To capture to a file, allocate a pty:

```bash
script -q /dev/null pyocd rtt -t nrf54lm20a -M attach -a 0x20000470 -s 0x100 | tee rtt.log
```

(This is a large part of why `rtt.sh` reimplements the poll loop in Python instead of shelling
out to `pyocd rtt`.)

### What live output looks like

```
[0000.010|C0] I: [WDT] reset reason: LOCKUP (0x10)
[00:00:00.062,417] <inf> fs_nvs: 2 Sectors of 4096 bytes
[0000.062|C0] I: Parsing config: 579 bytes
[0000.063|C0] I: Display 0: RST=16 BUSY=23 DC=189
[0030.265|C0] I: OpenDisplay alive uptime=30265 ms
[0030.266|C0] I: advertising as OD37324D (interval=1000-1000 ms)
```

Application lines are `[SSSS.mmm|Cn] L: message`; Zephyr subsystem records use Zephyr's own
timestamp format. The debug profile changes verbosity, not that format. Display pin values are
logged in **decimal** — `BUSY=23` is `0x17`, i.e. D5 (see `LM20_NCS.md` § Pin note).

Attaching drains whatever is already in the ring, so you often see history rather than only new
lines. The up buffer is **1024 bytes** (`CONFIG_SEGGER_RTT_BUFFER_SIZE_UP`) in
`SEGGER_RTT_MODE_NO_BLOCK_SKIP`: when the host is not draining, firmware **discards** output
rather than blocking. Gaps in a log are expected if you attach late or stall the reader — they
are not evidence the firmware stopped.

## Alternative: OpenOCD's RTT server

OpenOCD has a native RTT server, which gives you a TCP socket instead of a captive terminal — the
better option if you want RTT and GDB from one connection. The board config
(`boards/seeed/xiao_nrf54lm20a/support/openocd.cfg`) creates a plain `cortex_m` target
(`nrf54lm20a.cpu`) over CMSIS-DAP, which is all the RTT commands need.

```bash
openocd -f boards/seeed/xiao_nrf54lm20a/support/openocd.cfg \
  -c 'init' \
  -c 'rtt setup 0x20000470 0x100 "SEGGER RTT"' \
  -c 'rtt start' \
  -c 'rtt server start 9090 0'
# then, in another shell:
nc localhost 9090
```

> **Unverified on this machine.** No `openocd` binary is installed here (`brew install open-ocd`),
> so unlike the pyocd path above these commands come from the tool's interface and the board
> config, not from a run. The pyocd recipe is the one that has actually been exercised.

If you are already attached with `./debug-nrf54.sh lm20`, the same commands are available from
GDB as `monitor rtt setup …` / `monitor rtt start`.

## When you want GDB too

`./debug-nrf54.sh lm20` attaches OpenOCD + `arm-zephyr-eabi-gdb` with Zephyr thread awareness
(`-rtos Zephyr`), so `info threads` and `thread apply all bt` name every thread.

Thread awareness is a **build-time** export — `zephyr/prj_debug.conf` sets
`CONFIG_DEBUG_THREAD_INFO=y`. Without it a debugger only ever sees whichever thread the CPU was in
when it halted.

**Halting stops the radio**, as above. Memory reads and RTT do not halt; `halt`, `step` and
breakpoints do. GDB's `detach` does not reliably resume this target — check `monitor targets` and
issue `monitor resume` if it comes back halted. Repeated backtraces from a target that never
resumed look exactly like a spinning thread.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `Unable to claim interface for probe <uid>` | Another process holds the probe — or one you just killed has not released it yet. Wait ~10 s and retry; `ps aux \| grep -E 'pyocd\|openocd'`. |
| `Error: (19, 'Operation not supported by device')` *after* a good connect | Not a board fault — `pyocd rtt` needs a TTY. Wrap in `script -q /dev/null`. |
| `Memory transfer fault (FAULT ACK) @ 0x2007fc00…` | Search ran past mapped SRAM. Pass the exact `-a` with a small `-s`. |
| Control block not found | Flashed image and ELF disagree. Re-derive the address from the ELF you actually flashed, or re-flash. |
| Total silence | Usually MCUboot never chainloaded: a truncated primary slot fails validation, `main.c` hits `FIH_PANIC`, and the core spins in the bootloader with the app's RAM — and its RTT block — never initialised. Re-flash `./flash-nrf54lm20.sh` (factory). |
| Plausible but wrong log text | The ELF is a different build or board than what is flashed. The address resolves, the reads succeed, and the content is garbage. |
| Device stops advertising while debugging | A halted core. Use `-M attach`; see above. |
| Gaps in the log | `NO_BLOCK_SKIP` with a 1024-byte up buffer discards output when unread. Expected. |
| `refresh: BUSY NEVER ASSERTED` while the panel renders correctly | Not an RTT problem. Almost always the carrier board: the ePaper Driver Board V2 puts BUSY on D2 (`0xBE`), the Breakout Board on D5 (`0x17`), and nothing else differs. See `LM20_NCS.md` § "The carrier board changes BUSY". |

`nm` finding no `_SEGGER_RTT` at all means you are looking at a non-RTT build — the nRF52840
board config disables RTT (no probe on that board; it logs over USB CDC).

## Related

- `docs/LM20_NCS.md` — board NCS notes (pins, PMIC, LED). Its RTT stanza is **stale**: it shows
  `cd Firmware_NRF54` and `BUILD_DIR=build-lm20 … ./rtt.sh` with no variant argument, which skips
  target resolution and leaves the pyocd target at `nrf54l`. It also quotes the SRAM ceiling as
  `0x2007fe40`; the build's own `CONFIG_SRAM_SIZE=511` makes it `0x2007FC00`.
- `../../docs/HARDWARE_MATRIX.md` — profile → log transport table.
- `../../docs/FINDINGS_2026-08-30_NCS_3_4_MIGRATION.md` § F6 — `CONFIG_SRAM_BASE_ADDRESS` /
  `CONFIG_SRAM_SIZE` are deprecated in Zephyr 4.4 in favour of the `zephyr,sram` chosen node but
  still generated. Deriving the address from `nm` (as above) does not depend on either.
