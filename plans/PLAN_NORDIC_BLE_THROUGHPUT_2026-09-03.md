# Plan — nordic-zephyr BLE upload throughput: loop timing and RX/TX tuning

**SUPERSEDED 2026-09-20. Do not implement from this file.**

§ 1 (loop timing) is replaced by `plans/PLAN_NORDIC_IDLE_WAKE_2026-09-20.md`, which owns the
main-loop wake seam. Two designs for one loop cannot both land.

Never carried forward, and still unaddressed — re-plan from measurements rather than lifting
these sections as written:

- § 2 BLE RX/TX queue and controller tuning, § 3 the connection-parameter deferral, § 4 the
  per-target matrix, § 5 the RAM budget.
- Wake sources this file lists beyond connect/disconnect/RX enqueue: CCC subscribe, and the LED,
  buzzer and touch timers. The idle-wake plan leaves those on the periodic service tick.
- The hot/periodic split of `opendisplay_ble_process()` (§ 1.2).

Open review findings against this file, unfixed at the time it was superseded: the Channel
Sounding offset against a 15 ms event length and 15 ms connection interval (§ 2, P1); the shared
HCI RX pool accounting (§ 2); reading PIPE duplicate counts as link-layer retries (§ 0); the
connected wait not clamped to the housekeeping deadline (§ 1); and the double `k_uptime_get_32()`
underflow in the idle-loop remaining delay (§ 1).

Date: 2026-09-03
Baseline: HEAD `182ad2c`.
Targets: `nordic-zephyr` only — boards `xiao_ble` (nRF52840), `xiao_nrf54l15`, `xiao_nrf54lm20a`.
Reference for "as fast or faster": `../Firmware` on nRF52840 (Adafruit Bluefruit / SoftDevice S140).
Nothing under `shared/` changes except one optional log line (§ 0.2); `shared/core/od_pipe.c`,
`od_xfer.c`, `od_rxq.c` and `od_txq.c` are untouched.

## Why this plan exists

BLE upload on `nordic-zephyr` / nRF52840 measures ~10× slower than `../Firmware` on the same
silicon against the same host. The investigation behind this plan found four independent
contributors, all on this target's side of the `od_hal_*` boundary. None is in the protocol, the
wire geometry, the inflate engine, or the panel driver — those are identical on both sides (§ 7.1).

Two of the four are loop timing (§ 1), two are BLE stack provisioning (§ 2, § 3).

## Standing caveats

- **No throughput number in this plan is measured.** The ratios quoted are derived from packet
  timing and Kconfig semantics. Every one of them is an A/B hypothesis, and § 6 is how each gets
  settled. Do not record any of these as verified in
  `docs/HARDWARE_VERIFICATION_CHECKLIST.md` on the strength of the reasoning alone.
- **`build-nrf54l15/` and `build-nrf54lm20/` on this machine are stale** — their `.config` carries
  `CONFIG_BT_L2CAP_TX_MTU=512` / `CONFIG_BT_BUF_ACL_RX_SIZE=512`, predating the move to 256/260.
  The current trees are `build-xiao_nrf54l15/` and `build-xiao_nrf54lm20a/`. Quote Kconfig from
  those, or regenerate.
- `sdc_hci_cmd_vs_event_length_set()` **must be called before a connectable advertiser starts**
  (`nrfxlib/softdevice_controller/include/sdc_hci_vs.h:1094`). Event length therefore cannot be
  raised for the duration of a transfer and lowered again for battery — it is a build-time or
  pre-advertise decision. § 2.3 takes the build-time route.

---

## 0. Land the measurement harness first

Nothing else in this plan is worth doing blind. Both items are small and independently useful.

### 0.1 Link-parameter telemetry — `targets/nordic-zephyr/src/opendisplay_ble.c`

