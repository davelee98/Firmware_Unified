# Nordic panel SPIM at 8 MHz

**Status:** proposed; the nRF52840 topology is reconciled with the read-only `../Firmware`
production path, and both nRF54 topologies are reconciled with their NCS 3.3.1 SoC definitions and
board devicetrees; no production code implemented. Revised 2026-08-20 after a source audit of
NCS 3.3.1 nrfx/Zephyr, the Adafruit nRF52 Arduino core and the vendored `bb_epaper` -- see
§ 4.3 (DMA), § 4.5 (release ordering), § 5 (seam) and § 11 (what the audit closed and opened).
Revised again 2026-08-20 to require a retained bit-bang backend on every Nordic board, selected by
pin reachability at acquire time -- see § 4.7, which closes the open decision § 4.6 left standing.
Revised again 2026-08-20 to replace unbounded null-handler polling with bounded event/semaphore
completion, initialize conditional nrfx config fields from the default macro, make nRF54
reachability signal/package-specific, clear `CLOCKPIN` on release, and define the shared chunk-size
and explicit fault-reset contracts.
Revised again 2026-08-20 to default nRF52840 to S0S1 unless electrical qualification selects
H0H1, and to clear both the SPIM `END` event and interrupt-controller pending state during
timeout teardown before permitting reacquisition.
Revised again 2026-08-20 to make the retained reachability fallback the measured fast direct-GPIO
backend from Step 0: SCK/MOSI are decoded and resolved once per acquisition, and the byte loop
contains only pre-resolved Nordic port set/clear operations.
Revised again 2026-08-20 to make teardown backend-aware: the selected-backend enum is the ownership
state, fallback release restores only its saved GPIO/retention state, and no fallback path calls
nrfx or touches the SPIM IRQ.

**Targets:** every `targets/nordic-zephyr` board: `xiao_nrf52840`, `xiao_nrf54l15` and
`xiao_nrf54lm20a`.

## 1. Outcome

Replace the Nordic panel byte loop's GPIO bit-banging with the board's SPIM peripheral at a fixed
8 MHz:

| Board | Peripheral | Qualified panel SCK/MOSI | Instance ceiling |
|---|---|---|---|
| `xiao_nrf52840` (Zephyr board `xiao_ble`) | SPIM2 | P1.13 / P1.15 | 8 MHz |
| `xiao_nrf54l15` | SPIM00 | P2.01 / P2.02 | 32 MHz |
| `xiao_nrf54lm20a` | SPIM23 | P1.04 / P1.06 | 8 MHz |

**On two of the three instances 8 MHz IS the ceiling, not a chosen point below one.** The DT
`max-frequency` for `spi2` and `spi23` is 8 MHz; only `spi00` can go higher. § 11.2 derives this
from the MDK and explains why it is a board-routing consequence rather than a silicon limit.

Use one target-private nrfx backend with compile-time peripheral selection and runtime PSEL
assignment from `DisplayConfig`. The table records the deployed board pairs that must be
hardware-qualified; it does not replace runtime pin decoding.

**Every Nordic board also retains a fast direct-GPIO bit-bang backend behind the same seam.** It
resolves both runtime pins once per acquisition and is selected only when the compiled SPIM
instance cannot reach them -- never because a SPIM transfer failed. § 4.7 defines the rule;
§ 4.4's no-fallback-on-fault policy is unchanged.

The change is successful when every board transfers its qualified frame into panel RAM without a
PIPE tail/PTO stall, a logic analyzer measures an 8 MHz mode-0 clock **on each board separately**,
and all current panel transaction semantics remain intact. Per-family evidence is not per-board
evidence; § 8 is the binding form of this criterion.

## 2. Baseline and reason

`panel/od_bbep_zephyr_io.inl` currently emits every bit with three calls through Zephyr GPIO on
all three boards:

1. SCK low.
2. MOSI to the next bit.
3. SCK high.

A 48,000-byte frame therefore makes 1,152,000 GPIO writes. On `xiao_nrf52840` the observed
transfer took about 3.95 seconds. That measurement is not evidence for either nRF54 board; each
receives its own baseline in Step 0. There is no intentional delay in the image-data loop; the
GPIO API overhead is the limiting factor. The 800 ms rail delay is a separate, required
START-time delay.

**DO NOT RESTATE 3.95 s AS A BUS RATE.** Earlier revisions derived "12.2 KB/s, 97 kHz effective
clock" from it. 3.95 s over 1,152,000 writes is ~3.4 us, or ~220 cycles at 64 MHz, and the call
chain below plausibly costs 40-80 -- so the interval almost certainly includes BLE receive and
inflate rather than the sink alone. § 8 asks for a sink-only measurement as a future gate, which
means one has never been taken. Step 0 must re-measure the *unmodified* sink in isolation before
it measures the production direct-GPIO backend.

The per-write cost is structural, and this is why the direct-GPIO backend in § 2.2 is worth pricing.
`od_gpio_write()` traverses two cross-translation-unit calls (`od_gpio.c`, then the platform pin
codec), a `gpio_dev()` switch, `gpio_pin_set()`'s `port->config`/`port->data` loads and per-pin
invert-mask test, `gpio_pin_set_raw()`, and an **indirect call through the Zephyr driver vtable**
in `z_impl_gpio_port_set_bits_raw()`, before `gpio_nrfx_port_set_bits_raw()` performs the one
`OUTSET` store; the returned `int` is then tested against a `static` array on every call. Nothing
here is a Zephyr defect -- `gpio_pin_set()` is the portable per-pin API and the layering is what
buys portability -- but it is far more machinery than a bit-bang inner loop should touch. For
contrast, the BG22 backend's `digitalWrite()` is a same-translation-unit inline decode plus one
`GPIO_PinOutSet()` store, on a slower core.

**The nRF54 boards will be WORSE per GPIO write, not better.** `NRF_GPIO_HAS_RETENTION_SETCLEAR`
is true on nRF54L, so `gpio_nrfx_port_set_bits_raw()` wraps every store in `port_retain_clear()`
and `port_retain_set()` -- two extra APB writes per edge that compile out on nRF52840. That is the
concrete reason Step 0 requires each board's own baseline instead of extrapolating from the
nRF52840 number.

Firmware_Unified does not currently request 8 MHz on the Nordic path. All four Nordic
`bbepInitIO()` call sites pass `0`; `bb_spi_init()` discards that argument, and `iSpeed == 0` is the
bit-bang sentinel. GPIO bit-banging has no configured frequency: its rate is simply the execution
time of the per-bit calls. The `8000000` calls elsewhere in this repository are ESP32 hardware-SPI
calls and do not affect Nordic. BG22 also passes `0` to its separate bit-bang backend.

At 8 MHz, 48,000 wire bytes take 48 ms of wire time. Driver setup, RAM copying, command traffic
and decompression add overhead, but panel-RAM transmission should remain far below the current
multi-second interval on each board. A two-plane panel has twice the wire volume and is timed
against its actual byte count rather than the EP426's one-plane number.

**48 ms is this design's bound, NOT a donor result.** The donor never approached it; see § 2.1.

The host's finite PIPE tail-probe policy is a separate robustness issue. Faster panel I/O
removes the observed nRF52840 symptom, but this firmware change does not modify
`opendisplay.org`, `py-opendisplay`, or any sibling repository. Record any host timeout change as
external follow-up work.

### 2.1 Donor reconciliation

The only firmware donor for this work is the `TARGET_NRF` path in `../Firmware`, and it informs
the nRF52840 topology only:

- `platformio.ini` defines `SPI_32MHZ_INTERFACE=1`. In the Adafruit nRF52 core's `SPI.cpp`, that
  binds the global `SPI` object used by `bb_epaper` to `NRF_SPIM2` instead of SPIM3.
- The same configuration records why: SPIM3 alone carries nRF52840 anomalies 195 and 198; SPIM2
  makes both irrelevant and still provides the requested 8 MHz maximum.
- `src/display_service.cpp` and `src/split_panel.cpp` pass `8000000` to `bbepInitIO()`.
- The nRF Arduino `SPI.beginTransaction(SPISettings(...))` maps that request to
  `NRF_SPIM_FREQ_8M`, mode 0 and MSB-first, and deliberately leaves the nRF transaction open.
- The Arduino SPI backend initializes nrfx with a null event handler, making transfers blocking;
  it configures SCK and MOSI for `NRF_GPIO_PIN_H0H1` high drive.
- `bb_epaper` leaves nrfx SS disconnected and controls CS/DC in software. Its command-plus-data
  operation retains one CS assertion while DC changes, and its dual-controller modes drive both
  CS pins explicitly.
- The non-ESP `bb_epaper` arm calls `SPI.transfer()` once per byte because Arduino's in-place
  duplex operation clobbers the source. That is an Arduino API limitation, not required panel wire
  behavior and not a loop to reproduce.
- **That per-byte loop is a full EasyDMA transaction per byte, so the donor is field-proven for
  TOPOLOGY, NOT FOR THROUGHPUT.** `SPIClass::transfer(uint8_t)` calls `transfer(&data,1)` ->
  `transfer(buf,buf,1)` -> `nrfx_spim_xfer()`, which stores `TXD.PTR`, `TXD.MAXCNT`, `RXD.PTR`,
  `RXD.MAXCNT`, clears `EVENTS_END`, triggers `TASKS_START`, then busy-polls `EVENTS_END`. Six APB
  writes plus a poll loop plus a four-deep call chain, to buy 8 bits -- 1 us -- of wire time. The
  donor therefore clocks 48,000 separate transactions per frame and its own frame time is well
  above the 48 ms wire bound. Three consequences: Step 3.8's donor comparison is a **wire-shape**
  check and never a throughput pass; § 8's 150 ms gate is not donor-derived (see § 8); and if a
  donor board can be reached, its measured frame time is the single most valuable missing
  datapoint, because it is the only existing field evidence that 8 MHz SPIM clears the PIPE stall.
  Measuring it means observing existing firmware on hardware -- the sibling repos stay read-only.
- The XIAO variant maps the panel connector to SCK P1.13, MOSI P1.15 and unused MISO P1.14.

This plan ports that proven nRF52840 topology from Arduino to Zephyr: SPIM2, 8 MHz, synchronous
single-buffered nrfx, mode 0, MSB-first and manual CS/DC. It deliberately starts with nrfx's S0S1
drive at 8 MHz rather than inheriting the donor's H0H1 setting without electrical evidence; § 4.2
defines the qualification-only escape hatch. Internally, nrfx signals completion through its event
handler and the caller waits on a bounded semaphore; the public panel-bus seam remains blocking.
It does not import Arduino, PlatformIO or the donor's framework wrapper.

The nRF54 instances are not claimed as donor ports. NCS 3.3.1 and the checked-in board devicetrees
establish that `xiao_nrf54l15` exposes SPIM00 on P2.01/P2.02 and `xiao_nrf54lm20a` exposes SPIM23
on P1.04/P1.06. The common nrfx driver supports both peripherals, including the nRF54 clock-pin
configuration required by the SoC. Their correctness is established by the software and hardware
gates in this plan, not inferred from the nRF52840 donor.

