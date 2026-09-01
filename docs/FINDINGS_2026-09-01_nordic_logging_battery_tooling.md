# Nordic logging, battery paths and bench tooling — findings

Date: 2026-09-01

Found while investigating why the XIAO nRF54LM20A RTT console was unreadable. The investigation
started at "the log drops lines" and ended up crossing four unrelated subsystems, so the results
are recorded here rather than split across plans that do not yet exist.

**Evidence discipline** follows [FOLLOWUPS.md](FOLLOWUPS.md): `verified` means reproduced directly
against the source or by running it, with the command or `file:line` given. `reported` means it
came from a survey or a single observation and has not been independently reproduced. Do not
promote a `reported` item to a fix without checking it first.

Items 1-4 were fixed in PR #86 and are recorded only as context. Items 5-13 are open.

## Fixed in PR #86 — context only

1. **Nordic emitted a 1 Hz `OpenDisplay alive uptime=` heartbeat** with no counterpart in
   `../Firmware`, whose `loop()` has no periodic logging at all. Removed.
2. **The advertising `tx_power` line printed on every apply**, and the MSD refresh restarts the
   advertiser once per `sleep_timeout_ms` — so a static config fact became a periodic message.
   Now reported on change only.
3. **Two nPM1300 lines reprinted a steady state forever** (see item 6 for what they actually
   reported). Removed.
4. **The debug-profile deferred log buffer was too small to hold a burst.** See item 5 for the
   mechanism, which still applies to the other profiles.

## 5. The deferred-log drop mechanism, and the profiles still exposed to it

**Status: verified 2026-09-01**, from the built `.config` and an SWD dump of the live RTT ring.

Two independent drop layers, both on stock Zephyr defaults:

**Layer 1 — the deferred log buffer.** This is what prints `--- N messages dropped ---`.
`od_hal_log_write()` ends in `LOG_PRINTK("%s", line)`
([`targets/nordic-zephyr/src/od_hal_log.c`](../targets/nordic-zephyr/src/od_hal_log.c)), and in
deferred mode Zephyr copies the **entire formatted line** into the log package — which is why that
file insists on a mutable buffer. At 60-100 characters a line plus ~24 B of header, a 1 KiB
`CONFIG_LOG_BUFFER_SIZE` holds roughly **8-12 records**.

Against that, `CONFIG_LOG_PROCESS_TRIGGER_THRESHOLD=10` wakes the drain thread only at ten pending
messages; otherwise it sleeps `CONFIG_LOG_PROCESS_THREAD_SLEEP_MS=1000`. **Buffer capacity and wake
threshold are the same number**, so a burst reaches the drop point at almost exactly the moment it
would have triggered a drain. `CONFIG_LOG_MODE_OVERFLOW=y` discards the *oldest* records, so an
undersized buffer loses precisely the context for whatever survives. The panel pin dump alone is
seven consecutive lines.

**Layer 2 — the RTT ring. Silent, no message at all.** The backend writes in 16-byte chunks
(`CONFIG_LOG_BACKEND_RTT_OUTPUT_BUFFER_SIZE=16`), so one ~100-byte record is ~7 separate
`SEGGER_RTT_Write` calls. With `CONFIG_SEGGER_RTT_MODE_NO_BLOCK_SKIP`, a ring that fills mid-record
leaves a **truncated line** and announces nothing. `MODE_BLOCK` retries 4x at 5 ms per chunk, so up
to ~140 ms stalled per record, which back-pressures layer 1 into overflowing. The two feed each
other.

`CONFIG_RTT_CONSOLE=y` with `CONFIG_LOG_BACKEND_RTT_BUFFER=0` also puts the printk console and the
log backend on the same RTT up-channel 0.

**Still open.** PR #86 sized this for `PROFILE=debug` only. `zephyr/prj.conf` sets no
`CONFIG_LOG_BUFFER_SIZE`, so the **default/battery profile on both nRF54 boards still runs the
1 KiB buffer with the 1 s drain**. That profile logs at INFO, so it bursts less — but it bursts,
and it drops silently at layer 2. `boards/xiao_ble_nrf52840.conf:26` has carried
`CONFIG_LOG_BUFFER_SIZE=4096` since before this work, with a comment naming the same failure; the
nRF54 boards were simply never given the same treatment.