`BT_CONN_CB_DEFINE(conn_callbacks)` at [opendisplay_ble.c:604](targets/nordic-zephyr/src/opendisplay_ble.c#L604)
registers only `.connected` / `.disconnected`. There is currently **no way to know whether the 2M
PHY, DLE-251 or the requested interval were granted** — `request_fast_link()`
([opendisplay_ble.c:248-272](targets/nordic-zephyr/src/opendisplay_ble.c#L248-L272)) asks for all
three and logs only synchronous errors, and § 3 shows one of those three returns success without
sending anything.

`../Firmware` has had this since its own throughput work — `logLinkParams()`
(`../Firmware/src/ble_transport_nrf.cpp:94-110`) prints
`[LINK <phase>] PHY=2M ATT_MTU=247 DLE=251 octets connInterval=7.50 ms` at INFO, once at connect
(DEBUG) and once ~2.5 s later when negotiation has settled (INFO).

Add the three Zephyr equivalents to the same `BT_CONN_CB_DEFINE`:

| Callback | Reports |
|---|---|
| `.le_phy_updated(conn, param)` | `param->tx_phy` / `rx_phy` — 1M vs 2M |
| `.le_data_len_updated(conn, info)` | `info->tx_max_len` / `rx_max_len` — 27 vs 251 |
| `.le_param_updated(conn, interval, latency, timeout)` | the interval actually in force, in 1.25 ms units |

One INFO line each, wording deliberately close to `../Firmware`'s so captures from the two repos
can be diffed by eye. `.le_param_updated` also fires when the **central** changes the interval, so
it is the direct evidence for § 3.

No new Kconfig is needed: `CONFIG_BT_USER_PHY_UPDATE=y` and `CONFIG_BT_USER_DATA_LEN_UPDATE=y` are
already set in every one of the three builds (verified in each `build-xiao_*/zephyr/zephyr/.config`).

### 0.2 Transfer rate is already logged — use it, don't add one

`od_xfer_terminal_capture()` already computes `rate_tenths`
([od_xfer.c:371-376](shared/core/od_xfer.c#L371-L376)) as tenths of KiB/s over
`elapsed_ms`, and `od_pipe_log_suffix()` ([od_pipe.c:101-125](shared/core/od_pipe.c#L101-L125))
appends `p[f=… a=… r=… d=… q=…]` — frames, SACKs, reordered, duplicates, max queued — at INFO on a
completed transfer.

That is the A/B instrument for this whole plan, and it needs no change. `a=` (SACK count) and `d=`
(duplicates) are the two fields that separate the failure modes: duplicates climbing means the link
layer is retransmitting, which is § 2's problem; SACK count flat with a low rate means the ACK
round trip is stalling, which is § 1's.

**Deliverable of § 0:** a captured `DW complete` line plus the three `[LINK]` lines from each of the
three boards, at HEAD, before any tuning. This is the control arm.

---

## 1. Loop timing

### 1.1 What is wrong

[main.c:59-67](targets/nordic-zephyr/src/main.c#L59-L67), the connected arm of the superloop:

```c
if (opendisplay_ble_is_connected()) {
        opendisplay_ble_process();
        k_msleep(10);
        continue;
}
```

`../Firmware`'s equivalent is `delay(1)` when work is in flight
(`../Firmware/src/main.cpp:1053`), **and** its cooperative wait returns early the moment a frame
arrives — `if (bleRxQueuePending() || ble.eventPending()) return;` inside `idleDelay()`
(`../Firmware/src/main.cpp:1105`). So the reference implementation has a 1 ms floor and an
event-driven escape; this target has a 10 ms floor and no escape at all.

Three consequences, in descending order of cost:

1. **SACK latency.** PIPE frames are pushed to the SPSC ring on the BT RX thread
   ([opendisplay_pipe.c:44-56](targets/nordic-zephyr/src/opendisplay_pipe.c#L44-L56)) and sit there
   until the next 10 ms tick. SACKs are what return window credits to the host, so every ACK
   boundary — every `N` frames, `N=8` by default from py-opendisplay — pays up to a full 10 ms.
2. **TX retry granularity.** `od_hal_radio_send()` maps a `bt_gatt_notify()` `-ENOMEM` to
   `OD_RADIO_RETRY` ([od_hal_radio.c](targets/nordic-zephyr/src/od_hal_radio.c)); `od_txq_process()`
   then `break`s and leaves the frame at the queue head
   ([od_txq.c:206-208](shared/core/od_txq.c#L206-L208)). That file's own comment says pool
   exhaustion under a PIPE upload is "routine rather than exceptional". Each such retry costs one
   whole pass — 10 ms here, 1 ms in `../Firmware`.
3. **Connect during a long idle wait.** `idle_delay_ms()`
   ([main.c:16-33](targets/nordic-zephyr/src/main.c#L16-L33)) chunks at 1000 ms and calls
   `opendisplay_ble_process()` once per chunk. A client that connects while the loop is inside a
   `sleep_timeout_ms` wait is pumped at **1 Hz** until that wait expires. `sleep_timeout_ms` is
   configurable in minutes.

### 1.2 Design

Replace the fixed sleep with a **wake semaphore plus a state-derived wait bound**, and split
`opendisplay_ble_process()` into a hot half and a periodic half so that waking per frame does not
drag the whole housekeeping set with it.

**Wake semaphore.** `K_SEM_DEFINE(s_loop_wake, 0, 1)` — count limit 1, so N gives collapse to one
take and a burst of 16 PIPE frames produces one wake, not 16. `k_sem_give()` is ISR-safe, which
matters for the GPIO and timer sources below. Given from:

| Source | File | Why |
|---|---|---|
| GATT write accepted | `opendisplay_pipe_on_write()`, after `od_rxq_push()` | the RX path — the whole point |
| Connection closed | `opendisplay_pipe_on_connection_closed()` | run deferred cleanup now, not in 10 ms |
| Connected / disconnected | `connected()` / `disconnected()` | break a long `idle_delay_ms` |
| CCC subscribe | `od_ccc_cfg_changed()` | clears the `OD_RADIO_RETRY` "not yet subscribed" case |
| LED step timer | `opendisplay_led.c` timer callback | already sets `s_timer_due` |
| Buzzer step timer | `opendisplay_buzzer.c` `step_timer_cb` | already sets `s_step_due` |
| Touch IRQ | wherever `s_irq_mask` is raised in `od_touch_app.c` | already an edge, currently polled |

The give **must not** live in `shared/` — `od_rxq_push()` is shared and cannot see a `k_sem`. It
goes in the target-side callbacks listed above. This is the same seam `od_rxq_app.h` already uses.

**Wait bound.** The bound must come from *why* the pump stopped, not from whether the RX ring is
non-empty. `opendisplay_pipe_process()` has two different reasons to return with frames still
queued, and they want opposite waits:

- it drained a frame and more are behind it — go straight round again;
- it stopped on a **deferral**. `od_frame_policy(outcome).consume_rx == false` leaves the head
  unconsumed and breaks the drain
  ([opendisplay_pipe.c:151-157](targets/nordic-zephyr/src/opendisplay_pipe.c#L151-L157)). Re-offering
  that head immediately produces the identical answer, because what clears it is TX capacity or a
  finishing config read — neither of which advances without the radio.

So a rule keyed on `od_rxq_pending()` alone would spin at 100% CPU on the deferred head until a
connection event freed a TX buffer. Have the pump report which happened:

```c
typedef enum {
        OD_PUMP_IDLE,      /* ring empty: nothing to do */
        OD_PUMP_PROGRESS,  /* consumed >=1 frame and the ring is still non-empty */
        OD_PUMP_BLOCKED,   /* stopped on a deferral, or the txq head could not send */
} od_pump_result_t;
```

```
OD_PUMP_PROGRESS -> K_NO_WAIT                            /* zero sleep; real work is queued */
OD_PUMP_BLOCKED  -> K_MSEC(OD_LOOP_TICK_BUSY_MS)         /* waiting on the radio, not on us */
OD_PUMP_IDLE     -> K_MSEC(OD_LOOP_TICK_CONNECTED_MS)
```

`OD_PUMP_PROGRESS` is the hot path and it is **zero sleep** — the loop re-enters with no scheduler
delay at all, which is strictly better than `../Firmware`'s 1 ms poll.

`OD_PUMP_BLOCKED` is deliberately *not* zero sleep, and § 1.4 records why.

**Hot / periodic split.** `opendisplay_ble_process()`
([opendisplay_ble.c:1082-1095](targets/nordic-zephyr/src/opendisplay_ble.c#L1082-L1095)) currently
runs the pipe pump, LED, buzzer, button, touch, NFC, the advertising tick and
`ble_service_advertising()` on every pass. `ble_service_advertising()` calls `connected_count()`,
which is a `bt_conn_foreach()` — a host lock plus a ref/unref per connection. Running that once per
received frame is waste, and it contends with the BT RX thread for the same lock.

Split into:

- `opendisplay_pipe_process()` — called on **every** wake. Already self-bounded to `OD_RXQ_SLOTS`
  iterations ([opendisplay_pipe.c:123](targets/nordic-zephyr/src/opendisplay_pipe.c#L123)).
- `opendisplay_ble_process_periodic()` — the other seven calls, run when the housekeeping deadline
  is due **or** when a periodic-half edge is pending. Add a small
  `opendisplay_ble_periodic_pending()` predicate ORing the flags the timer/IRQ sources already
  raise (`s_timer_due`, `s_step_due`, touch `s_irq_mask`). Without that predicate a 1 ms LED step
  (`OD_LED_MIN_STEP_DELAY_MS`, `shared/core/od_led.h:34`) would be stretched to the housekeeping
  period.

`opendisplay_ble_process()` stays as the composed call for `idle_delay_ms()` and any other caller,
so no external signature changes.

**`idle_delay_ms()` rework.** Keep the total wait duration — the caller uses it as the MSD refresh
cadence — but make each chunk interruptible and bail out entirely on connect:

```c
static void idle_delay_ms(uint32_t delay_ms)
{
        const uint32_t deadline = k_uptime_get_32() + delay_ms;

        while ((int32_t)(k_uptime_get_32() - deadline) < 0) {
                uint32_t remaining = deadline - k_uptime_get_32();
                uint32_t step = MIN(remaining, OD_LOOP_IDLE_CHUNK_MS);

                od_watchdog_app_service();
                opendisplay_ble_process();
                if (opendisplay_ble_is_connected()) {
                        return;      /* let the connected arm take over this pass */
                }
                (void)k_sem_take(&s_loop_wake, K_MSEC(step));
        }
}
```

The early return on connect mirrors `../Firmware`'s `idleDelay()` and fixes consequence 3 above.
Skipping that cycle's `opendisplay_ble_update_msd(true)` is correct and is also what `../Firmware`
does — MSD is not refreshed while a client is attached.

### 1.3 Loop parameters

One header block, `targets/nordic-zephyr/src/opendisplay_constants.h` (which already exists), so
the numbers are named rather than inline literals:

| Constant | Value | Replaces | Rationale |
|---|---|---|---|
| `OD_LOOP_TICK_BUSY_MS` | `1` | — | Matches `../Firmware`'s `delay(1)`. Only reached with a reply stuck on TX capacity; the semaphore covers RX. |
| `OD_LOOP_TICK_CONNECTED_MS` | `20` | `k_msleep(10)` | Connected-but-idle bound. Sized by the **button level poll**, which has no GPIO IRQ in `opendisplay_button.c` and is the only consumer with no event source. Doubling the current 10 ms is a battery *improvement*, safe because RX no longer depends on it. |
| `OD_LOOP_TICK_ADV_PENDING_MS` | `50` | inline `50u` | Unchanged behaviour, named. |
| `OD_LOOP_TICK_IDLE_MS` | `500` | inline `500u` | Unchanged behaviour, named. |
| `OD_LOOP_IDLE_CHUNK_MS` | `1000` | inline `chunk_ms` | Unchanged; now the *maximum* chunk, cut short by the semaphore. |
| `OD_LOOP_HOUSEKEEPING_MS` | `10` | — | Periodic-half cadence. Below `OD_LOOP_TICK_CONNECTED_MS` so a connected-idle pass always runs it; edges pull it forward. |

All three boards take the same values. There is no per-target reason to differ: the nRF54L parts
are faster (Cortex-M33 @128 MHz vs M4F @64 MHz) so if anything they tolerate the busy tick better.

### 1.4 Why `OD_PUMP_BLOCKED` is not zero sleep either

A busy-spin at main is **safe for BLE** on this target and it is worth stating why, so the question
is not reopened: main runs at `CONFIG_MAIN_THREAD_PRIORITY=0`, which is preemptible, while the BT
RX thread is `K_PRIO_COOP(CONFIG_BT_RX_PRIO)` = cooperative
(`zephyr/subsys/bluetooth/host/hci_core.c:4757`) and the system workqueue is at
`CONFIG_SYSTEM_WORKQUEUE_PRIORITY=-1`, also cooperative. Both preempt main whenever they become
ready, so no amount of spinning at main can delay frame reception.

It is still the wrong thing to do here, for two reasons:

1. **It would spin for milliseconds, not microseconds.** `OD_PUMP_BLOCKED` means the ATT/L2CAP TX
   pool is momentarily empty. What refills it is an HCI Number-Of-Completed-Packets for a
   notification the radio has actually sent — that arrives on the **connection-event** timescale
   (7.5-15 ms once § 3 lands), not the instruction timescale. Zero sleep buys nothing against a
   resource that frees on the radio's schedule.
2. **It starves the logging thread.** `CONFIG_LOG_PROCESS_THREAD_CUSTOM_PRIORITY` is unset, so the
   logging thread runs at `K_LOWEST_APPLICATION_THREAD_PRIO`
   (`zephyr/subsys/logging/log_core.c:59-63`) = `CONFIG_NUM_PREEMPT_PRIORITIES - 1` = **14** —
   preemptible, and strictly below main's 0. With `CONFIG_LOG_MODE_DEFERRED=y` and a 4096-byte
   buffer, a main thread that never blocks means the log thread never runs and records are dropped.
   That failure is already familiar on this target. `CONFIG_TIMESLICING` does not help: it rotates
   threads of *equal* priority only, so a ready prio-0 thread still shuts out prio 14 entirely.

If the 1 ms proves measurable in § 6, the ordered options are:

- **Preferred — make it event-driven.** Promote § 1.5: give `s_loop_wake` from a
  `bt_gatt_complete_func_t` passed to `bt_gatt_notify_cb()`, so TX-blocked becomes a real wait on
  the real event with the 1 ms only as a safety net. This is the version that beats `../Firmware`
  on this path rather than matching it.
- **Cheap middle — `k_sleep(K_TICKS(1))`.** One tick is 30.5 us on nRF52840
  (`CONFIG_SYS_CLOCK_TICKS_PER_SEC=32768`) and 32 us on both nRF54L parts (31250). ~30x tighter
  than 1 ms while still yielding, so the logging thread and the idle thread both still get served.
  Note the tick rates are already fine-grained on all three boards — `k_msleep(1)` really is ~1 ms
  here, not a tick-granularity rounding artefact.
- **Not recommended — a bounded spin.** If one is ever added it must call `k_yield()` and carry an
  iteration cap, and it still does not let prio-14 run.

### 1.5 Expected effect

Removes the 10 ms SACK latency and drops TX retry cost 10×. This alone should put the target at or
past `../Firmware`'s loop behaviour, because event-driven wake is strictly better than that repo's
1 ms poll. It does **not** address the link layer, which is § 2 and § 3.

### 1.6 Optional refinement — wake on notify completion

`bt_gatt_notify_cb()` takes a `bt_gatt_complete_func_t` that fires when the notification leaves the
stack. Giving `s_loop_wake` from it would let the `od_txq_depth() > 0` case use `K_FOREVER`-style
event waiting instead of the 1 ms busy tick. This changes `opendisplay_ble_pipe_notify()`'s
contract ([opendisplay_ble.c:644-650](targets/nordic-zephyr/src/opendisplay_ble.c#L644-L650)) and
is not needed to reach parity. Do it only if § 6 shows the busy tick still costing measurable time; § 1.4 is the
full argument for why this, and not a spin, is the answer there.

---

## 2. BLE RX/TX queue and controller tuning

### 2.0 What the Arduino/SoftDevice reference actually tunes

Before proposing numbers, it is worth being precise about what `../Firmware` sets, because the two
stacks do not expose the same knobs and a naive "match the defaults" reading gets it backwards.

`Bluefruit.configPrphBandwidth(BANDWIDTH_MAX)`
(`../Firmware/src/ble_transport_nrf.cpp:222`) expands to
`configPrphConn(247, 100, 3, BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT)`
(`Bluefruit52Lib/src/bluefruit.cpp:246`), which sets exactly four SoftDevice fields
(`bluefruit.cpp:391-403`):

| SoftDevice field | S140 default | BANDWIDTH_MAX | Direction |
|---|---|---|---|
| `ble_gap_conn_cfg_t.event_length` | 3 (= 3.75 ms) | **100 (= 125 ms)** | both |
| `ble_gatt_conn_cfg_t.att_mtu` | 23 | 247 | both |
| `ble_gatts_conn_cfg_t.hvn_tx_queue_size` | 1 | 3 | **TX only** (notifications) |
| `ble_gattc_conn_cfg_t.write_cmd_tx_queue_size` | 1 | 1 (left at default) | **TX only**, and client-role — inert on a peripheral |

**There is no RX buffer count in the SoftDevice API at all.** `ble_gatts_conn_cfg_t` contains
exactly one member, `hvn_tx_queue_size` (`s140_nrf52_7.3.0_API/include/ble_gatts.h:211-213`), and
`ble_gattc_conn_cfg_t` exactly one, `write_cmd_tx_queue_size` (`ble_gattc.h:137-141`). Inbound
write-commands are buffered inside the SoftDevice and surface as `BLE_GATTS_EVT_WRITE` events that
the app pulls with `sd_ble_evt_get()` on Bluefruit's `"BLE"` task at `TASK_PRIO_HIGH`
(`bluefruit.cpp:473`, `:764`). The application never sizes that path.

Three consequences for this plan:

1. **`BT_BUF_ACL_RX_COUNT_EXTRA` and `BT_CTLR_SDC_RX_PACKET_COUNT` have no SoftDevice counterpart.**
   They are artefacts of Zephyr's split host/controller model, where a received ACL packet must be
   copied into a host-owned `net_buf` before ATT can parse it. There is nothing to "match" — but
   there is also nothing protecting us: Zephyr hands the application a knob that the SoftDevice
   never asked it to get right, and this target left it at 2.
2. **The one genuinely comparable RX knob is radio time**, and the gap there is the largest single
   number in this document: **125 ms reserved per interval on `../Firmware` against 7500 us here**,
   a factor of ~33. `event_length` is what BANDWIDTH_MAX is *for* — it is the SoftDevice's only
   throughput lever, and Bluefruit takes it to the ceiling.
3. **Our TX provisioning is already far ahead of the reference** — 12 buffers at every layer against
   `hvn_tx_queue_size = 3`. Nothing on the TX side needs raising; § 1 is what makes those 12 usable
   by cutting retry cost 10x.

The application-level ring is a wash and should be left alone on both sides: `../Firmware` pushes
into `BLE_RX_QUEUE_SLOTS = PIPE_MAX_W + 2` (`../Firmware/src/command_queue.h:63`) and this target
into `OD_RXQ_SLOTS = 34` — the same 34 slots, drained the same way.

**Ranking consequence.** § 2.3 (event length) was originally placed below § 2.2 (buffer counts) on
the grounds that SDC connection-event extension softens the 7500 us. That reasoning still holds,
but this comparison shows event length is the *only* thing the reference implementation deliberately
tuned for throughput, and it tuned it hard. Treat § 2.2 and § 2.3 as co-equal, and if § 6 forces a
choice of one arm to run first, run § 2.3.

### 2.1 The asymmetry

`targets/nordic-zephyr/zephyr/prj.conf` lines 69-81 provision the **TX** side generously, and the
comment above them says exactly why:

> *Config read notifies up to 10 chunks back-to-back from the BT RX thread; the default 3
> buffers/contexts cannot refill mid-loop … provision enough to hold the whole burst.*

That is the **notify** path. An upload runs the other direction, and the RX side was never touched
— it is still at NCS defaults on all three boards:

| Symbol | TX side | RX side |
|---|---|---|
| Host L2CAP/ATT/conn buffers | `BT_L2CAP_TX_BUF_COUNT=12`, `BT_ATT_TX_COUNT=12`, `BT_CONN_TX_MAX=12` | — |
| Host HCI ACL buffers | `BT_BUF_ACL_TX_COUNT=12` | `BT_BUF_ACL_RX_COUNT_EXTRA=1` → **total 2** |
| Link-layer buffers | `BT_CTLR_SDC_TX_PACKET_COUNT=12` | `BT_CTLR_SDC_RX_PACKET_COUNT=2` (NCS default) |

The host ACL RX pool is `1 + CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA`
(`zephyr/include/zephyr/bluetooth/buf.h:155`, pool defined at
`zephyr/subsys/bluetooth/host/buf.c:96`) — **two 260-byte buffers for the entire upload path**.
A 244-byte PIPE value is 247 ATT + 4 L2CAP = 251 bytes, exactly one DLE-251 link-layer PDU, so
every received frame consumes one of those two buffers for the whole trip through the HCI driver
thread (`CONFIG_BT_DRIVER_RX_HIGH_PRIO=6`), the BT RX thread (`CONFIG_BT_RX_PRIO=8`), ATT parsing
and the `od_rxq_push()` memcpy, before it is freed. When both are held, the SDC's own two RX packet
buffers fill, and the link layer stops acknowledging — the central retransmits, burning air time.

The `d=` (duplicate) field in the existing PIPE summary (§ 0.2) is the direct symptom.

### 2.2 Buffer counts

| Symbol | Current | Proposed | Why |
|---|---|---|---|
| `CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA` | `1` (total 2) | **`11`** (total 12) | Mirrors the TX side's 12 and covers a full 8-frame ACK group plus slack, so the host pool stops being the serialization point. |
| `CONFIG_BT_CTLR_SDC_RX_PACKET_COUNT` | `2` (default) | **`8`** | Range is 1..20 (`nrf/subsys/bluetooth/controller/Kconfig:234`). The Kconfig help says >2 is for when "the application is not able to read data fast enough during connection events" — with a 10 ms superloop that is precisely this application. 8 lets an extended connection event absorb a burst while the host recycles net_bufs. Beyond 8 gives little once the host pool is 12. |
| `CONFIG_BT_BUF_ACL_RX_SIZE` | `260` | unchanged | Correct for ATT MTU 256. Do not raise — it multiplies against the new count. |
| `CONFIG_BT_BUF_EVT_RX_COUNT` | `13` | unchanged | Already raised alongside the TX work. |
| TX symbols (all five) | `12` | unchanged | Already sized for the config-read burst; § 1 makes retries cheap. |

If a board's RAM check (§ 5) fails, back `BT_BUF_ACL_RX_COUNT_EXTRA` down to `5` (total 6) before
touching `BT_CTLR_SDC_RX_PACKET_COUNT` — the host pool is cheaper per unit of benefit.

### 2.3 Connection event length

`CONFIG_BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT` is **7500 µs** on all three boards. That is the
radio time the controller *reserves* per connection interval.

`../Firmware` reserves **125 ms**: `Bluefruit.configPrphBandwidth(BANDWIDTH_MAX)`
(`../Firmware/src/ble_transport_nrf.cpp:222`) resolves to `configPrphConn(247, 100, 3, …)`
(`Bluefruit52Lib/src/bluefruit.cpp:246`), where `100` is `event_length` in 1.25 ms units. So the
reference implementation reserves the whole interval and this target reserves a slice of it.

For scale: the SoftDevice's own default is 3 units = 3.75 ms
(`s140_nrf52_7.3.0_API/include/ble_gap.h:617-619`), so Bluefruit is not merely inheriting a
generous default — `BANDWIDTH_MAX` raises it 33x on purpose. See § 2.0.

`CONFIG_BT_CTLR_SDC_CONN_EVENT_EXTEND_DEFAULT=y` on all three boards, so the 7500 µs is a soft
floor rather than a hard cap — the controller extends the event while both peers have data and no
higher-priority role conflicts. That is the one thing arguing this item down, and it is why § 2.2
is not simply subordinate to it: extension stops when the peripheral runs out of somewhere to put
data, which is exactly the § 2.2 condition. The two are co-equal, and § 6.1 runs this one first
because it is the knob the reference implementation actually tuned.

| Symbol | Current | Proposed |
|---|---|---|
| `CONFIG_BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT_OVERRIDE` | unset | **`y`** |
| `CONFIG_BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT` | `7500` | **`15000`** |

15 ms covers the upper bound of the interval requested in § 3 (7.5–15 ms) in full, and gives ~50%
duty instead of ~25% against a central that holds 30 ms. Set the `_OVERRIDE` bool as well as the
value: the value's `default` lines are conditioned on it
(`nrf/subsys/bluetooth/controller/Kconfig:140-148`), and this build treats Kconfig
assigned-but-not-taken as an error.

**Do not go higher without evidence.** The reservation is what MPSL arbitrates against; a large one
on the two nRF54 boards, which compile in Channel Sounding, could starve a CS procedure when
`device_flags` bit 5 enables it. 25000 is a legitimate § 6 experiment, not a default.

### 2.4 Not changed, and why

- `CONFIG_BT_L2CAP_TX_MTU=256` / `BT_BUF_ACL_RX_SIZE=260` — correct as-is. 244-byte value → 251-byte
  LL PDU → exactly one DLE-251 packet. `../Firmware` reaches the same 251 via MTU 247. Raising
  either would fragment.
- `CONFIG_BT_RX_STACK_SIZE=8192` — already generous.
- `CONFIG_BT_RX_PRIO=8` / `CONFIG_BT_DRIVER_RX_HIGH_PRIO=6` — both cooperative, both preempt the
  preemptible main thread. Correct already; changing them risks starving the pump.
- `CONFIG_BT_HCI_ACL_FLOW_CONTROL` — controller-to-host flow control. Off, and should stay off:
  it adds an HCI Host-Number-Of-Completed-Packets round trip per buffer, which is the opposite of
  what § 2.2 is buying.
- `CONFIG_BT_CTLR_DATA_LENGTH_MAX=251`, `CONFIG_BT_CTLR_PHY_2M=y`, `CONFIG_BT_USER_PHY_UPDATE=y`,
  `CONFIG_BT_USER_DATA_LEN_UPDATE=y` — all already correct on all three boards. § 0.1 exists to
  confirm they are also *granted at runtime*, which no evidence currently covers.

---

## 3. Connection parameter timing — the silent 5-second deferral

### 3.1 What happens

[opendisplay_ble.c:268](targets/nordic-zephyr/src/opendisplay_ble.c#L268), inside `connected()`:

```c
err = bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(6, 12, 0, 400));
```

In the **peripheral** role Zephyr does not send this. `zephyr/subsys/bluetooth/host/conn.c:3786-3800`
stores the parameters, sets `BT_CONN_PERIPHERAL_PARAM_SET`, and **returns 0** — so the `err != 0`
check on the next line can never fire and the call looks successful. The request is actually sent
from `deferred_work()` (`conn.c:2293-2306`), scheduled at connection establishment with
`CONN_UPDATE_TIMEOUT` = `CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT` = **5000 ms** — the
TGAP(conn_pause_peripheral) pause.

`prj.conf` lines 55-58 already document the 5 s timer's symptom
(`"Send auto LE param update failed (err -128)" ~5s after a short-lived connection`) without
connecting it to this call.

For the first 5 seconds the link therefore runs at whatever the central chose. BlueZ defaults to
30–50 ms. `../Firmware` never requests an interval at all, so it also inherits the central's — but
it pairs that with the 125 ms event reservation of § 2.3, which is how it stays fast at a long
interval.

A second, smaller inconsistency: the GAP Peripheral Preferred Connection Parameters characteristic
advertises 30–50 ms with a 420 ms supervision timeout (`BT_PERIPHERAL_PREF_MIN_INT=24`,
`MAX_INT=40`, `TIMEOUT=42` — all Zephyr defaults, unset in `prj.conf`), which contradicts the
7.5–15 ms / 4 s the code asks for. Centrals that honour PPCP — Android and iOS do; BlueZ generally
does not — are being told the slow numbers. `../Firmware` advertises 20–30 ms
(`BLE_GAP_CONN_MIN_INTERVAL_DFLT`/`MAX` = `MS100TO125(20)`/`MS100TO125(30)`,
`Bluefruit52Lib/src/bluefruit_common.h:52-53`).

### 3.2 Changes

| Symbol | Current | Proposed | Why |
|---|---|---|---|
| `CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT` | `5000` (default) | **`1000`** | Sends the stored 7.5–15 ms request at 1 s instead of 5 s. |
| `CONFIG_BT_PERIPHERAL_PREF_MIN_INT` | `24` (default) | **`6`** | Match what the code actually requests, so PPCP and the L2CAP request tell one story. |
| `CONFIG_BT_PERIPHERAL_PREF_MAX_INT` | `40` (default) | **`12`** | ditto |
| `CONFIG_BT_PERIPHERAL_PREF_TIMEOUT` | `42` (default) | **`400`** | Match the `BT_LE_CONN_PARAM(6, 12, 0, 400)` in `request_fast_link()`. Legal: `timeout > (1+latency) × interval_max × 2`. |

Also add a comment at `request_fast_link()` recording that `bt_conn_le_param_update()` returns 0
without sending in the peripheral role, so the next reader does not re-derive it. That comment is
the non-obvious constraint CLAUDE.md's § Comments asks to keep.

### 3.3 Deliberate deviation, stated

Lowering `BT_CONN_PARAM_UPDATE_TIMEOUT` below 5000 ms departs from the TGAP(conn_pause_peripheral)
recommendation. The risk is a central still doing service discovery when the update lands; some
stacks handle a mid-discovery parameter change poorly. 1000 ms is the common field value and the
host here is py-opendisplay, which discovers and then uploads. If § 6 shows connect-time
regressions on any board, raise to 2000 ms rather than reverting — the 5 s default is what makes
the request useless for short transfers.

PPCP at 6..12 (7.5–15 ms) is below the 15 ms floor iOS enforces. No iOS client exists in this
fleet. If one appears, 12..24 (15–30 ms) is the compatible variant; record the choice rather than
letting it drift.

---

## 4. Per-target matrix

Every symbol below goes in `targets/nordic-zephyr/zephyr/prj.conf` — all three boards want the same
values, and none of the three board fragments under `zephyr/boards/` needs to override any of them.
Putting them in `prj.conf` is what stops the three boards drifting apart.

| Symbol | nRF52840 (`xiao_ble`) | nRF54L15 | nRF54LM20A |
|---|---|---|---|
| `BT_BUF_ACL_RX_COUNT_EXTRA` | 11 | 11 | 11 |
| `BT_CTLR_SDC_RX_PACKET_COUNT` | 8 | 8 | 8 |
| `BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT_OVERRIDE` | y | y | y |
| `BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT` | 15000 | 15000 | 15000 |
| `BT_CONN_PARAM_UPDATE_TIMEOUT` | 1000 | 1000 | 1000 |
| `BT_PERIPHERAL_PREF_MIN_INT` / `MAX_INT` / `TIMEOUT` | 6 / 12 / 400 | 6 / 12 / 400 | 6 / 12 / 400 |
| Loop constants (§ 1.3) | same | same | same |

Per-target notes that do **not** change the values:

- **nRF52840** is the board with a `../Firmware` counterpart, so it is the only one where "match or
  beat" can be measured directly. Do it here first. Console is USB CDC ACM, not RTT
  (`zephyr/boards/xiao_ble_nrf52840.conf`) — capture accordingly.
- **nRF54L15 / nRF54LM20A** compile in Channel Sounding, RAS and transmit power control
  (`zephyr/boards/xiao_nrf54l15_nrf54l15_cpuapp.conf`, `…lm20a….conf`). CS is runtime-gated off by
  `device_flags` bit 5, so it does not contend by default — but § 2.3's reservation is the one
  change that could interact with it if a user enables CS. Test at least one upload on the 54L15
  with CS enabled before calling § 2.3 done.
- Both nRF54 boards are Cortex-M33 @128 MHz against the 52840's M4F @64 MHz, so the host-side
  per-packet cost that § 2.2 relieves is roughly halved there. Expect a smaller § 2.2 delta and a
  proportionally larger § 1 delta on those two.

---

## 5. RAM budget

Current usage, measured from the checked-in build trees at HEAD (`arm-zephyr-eabi-size`):

| Board | `data`+`bss` | RAM region | Free |
|---|---|---|---|
| `xiao_ble` (nRF52840) | 145,675 B | 0x40000 = 262,144 B | ~116 KB |
| `xiao_nrf54l15` | 161,738 B | 0x40000 = 262,144 B | ~98 KB |
| `xiao_nrf54lm20a` | 161,797 B | 0x7FC00 = 523,264 B | ~353 KB |

Cost of the § 2.2 changes:

- **Host ACL RX pool**: 10 extra buffers × (260 B data + `struct net_buf` + user data) ≈ **~3.1 KB**.
- **SDC link memory**: `__MEM_ADDITIONAL_LINK_SIZE` charges `rx_count × (rx_size + 15)`
  (`nrfxlib/softdevice_controller/include/sdc.h:164-171`), with `rx_size = BT_CTLR_DATA_LENGTH_MAX
  = 251`. Going 2 → 8 is 6 × 266 = **1,596 B**.

Total ≈ **4.7 KB**, against ~98 KB of headroom on the tightest board. Comfortable, but confirm
rather than assume: build each board with `-t ram_report` and diff against the control-arm numbers
above. `CONFIG_BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT` costs no RAM.

---

## 6. Verification

### 6.1 A/B protocol

Same board, same image except the change under test, same host, same payload — use a full-panel
compressed upload, which is the longest-running and most window-bound case. Read
`rate_tenths` and the `p[…]` suffix off the `DW complete` line (§ 0.2). Three runs per arm; report
the median, not the best.

Arms, in this order — each is independently revertable, which is the point of the ordering:

| # | Arm | Isolates |
|---|---|---|
| 0 | HEAD, telemetry only (§ 0) | control; also proves PHY/DLE/interval are or are not granted |
| 1 | + § 1 loop rework | SACK latency and TX retry granularity |
| 2 | + § 2.3 event length | duty cycle per interval — the one knob `../Firmware` deliberately tuned (§ 2.0) |
| 3 | + § 2.2 RX buffer counts | link-layer retransmission (watch `d=`); no SoftDevice counterpart exists |
| 4 | + § 3 conn-param timing | the first-5-seconds interval |

If arm 1 alone reaches `../Firmware`'s number, arms 2-4 are still worth landing — they are what
make the result hold when the central picks a long interval — but they stop being urgent.

### 6.2 Regressions to check explicitly

- **Config read** (`0x0044` / the multi-chunk read path) — the TX burst the current buffer counts
  were sized for. It must not regress; § 1's `od_txq_depth() > 0` busy tick is what protects it.
- **Idle battery.** `OD_LOOP_TICK_CONNECTED_MS` 10 → 20 halves connected-idle wakeups, but the
  semaphore adds wakes during transfers. Measure a quiet connected link and a disconnected
  advertising board over ≥10 min on each board.
- **LED and buzzer timing.** Both are already `k_timer`-driven; § 1.2's periodic-pending predicate
  is what keeps a 1 ms LED step from stretching. Run a pattern that uses the minimum step.
- **Button latency.** The only consumer with no event source; bounded by
  `OD_LOOP_TICK_CONNECTED_MS` while connected and `OD_LOOP_TICK_IDLE_MS` otherwise — unchanged from
  today in the disconnected case.
- **Connect / service discovery** after § 3, on every board.
- **Channel Sounding** on the nRF54L15 with `device_flags` bit 5 set, after § 2.3.

### 6.3 Gate

`tools/check.sh --targets` before merge, per CLAUDE.md § Status. A skip is not a pass — the summary
exits 2 on any skip. Note that host tests cover none of this: every change here is either Kconfig
or target-side loop code, so the hardware runs in § 6.1 *are* the evidence.

### 6.4 Record

`docs/HARDWARE_VERIFICATION_CHECKLIST.md` takes the row-level result per board and per arm.
`docs/DIVERGENCE_MATRIX.md` takes § 3's deliberate TGAP deviation and § 3.2's PPCP choice, since
both are wire-visible decisions a future reader will otherwise re-litigate. The `Status` section of
`CLAUDE.md` gets at most one sentence pointing at the checklist.

---

## 7. Ruled out, and rejected options

### 7.1 Confirmed identical on both sides — do not spend time here

- **Wire geometry.** 244-byte value → 247 ATT + 4 L2CAP = 251 = one DLE-251 PDU, on both.
- **PIPE window.** `OD_PIPE_MAX_W`/`_N` are 32/32 here (`shared/core/od_pipe.h:15-19`, not overridden
  by this target) and 32/32 in `../Firmware` (`src/structs.h:52-53`); the host negotiates both down
  to its own W=16 / N=8 defaults.
- **Inflate engine.** Both use the uzlib-derived streaming inflater on nRF52840. `tinfl` is gated
  to ESP32-with-WiFi builds (`../Firmware/src/od_inflate_tinfl.h:56`).
- **Panel SPI.** 8 MHz on both (`od_epd_spi_nrfx.c:28` / `../Firmware/src/display_service.cpp:264`).
  Worth one runtime check, though: `od_epd_bitbang_hz()` returns `0`, so a silent fallback from
  SPIM to the bit-bang path is detectable in a log line — confirm it did not engage.
- **CCM key handling.** The session key lives in a persistent PSA slot
  (`od_hal_crypto.c:85`); it is not re-imported per frame. ~100 µs/frame of software AES against
  ~1.2 ms of air time per packet. Real, but not a 10× term. (`CONFIG_MBEDTLS_BUILTIN` rather than
  nrf_security/CryptoCell is a separate, later question.)
- **Advertising during a connection.** Stops — `od_adv_process()` gates on `connection_count > 0`
  (`shared/core/od_adv_control.c:139`). No radio contention from it.
- **RX ring depth.** `OD_RXQ_SLOTS = 34`, ample for a 32-frame window plus END.

### 7.2 Rejected

- **Dispatching frames on the BT RX thread.** Would remove the loop latency entirely and is wrong:
  the comment at `opendisplay_pipe.c:36-42` records that EPD waits and CCM crypto on that thread
  starve ATT and break service discovery on reconnect. The semaphore gets the latency back without
  moving the work.
- **Shortening `k_msleep(10)` to `k_msleep(1)` and stopping there.** Matches `../Firmware` rather
  than beating it, and burns 1000 wakeups/s while connected-idle. The semaphore is both faster and
  cheaper.
- **Runtime event-length boost for the duration of a transfer.**
  `sdc_hci_cmd_vs_event_length_set()` must be called before the connectable advertiser starts
  (`sdc_hci_vs.h:1094`), so it cannot be toggled per transfer. Build-time only.
- **Raising `CONFIG_BT_L2CAP_TX_MTU` / `BT_BUF_ACL_RX_SIZE`.** Already at the value that makes a
  PIPE frame exactly one LL PDU; larger fragments and costs RAM per the new buffer count.
- **`CONFIG_BT_HCI_ACL_FLOW_CONTROL`.** Adds an HCI round trip per buffer — the opposite of § 2.2.

---

## 8. Step order

Each step builds, flashes and is separately revertable, per CLAUDE.md § Migration constraints
("one subsystem per swap, hardware verify between each").

1. **§ 0.1** link telemetry. Capture the control arm on all three boards.
2. **§ 1** loop rework — `main.c`, `opendisplay_pipe.c`, `opendisplay_ble.c`,
   `opendisplay_constants.h`, plus the `k_sem_give()` sites in `opendisplay_led.c`,
   `opendisplay_buzzer.c`, `od_touch_app.c`. Measure.
3. **§ 2.3** event length — `prj.conf` only. Measure; re-check CS on the nRF54L15.
4. **§ 2.2** RX buffer counts — `prj.conf` only. Measure, and check `ram_report` on all three.
5. **§ 3** connection parameter timing — `prj.conf` plus one comment in `opendisplay_ble.c`.
   Measure, and re-check connect/discovery on all three.
6. **§ 6.4** record results; `tools/check.sh --targets`.

Step 1 is a prerequisite for judging any of 2-5. Steps 2-5 are independent of each other and can be
reordered if a board's evidence suggests it — but land them one at a time, or the measurement
tells you nothing.

## 9. External follow-up (not in scope here)

- `../Firmware` and the sibling repos are read-only from this repository (CLAUDE.md). Nothing in
  this plan touches them; the `logLinkParams()` reference is a read, not a change.
- If § 6 shows software AES-CCM to be material after the four steps land, moving the nRF targets
  from `CONFIG_MBEDTLS_BUILTIN` to `nrf_security`/CryptoCell is a separate plan with its own
  RAM/flash and PSA-behaviour analysis. Note it in `FOLLOWUPS.md`; do not fold it in here.