There are four deliberate adaptations. First, unified firmware continues to program SPIM PSEL
from the runtime display configuration instead of depending on fixed board pinctrl; the deployed
configurations must still resolve to the qualified pairs above. Second, one common backend chooses
SPIM2, SPIM00 or SPIM23 at compile time instead of duplicating the transfer loop. Third, the new
backend is transmit-only, so it does not reproduce Arduino's in-place duplex clobbering. Fourth,
nrfx errors become target-visible transfer failures instead of being ignored by the Arduino
wrapper. No other sibling supplies design input for this plan.

### 2.2 Price the GPIO-only alternative first

Before changing SPIM ownership, implement and benchmark the production fast bit-bang backend on
each available board. Decode both pins once at acquisition, resolve each to an `NRF_GPIO_Type *`
and bit mask once, and replace the per-edge `od_gpio_write()` path with direct, pre-resolved Nordic
port set/clear operations. Measure the board's qualified frame with logging outside the timed
interval. § 4.7 is the binding implementation contract.

Record its elapsed time and effective clock in the implementation checkpoint. If SPIM proceeds,
retain this implementation as the reachability fallback; do not restore the current generic GPIO
loop. If it meets the 150 ms panel-RAM gate with correct mode-0 edges, stop and ask whether SPIM's
DMA/CPU-offload still justifies the larger change on that board. If it misses, its measured result
is the quantitative justification for SPIM while remaining a faster functional fallback.

**There is a likely third outcome, and it is a user decision, not an implementer's.** A
pre-resolved-port bit-bang may well clear the PIPE tail/PTO stall -- the actual reported symptom --
while still missing 150 ms. State the rule before running: the *gate* is 150 ms, but the
*acceptance question* is whether the stall is gone. Report both answers separately and ask; do not
let a passing stall test silently satisfy a failing timing gate, or the reverse.

## 3. Scope

In scope:

- SPIM2 on nRF52840, SPIM00 on nRF54L15 and SPIM23 on nRF54LM20A; 8 MHz, mode 0, MSB first and
  transmit only on all three.
- Runtime MOSI and SCK selection from `DisplayConfig.data_pin` and `DisplayConfig.clk_pin`.
- Manual CS and DC control exactly where `bb_epaper` controls them today.
- EasyDMA-safe writes from both mutable image buffers and const command tables.
- Compile-time peripheral selection with no runtime registry.
- Explicit SPIM acquire, error reporting, release and GPIO parking.
- A retained fast direct-GPIO bit-bang backend on every Nordic board, with SCK/MOSI resolved once
  per acquisition and selected when the compiled instance cannot reach the configured pins (§ 4.7).
- Three-board build, ratchet, timing and hardware verification.

Out of scope:

- Changing the PIPE protocol, ACK policy, upload window or host PTO constants.
- Changing panel refresh waveforms or the required rail-settle delay.
- Moving Nordic target code into `shared/`.
- Automatically selecting a slower clock for an unqualified panel.
- Falling back to bit-bang because a SPIM init or transfer *failed*. § 4.4 still forbids this; the
  § 4.7 fallback is reachability-driven and is decided before nrfx is touched.
- Claiming that an arbitrary runtime pin pair is hardware-qualified merely because nrfx accepts
  its PSEL values.
- Folding the separate PIPE partial-flag correction into the SPIM commit.

## 4. Decisions

### 4.1 Use one direct nrfx backend with per-board instances

Use the NCS 3.3.1 `nrfx_spim` API directly. Select the register at compile time:

- `NRFX_SPIM_INSTANCE(NRF_SPIM2)` for `CONFIG_OD_PLATFORM_NRF52840`;
- `NRFX_SPIM_INSTANCE(NRF_SPIM00)` for `CONFIG_SOC_NRF54L15`;
- `NRFX_SPIM_INSTANCE(NRF_SPIM23)` for `CONFIG_SOC_NRF54LM20A`.

NCS 3.3.1 uses the generic `CONFIG_NRFX_SPIM=y`; nrfx 4 removed instance-specific enablement
symbols, and `nrfx_spim.c` carries no `NRFX_SPIM*_ENABLED` guard at all. Do not add the obsolete
`CONFIG_NRFX_SPIM2`, `CONFIG_NRFX_SPIM00` or `CONFIG_NRFX_SPIM23` symbols. `NRFX_SPIM_INSTANCE()`
takes the **register pointer** in nrfx 4, not an index -- `NRFX_SPIM_INSTANCE(NRF_SPIM2)`, never
`NRFX_SPIM_INSTANCE(2)`. Add a compile-time error for an unknown Nordic board so a fourth target
cannot silently inherit the wrong peripheral.

**THE nrfx 4 API RETURNS `int` ERRNO, NOT `nrfx_err_t`.** `nrfx_spim_init()` and `nrfx_spim_xfer()`
return `0`, `-EINVAL`, `-EALREADY` and friends. Code written against nrfx 3 habits -- comparing to
`NRFX_SUCCESS` -- will not compile here. `nrfx_spim_config_t.frequency` is likewise **in Hz**
(`NRFX_MHZ_TO_HZ(8)`), not the `NRF_SPIM_FREQ_8M` enum the donor's Arduino wrapper uses.

Configure PSEL from the decoded runtime `DisplayConfig` pin values with
`NRF_GPIO_PIN_MAP(port, pin)`; do not bind the panel bus to a compile-time devicetree pinctrl
group. Reject invalid, identical or target-reserved pins before touching the peripheral; an
instance-inaccessible pair selects the § 4.7 bit-bang backend instead of failing. Runtime pin
selection remains part of the firmware contract. The qualified board pairs in section 1 are the
required hardware evidence, not an assertion that every other accepted pair has been electrically
tested.

Keep `spi2`, `spi00` and `spi23` disabled in their respective application overlays. This prevents
Zephyr's SPI driver and pinctrl from becoming a second owner while direct nrfx owns the peripheral.
Update all three overlay comments to name the direct nrfx reservation instead of claiming that no
hardware SPI path exists -- but **keep the reason each carries**, which is that the pad must have
exactly one owner; that argument survives the change of owner and is the most useful thing in those
files. On LM20A, keep both `spi23` and the unrelated `spi00` disabled; only SPIM23 is selected for
the panel.

This is not a risk to design around: `CONFIG_NRFX_SPIM` depends on `$(dt_has_compat,...)`, and
Zephyr's `dt_has_compat` ignores node `status` (unlike `dt_compat_enabled`). The symbol is
selectable with all three nodes disabled. Still confirm from clean builds that no Zephyr SPI device
is instantiated for them.

Use direct nrfx with a non-null event handler, a connected SPIM IRQ and one `k_sem`. The public
`od_epd_spi_write()` call remains synchronous and single-buffered: it starts one TX-only transfer,
then waits for that transfer's `NRFX_SPIM_EVENT_DONE` before reusing the bounce buffer or returning.
The event handler does no policy work; it records the event and gives the semaphore.

This is required because NCS 3.3.1's null-handler path enters `nrfy_spim_xfer_start()` and
busy-polls `EVENTS_END` with no timeout. `nrfx_spim_xfer()` cannot return if the peripheral never
completes, so the display workqueue can wedge while `main()` continues feeding the watchdog. For a
chunk of `len <= 256`, compute the deadline as the rounded-up wire duration plus a fixed 20 ms
allowance:

```c
timeout_ms = DIV_ROUND_UP(len * 8u * 1000u, OD_EPD_SPI_HZ) + 20u;
```

At 8 MHz this is at most 21 ms. Before every start, reset the semaphore and mark the wait active.
On completion, clear that mark and continue. On timeout, clear it and run the full § 4.5 teardown:
disable the selected IRQ, abort and uninitialize nrfx, clear the peripheral `END` event, clear the
pending IRQ at the interrupt controller, and only then permit a deliberate fault reset and later
reacquisition. Require the `bb_epaper` adapter's cleanup arm to drive CS inactive and latch the
panel-bus fault. Clearing only the semaphore is insufficient: NCS 3.3.1's
`nrfx_spim_uninit()`/`nrfy_spim_int_uninit()` disables the IRQ but does not clear its pending state.
Drain/reset the semaphore as the final software-side cleanup. No timed-out chunk is retried and no
next chunk starts. Connect the disabled node's IRQ explicitly and route its ISR to
`nrfx_spim_irq_handler(&s_spim)`; disabled devicetree status prevents driver ownership, not access
to the node's IRQ metadata.

Clock policy is verified per SoC rather than copied from the donor. On nRF52840, record
`HFCLKSTAT`; HFINT is permitted and BLE is not assumed to hold HFXO between radio events.

**No explicit clock request is required on either nRF54L part.** `spi00`'s `clocks = <&hfpll>`
names a node whose compatible is `fixed-clock`; there is no `clock_control` consumer for it, and
Zephyr's own `spi_nrfx_spim.c` makes no such request. `spi23` carries no `clocks` property at all.
Record the source and domain state per board in Step 1 as evidence, but do not design an
acquire/release path for a request that does not exist.

The nrfx pin setup also enables the nRF54 `CLOCKPIN` attributes required for SCK/MOSI (guarded by
`NRF_GPIO_HAS_CLOCKPIN` in `configure_pins()`); do not replace that setup with generic GPIO
configuration. Hardware qualification measures SCK with BLE idle and active on every board.

**Expect the nRF54L errata 8/212 workaround to be active on `xiao_nrf54l15` and NOT on
`xiao_nrf54lm20a`.** nrfx arms it when the prescaler exceeds 2 with CPHA=0. SPIM00 runs a 128 MHz
core, so 8 MHz is prescaler 16 and the workaround applies -- it raises CSN duration and pokes
offsets 0xc80/0xc84 during transfers. SPIM23 runs a 16 MHz core, so 8 MHz is prescaler 2 and it does
not. Read those register accesses in a trace as nrfx doing its job, not as a defect. If LM20A is
ever dropped to 4 MHz the workaround becomes active there too.

Code inspection confirms that the active `bb_epaper` core never drives `iMOSIPin` or `iCLKPin`;
only the selected IO backend does. The selected SPIM instance may therefore own SCK and MOSI
exclusively from successful init through deinit, while GPIO continues to own CS, DC, reset and
busy. Do not add an Arduino-compatible SPI class or any PlatformIO/framework wrapper.

### 4.2 Keep protocol pins outside the SPIM driver

SPIM owns only SCK and MOSI. MISO and hardware SS are unused. Existing target code continues to
drive DC and both possible CS pins. Preserve these transaction shapes:

- command: assert CS, DC low, transmit command, deassert CS;
- data: assert CS, DC high, transmit bytes, deassert CS;
- command plus data: one CS assertion, command with DC low, payload with DC high;
- dual-CS selection: retain `od_bbep_cs()` behavior without giving either CS to SPIM.

The electrical mode is SPI mode 0: idle-low clock, MOSI sampled on the rising edge. The backend is
fixed at 8 MHz on all three boards; the legacy `speed == 0` bit-bang sentinel must not select the
new frequency.

**Start with nrfx's S0S1 drive at 8 MHz.** Below 32 MHz, nrfx's `configure_pins()` selects
`NRF_GPIO_PIN_S0S1` unless `NRF_SPIM_FORCE_H0H1` is defined. Do not define that symbol in the
initial nRF52840 build merely to copy the donor: H0H1 increases edge current and can increase
ringing and EMI, while the 8 MHz link does not inherently require it. Qualify S0S1 at the panel
connector with the production cable and BLE idle/active. If and only if S0S1 shows inadequate
rise/fall margin or transfer errors attributable to drive strength, repeat the same captures with
H0H1 and record the comparison. If both pass, retain S0S1.