## 6. nPM1300 conflates "bus error" with "no battery fitted"

**Status: verified 2026-09-01**, `targets/nordic-zephyr/src/opendisplay_sensor_npm1300.c`.

`npm1300_sample()` returns `bool`, and the caller treats `false` as failure. Four things produce
`false`, and only two are failures:

| Line | Condition | Actually a failure? |
|---|---|---|
| :122 | `bus_id == 0xFF` — no `data_bus` in config | No. A config state that cannot change at runtime. |
| :127 | ADC task write NACK'd | Yes — I2C write failed |
| :136 | ADC results read failed | Yes — I2C read failed |
| :153 | `return s_gauge_ok`, i.e. `s_batt_v > 0.5f` | **No** — a fully successful read of a low or absent battery |

The last row is the common case on a board with no cell fitted: the I2C transaction succeeded, the
ADC converted, and the answer is "no battery". Consequences:

- **The retries are meaningless on that path.** `npm1300_sample_retries(s, 3u)` (:249) exists for
  transient bus errors. Re-reading VBAT three times when no battery is connected returns the same
  value three times.
- **`keeping last reading` was false on that path.** `s_batt_v` is assigned at :149, *before* the
  verdict at :150, so the previous reading is already gone. The message was only accurate for the
  two genuine I2C paths.

It does not reach the wire: `npm1300_publish_msd()` gates on `s_gauge_ok` and publishes `0xFF`
(unknown), matching `opendisplay_sensor_npm1300_voltage_volts()` returning `-1.0f` (:186). The
defect was confined to the log, and PR #86 silenced it. **The conflated return value remains.** The
fix is to split it: a bus error deserves retries and a `warn`; an absent battery is a determined
steady state.

## 7. nPM1300 `s_charging` is probably dead code

**Status: reported 2026-09-01.** Register semantics verified against the nPM1300 Product
Specification; runtime behaviour **not** verified — it needs a charger attached, which was not
available.

`opendisplay_sensor_npm1300.c:151`:

```c
s_charging = ((chg_stat & 0x0Fu) == 0x0Cu || (chg_stat & 0x0Fu) == 0x0Du ||
              (chg_stat & 0x0Fu) == 0x0Fu);
```

`chg_stat` is `BCHGCHARGESTATUS` (CHGR base `0x03`, offset `0x34`). Its bit 0 is `BATTERYDETECTED`,
and the nPM1300 Product Specification, CHARGER chapter, states outright:

> Note: Events EVENTBATDETECTED and EVENTBATLOST and status bit BATTERYDETECTED are not available.

The register table marks that field "Reserved (battery is connected)". nPM1304 carries the same
note. So bit 0 always reads 0, which makes `0x0D` (bits 0,2,3) and `0x0F` (bits 0,1,2,3)
**unreachable** — two of the three comparisons are dead. Only `0x0C` can match, and that requires
`TRICKLECHARGE` and `CONSTANTCURRENT` set simultaneously, which the datasheet describes as
sequential phases. The MSD charging bit may therefore never set.

A phase-based form that does not depend on the reserved bit:

```c
s_charging = (chg_stat & 0x1Cu) != 0u;   /* TRICKLE | CONSTANTCURRENT | CONSTANTVOLTAGE */
```

Not applied. Guessing at charge-state semantics against hardware nobody can observe is how a
confidently wrong bit ends up in the advertising payload.

## 8. nPM1300 cannot detect battery presence, and the current method is the only one

**Status: verified 2026-09-01** against the nPM1300 Product Specification.

Worth recording because the obvious approach is wrong and someone will try it. Per item 7,
`BATTERYDETECTED` is not implemented on this part. `NTCSTATUS` (`0x32`) reports only
COLD/COOL/WARM/HOT — there is no "absent" state, and with no thermistor fitted the NTC pin must be
tied to ground and the function disabled via `BCHGDISABLESET`. The datasheet's only presence hint
is negative: *"When attempting to start charging when VBUS is present but no battery is connected,
the host software will see repeated charger events (such as EVENTCHGCOMPLETED)."*

