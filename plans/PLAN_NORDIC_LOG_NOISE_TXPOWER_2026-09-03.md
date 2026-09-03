# Plan — cut Nordic log noise, and fix `DW complete`'s timing

Date: 2026-09-03. HEAD `5541b4d`.
Goal: **less log noise, and logs that are complete and mean what they say.** Nothing here is a
subsystem redesign; if a step starts growing one, stop and re-scope.

## 1. "OpenDisplay alive" heartbeat — already gone

Removed in `77e89b4` (PR #86), both call sites and the `ticks` counter.
[main.c](targets/nordic-zephyr/src/main.c) is 87 lines and has neither. The capture that prompted
this predates that commit, so **step 1 of § 5 is the whole fix**: rebuild and reflash.

## 2. Advertising TX power: apply once, not per advertising restart

PR #86 made the `tx_power` **line** print on change only, which fixed the noise. It did not change
what is **sent**: the HCI command still goes out on every successful `bt_le_adv_start()`
([opendisplay_ble.c:842](targets/nordic-zephyr/src/opendisplay_ble.c#L842)) and every config reload
([:971](targets/nordic-zephyr/src/opendisplay_ble.c#L971)). Advertising restarts on boot,
disconnect, boost engaging, boost expiring (3000 ms), config write, and MSD publication — so the
command runs several times per connection cycle.

It doesn't need to. nrfxlib `sdc_hci_vs.h` says TX power is set on the **advertising role** and can
be changed "without the need of restarting the advertiser", so re-issuing per start rewrites a value
the controller already holds.

### Change — one flag, ~15 lines

```c
/* SDC applies advertising TX power to the ADV ROLE, and can change it on a RUNNING advertiser
 * without restarting it (nrfxlib sdc_hci_vs.h), so applying on every od_hal_adv_start() rewrites a
 * value the controller already holds. Apply only where it can have changed: boot, disconnect,
 * config reload. (That the role value SURVIVES a stop/start is assumed, not documented -- the RSSI
 * check in the plan is what tests it.)
 * Set from the BT RX thread (disconnected()), consumed on the loop thread -- hence the same
 * __atomic_* idiom as s_adv_ended_pending / s_msd_publish_pending beside it. */
static uint8_t s_adv_txp_pending = 1u;   /* boot */
```

1. In `ble_service_advertising()`, after `od_adv_process()`:
   ```c
   if (s_adv.active && !s_adv.faulted
       && __atomic_exchange_n(&s_adv_txp_pending, (uint8_t)0, __ATOMIC_ACQUIRE)) {
           apply_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0);
   }
   ```
   `!s_adv.faulted` is not optional: a refused stop sets `faulted` **without** clearing `active`
   ([od_adv_control.c:139-145](shared/core/od_adv_control.c#L139-L145)), and writing the ADV role of
   an advertiser the controller just failed to command is not defensible.
2. `disconnected()`, beside `od_adv_app_boost()` —
   `__atomic_store_n(&s_adv_txp_pending, (uint8_t)1, __ATOMIC_RELEASE);`
3. `opendisplay_ble_reload_config_from_nvm()` — same, replacing the direct apply at
   [:971](targets/nordic-zephyr/src/opendisplay_ble.c#L971).
4. Delete the apply from `od_hal_adv_start()`
   ([:842](targets/nordic-zephyr/src/opendisplay_ble.c#L842)), and fix the comment at
   [:1075](targets/nordic-zephyr/src/opendisplay_ble.c#L1075) that says the apply happens there.
5. Delete PR #86's change-gating statics
   ([:731-748](targets/nordic-zephyr/src/opendisplay_ble.c#L731-L748)). With the send gated, two
   suppression mechanisms means a same-value config write applies and prints nothing — which reads
   as "the write didn't take".

Declare the flag with the other callback-published facts, not below the advertising-ownership
comment at [:215-226](targets/nordic-zephyr/src/opendisplay_ble.c#L215-L226), which states nothing
under it is touched from a BT callback.

**A failed apply is not retried, deliberately.** The flag clears either way. If the HCI command
fails, the existing error line says so and the level is normally attempted again at the next
disconnect or config write. Two caveats, so nobody reads this as a guarantee: after a
`bt_hci_cmd_send_sync()` error or timeout the controller state is *unknown* rather than
"unchanged" — the command may have executed with its completion lost; and if advertising has
latched `faulted`, the gate above suppresses every further attempt until reboot. Retrying sooner
would need paced
retries, an unbounded `bt_hci_cmd_alloc(K_FOREVER)` turned into a finite timeout, and an epoch
counter to avoid lost updates: about ten times this much code to shave a transient off a
non-safety-critical radio setting. Not worth it. Revisit only if the failure is ever observed.

### Verification

- Boot idle → one `tx_power type=0`.
- Connect / upload / disconnect → one `type=2`, one `type=0`; **no** second `type=0` when the boost
  interval expires 3000 ms later.
- Idle with `sleep_timeout_ms` set → MSD refresh restarts the advertiser with no `tx_power` line.
- Config write changing the level → one line, new value. Same value → still one line. Config write
  *while connected* → the line appears after the disconnect, not at the write.
- **RSSI at a fixed distance, unchanged vs. a pre-change build, measured after the boost→normal
  restart** (not across a connect/disconnect — that restart re-applies, so it proves nothing). This
  is the one check that catches the SDC doc being wrong about persistence. If it fails, revert to
  applying per start and keep PR #86's line suppression.

## 3. Is logging over-buffered? Yes, and mostly already fixed

Nordic logs are deferred: `od_log` → Zephyr log ring → CDC ACM ring → USB. Two answers:

**The `--- N messages dropped ---` lines are the log ring overflowing.** The ~53-line config dump
packages to ~5 KB and cannot fit a 4 KB ring. **PR #86 already fixed this for `PROFILE=debug`**
(`prj_debug.conf`: buffer 8192, wake threshold 1, drain sleep 10 ms), and those override the board's
4096 because `EXTRA_CONF_FILE` merges after `boards/<board>.conf`. The default/battery profile on
xiao_ble still runs 4096 / 10 / 1000 — decide deliberately whether that matters, and say so in the
comment on [boards/xiao_ble_nrf52840.conf:26](targets/nordic-zephyr/zephyr/boards/xiao_ble_nrf52840.conf#L26),
Its stated reason still holds there: the board defconfig sets
`LOG_PROCESS_THREAD_STARTUP_DELAY_MS=4000` for **every** profile, so the battery build rides out
that delay on 4096 while debug now has 8192.

**The stalls during refresh are probably not the device.** Logs arrive *late but intact* with
original timestamps, which is what CDC backpressure looks like when the host stops reading — the
board sets `hw-flow-control` ([overlay:198](targets/nordic-zephyr/zephyr/boards/xiao_ble_nrf52840.overlay#L198),
commit `63fcd9e`) so `poll_out()` blocks the logging thread instead of discarding. The capture came
from a browser web-serial console that reports `NetworkError: The device has been lost` mid-run.

**So: re-capture with a real serial reader before changing anything.** `tio -t` or `ts` — something
that timestamps on arrival; not the browser, not bare `cat`. If the bursts disappear, the current
behaviour is the designed trade and there is nothing to fix.

If they persist, the next steps in rough order of cost:
1. `CONFIG_USBD_CDC_ACM_WORKQUEUE=y` in the board conf — gets CDC TX off the shared coop system
   workqueue. Cheap, defensible on its own merits.
2. `CONFIG_USBD_CDC_ACM_LOG_LEVEL_ERR=y` in `prj_debug.conf` (the *choice* symbol — plain
   `CONFIG_USBD_CDC_ACM_LOG_LEVEL` has no prompt and assigning it is ignored). The CDC TX worker can drop a
   bufferful and stop rearming on an enqueue or transfer error (`usbd_cdc_acm.c:661-670`, `:296` vs
   `:329-331`), and the board defconfig currently silences those errors entirely. If
   `Failed to enqueue` appears in a stall window, it is device-side data loss, not a host artefact.
3. `CONFIG_LOG_BACKEND_UART_BUFFER_SIZE=64` (currently 1, i.e. one `uart_poll_out` per byte).

Do **not** add `od_log_flush()` before a refresh: `log_flush()` is an unbounded loop with no
timeout, and under a stalled host the logging thread is the thing that's blocked.

## 4. `DW complete` includes the panel refresh

`t=36.4s r=5.2KB/s` in the capture, for a transfer that took ~3.6 s — the other ~32.7 s is the panel
refreshing. Real rate is ~52 KB/s, a 10x understatement, and since it varies with refresh time `r=`
can't be compared between runs.

Cause: `elapsed_ms` is computed at snapshot time ([od_xfer.c:369](shared/core/od_xfer.c#L369)) and
the snapshot is taken **after** `od_xfer_app_refresh()` in all three modes — PIPE
[od_xfer.c:964→985](shared/core/od_xfer.c#L964), direct
[od_xfer_direct.c:204→221](shared/core/od_xfer_direct.c#L204), partial
[od_xfer_partial.c:307→314](shared/core/od_xfer_partial.c#L307). `../Firmware` measures to
finalization, before the ACK and refresh, at all three of its call sites
(`display_service.cpp:2415`, `:2462`, `:3061`) — so this is promotion drift, and per CLAUDE.md
`Firmware` wins.

### Change — stamp the end time, don't move the snapshot

Do **not** move the capture calls. Record when the stream finished, and measure to that:

1. Add to the transfer state beside `started_ms`
   ([od_xfer_internal.h:38](shared/core/od_xfer_internal.h#L38)):
   ```c
   uint32_t finalized_ms;   /* when the stream finished, if it did */
   bool     finalized;      /* separate flag so no uptime value doubles as a sentinel */
   ```
   The separate flag follows the same rule `adv_boost_active()` states — 0 is a legal uptime.
2. Stamp both at each finalization point, **after** the final zlib flush and completeness check and
   before the END ACK, so `wr=` and `z=` are settled. Stamp as the **first statement of the shared
   helper** in each mode — unambiguous, and it covers the auto-END entry as well as the explicit
   one: direct — first statement of `finish_refresh()` (validation ends at
   [od_xfer_direct.c:266](shared/core/od_xfer_direct.c#L266), and `:267` calls it); partial — after
   [od_xfer_partial.c:278](shared/core/od_xfer_partial.c#L278); PIPE — in `pipe_finish()` before the
   ACK at [od_pipe.c:289](shared/core/od_pipe.c#L289), where both entries converge.
3. In `terminal_capture_at()` ([od_xfer.c:369](shared/core/od_xfer.c#L369)):
   ```c
   out->elapsed_ms = (s_xfer.finalized ? s_xfer.finalized_ms : now_ms) - s_xfer.started_ms;
   ```
   `od_xfer_clear_state()` already zeroes the struct, so nothing else is needed.

That is one field pair, three stamps and one conditional. It fixes `t=` and `r=` on the success path,
and failure paths that got as far as finalization inherit the same interval for free — while
failures *before* finalization keep measuring to now, which is what you want there.

**Why not move the captures instead.** Moving them ahead of the END ACK means a failure path
captures twice, and `log_final_frame()` lives inside the capture
([od_xfer.c:383](shared/core/od_xfer.c#L383)) — so `DW final frame` would print twice, and
suppressing that needs either a guard flag or snapshot parameters on `od_xfer_fail_active()` and
`od_xfer_pipe_barrier_abort()`. Stamping avoids all of it: no call site moves, no API changes,
nothing captures twice.

### Verification

- Re-run the capture's upload: `t=` ~36.4 s → ~3.6 s, `r=` 5.2 → ~50-55 KB/s. **`rx=`, `wr=`, `n=`,
  `z=` and `p[...]` must be byte-identical** — the change touches only the interval.
- `t=` ends at **stream finalization**, which for a compressed PIPE upload is the `0x0082` END
  frame being processed — not the last `0x0081` chunk (compressed PIPE does not auto-complete;
  [od_pipe.c:536](shared/core/od_pipe.c#L536), [:639](shared/core/od_pipe.c#L639)). In the capture
  that is `0109.277` → ~`0112.9`. What it must **not** span is `refresh: busy asserted` →
  `busy released`.
- Direct and partial too — three stamp sites, so verifying one proves nothing about the others.
- Note `r=` is **decompressed bytes written** per episode, not link throughput (on-wire was
  `rx=2.9KB` over the same interval, ~0.8 KiB/s). Don't read the fixed number as a BLE rate.

**Tests, and do these first.** Neither refresh stub advances the clock
([xfer_test.c:262](tests/host/xfer_test.c#L262), [pipe_test.c:199](tests/host/pipe_test.c#L199)),
which is why the suite never caught this. Advance the clock in both, and add `t=`/`r=` assertions to
partial and PIPE — only direct has them today. **Keep the existing direct expected strings**: they
already encode the correct values, so once the stub advances they become the regression detector.
Confirm they fail against current `shared/` before changing anything.

## 5. Sequencing

Two traps in the build/flash scripts, both of which silently test the wrong image:

- **`flash-nrf52840.sh` copies an existing UF2, it does not build**
  ([:9-34](targets/nordic-zephyr/flash-nrf52840.sh#L9-L34)). A bare re-flash after a change
  reinstalls the previous binary.
- **`flash-nrf52840.sh` is the battery-profile script and ignores `PROFILE`.** `PROFILE=debug`
  makes `build-nrf52840.sh` write to `<dir>-debug`
  ([:9-10](targets/nordic-zephyr/build-nrf52840.sh#L9-L10)) while `flash-nrf52840.sh` always reads
  `<dir>` ([:10](targets/nordic-zephyr/flash-nrf52840.sh#L10)), so pairing them flashes the battery
  image. **Use `flash-nrf52840-debug.sh`** — the build script prints the right one as
  `Flash: ./flash-nrf52840-debug.sh` when it finishes.

Every "flash" below therefore means: `PROFILE=debug ./build-nrf52840.sh`, then
`./flash-nrf52840-debug.sh`.

1. Build + flash at HEAD. **This alone removes the heartbeat.**
2. Capture boot + upload + disconnect with `tio -t`, not the browser. This is § 3's test.
3. § 2 (TX power). Build, flash, run § 2's verification including the RSSI check.
4. § 4 (`DW complete`). Tests first, confirm they fail, then fix. Host suite only — no board needed.
5. Only if step 2 showed real device-side stalls: § 3's numbered steps, rebuild, re-capture.

## 6. Gate

`tools/check.sh --targets` before merge. § 2 is Nordic-only and touches no host test. **§ 4 changes
`shared/core/` for every target** and adds host assertions, so it needs the full host suite across
all three target compositions plus the wire corpus, an ESP32 and Silabs build, and a
`docs/DIVERGENCE_MATRIX.md` entry noting that `elapsed_ms` now measures to stream finalization,
matching `../Firmware`. The snapshot itself still happens where it does; only the interval's
endpoint moves.

`tools/check.sh` cannot pass on a **stock Xcode** toolchain — three checks fail at link on a
missing `libclang_rt.fuzzer_osx.a` (`FINDINGS_2026-09-01` § 12, which notes Homebrew LLVM ships the
runtime). Run it on this Linux box, and record which checks actually ran rather than reporting a
pass.

Hardware rows for § Nordic `xiao_nrf52840` in `docs/HARDWARE_VERIFICATION_CHECKLIST.md`: the step-2
and step-3 captures, TX-power apply count per connection cycle, and the RSSI measurement.

## 7. Deliberately not doing

- **Retry/pacing/epoch machinery for a failed TX-power apply** (§ 2) — the failure is rare and the
  next disconnect or config write gives it another attempt (not a guarantee: that one can fail too).
  The fix costs ~10x the code.
- **`od_log_flush()` at refresh boundaries** — `log_flush()` has no timeout and would hang the main
  thread under exactly the condition it's meant to help.
- **Raising the default-profile log buffer** without deciding it's needed (§ 3).
- **The `s_adv_boost_on` / `s_adv_boost_start_ms` data race** — real, pre-existing, and already
  recorded as `docs/FOLLOWUPS.md` item 20. Not this change.
- **Chasing `elapsed=` on the failure lines** — accepted as-is. Note what the stamp actually does:
  failures *after* finalization (END-ACK, barrier, refresh, final-status) pick up the corrected
  interval for free, so only *pre*-finalization failures still measure to now. Nothing to do either
  way; this is what keeps § 4 to one field pair and three stamps rather than an API change across
  two helpers.
- **Moving `DW final frame` back into stream order.** It prints after the refresh because
  `log_final_frame()` is bundled inside the snapshot function. Cosmetic, DEBUG-only, and unbundling
  it touches every capture site.