If that evidence selects H0H1, obtain it through nrfx's own compile-time knob rather than rewriting
`PIN_CNF` after init. Under the nRF52840 board conditional, use
`zephyr_compile_definitions(NRF_SPIM_FORCE_H0H1)` so the definition reaches the separately compiled
nrfx library; an application-private target definition is insufficient. Verify the
`nrfx_spim.c` compile-command entry contains the definition only for the qualified nRF52840 image
and that it is absent from both nRF54 builds. The symbol is image-global across SPIM instances,
which is acceptable only while no other SPIM user is linked. On nRF54, preserve nrfx's SoC-specific
GPIO and `CLOCKPIN` setup and change drive strength only if the same HAL contract and electrical
evidence support it. Log the actual backend, instance, frequency and decoded pins.

The unified target and donor both require manual command/data boundaries and a second CS.
Configure nrfx SS as not connected and preserve all CS writes in the `bb_epaper` adapter.
`BBEP_CS_EVERY_BYTE` has no producer in the vendored panel table. Retain its existing compatibility
branch unchanged, but do not optimize the backend around it or apply the bulk-throughput gate to
that hypothetical one-byte-transfer mode.

### 4.3 Copy every span through one EasyDMA bounce buffer

**`bb_epaper` HAS NO CONCEPT OF DMA, AND ITS CONTRACT ACTIVELY DEFEATS ONE.** Three facts from the
vendored source, all verified, drive this decision:

1. **Sources can be flash.** `bbepSendCMDSequence()` (`bb_ep.inl`) passes `&s[1]` -- a pointer
   straight into the `const` init-sequence table -- to `bbepWriteCmdData()`. EasyDMA cannot read
   flash on nRF52840 or on either nRF54L part. This is not a hypothetical address domain; it is the
   init path of every panel family.
2. **The library expects its buffer to be clobbered.** `bbepFill()` re-`memset`s `u8Cache` before
   *every* row, commented "the data is overwritten after each write". The non-`const`
   `uint8_t *` in the backend contract is load-bearing, not an oversight.
3. **Every span is small.** The image path is bounded by `OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE`
   (256 on this target) or the BLE payload (<=253); `bbepFill()` writes `iPitch` per row; LUTs are
   42-44 bytes; commands 1-4. `u8Cache` is 128 bytes (512 for 4-bpp panels).

So the source-address domain is unconstrained, the length is bounded and small, and the buffer
lifetime is the call. Give the common Nordic backend one static bounce buffer, copy every span into
it, and issue one synchronous, interrupt-completed TX-only transfer with the RX buffer null.
**Copy unconditionally**: a 256-byte `memcpy` is on the order of 40 cycles against the 256 us of
wire time it feeds -- roughly 0.02% -- and buying that back with a per-SoC "is this pointer
DMA-reachable" predicate costs two code paths, a fragile address-range table per part, and a bug
class that surfaces only on one panel family's init sequence.

Move the Nordic decompression chunk size out of `opendisplay_display.cpp` into a target-private
header included by both the display owner and SPIM backend. Size the buffer from that shared target
constant and assert the coupling, so the two cannot drift:

```c
BUILD_ASSERT(OD_EPD_SPI_BOUNCE == OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE);
static uint8_t s_dma[OD_EPD_SPI_BOUNCE] __aligned(4);
```

Four-byte alignment is for word-wide `memcpy`, not a hardware requirement. **Chunk at the buffer
size, not at a hardware limit**: all three instances carry `easydma-maxcnt-bits = <16>`, so 65,535
is the hardware cap and 256 is purely a RAM decision. Do not "optimize" on an assumed 255-byte
MAXCNT; that limit belongs to other Nordic peripherals.

Because the copy makes the promise real, the seam takes `const uint8_t *` even though `bb_epaper`
passes non-`const`. Fact 2 above then describes a hazard this backend does not have.

This costs the buffer plus driver state on every Nordic image. Measure each board's exact
static-RAM and flash delta from its link map. Do not allocate a second transfer-sized buffer and do
not change the existing 256-byte inflate scratch buffer.

**No DMM or RAM-domain placement is required on these three parts.** No Nordic devicetree in
NCS 3.3.1 carries `memory-regions` on a SPIM node, so Zephyr's `DMM_MEMORY_SECTION()` /
`NRF_DT_CHECK_NODE_HAS_REQUIRED_MEMORY_REGIONS()` machinery has nothing to bind here and a plain
`static` buffer is reachable. That concern returns only on nRF54H/nRF92. Confirm the absence per
board in Step 1 rather than assuming it forward.

**A note on precedent, because ESP32 looks like one and is not.** `targets/esp32-idf` runs
`SPI_DMA_CH_AUTO` and hands caller pointers -- including those same flash pointers -- straight to
`spi_device_polling_transmit()` with no bounce buffer. It works only because IDF silently detects a
non-DMA-capable pointer and does a `heap_caps_aligned_alloc()` plus `memcpy` per transaction
(`setup_dma_priv_buffer()`, `spi_master.c`). nrfx does nothing of the kind. Do not cite ESP32 as
evidence that `bb_epaper` buffers are DMA-safe. (That IDF behaviour also means ESP32's panel path
carries a hidden per-transaction heap allocation; unrelated to this plan, record it in
`docs/FOLLOWUPS.md` rather than acting on it here.)

### 4.4 Fail explicitly; do not silently fall back

Use synchronous, interrupt-completed nrfx transfers and check both the immediate
`nrfx_spim_xfer()` result and the bounded completion result. Latch the first initialization,
transfer-start or completion-timeout failure in the target backend, log it once with the nrfx
status or timeout, and make the display orchestrator refuse refresh/complete the active transfer
as an error. A failed hardware-SPIM write must not be reported as consumed successfully.

**`-EALREADY` IS NOT A BUS FAULT, AND LATCHING IT WOULD BRICK THE PANEL.** `nrfx_spim_init()`
returns `-EALREADY` when the instance is already initialized. Four call sites can re-acquire, and
at least one (`opendisplay_display_direct_write_start()`) runs `opendisplay_display_abort()` first,
so a benign re-acquire is reachable. A blanket "latch every non-success" rule therefore turns a
bookkeeping slip into a permanently dead panel. `s_backend != OD_EPD_SPI_BACKEND_NONE`, together
with the saved pin pair, is the acquisition truth: a repeat acquire on the same pins returns `true`
without calling nrfx. Only the SPIM selection may call `nrfx_spim_init()`. An `-EALREADY` there
while `s_backend == NONE` means state desynced -- log it loudly, uninitialize the instance that the
return value proves is live, retry once, and only latch if the retry fails. This is the one place
where "check every returned status" and "init and deinit must be idempotent" (§ 4.5) would otherwise
contradict each other.

Use the existing verdict seams rather than inventing another fault protocol:

- `od_xfer_app_write()` returns the offered length only if every SPIM chunk succeeds. On any
  latched failure it returns `0`, which is fewer than the non-empty offered span; shared `od_xfer`
  already treats that as sink refusal and aborts rather than retrying a partially clocked span.
- `od_xfer_app_refresh()` returns `false` without initiating refresh when the bus fault is latched.
- The retained PIPE sink, `opendisplay_display_direct_write_data()`, returns non-zero on the same
  short/failed write so `opendisplay_pipe_write.cpp` rejects the payload.
- Boot rendering checks the latch before fallback and before refresh, then uses its existing
  failed-render/retry policy.

The donor's Arduino wrapper ignores the `nrfx_spim_init()` and `nrfx_spim_xfer()` results. Do not
carry that omission into the new seam. A panel-bus failure must abort the active command through
the unified transfer owner, while watchdog policy remains the only authority for forcing a system
reset.

**Do not fall back to bit-banging because SPIM failed.** A fault-triggered fallback would hide a
pin-ownership, clock-domain or DMA defect and reintroduce the timing failure this change is meant
to eliminate. On a pin pair the compiled instance *can* reach, an initialization or transfer
failure is a visible panel-bus failure and it latches.

That rule is about **faults**, and it is not the same question as **reachability**. A configured
pin pair the compiled instance was never routed to is not a defect -- it is a valid deployment this
build cannot serve, and § 4.7 answers it with the bit-bang backend, decided at acquire time before
nrfx is touched. Keep the two apart: a fault latches, an unreachable pair selects.

### 4.5 Release the selected backend before parking pins

On panel power-down, send any required sleep command first, then release the selected backend before
`opendisplay_display_park_pins()` configures SCK and MOSI as parked GPIOs. Init and deinit must be
idempotent so abort, disconnect, watchdog and ordinary power-down paths converge on the same safe
state. `s_backend` is the ownership state and is set to SPIM or BITBANG only after that backend's
acquisition succeeds; NONE means deinit is a no-op. Do not use one generic `s_ready` boolean for
both backends.

**nrfx DOES LESS ON RELEASE THAN ITS NAME SUGGESTS, AND THE ORDER IS NOT FREE.** `nrfx_spim_uninit()`
resets the GPIO configuration of the pins it finds in PSEL -- it clears **neither PSEL nor ENABLE**.
It also opens with `NRFX_ASSERT(state != NRFX_DRV_STATE_UNINITIALIZED)`, so "safe before init" has
to be the wrapper's backend switch, not a call into nrfx. Save the successfully selected SCK and
MOSI pin numbers and the fallback port/mask pairs in backend state before setting `s_backend`. The
required sequence is:

```c
switch (s_backend) {
case OD_EPD_SPI_BACKEND_NONE:
    return;

#if !defined(OD_EPD_SPI_REQUIRE_SPIM)
case OD_EPD_SPI_BACKEND_BITBANG:
    od_epd_bb_retain_disable_saved(); /* no-op when retention set/clear is absent */
    nrf_gpio_port_out_clear(s_bb_sck.port, s_bb_sck.mask);
    nrf_gpio_port_out_clear(s_bb_mosi.port, s_bb_mosi.mask);
    od_epd_bb_retain_enable_saved();  /* leaves nRF54 retention enabled */
    break;                          /* no nrfx or SPIM IRQ operation in this arm */
#endif

case OD_EPD_SPI_BACKEND_SPIM:
    NRFX_IRQ_DISABLE(NRFX_IRQ_NUMBER_GET(OD_EPD_SPIM_REG));
    nrfx_spim_abort(&s_spim);       /* stop transfer before destroying driver state */
    nrfx_spim_uninit(&s_spim);      /* reads pin list from PSEL, so precedes PSEL clear */
    nrf_spim_event_clear(OD_EPD_SPIM_REG, NRF_SPIM_EVENT_END);
    NRFX_IRQ_PENDING_CLEAR(NRFX_IRQ_NUMBER_GET(OD_EPD_SPIM_REG));
    nrf_spim_disable(OD_EPD_SPIM_REG);
    nrf_spim_pins_set(OD_EPD_SPIM_REG, NRF_SPIM_PIN_NOT_CONNECTED,
                      NRF_SPIM_PIN_NOT_CONNECTED, NRF_SPIM_PIN_NOT_CONNECTED);
#if NRF_GPIO_HAS_CLOCKPIN && defined(NRF_SPIM_CLOCKPIN_SCK_NEEDED)
    nrfy_gpio_pin_clock_set(s_sck_pin, false);
#if defined(NRF_SPIM_CLOCKPIN_MOSI_NEEDED)
    nrfy_gpio_pin_clock_set(s_mosi_pin, false);
#endif
#endif
    k_sem_reset(&s_done);
    break;
}

s_backend = OD_EPD_SPI_BACKEND_NONE;
od_epd_invalidate_saved_pins();
```