**VBAT via the ADC with a threshold is the only practical method**, which is what the driver
already does (`s_gauge_ok = s_batt_v > 0.5f`). It is correct; it is not a stopgap.

## 9. Battery read paths are two hand-written aggregators, and SoC means two different things

**Status: verified 2026-09-01** by tracing every accessor and its consumers.

Three physical sources, two independent aggregators:

| Aggregator | Order tried | Location |
|---|---|---|
| Nordic | nPM1300 (I2C ADC) -> BQ27220 (I2C gauge) -> SAADC pin | `opendisplay_battery.c:191` |
| ESP32 | BQ27220 (I2C gauge) -> ADC pin | `display_service.cpp:1526` |
| Silabs | cached mV only | `efr32bg22-slc/opendisplay_ble.c:1620` |

Only BQ27220 is shared (`shared/core/od_sensor_bq27220.c`); ESP32 reaches it through a three-line
shim. AXP2101 is power-path only on ESP32, not a voltage source.

**Normalized, and well:** the advertised voltage field — all three targets converge on
`od_advert_battery_10mv_from_mv()`, and `od_advert_build()` clamps *again*, with a comment
explaining that the second clamp exists because Nordic reaches the field already in 10 mV units and
would otherwise push bit 9 into the reserved bit. The accessor contract (`float` volts, `-1.0f` =
unknown) is uniform. So is the SoC byte *encoding*: `0xFF` unknown, else `soc & 0x7F` with `0x80` =
charging.

**Not normalized:**

1. **SoC means two incompatible things in the same wire byte.** BQ27220 reads the gauge's own
   coulomb-counted SoC register (`od_sensor_bq27220.c:151`). nPM1300 uses `soc_from_voltage()` — a
   linear map, 3.30 V -> 0%, 4.20 V -> 100%. A measured state of charge and a voltage guess,
   published identically, with nothing telling the host which it received. **This is the one with
   wire-visible consequences.**
2. **Two hand-written aggregators** implementing the same fallback-ladder idea in different
   languages with different priority orders and no shared code.
3. **nPM1300 bypasses the shared MSD seam.** `od_sensor_app_msd_write()` is a real seam implemented
   by both targets and used by shared bq27220 and sht40; nPM1300 calls
   `opendisplay_ble_set_dynamic_byte()` directly.
4. **The charging bit is defined twice** — `NPM1300_MSD_CHARGING_BIT` and
   `BQ27220_MSD_CHARGING_BIT`. Two names, one wire bit, neither in a shared header.