The fallback arm uses only the saved direct-GPIO state. It brackets the parked-low writes with the
§ 4.7 retention helpers and never calls nrfx, disables/clears the SPIM IRQ, clears a SPIM event or
touches PSEL/CLOCKPIN; the whole case compiles out with `OD_EPD_SPI_REQUIRE_SPIM`. The SPIM arm
disables the IRQ before abort so no completion handler races teardown. Uninit precedes PSEL-clear
because it reads PSEL to know which pins to reset; clear the peripheral event and interrupt-
controller pending state after uninit and before reacquisition. Disable the peripheral before
PSEL-clear because PSEL is only writable while disabled. Skipping disable/PSEL-clear leaves a
parked GPIO still connected to a peripheral. The explicit `CLOCKPIN` clear is required on nRF54
because `nrfy_gpio_cfg_default()` preserves the attribute that `configure_pins()` enabled.
nRF52840 compiles that arm out. Reset/drain the completion semaphore only in the SPIM arm. Both
successful arms set the backend to NONE and invalidate saved pins only after their cleanup
completes; neither clears the latched fault.

**`opendisplay_display_park_pins()` IS NOT THE COMMON RELEASE BOUNDARY.**
`display_power_set(false)` calls it only inside `if (has_pwr_pin)`, so on a permanently powered
panel (`pwr_pin == 0xFF`) it is never reached at all. Put `od_epd_spi_deinit()` at the **top of
`display_power_set(false)`, ahead of that branch**, so both paths release. After release,
`has_pwr_pin` parks as it does today; `pwr_pin == 0xFF` restores SCK/MOSI as driven-low GPIOs
rather than leaving them disconnected into a live controller. The next acquisition initializes the
board-selected instance again.

The donor leaves its nRF SPI transaction open while the display owns the panel. Preserve that
lifetime, but end it explicitly at the unified display power boundary. Keep the selected-backend
enum as the single ownership state; a separate boolean or reference count could disagree with it
and conceal a leaked acquisition.

The acquire audit has five concrete points in `opendisplay_display.cpp`: the four `bbepInitIO()`
calls in `partial_prepare_panel_ram_hardware()`, `opendisplay_display_boot_screen()`,
`od_xfer_app_begin_full()` and `opendisplay_display_direct_write_start()`, plus the
`display_power_set(false)` release boundary above. Convert all four calls from literal `0` to the
selected backend's reported frequency and verify every failure arm reaches deinit. Changing those
four literals is behaviourally inert outside the backend: no `bb_epaper` core code reads `iSpeed`,
only IO backends do, and this target's own `bb_spi_init()` currently discards it.

**Remove `bb_spi_init()`'s GPIO configuration of MOSI and SCK.** The selected backend owns it:
nrfx configures the SPIM pins, while the direct-GPIO fallback configures its already-resolved
port/pin pairs without another decode or Zephyr device lookup. Leaving the adapter calls in place
after nrfx has claimed the pads is the two-owner bug the three application overlays exist to
prevent; using them for fallback would violate § 4.7's resolve-once contract.

### 4.6 Runtime pins are now constrained by the compile-time instance -- survey before building

**THIS WAS THE LARGEST RISK IN THE CHANGE. § 4.7 IS THE ANSWER TO IT.** Bit-banging works on any
pin `od_pin_decode()` accepts. A SPIM instance does not: on nRF54L15 the 128 MHz `spi00` sits in
the fast peripheral domain and has dedicated signal routes on P2, while LM20A's `spi23` may use P1
or P3 but requires a clock-capable SCK and a nearby data pin on the exact CSP98 package.
Selecting the instance at compile time therefore constrains which runtime pin pairs the *fast* path
can serve. Before § 4.7 that meant a deployed configuration outside the reachable set lost the
panel permanently -- a regression against today's behaviour, produced by a change intended to fix a
performance bug. With the retained bit-bang backend it degrades to the Step 0 measured direct-GPIO
rate instead, which is still slower and more CPU-intensive than SPIM but no longer pays today's
per-edge pin decode and Zephyr GPIO dispatch cost.

The survey below therefore still matters, but its purpose has changed: it no longer decides whether
a device bricks, it decides **how many deployed devices would silently keep bit-banging** after this
change ships. That is a rollout question, and it must be answered with numbers rather than assumed
to be zero.

Three things are required, none optional:

1. **State the constraint as data.** Implement the exact signal-specific table below as the
   `od_board_spim_pin_ok()` predicate named in § 5, taking the already-decoded SCK/MOSI port and
   pin values. A broad `port == 2` or
   `port == 1 || port == 3` test is forbidden: on nRF54, SCK must be a package clock pin and
   SPIM00 has dedicated SCK/SDO routes. Keep the table keyed to the exact package carried by each
   XIAO board.
2. **Survey the deployed configurations before Step 3.** Check the factory/config corpus and
   `../py-opendisplay` for any Nordic display `data_pin`/`clk_pin` pair outside that set. Record
   every such configuration and its Step 0 direct-GPIO rate. The survey blocks rollout only if it
   shows fallback would be the common path rather than the exception.
3. **Decided: an unreachable pair bit-bangs (§ 4.7).** It is logged once, reported through
   `od_epd_spi_backend()`, and runs at its Step 0 measured direct-GPIO rate. It does **not** latch a
   fault. Genuinely invalid pins -- undecodable, `SCK == MOSI`, target-reserved -- are still
   rejected outright by both backends; "unreachable by this instance" is the only case that
   selects.

Items 1 and 2 remain required. Item 1 is what makes the selection deterministic and testable rather
than a comment; item 2 is what tells the project how much of the fleet this change will actually
speed up.

The initial accepted fast-path table is intentionally conservative and contains only pairs whose
signal roles are explicit in the product pin table or whose clock/data proximity is established by
the exact package layout:

| Board / package / instance | Accepted `(SCK, MOSI/SDO)` tuples | Required second test pair |
|---|---|---|
| `xiao_nrf52840`, nRF52840, SPIM2 | Any two distinct decoded, non-reserved GPIOs on P0/P1 | `(P0.29, P0.28)`; P1.13/P1.15 is default |
| `xiao_nrf54l15`, nRF54L15 CSP47 (CAAA), SPIM00 | `(P2.01, P2.02)`, `(P2.06, P2.08)` | `(P2.06, P2.08)` |
| `xiao_nrf54lm20a`, nRF54LM20A CSP98, SPIM23 | `(P1.04, P1.06)`, `(P1.03, P1.05)` | `(P1.03, P1.05)` |

For nRF54L15, both tuples are dedicated SPIM00 SCK/SDO groups and P2.01/P2.06 are clock pins. For
LM20A, SPIM23 is allowed on P1/P3; the CSP98 table marks P1.04 and P1.03 as clock pins, and pairs
them with the physically adjacent P1.06 and P1.05 data pads respectively. The XIAO connector
exposes both LM20A pairs. Seeed's XIAO L15 schematic names `nRF54L15-CAAA-R`; the LM20A schematic's
K10/H9/J9 ball map identifies the CSP98/PAAA footprint. Do not infer cross-products between rows or
accept every pin on the right port. Expanding this table later requires product-spec evidence for
the exact package plus a hardware timing row; an omitted but otherwise valid pair safely selects
bit-bang meanwhile.

### 4.7 Bit-bang fallback: reachability only, never fault recovery

Every Nordic board retains a fast direct-GPIO bit-bang backend behind the § 5 seam. It answers
exactly one question -- *can the compiled SPIM instance reach the pins this device is configured
with?* -- and it is selected **once, at acquire time, before any byte is clocked**. It is a
production backend, not a throwaway benchmark and not today's `od_gpio_write()` loop under a new
name.

| Cause | Known when | Backend |
|---|---|---|
| Pins outside the compiled instance's reachable set | acquire, pre-nrfx | **bit-bang** |
| `nrfx_spim_init()` fails on a reachable pair | acquire | **latch, no fallback** |
| `nrfx_spim_xfer()` fails mid-frame | during transfer | **latch, no fallback** |
| Pins undecodable, `SCK == MOSI`, target-reserved | acquire | **reject** |

Row 1 is a valid deployed configuration this build's instance cannot serve; today's firmware drives
it, so refusing would be a regression this plan introduced. Rows 2 and 3 are the pin-ownership,
clock-domain and DMA defect class § 4.4 exists to surface, and switching backends mid-frame would
additionally leave a partially clocked plane. Row 4 is invalid on both backends and is not a
fallback case at all.

Seven rules, none optional:

1. **Selection is by predicate, not by failure.** The only input is `od_board_spim_pin_ok()` from
   § 4.6 item 1, whose inputs are the SCK/MOSI `(port, pin)` values decoded once from the two config
   bytes at acquisition. There is no code path from an nrfx return value to backend selection and
   the predicate does not decode the pins again.
2. **Selection is sticky for the acquisition lifetime.** Decided in `od_epd_spi_init()`, unchanged
   until `od_epd_spi_deinit()`. Never re-evaluated per chunk, per frame or per transfer.
3. **A fallback selection logs at WARNING, once per distinct pin pair.** `od_log_warn()`, naming
   the decoded SCK/MOSI pins, the compiled instance they were rejected against, and the rate the
   fallback will actually run at. `od_epd_spi_backend()` returns the selection programmatically and
   `od_epd_spi_hz()` reports the bit-bang path's measured-order rate rather than `8000000`, so a
   caller cannot read a fallback acquisition as a SPIM one -- but the log line is what a field
   device gives you, and WARNING is the level that reaches it.

   **The suppression key is the pin pair, not the acquisition and not a config-load event.**
   Acquisition is the wrong granularity: `opendisplay_display.cpp` has four `bbepInitIO()` sites
   against eighteen `display_power_set(false)` sites, so a tag cycles the panel bus several times
   per upload and would emit the same warning on each. The fact being reported is a property of the
   configuration, which changes far more rarely.

   Config load is the right granularity but has no signal to key on: `struct od_config` carries
   `loaded` flags and no generation counter, so keying on it means new plumbing from the config
   reload path into the display layer. Latch the decoded `(sck, mosi)` pair instead. It needs no
   plumbing, and it is strictly more precise than a config-load key in both directions -- a reload
   that leaves the panel pins alone correctly says nothing, and any change to the pins warns again,
   including a change to a *different* unreachable pair. `od_gpio.c` already rate-limits this way
   and says so ("ONCE PER PIN ... further failures on this pin are suppressed"); follow it.

   The latch lives in RAM, so a reboot re-warns. **One warning per boot is the guaranteed floor**,
   which is what makes a field log diagnosable without making it noisy.

   **Do not fold this into the existing panel-pin dump.** That dump is `od_log_debug()`
   (`od_bbep_zephyr_io.inl`), and `shared/core/od_log.h` defaults `OD_LOG_LEVEL` to `OD_LOG_INFO`,
   so every `od_log_debug()` is dead-code-eliminated in a production build. A fallback reported
   there would be invisible on exactly the builds that ship. WARN survives every level this repo
   compiles, debug and production alike, and it is the lowest one that does.

   A SPIM selection stays quiet -- it is the expected path, and warning on it would train the
   warning away. The pins and instance still appear in the debug dump on both paths.

   **The predicate itself still runs on every acquire.** Only the log is latched. Caching the
   *decision* across acquisitions would go stale the moment a config reload changed the pins
   without a matching re-acquire; `od_board_spim_pin_ok()` is a comparison over the already-decoded
   pins, so there is nothing to save by skipping it.
4. **`OD_EPD_SPI_REQUIRE_SPIM` turns the fallback off at build time.** With it defined, an
   unreachable pair is a hard init failure instead of a selection. Off in production; **on for every
   § 8 qualification build**, so a hardware gate cannot be passed by a board that quietly bit-banged
   its way through the rows. This is the one real hazard a fallback introduces and it is closed by
   construction rather than by discipline.
5. **The fallback has a structural and measured performance contract.** § 8's 150 ms sink gate
   still applies to SPIM unless Step 0 proves the direct-GPIO backend meets it. Independently of
   that gate, fallback must beat the unmodified sink on each board and its warning/report must use
   that board's measured effective rate. It must not exceed the plan's 8 MHz panel-clock ceiling;
   if the unthrottled direct loop does, add the smallest deterministic per-SoC hold needed and
   qualify the resulting rate with BLE idle and active. An estimate or another SoC's number is not
   sufficient.
6. **Resolve once; never look up a pin in the byte loop.** At fallback acquisition, call
   `od_pin_decode()` once for SCK and once for MOSI, pass those decoded values to the reachability
   predicate, form each absolute pin with `NRF_GPIO_PIN_MAP()`, and call
   `nrf_gpio_pin_port_decode()` once to retain an
   `NRF_GPIO_Type *` plus a relative-pin bit mask. The byte loop may call only
   `nrf_gpio_port_out_clear()`/`nrf_gpio_port_out_set()` on those saved values. It must not call
   `od_gpio_write()`, `od_pin_decode()`, `gpio_dev()`, `gpio_pin_set()` or any Zephyr GPIO API per
   edge. SCK remains low -> MOSI changes -> SCK rises for every bit, and SCK returns low after the
   span, preserving mode 0 exactly.
7. **Configure directly and handle nRF54 retention outside the loop.** At acquisition, use the
   already-resolved port and relative pin with `nrf_gpio_port_pin_output_set()` rather than calling
   `od_gpio_configure_output()` and decoding again. Clear the output latch before enabling output
   so acquisition cannot produce a high glitch. When `NRF_GPIO_HAS_RETENTION_SETCLEAR` is true,
   bracket that configuration with one retention disable/enable on the saved masks. Then disable
   retention once at the start of each `od_epd_spi_write()` span and re-enable it once before that
   call returns, after driving SCK low. Do not hold retention disabled across panel delays or BUSY
   waits, and never clear/set it per edge. On fallback deinit, bracket the parked-low writes with
   one disable/enable and leave retention enabled before the ordinary GPIO parking path runs. If
   both pins share a port their masks may be combined, but correctness may not depend on that
   optimization. nRF52840 compiles these retention operations out.

**Why this is not the hazard § 4.4 names.** The failure § 4.4 protects against is a defect that
masquerades as working firmware -- a wrong PSEL, a contended pad, an unclocked domain -- which the
board would paper over by quietly running slowly. Under these rules a defect still latches and still
surfaces; the only route to bit-bang is a predicate over two configuration bytes, evaluated before
nrfx is touched, and it is reported in the log and disabled entirely in qualification builds.

**Cost.** The direct bit loop, its two saved port/mask pairs and the selection branch stay in every
Nordic image. Record the flash delta against an `OD_EPD_SPI_REQUIRE_SPIM` build in Step 5 so the
retained cost is a number rather than an assumption. The same source compiles on all three boards;
SoC differences are compile-time retention guards, not separate transfer loops.

## 5. Target seam and files

Add a target-private panel bus seam:

```c
/* Acquire the board's panel SPIM on runtime-configured pins. Idempotent: a repeat
 * call with the same pins is a no-op returning true. Different pins release and
 * re-acquire. */
bool od_epd_spi_init(uint8_t mosi_cfg, uint8_t sck_cfg);

/* Clock out len bytes, MSB first, mode 0. src may live anywhere the CPU can read --
 * flash included -- and is never modified. Returns false and latches on the first
 * failed chunk; later calls return false without touching the peripheral. */
bool od_epd_spi_write(const uint8_t *src, size_t len);

void     od_epd_spi_deinit(void);   /* safe before init, safe repeated */
bool     od_epd_spi_faulted(void);
/* Clear the latched bus fault only while deinitialized. This is called by the
 * display owner's explicit abort/session-reset recovery boundary, never by an
 * ordinary acquire or release. Returns false if the bus is still acquired. */
bool     od_epd_spi_fault_reset(void);
uint32_t od_epd_spi_hz(void);

/* Which backend the current acquisition selected (§ 4.7). Valid only between a
 * successful init and deinit; sticky for that lifetime. */
typedef enum { OD_EPD_SPI_BACKEND_NONE = 0,
               OD_EPD_SPI_BACKEND_SPIM,
               OD_EPD_SPI_BACKEND_BITBANG } od_epd_spi_backend_t;
od_epd_spi_backend_t od_epd_spi_backend(void);
```

`od_epd_spi_write()` dispatches on the selected backend. The bit-bang arm replaces only the
SCK/MOSI edge implementation; CS/DC sequencing stays where it is today, so the § 4.2 transaction
shapes and the `BBEP_CS_EVERY_BYTE` arm are unaffected by which backend is active.

`const uint8_t *` is deliberate even though `bb_epaper` passes non-`const`: the § 4.3 bounce buffer
makes the promise real, and it records that `bbepFill()`'s per-row re-`memset` guards against a
behaviour this backend does not have.

Instance selection follows the repo's existing platform/SoC split
(`zephyr/CMakeLists.txt` already keys on `CONFIG_OD_PLATFORM_NRF52840` with a `CONFIG_SOC_*`
sub-selection for the nRF54 boards):

```c
#if   defined(CONFIG_OD_PLATFORM_NRF52840)
#  define OD_EPD_SPIM_REG  NRF_SPIM2
#elif defined(CONFIG_SOC_NRF54L15)
#  define OD_EPD_SPIM_REG  NRF_SPIM00
#elif defined(CONFIG_SOC_NRF54LM20A)
#  define OD_EPD_SPIM_REG  NRF_SPIM23
#else
#  error "No panel SPIM instance selected for this Nordic board"
#endif

static nrfx_spim_t s_spim = NRFX_SPIM_INSTANCE(OD_EPD_SPIM_REG);
```

The configuration is common to all three; only the instance and the pin-reachability policy differ.
Start from the nrfx default macro so every SoC-conditional member receives its proper default,
especially `dcx_pin = NRF_SPIM_DCX_DEFAULT` on parts that have DCX. A partial designated initializer
would zero an omitted `dcx_pin` and make nrfx configure P0.00 as a live output:

```c
nrfx_spim_config_t cfg = NRFX_SPIM_DEFAULT_CONFIG(
    NRF_GPIO_PIN_MAP(sp, sn),
    NRF_GPIO_PIN_MAP(mp, mn),
    NRF_SPIM_PIN_NOT_CONNECTED,
    NRF_SPIM_PIN_NOT_CONNECTED);               /* CS stays in the bb_epaper adapter */

cfg.frequency = NRFX_MHZ_TO_HZ(8);             /* Hz in nrfx 4, not NRF_SPIM_FREQ_8M */
cfg.mode = NRF_SPIM_MODE_0;
cfg.bit_order = NRF_SPIM_BIT_ORDER_MSB_FIRST;
cfg.irq_priority = DT_IRQ(OD_EPD_SPIM_NODE, priority);
cfg.skip_gpio_cfg = false;                     /* nrfx sets idle level + nRF54 CLOCKPIN */
cfg.skip_psel_cfg = false;
```

Acquire decodes each pin once, rejects undecodable pins and `SCK == MOSI` before touching the
peripheral, then passes those decoded values to the § 4.7 reachability predicate. A pair the
selected instance cannot reach (§ 4.6) selects the bit-bang backend, which resolves the same
decoded values to its saved port/mask pairs without decoding again. Only on the SPIM path does
acquire go on to handle `-EALREADY` per § 4.4. Write copies each chunk through the bounce buffer,
starts a TX-only `nrfx_spim_xfer()` and waits on the § 4.1 bounded semaphore, latching on the first
non-zero start result or completion timeout. Deinit follows the § 4.5 ordering. Ordinary
init/deinit never clears the latch: only `od_epd_spi_fault_reset()`, called while deinitialized
from the display owner's explicit recovery boundary, may do so. A reset also clears stale
completion state before another acquire.

Expected files:

- `targets/nordic-zephyr/panel/od_epd_spi.h` -- the seam above.
- `targets/nordic-zephyr/panel/od_epd_sizes.h` -- target-private owner of
  `OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE`/`OD_EPD_SPI_BOUNCE`, included by the display owner and
  SPIM backend so their compile-time coupling is real rather than file-local.
- `targets/nordic-zephyr/src/od_epd_spi_nrfx.c` -- the single transfer, bounce-buffer, fault and
  lifecycle implementation, with one compile-time selection block for SPIM2/SPIM00/SPIM23, a
  one-event callback/semaphore and bounded timeout, plus the acquire-time backend decision and its
  one-line report.
- `targets/nordic-zephyr/src/od_epd_spi_bitbang.c` -- the Step 0 fast backend: resolve SCK/MOSI to
  saved Nordic port pointers and masks once per acquisition, perform only direct set/clear in the
  byte loop, and bracket each nRF54 write span with retention disable/restore outside the loop.
  Compiled on every Nordic board; compiled out only under `OD_EPD_SPI_REQUIRE_SPIM`.
- `targets/nordic-zephyr/src/platform/nrf52840/` and `.../nrf54/` -- the per-SoC pin-reachability
  predicate lives beside the existing `od_board_*` and `od_pin_codec_*` files, which already carry
  `BUILD_ASSERT`s on the same platform macros. Do not duplicate the nrfx transfer loop per SoC.
- `targets/nordic-zephyr/panel/od_bbep_zephyr_io.inl` -- route byte writes through the seam, move
  `bb_spi_bitbang()` out to the file above, and retain CS/DC sequencing and the `bb_epaper` ABI.
  **Remove `bb_spi_init()`'s `od_gpio_configure_output()` calls on MOSI and SCK.** The fallback
  configures its already-resolved pins once through the direct Nordic port HAL, while nrfx
  configures them on the SPIM path. Leaving the adapter calls in either path would either decode
  twice or recreate the two-owner bug the three overlays exist to prevent. Pin decode, register
  resolution, output configuration and nRF54 retention ownership belong to the seam/backend, not
  this adapter.
- `targets/nordic-zephyr/src/opendisplay_display.cpp` -- replace all four literal speed-zero calls
  with `od_epd_spi_hz()`, acquire at panel bring-up, observe the fault latch through the named
  verdict seams, and release at the top of `display_power_set(false)` ahead of the `has_pwr_pin`
  branch.