5. **Voltage and SoC reach the advert by different plumbing** and never consult each other. On a
   board where the gauge fails but SAADC works, voltage is valid while SoC is `0xFF`. Nordic also
   stacks two caches (`OD_BATTERY_TTL_MS` plus nPM1300's own 30 s poll TTL).

None of this is a regression — the area was never part of a promotion plan.

## 10. `rtt.sh` cannot stream a live console on the LM20A

**Status: verified 2026-09-01, reproducible.** Root cause **not** established.

It resolves the control block correctly (`RTT cb: 0x20001070`), prints ~33 valid bytes of the boot
line, then emits stale RAM — including `SigEd25519 no Ed25519 collisions`, a string from the image
rather than the ring. Printable ratio 8.9%.

The firmware is not at fault. An SWD dump of the ring at `0x20000070` is **99.8% printable** and
well-formed throughout, and the control block is intact (`SEGGER RTT` magic, `pBuffer=0x20000070`,
`SizeOfBuffer=0x1000`).

The ring-read loop itself is correct — the `size - rdoff` read at `rtt.sh:303` is the wrap branch,
paired with a second read of `wroff` from the start. The most likely cause is a **start-up race**:
the script reads `buf_ptr`/`size`, then does `reset_and_halt()` -> force `RdOff = 0` -> `resume()`
(`rtt.sh:288-290`), and begins streaming before the firmware has re-initialised the control block,
treating whatever stale `WrOff` it finds as valid. With the old 1 KiB ring the stale tail was small
and the real log dominated; at 4 KiB there is 4x more stale RAM to print. **This hypothesis fits the
evidence but was not confirmed by instrumenting the script.**

Note that `docs/RTT_LM20.md` (PR #85) documents a *different* cause with a similar symptom — a
truncated primary slot leaving MCUboot spinning with the app's RTT block never initialised. That is
not this case: the app was demonstrably running.

**This blocks runtime observation of anything on this board**, including verifying the `tx_power`
gating and `DW start` level that PR #86 merged.

## 11. `ncs-env.sh` looks for the SDK only under `$HOME/ncs`

**Status: verified 2026-09-01.** Every Nordic build front door fails on this machine until
`NCS_ROOT=/opt/nordic/ncs/v3.3.1` is exported by hand:

```
NCS v3.3.1 not found at ~/ncs/v3.3.1.
```

`find_ncs_root()` (`ncs-env.sh:19-20`) checks only `$HOME/ncs/${OD_NCS_VERSION}`, while the
*toolchain* lookup twelve lines further down (`:94-95`) already knows about
`/opt/nordic/ncs/toolchains/` — the install root used by nRF Connect for VS Code. The two lookups
disagree about where Nordic installs things.

Adding `/opt/nordic/ncs/<version>` as a second candidate preserves the version pin intact: it is
still an exact version match, not a glob, which is what the comment at `:9-16` exists to guard
against.

## 12. `tools/check.sh` cannot pass on a stock macOS/Xcode toolchain

**Status: verified 2026-09-01.** Three of the checks fail at **link**, before running anything:

```
ld: library '/Applications/Xcode.app/.../usr/lib/clang/21/lib/darwin/libclang_rt.fuzzer_osx.a' not found
```

Xcode's bundled clang ships no libFuzzer runtime, so `host suite (clang)`, `host suite
(ASan + UBSan)` and `fuzz: pre-auth surface` all fail. Summary reads `40 passed, 3 failed,
3 skipped`. `host suite (gcc)` passes 81/81, so the tests themselves are fine.

This matters beyond one branch: `CLAUDE.md` names `tools/check.sh --targets` as the only merge
gate, and a skip is not a pass. **On this machine the gate cannot be satisfied at all**, so
anything merged here is merged without it. Homebrew LLVM ships the runtime; either require it, or
teach `check.sh` to report the missing runtime as an explicit environment failure rather than three
opaque link errors.

## 13. The partial-write `x 2` is undocumented at both sites

**Status: verified 2026-09-01.** Cosmetic, but it reads as a bug and has already prompted the
question once.

`shared/core/od_xfer_partial.c:120` sets `expected = plane_bytes * 2u`, and
`shared/core/od_xfer.c:780` validates the PIPE path against the same invariant. Neither carries a
comment. It is not a color-plane doubling: the rectangle geometry is computed with
`OD_COLOR_SCHEME_MONO` explicitly, and the protocol requires a 1bpp panel.

The two halves go to the controller's two RAM buffers — `display_service.cpp:1721` routes
`logical < plane_bytes ? PLANE_1 : PLANE_0`, and `third_party/bb_epaper/src/bb_ep.inl:4812-4824`
maps `PLANE_0` to `SSD1608_WRITE_RAM` (the new image) and `PLANE_1` to `SSD1608_WRITE_ALTRAM` (the
previous image). A differential partial update selects the waveform per pixel from the old->new
*transition*, so the controller needs both. These parts have no RAM readback and the firmware keeps
no retained shadow framebuffer, so the host must supply the previous image; `old_etag` only
*validates* which image that is, it does not carry the pixels.

Note the deliberate inversion: a full direct write sends `PLANE_0` then `PLANE_1`
(`display_service.cpp:1692`); a partial sends `PLANE_1` then `PLANE_0`.

## 14. `bt_l2cap: Ignoring data for unknown channel ID 0x003a`

**Status: reported 2026-09-01**, observed once, not reproduced or root-caused.

Zephyr's host parsed a well-formed L2CAP PDU, found no channel registered for CID `0x003a`, and
dropped it (`zephyr/subsys/bluetooth/host/l2cap.c:2879-2886`). This build registers only ATT
`0x0004`, LE signaling `0x0005` and SMP `0x0006`; `CONFIG_BT_EATT` is not set and the application
registers no L2CAP channels. `0x003a` is also below the LE dynamic range (`0x0040`-`0x007F`), so it
cannot be a peer-opened credit-based channel.

Two candidates, not distinguished:

- **(a)** The peer genuinely addressed a SIG-assigned fixed channel this stack does not implement.
  Harmless.
- **(b)** ACL reassembly desynchronised, so the host read *payload* bytes as an L2CAP header. This
  one matters — it means a real ATT PDU was lost, which during a transfer surfaces as a stalled or
  retried chunk.

To discriminate: set `CONFIG_BT_L2CAP_LOG_LEVEL=4`, which enables `LOG_DBG("Packet for CID %u len
%u")` on every PDU. A constant CID at the same point in every connection is (a); varying CIDs under
load is (b). Note `CONFIG_BT_BUF_ACL_RX_SIZE=260` against `CONFIG_BT_L2CAP_TX_MTU=256` leaves little
reassembly headroom if the peer negotiates a large ATT MTU while Channel Sounding is also active.

It appeared ~150 ms after `CS reflector defaults applied`, but that is the connection-setup burst,
not evidence of causation: RAS runs over GATT on ATT `0x0004` and opens no L2CAP channel of its own.

## 15. Logging with no ESP32 counterpart — audit result

**Status: verified 2026-09-01** by extracting every `od_log_*` / `ESP_LOG*` / `printf` format string
from `targets/nordic-zephyr`, `targets/efr32bg22-slc`, `targets/esp32-idf` and `../Firmware/src`
(1,160 ESP32-side call sites), scoring each Nordic/Silabs line by vocabulary coverage against the
ESP32 corpus, then hand-checking every low-scoring candidate against the actual ESP32 code. The
score finds candidates; it does not decide.

**Nordic: 42 of 137 sites have no ESP32 counterpart**, splitting cleanly:

- **27 are correct** — the hardware or feature does not exist on ESP32: Channel Sounding (6),
  nPM1300 (7), nrfx SPIM backend (6), nRF pin decode (3), PSA key slots (3, and HAL internals are
  out of scope by `PLAN_LOGGING_CONVERGENCE_2026-08-30.md` §10), MCUboot SMP OTA (2).
- **15 were Nordic inventions.** PR #86 addressed the two noisiest (heartbeat, `tx_power`). The
  remainder are open and minor: the boot-display retry ladder narrates each attempt
  (`opendisplay_display.cpp:503`, `:532`, `:541`, plus `opendisplay_ble.c:329`) where Firmware's
  whole boot-screen path logs four lines; `adv stop failed` is INFO for a failure; the boot banner,
  `enabling Bluetooth` and the panel rail lines have counterparts in different words.

**Silabs: 54 of 95 sites**, mostly legitimate — ~20 NFC/NDEF decode lines, ~12 BGAPI `sc=0x%04lX`
status reports, plus button/flash/MTU/bootloader-entry, none of which ESP32 has. Note
`PLAN_LOGGING_CONVERGENCE_2026-08-30.md` §10 put BG22 wording explicitly out of scope, and its
`printf`s are its only diagnostics. Four are worth a look because they report **shared**-subsystem
events yet exist only on BG22: `opendisplay_pipe.c:192` (`invariant: shared dispatch deferred an
admitted BGAPI frame`), `:204` (auth-abuse limit), `:284` and `:300` (transport hold/deadline).

**Separately — level divergences**, which are a distinct and more dangerous class than absences:

- `opendisplay_display.cpp:260` logs `BUSY NEVER ASSERTED` at **DEBUG**; `../Firmware` logs the same
  condition at **ERROR** (`display_service.cpp:868`). On a default INFO build the root cause of a
  failed refresh is invisible — only `boot display failed after bounded retry` survives. **This is
  the most consequential open item in this section.**
- `[OD]` prefix at `opendisplay_ble.c:468` is a BG22 `printf` convention; Firmware's bracketed tags
  are subsystem tags (`[BLE][Q:n]`, `[EPD session]`, `[wake]`).

**Method caveat.** The threshold is a candidate filter, not a verdict. A Nordic line reworded beyond
recognition on ESP32 scores low and reached hand review; a Nordic line sharing vocabulary with an
unrelated ESP32 message scores high and never surfaced. The counterpart judgements are the author's,
not the script's.