- `targets/nordic-zephyr/zephyr/CMakeLists.txt` -- compile the common backend for every Nordic
  board and enforce that exactly one supported SoC selection is active. Leave
  `NRF_SPIM_FORCE_H0H1` undefined by default; add it with board-conditional
  `zephyr_compile_definitions()` only if § 4.2's nRF52840 evidence selects H0H1.
- `targets/nordic-zephyr/zephyr/prj.conf` -- select generic nrfx SPIM once because every supported
  Nordic board now uses the backend; do not repeat the same symbol in three board fragments.
- All three application overlays -- retain disabled Zephyr ownership for the selected panel SPIM
  node and correct their ownership rationale.
- `tools/check.sh` and focused tests -- enforce backend selection and transaction behavior.

Do not put nrfx or Zephyr headers in `shared/`, and do not add SoC conditionals to the vendored
`third_party/bb_epaper` copy. The `shared boundary:` greps in `tools/check.sh` already ban `nrfx`
and `zephyr/` includes from `shared/`; this backend is target-private by construction.

### 5.1 Accepted bounded completion; rejected transfer overlap and double buffering

The non-null nrfx handler in § 4.1 is an internal completion mechanism, not an asynchronous panel
API. `od_epd_spi_write()` does not return until the one outstanding transfer completes or times out,
so `bb_epaper` retains its synchronous CS/DC ordering and the single bounce buffer is never reused
while EasyDMA owns it.

An overlapped ping-pong pipeline that returns before completion or decompresses into a second DMA
buffer while the first clocks is rejected. Its roughly 48 ms maximum frame-wire saving is
negligible beside a panel refresh bounded at ~240 s, while it would require deferred CS/DC barriers
against synchronous library code. The one callback and semaphore exist solely to make completion
bounded and recoverable. Revisit overlap only if a future panel makes wire time comparable to
refresh time.

## 6. Implementation sequence

### Step 0 — Build and benchmark the production direct-GPIO fallback

1. Retain the existing `xiao_nrf52840` 48,000-byte mono-plane result (~3.95 s) as context, then
   measure the unmodified bit-bang sink in isolation on every available board, including
   nRF52840, with its actual wire-byte count.
2. Implement the common fallback algorithm described by § 4.7 in its final production source,
   initially behind a narrow benchmark harness that Step 2 replaces with the panel-bus seam.
   Resolve both pins to saved Nordic port pointers/masks once per acquisition, use only direct port
   set/clear in the byte loop, and bracket each nRF54 write span with retention changes outside the
   loop. Keep CS/DC in their existing owner.
3. Instrument the production source to prove each pin is decoded and each port is resolved exactly
   once per acquisition, with zero `od_gpio_write()`, `gpio_pin_set()` or device lookup calls while
   bytes are clocked.
4. Verify mode-0 ordering with a logic analyzer and record elapsed time/effective clock per board.
   Require it to beat that board's unmodified sink without exceeding 8 MHz; do not substitute
   another SoC's result. If the direct loop exceeds the ceiling, add and record the minimum
   deterministic per-SoC hold required by § 4.7 rule 5.
5. Retain this exact implementation as the § 4.7 fallback if SPIM proceeds. Record its measured
   rate in the fallback warning/report and rollout note from § 4.6 item 2.

Do not report an estimated speedup as a measurement. If this step cannot be run on hardware,
record it as an open prerequisite for that board rather than inventing a measurement. One board's
result does not qualify another SoC.

### Step 1 — Prove the board configuration

1. Record the donor mapping in the implementation checkpoint: `SPI_32MHZ_INTERFACE=1` selects
   SPIM2, `8000000` selects 8 MHz, transfers are blocking, SS is disconnected, and SCK/MOSI use
   high drive. Mark it explicitly as nRF52840 evidence, not an nRF54 donor.
2. Record the checked-in board mapping: nRF52840 SPIM2/P1.13/P1.15, nRF54L15
   SPIM00/P2.01/P2.02 and nRF54LM20A SPIM23/P1.04/P1.06.
3. Enable generic `CONFIG_NRFX_SPIM=y` once in common `prj.conf` while leaving each applicable
   devicetree node disabled.
4. Build all three from clean NCS 3.3.1 trees. Confirm each map contains `nrfx_spim.c`, exactly the
   selected register is referenced by the application backend, and no Zephyr SPI device owns the
   disabled node.
5. Pin the bounded-completion path on all SoCs: connect the selected disabled node's IRQ exactly
   once, initialize nrfx with the non-null event handler, and prove the ISR dispatches to
   `nrfx_spim_irq_handler(&s_spim)`. Confirm a missing END event reaches the 21 ms maximum timeout,
   abort and deinit path instead of the null-handler `EVENTS_END` poll. Re-check the handler/event
   contract on an NCS upgrade.
6. Confirm nRF52840 clock policy from `HFCLKSTAT`. For SPIM00 and SPIM23, record the base
   frequency, domain state and `CLOCKPIN` configuration as **evidence**; § 4.1 already establishes
   that neither needs an explicit clock request, so this row confirms rather than decides.
7. Confirm no SPIM node on any of the three boards carries a `memory-regions` property, closing the
   DMM/RAM-domain question for these parts (§ 4.3).
8. Record baseline and nrfx-enabled flash/RAM figures for all three before adding the bounce
   buffer.

If the Kconfig symbol is unavailable or the instance is reserved by another enabled subsystem,
stop and reconcile ownership explicitly. Do not switch to raw registers merely to bypass a build
conflict.

### Step 2 — Introduce the common backend seam

1. Add the target-private seam and make `od_bbep_zephyr_io.inl` independent of the implementation.
2. Compile one nrfx backend on all three boards. Restrict SoC differences to one compile-time
   instance-selection block and narrowly named pin/clock policy helpers.
3. Move the image-data bit loop out of `od_bbep_zephyr_io.inl` into `od_epd_spi_bitbang.c` behind
   the seam and use the Step 0 production direct-GPIO implementation as the § 4.7 fallback. Give it
   no second entry point and let no caller reach it except through `od_epd_spi_write()`.
4. Compile the production source three ways against a test-local fake nrfx implementation so each
   selection proves the expected register, pins and clock policy.
5. Cover command/data CS/DC calls with a narrow recording fake at the seam boundary; do not
   host-compile the panel algorithms or the whole `bb_epaper` core.

At this checkpoint all Nordic boards must build and contain only the appropriate instance path.

### Step 3 — Implement SPIM2, SPIM00 and SPIM23

1. Decode each platform's runtime pin encoding, convert it with `NRF_GPIO_PIN_MAP`, and reject
   invalid, identical or target-reserved SCK/MOSI pins before initialization. An
   instance-inaccessible pair is **not** rejected here -- it selects the fallback in item 6.
2. Select SPIM2, SPIM00 or SPIM23 at compile time and configure it for 8 MHz, mode 0, MSB first,
   MISO unused and SS unused.
3. Start nRF52840 at nrfx's 8 MHz default, `NRF_GPIO_PIN_S0S1`. Qualify it as § 4.2 specifies; only
   evidence of inadequate margin or drive-attributable errors permits a board-conditional
   `zephyr_compile_definitions(NRF_SPIM_FORCE_H0H1)`. If selected, verify that definition reaches
   the separately compiled `nrfx_spim.c` and remains absent from nRF54. Do not rewrite `PIN_CNF`
   after init. On nRF54, retain nrfx's required `CLOCKPIN` setup and qualified drive policy.
4. Copy each offered span through the 256-byte DMA buffer and issue one synchronous,
   interrupt-completed TX-only transfer per chunk; do not reproduce the donor library's per-byte
   Arduino loop. Wait no longer than rounded-up wire time plus 20 ms.
5. Latch and expose init, transfer-start and completion-timeout failures. Keep the latch reachable
   **only** from an nrfx operation/completion failure, never from the § 4.7 reachability predicate.
6. Implement the acquire-time backend decision: evaluate `od_board_spim_pin_ok()` on **every**
   acquire and select bit-bang when it refuses. Emit `od_log_warn()` on that path -- carrying the
   decoded pins, the compiled instance and the fallback rate -- latched on the `(sck, mosi)` pair so
   a repeat acquire on the same pins is silent and a pin change warns again (§ 4.7 rule 3). Emit
   nothing extra on the SPIM path. Do not route the warning through the `od_log_debug()` pin dump,
   which production builds compile out. Under `OD_EPD_SPI_REQUIRE_SPIM`, return failure there
   instead and compile the bit-bang arm out.
7. Report `8000000` from the backend on the SPIM path and the bit-bang path's own measured-order
   rate on the fallback path; include the value **and the selected backend** in the existing
   panel-pin debug dump, in addition to -- not instead of -- the WARNING in item 6.
8. Implement deinit as the § 4.5 switch on `s_backend`: NONE returns immediately; SPIM disables the
   selected IRQ, aborts/uninitializes, clears `END` and pending IRQ state, disconnects PSEL and
   explicitly clears nRF54 `CLOCKPIN`; BITBANG drives its saved SCK/MOSI port/mask pairs low and
   restores retention without calling any nrfx, IRQ or SPIM operation. Only after either acquired
   arm completes does it set the backend to NONE and invalidate saved pins. Ordinary deinit
   preserves the fault latch; only the named reset API at the display owner's recovery boundary
   clears it while deinitialized.
9. Compare nRF52840 command/data capture with the donor's blocking, manual-CS/DC ordering. Compare
   all three against the current target's single/dual-CS behavior; donor parity alone does not
   qualify nRF54. Capture the fallback path too: its SCK/MOSI implementation is new, but its wire
   bytes, mode-0 edges and CS/DC ordering must be indistinguishable from today's path.

### Step 4 — Integrate display lifecycle and verdicts

1. Acquire the selected SPIM only after the panel rail and signal pins are ready and before
   `bbepWakeUp()`.
2. Replace the four literal `bbepInitIO(..., 0)` arguments with `od_epd_spi_hz()` so every Nordic
   board explicitly requests/logs 8 MHz.
3. Make `od_xfer_app_write()` return the offered length only after complete success and `0` after
   a latched fault, make `od_xfer_app_refresh()` refuse that fault, and make the retained PIPE sink
   return non-zero.
4. Ensure boot-screen rendering checks the latch and follows its existing failed render path.
5. Release SPIM after the panel sleep command at the **top of `display_power_set(false)`, ahead of
   the `has_pwr_pin` branch**, so every power-down, abort, disconnect and watchdog recovery path
   reaches it -- including the permanently powered panel, which never calls
   `opendisplay_display_park_pins()` at all. Follow the § 4.5 uninit/disable/clear-PSEL ordering.
6. Preserve the fault latch across ordinary release/re-acquire cycles. At the display owner's
   deliberate abort/session-reset recovery boundary, deinitialize first and then call
   `od_epd_spi_fault_reset()` before a fresh acquisition. No normal panel power cycle or
   `od_epd_spi_init()` call clears a fault implicitly.

Do not alter reply encryption or construct new wire replies in the panel backend. The existing
transfer owner remains responsible for the appropriate NACK or silent transport outcome.

### Step 5 — Ratchet and qualify

1. Add a gate proving nRF52840 selects SPIM2, nRF54L15 selects SPIM00 and nRF54LM20A selects
   SPIM23, and that an unknown Nordic platform is a compile failure.
2. Add a gate proving the bit loop exists in exactly one place -- `od_epd_spi_bitbang.c` -- and is
   reachable only through `od_epd_spi_write()`. `od_bbep_zephyr_io.inl` must contain no image-data
   `od_gpio_write()` loop, the direct loop must contain no pin decode/device lookup/Zephyr GPIO
   call, and no backend-selection call site may test an nrfx return value.
3. Add a gate proving `OD_EPD_SPI_REQUIRE_SPIM` builds link no bit-bang symbol on any board, and
   that the fallback-selection branch reports through `od_log_warn` -- not `od_log_debug`, which
   production builds compile out (§ 4.7 rule 3).
4. Pin each overlay's single-owner rule without depending on whitespace formatting.
5. Run `./tools/check.sh --targets` and require no failures or skips.
6. Record flash/RAM deltas for all three images, confirm exactly one 256-byte bounce buffer in
   each, and confirm no image references another board's SPIM register.
7. Record the retained fallback's flash cost per board: the default image against the same board
   built with `OD_EPD_SPI_REQUIRE_SPIM`. § 4.7 asks for a number, not an assumption.

## 7. Required software tests

The fake nrfx/Zephyr/GPIO surface is larger than it looks; name it up front so it is not discovered
mid-implementation. It must supply `nrfx_spim_t`, `nrfx_spim_config_t`, `NRFX_SPIM_INSTANCE`, the
three `NRF_SPIM*` register symbols, `NRF_SPIM_PIN_NOT_CONNECTED`, `NRF_GPIO_PIN_MAP`, the nrfx event
handler/event types, `nrfx_spim_irq_handler()`, `nrfx_spim_abort()`, `NRFX_IRQ_DISABLE`,
`NRFX_IRQ_PENDING_CLEAR`, `nrf_spim_event_clear`, `nrf_spim_disable`, `nrf_spim_pins_set`,
`nrfy_gpio_pin_clock_set`, `NRF_GPIO_Type`, `nrf_gpio_pin_port_decode`,
`nrf_gpio_port_pin_output_set`, direct port set/clear and conditional retention operations, the one
IRQ connection and the semaphore/time surface, plus
**errno-style returns**. It must be able to complete immediately, complete later, never complete
and deliver a deliberately late callback. `tests/host/` already precedents a second shared-library
variant (`od_shared_silabs`), so three backend translation units is consistent with existing
practice.

The production SPIM implementation compiled in all three board configurations against fake nrfx
must cover:

- zero-length writes perform no transfer and succeed;
- arbitrary writes split at 256-byte boundaries without loss or duplication;
- exact common config: 8 MHz, mode 0, MSB first, no MISO/SS, DCX not connected when present, and
  the default IRQ/RX-delay/hardware-CS fields retained from `NRFX_SPIM_DEFAULT_CONFIG`;
- exact selection: SPIM2 for nRF52840, SPIM00 for nRF54L15 and SPIM23 for nRF54LM20A;
- runtime pin decoding reaches each board's qualified SCK/MOSI PSEL values and accepts the exact
  second pair from § 4.6 without recompilation: P0.29/P0.28 on nRF52840, P2.06/P2.08 on L15 and
  P1.03/P1.05 on LM20A;
- nRF52840 defaults to S0S1. If § 4.2's hardware comparison selects H0H1, build inspection proves
  `NRF_SPIM_FORCE_H0H1` reaches `nrfx_spim.c` only in that board image; source/build inspection
  pins that nRF54 uses nrfx's required `CLOCKPIN` configuration and selected clock-domain policy;
- init receives a non-null handler, the selected node's IRQ is connected exactly once, and the ISR
  dispatches to `nrfx_spim_irq_handler(&s_spim)`;
- a normal DONE event releases the synchronous writer and only then permits the next chunk to
  overwrite the bounce buffer;
- a missing DONE event reaches the computed timeout, disables the IRQ, calls abort, performs
  deinit, clears the peripheral `END` event and pending interrupt-controller state, leaves no next
  chunk started, forces the adapter cleanup arm to drive CS inactive and latches once;
- a callback delivered after timeout cannot satisfy a transfer after deliberate fault reset and
  re-acquire;
- initialization failure latches once and blocks writes;
- an immediate `nrfx_spim_xfer()` failure latches a fault and is not reported as consumed;
- a **flash-resident source** reaches the wire byte-for-byte, and the caller's buffer is
  unmodified after any write (the two properties § 4.3's bounce buffer exists to provide);
- deinit with backend NONE performs no GPIO, nrfx, IRQ or SPIM operation and repeated deinit remains
  a no-op;
- a failed acquisition leaves the backend NONE, invalidates any partially prepared saved-pin state
  and makes the caller's cleanup deinit a no-op;
- SPIM deinit follows the § 4.5 order: IRQ disable, abort, uninit, `END`/pending-IRQ clear,
  peripheral disable, PSEL cleared to not-connected, then saved nRF54 `CLOCKPIN` attributes
  cleared;
- BITBANG deinit brackets saved SCK/MOSI low writes with the required nRF54 retention operations,
  performs **zero** nrfx, IRQ, event, PSEL or CLOCKPIN operations, then sets the backend to NONE;
- `OD_EPD_SPI_REQUIRE_SPIM` compiles out the BITBANG deinit arm and every fallback helper symbol;
- both acquired deinit arms invalidate saved pins only after cleanup and preserve the fault latch;
- a repeat acquire on the same pins is a no-op returning success, and an `-EALREADY` from nrfx is
  recovered rather than latched (§ 4.4);
- init and ordinary deinit preserve a latched fault; `od_epd_spi_fault_reset()` refuses while
  acquired, succeeds while deinitialized, drains stale completion state, and only then permits a
  later init with new runtime pins on the same compiled board target;
- invalid pin encodings and SCK equal to MOSI are rejected on **both** backends -- these are not
  fallback cases;
- an instance-inaccessible pin pair selects the bit-bang backend without touching nrfx, reports
  `OD_EPD_SPI_BACKEND_BITBANG`, and does **not** set the fault latch (§ 4.7);
- that selection emits **exactly one record at `OD_LOG_WARN`**, and the recording log fake still
  observes it when the unit is compiled at the production `OD_LOG_LEVEL` (`OD_LOG_INFO`) -- the
  regression this guards is a fallback reported only through a `od_log_debug()` call that ships
  compiled out;
- a successful SPIM acquire emits no warning, so the fallback warning is not background noise;
- repeated acquire/release cycles on the same unreachable pins warn **once in total**, not once per
  cycle and not once per write -- the case that matters, since a tag cycles the bus several times
  per upload;
- changing to a different unreachable pair warns again, and changing back warns again: the latch is
  the last pair seen, not a one-shot "already warned" boolean;
- the predicate is still evaluated on every acquire even while the log is suppressed, so a config
  change that moves pins from reachable to unreachable selects bit-bang on the very next acquire;
- fallback acquisition decodes SCK/MOSI once each, resolves each saved port/mask once and performs
  no further decode, device lookup, `od_gpio_configure_output()`, `od_gpio_write()` or Zephyr GPIO
  call for zero, one or multiple write calls in that acquisition; output configuration uses the
  saved port and relative pin and starts both latches low;
- fallback byte output uses only the saved port/mask pairs and direct clear/set operations in
  mode-0 order, including pins on different ports;
- each nRF54 fallback write disables retention once per saved pin (or once for their combined mask
  on a shared port), performs no retention operation per edge, drives SCK low and restores
  retention before returning; deinit leaves both pins low with retention enabled, while nRF52840
  emits no retention operation;
- the same pair under `OD_EPD_SPI_REQUIRE_SPIM` fails init instead and links no bit-bang symbol;
- every reachable-pair fault -- init and mid-transfer -- still latches and never selects bit-bang,
  proven by asserting the reported backend is unchanged across the failure;
- backend selection is sticky: an nrfx transfer failure after a successful SPIM acquire leaves
  `od_epd_spi_backend()` reporting SPIM, and no chunk is retried on the other backend;
- `od_epd_spi_hz()` reports 8 MHz on the SPIM path and the fallback's own rate on the bit-bang
  path, so the two acquisitions are distinguishable by a caller;
- the bit-bang arm produces byte-for-byte identical wire output, mode-0 edges and CS/DC ordering to
  the current `bb_spi_bitbang()` loop, against the same recording fake, while its fake call counts
  prove the per-edge generic GPIO path is gone.

The narrow seam-boundary recording test must cover ordinary command, data and command-plus-data
CS/DC ordering, dual-CS selection, and CS-inactive cleanup after both an immediate transfer failure
and a completion timeout. Preserve `BBEP_CS_EVERY_BYTE` in production source, but do not build the
seam or throughput design around a flag no supported panel sets.

Target builds must cover all three Nordic boards. Host tests alone cannot prove DMA address/domain
validity, frequency, clock-domain behavior, pin routing or electrical transaction ordering.

## 8. Hardware gate — all three boards

Do not mark a board complete until it has its own recorded device evidence. Passing nRF52840 does
not qualify either nRF54 peripheral, and passing one nRF54 part does not qualify the other.

Run these rows separately on `xiao_nrf52840`, `xiao_nrf54l15` and `xiao_nrf54lm20a`:

- **Build every qualification image with `OD_EPD_SPI_REQUIRE_SPIM` defined (§ 4.7 rule 4).** A gate
  row passed by a board that silently fell back to bit-bang proves nothing about the peripheral it
  claims to qualify. This is not optional and it is not a debugging aid.
- Record the selected peripheral, the reported backend and the resolved runtime pins. They must be
  SPIM2/P1.13/P1.15, SPIM00/P2.01/P2.02 and SPIM23/P1.04/P1.06 respectively for the deployed
  configurations, and `od_epd_spi_backend()` must report SPIM on every row below.
- Logic analyzer: SCK is nominally 8.0 MHz within the selected SoC clock source's specified
  tolerance, idle low, rising-edge sample and MSB first; capture with BLE radio idle and active.
- On nRF52840, run the first electrical gate with S0S1 and record rise/fall time, overshoot/ringing
  and transfer correctness at the panel connector with the production cable. Test H0H1 only if
  that evidence shows inadequate margin or drive-attributable errors. If H0H1 is selected, retain
  both captures and prove its compile definition reached `nrfx_spim.c` only in the nRF52840 image;
  if both modes pass, S0S1 is the required result.
- Record the relevant clock status/domain state before, during and after the transfer. On
  nRF52840 this includes `HFCLKSTAT`; on nRF54 it includes the source/domain established in Step 1.
- Logic analyzer: command/data DC transitions and CS assertions match section 4.2, including one
  command-plus-data operation and any dual-controller mode supported by that board/panel.
- Boot screen renders after cold boot.
- An encrypted, compressed PIPE upload of the board's qualified image completes without a
  tail/PTO probe and displays correctly.
- A subsequent PIPE partial update completes and updates the intended region.
- Legacy full direct (`0x70/0x71/0x72`) and partial (`0x76`) transfers complete where the board's
  capability set exposes them; record capability-off results rather than marking them exercised.
- Abort or BLE disconnect mid-transfer releases the selected SPIM and permits the next upload.
- Power-off leaves SCK/MOSI parked, clears their nRF54 `CLOCKPIN` attributes, releases any explicit
  clock-domain request and shows no SPIM-driven edges while the panel is off.
- Repeated uploads produce no nrfx errors, watchdog reset or data corruption.
- Measure the panel-RAM write interval around the sink only and record bytes, planes and elapsed
  time. The gate is at most 150 ms per 48,000 wire bytes, scaled linearly for the actual wire-byte
  count; rail bring-up, decompression and physical refresh are reported separately. It does not
  apply to the unused `BBEP_CS_EVERY_BYTE` compatibility arm.
  **The interval must exclude command and init sequences.** `bbepWriteCmd()` and
  `bbepWriteCmdData()` each call `delay(1)` -> `od_msleep(1)` around their DC transitions; that is
  unchanged by this plan and out of scope, but a measurement spanning an init sequence reports
  sleeps rather than bus throughput.
  **Where 150 ms comes from:** it is roughly 3x the 48 ms wire bound, chosen to leave headroom for
  per-chunk overhead. It is **not** derived from the donor, which is slower than this (§ 2.1). If a
  donor frame time is ever measured, record it beside this row as context rather than replacing the
  gate with it.

On `xiao_nrf52840`, retain the deployed EP426/800x480 fact: color scheme `MONO`, layout
`PACKED_ROWS`, and 48,000 bytes is one plane rather than half of a two-plane transfer. Record the
corresponding panel, scheme, layout and plane count for both nRF54 boards instead of assuming the
same geometry.

If a supported board/panel cannot operate reliably at 8 MHz with a clean trace, stop and make
panel-specific maximum-clock policy a separate design decision; do not silently lower this plan's
fixed rate, and do not reach for the § 4.7 fallback to make the row pass. That fallback answers
unreachable pins, not a marginal trace on reachable ones.

**One additional row, run once per board in a default (non-`REQUIRE_SPIM`) build at the production
log level:** configure a panel pin pair the compiled instance cannot reach, then confirm the board
emits the § 4.7 WARNING naming the pins, instance and fallback rate; renders the boot screen;
completes an upload at the Step 0 fallback rate; beats that board's unmodified sink measurement;
stays at or below 8 MHz with correct mode-0 edges under BLE idle and active; and latches no fault.
Capture the log from a production-level build, not a `PROFILE=debug` one -- a warning only visible
under debug logging fails this row, because the fleet does not run debug builds. This is the only
row that exercises § 4.7's selection on hardware; without it the fallback is a code path no device
has ever run.

While that fallback acquisition is active, exercise ordinary power-off, abort and BLE disconnect.
Each must park SCK/MOSI low, leave nRF54 retention enabled, emit no nrfx assertion/error and permit
a fresh fallback upload. This is the hardware proof that § 4.5's backend switch never enters its
SPIM arm for a bit-bang acquisition.

**Then upload a second time without rebooting and confirm the warning does NOT repeat.** The
suppression latch is keyed on the pin pair (§ 4.7 rule 3), so silence on the second upload is the
pass condition, not a defect -- record it that way so a later reader does not "fix" it. The panel
bus is acquired and released several times per upload, so this is also the only hardware evidence
that the latch actually holds across acquire/release cycles rather than merely across writes.
A reboot must warn once again.

## 9. Stop conditions

Stop implementation and revise the plan if:

- SPIM2, SPIM00 or SPIM23 is already owned by an enabled Zephyr device or another subsystem on
  its selected board;
- the measured direct-GPIO backend meets the performance and correctness gate, until the user
  decides whether DMA/CPU-offload still justifies SPIM;
- runtime pin selection cannot be represented safely through nrfx PSEL. The § 4.6 survey finding
  an unreachable deployed configuration is **no longer a stop condition** -- § 4.7 degrades it to
  the board's measured direct-GPIO rate. It becomes a rollout note, and a stop condition only if
  the survey shows fallback would be the common case rather than the exception, which would mean
  most devices use the measured direct-GPIO rate rather than the intended SPIM path;
- backend selection turns out to be reachable from an nrfx return value, or a fault is observed
  selecting bit-bang rather than latching (§ 4.7 rules 1 and 2);
- the non-null-handler path cannot connect the selected IRQ or cannot provide a bounded,
  unambiguous completion/timeout result;
- EasyDMA requires more than the single bounce buffer;
- a SPIM node on any of the three boards turns out to carry `memory-regions`, or either nRF54 part
  otherwise requires a DMA-visible memory domain or cache policy that the bounded backend cannot
  express safely (Step 1.7 is expected to close this, not open it);
- any image references another board's selected peripheral or links more than one bounce buffer;
- the required SPIM IRQ is already owned by another enabled peripheral/driver, cannot be connected
  exactly once, or a clock owner conflicts with Zephyr;
- CS/DC behavior changes to accommodate the driver;
- 8 MHz is unstable on any deployed panel with a clean logic-analyzer trace;
- completing the change requires a host protocol or timeout change.

## 10. Exit state

At exit, every Nordic board has one and only one owner of its selected panel SPIM, clocks panel
bytes at a measured 8 MHz on its qualified pins, uses one bounded EasyDMA-safe bounce buffer,
reports bus failure rather than silently completing, times out a missing completion without
wedging the display workqueue, and releases the peripheral, `CLOCKPIN` attributes and any
clock-domain ownership before GPIO parking.

Every Nordic board also carries the fast direct-GPIO bit-bang backend behind the same seam,
reachable only through the § 4.7 reachability predicate, reported through
`od_epd_spi_backend()`, and compiled out of `OD_EPD_SPI_REQUIRE_SPIM` builds. Its pins are resolved
once per acquisition and its byte loop performs no decode, device lookup or Zephyr GPIO call. No
fault-triggered fallback exists on any board, the image-data bit loop lives in exactly one file,
and every § 8 hardware row was recorded on a build where the fallback could not have been taken.

The full target gate and every board's explicit hardware rows pass, and the host tail-timeout
concern remains recorded as a separate external follow-up rather than hidden inside the firmware
change.

## 11. Source audit, 2026-08-20

Read against NCS 3.3.1 (`~/ncs/v3.3.1`), the Adafruit nRF52 Arduino core in
`~/.platformio/packages/framework-arduinoadafruitnrf52-seeed`, and the vendored
`third_party/bb_epaper`; exact XIAO package identification was cross-checked against Seeed's
official L15 and LM20A schematics. Confirmations and corrections are folded into the sections
above; this section records the two findings that needed room of their own.

### 11.1 What the audit closed

- `NRFX_SPIM_INSTANCE(reg)` takes the register pointer; the plan's form was already right.
- nrfx 4 has no per-instance enable macro, and `CONFIG_NRFX_SPIM` depends on `dt_has_compat`,
  which ignores node `status`. The disabled-overlay plan works.
- A null handler needs no `IRQ_CONNECT`, but it busy-polls `EVENTS_END` without a timeout; the plan
  therefore uses a non-null handler, an explicit IRQ connection and a bounded semaphore wait.
  Anomalies 195/198 remain gated on `NRF_SPIM3` in nrfx, so SPIM2 avoids both as the donor's
  `SPI_32MHZ_INTERFACE=1` intends.
- `nrfx_spim_uninit()` disables the selected IRQ but does not clear its interrupt-controller
  pending state. Timeout teardown therefore disables the IRQ before abort, then clears both the
  peripheral `END` event and pending IRQ after uninit, before any fault reset or reacquisition.
- `NRFX_SPIM_DEFAULT_CONFIG` initializes SoC-conditional DCX, RX-delay and hardware-CS members;
  direct partial initialization would incorrectly make an omitted DCX select P0.00.
- `nrfy_gpio_cfg_default()` does not clear the nRF54 `CLOCKPIN` bit that nrfx enables, so release
  explicitly clears it on the saved SCK/MOSI pins.
- `hfpll` is a `fixed-clock` with no `clock_control` consumer: no explicit clock request anywhere.
- No Nordic devicetree carries `memory-regions` on a SPIM node: no DMM placement needed.
- No `bb_epaper` core code reads `iSpeed`, so changing the four `bbepInitIO()` literals is inert.
- `bb_epaper`'s core never drives `iMOSIPin` or `iCLKPin`; only the IO backend does.
- The `Firmware_NRF54` donor's panel backend bit-bangs through `nrf54_gpio_write()`, which decodes
  the pin and calls Zephyr `gpio_pin_set()` on every edge. It establishes wire ordering, not the
  fallback implementation: NCS's `nrf_gpio_pin_port_decode()` and direct port set/clear HAL calls
  allow the unified backend to resolve once without open-coding SoC register addresses.

### 11.2 Why `xiao_nrf54lm20a` tops out at 8 MHz -- and why that is not about DMA

The devicetree gives `spi00` a 32 MHz `max-frequency` and `spi23` only 8 MHz, on **both** nRF54L
parts. It is worth being precise about the cause, because "the LM20A is the slow board" is the
wrong conclusion and would lead someone to look for a fix that does not exist.

`nrf54lm20a_application_peripherals.h` states it directly:

| | `SPIM00` | `SPIM23` |
|---|---|---|
| `CORE_FREQUENCY` | 128 MHz | 16 MHz |
| `PRESCALER_DIVISOR_RANGE_MIN` | 4 | 2 |
| implied maximum | 128 / 4 = 32 MHz | 16 / 2 = **8 MHz** |
| `MAX_DATARATE` | 32 | 8 |
| `EASYDMA_MAXCNT_MAX` | 15 (16-bit) | 15 (16-bit) |

**EasyDMA is identical on the two instances and is not the limiter.** The cap is the peripheral's
core clock and its prescaler floor. `SPIM00` is the one instance in the high-speed domain, fed by
the 128 MHz HFPLL -- which is exactly why its node carries `clocks = <&hfpll>` while `spi23` carries
no `clocks` property at all.

The same split exists on `xiao_nrf54l15`: `spi00` at 32 MHz, `spi20`/`spi21`/`spi22`/`spi30` at 8.
LM20A additionally has `spi00` at 32 MHz. So **the ceiling is a board-routing consequence, not a
silicon one**: `seeed_xiao_connector.dtsi` maps `xiao_spi: &spi23`, putting the XIAO SPI pads --
and therefore the panel's CLK and DATA -- on a low-power-domain instance. The 32 MHz-capable
`spi00` exists on this part but is routed to P2.01/P2.02, which is not where the panel is wired.

Three consequences for this plan:

1. **8 MHz is `SPIM23`'s ceiling, at prescaler 2 -- the minimum divisor.** The design runs that
   instance flat out, with no headroom to trade for signal integrity if a trace is marginal. The
   only step down is 4 MHz (prescaler 4), which halves throughput and, per § 4.1, newly arms the
   errata 8/212 workaround on that board.
2. The same is true of `xiao_nrf52840`: `spi2`'s DT `max-frequency` is 8 MHz. Only `spi00` on the
   nRF54L boards has room above the plan's fixed rate.
3. Reaching 32 MHz on LM20A is not a firmware change. It would require routing the panel to
   `spi00`'s pads -- a hardware revision -- and would then collide with § 4.6's pin-domain
   constraint from the other direction. Out of scope; recorded so the option is not re-litigated as
   a software one.
