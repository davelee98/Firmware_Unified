# Follow-ups

Defects and open items found while building the scaffold that are **not fixed by the commit
that found them** — because they live in another repo, need a decision, or belong to a later
migration step. Opened 2026-07-25.

This file exists so these do not decay into folklore. It is not a duplicate of the open
decisions in [DESIGN_REVIEW_2026-07-25.md](DESIGN_REVIEW_2026-07-25.md) § "Open decisions", nor
of the risks in [MIGRATION.md](MIGRATION.md) § "Risks to watch" — where an item is tracked
there, this file points at it rather than restating it.

**Evidence discipline.** Each item says how it was established. `verified` means it was
reproduced directly against the source or by running it, with the command or file:line given.
`reported` means it came from a survey and has not been independently reproduced. Do not
promote a `reported` item to a fix without checking it first.

---

## 1. `py-opendisplay` — host-side defects found by the wire corpus

Found on day one of the corpus existing, which is the argument for the corpus. All are in a
repo this project does not own (`CODEOWNERS: * @g4bri3lDev`), so the route is an upstream issue
with a named vector, a byte string, and two decodes attached — as
[TEST_OWNERSHIP.md](TEST_OWNERSHIP.md) § Ownership anticipates. **None has been filed yet.**

### 1.1 `0x0052` means POWER_OFF, and the host calls it DEEP_SLEEP — *high severity*

**verified** — `py-opendisplay/src/opendisplay/protocol/commands.py:40`
(`DEEP_SLEEP = 0x0052`) and `:173` (`build_deep_sleep_command()` emits `0x0052`), against
`shared/protocol/opendisplay_protocol.h:410` (`0x0052 CMD_POWER_OFF`) and `:444`
(`0x0053 CMD_DEEP_SLEEP`).

Protocol **2.1 split these opcodes**. The library never adopted the split and has no
`POWER_OFF` command at all. A host asking a device to sleep with a timed wake therefore sends
power-off.

Why this is the most serious item here: on hardware with the D-FF power latch, `0x0052` is an
absolute rail cut. The canonical header states the device then wakes **only on a physical
button press** — "No timer, no wake interval — power-off is absolute." So the failure mode is a
deployed display that goes dark and needs someone to walk to it. The affected version,
`py-opendisplay==7.14.0`, is exactly what `Home_Assistant_Integration`'s `manifest.json` pins.

The fix upstream is small — add `POWER_OFF = 0x0052`, move `DEEP_SLEEP` to `0x0053`, keep the
old builder emitting `0x0052` under its correct name. The cost that cannot be fixed is any
device already put to "sleep" this way.

Note the library's own docstring at `commands.py:164` describes the latch behaviour
("replies 0x0052, then powers off after ~100 ms") — so the behaviour was observed and
documented, and only the *name* is wrong. That is why it survived review.

### 1.2 `validate_ack_response` cannot distinguish a NACK from an ACK

**verified** — reproduced by execution against PyPI `py-opendisplay==7.14.0`:

```
ACK                  0070     -> ACCEPTED as ACK
auth-required 1.1    0070fe   -> ACCEPTED as ACK
decrypt-fail 1.2     0070ff   -> ACCEPTED as ACK
```

`responses.py:111-133` validates the 2-byte echo and ignores the trailing status byte.
DIVERGENCE §1.1 auth-required `[00][echo][0xFE]` and §1.2 decrypt-failure `[00][echo][0xFF]`
are both read as success. A host using this validator treats "you are not authenticated" and
"decryption failed" as an acknowledgement.

This is also the first concrete case of the corpus doing what it exists for: firmware is
unanimous and correct here, and the host disagrees.

### 1.3 `parse_tlv_config` logs "skipping" and then stops

**verified** — `config_parser.py:129-131`:

```python
if packet_size is None:
    _LOGGER.warning("Unknown packet type 0x%02x at offset %d, skipping", packet_type, offset - 2)
    break
```

The message says *skipping*; the code abandons the walk and silently discards every later
packet. This is the DIVERGENCE §2.2 shape — the one that loses the `0x27` security packet —
**on the host side**, and it matters more than it first looks: §2.2 was framed as a firmware
divergence where NRF54 is right and Firmware/Silabs are wrong. The host has the same bug. So
"add a new config packet type" is unsafe on *both* sides of the wire, not just the firmware
side, and the NRF54 size-table parser only fixes one of them.

In fairness to the implementation: TLV entries here are fixed-size-by-type with no length
field, so a parser that meets an unknown type genuinely cannot know how far to advance. `break`
may be the only correct action available — but then the log message asserts the opposite of the
behaviour, and the real fix is a length field, which is a frozen-header change.

### 1.4 Serializer and parser disagree on which packets are required

**verified** — `config_parser.py:208-219` requires system, manufacturer, power **and at least
one display packet**; `config_serializer.py` requires only the first three. The canonical header
marks `0x20 display` `@repeatable max=4`, not `@required`.

So a config `py-opendisplay` serializes can fail to parse back in `py-opendisplay`. The
corpus's `minimal-required-singletons` vector (the three required singletons, per the header)
is rejected by the host parser.

### 1.5 `parse_firmware_version` never reads the patch byte

**verified** — corpus run output: the `firmware-version-response-with-patch` vector decodes to
`1.5 1a2b3c4d`, with no patch component.

This bears on a decision already recorded: [README.md](../README.md) § "Versioning and
releases" adopts semver partly *because* the `0x43` response carries a trailing patch byte, so
semver maps onto the frozen wire with no protocol change. That remains true of the wire, but
the shipped host discards the third component. Until this is fixed, a host can only distinguish
firmware builds by major.minor plus the commit SHA.

---

## 2. `opendisplay-protocol` — canonical header defects

The headers are **frozen** (see the project memory and CLAUDE.md), so nothing here is actioned
yet. DIVERGENCE_MATRIX.md § 8 already carries the accumulated spec-correction list and the
sync-tool copy-map gap; these two are additions to it.

### 2.1 `0x52` NACK width — the header contradicts itself

**verified** — `opendisplay_protocol.h`, same opcode block:

| Line | Says |
|---|---|
| `:420` (`@response`) | `[0xFF][0x52][0x00][0x00]` — four bytes |
| `:432` (`@targets`) | `NACK [0xFF][0x52][0x00]` — three bytes |

The only shipped implementation (Silabs) sends **four**. The corpus follows the shipped shape,
on the §1.1 precedent that a spec disagreeing with unanimous shipped behaviour is the thing
that gets corrected. Fix the `@targets` line when the freeze lifts.

**Still open after C11 (2026-08-16), and note what did NOT change.** Nordic's `0x52` now answers
`{FF,52,OD_ERR_POWER_OFF_UNSUPPORTED,00}` where it used to fall silent — a second implementation,
also four bytes, chosen to match Silabs rather than to take a position on the contradiction. The
header still disagrees with itself and the vendored copies here are frozen; this is a protocol
defect and firmware agreeing with firmware does not close it.

### 2.2 `0x0040` never says what an UNPROVISIONED device answers

**verified 2026-08-16** — `opendisplay_protocol.h:293-301` describes two cases and the field has a
third:

| The header says | Covers |
|---|---|
| `@response` — "Empty config yields a single `[0x00][0x40][0x00 0x00][0x00 0x00]`" | a config that IS stored, of length zero |
| `@errors` — `[0xFF][0x40][0x00][0x00]` "on storage-init failure" | storage that will not initialise |

Neither covers **nothing stored** — an unprovisioned device, or one whose config failed
validation — which is the case that actually occurs. `Firmware` answers the 4-byte error frame for
it, and `py-opendisplay` tests for exactly that (`device.py:1111`), raising "Device has no stored
configuration". Its comment names the failure the test prevents: without it the `{00,00}` of an
error frame reads as a zero-length config *instead of* "no config".

So the shipped answer is unambiguous even though the prose is not, and the §1.1 precedent applies —
a spec that does not describe unanimous shipped behaviour is the thing that gets corrected. Add an
`@response` clause for "no stored configuration" when the freeze lifts. Until then the header must
not be read as authorising the zero-length ACK for an absent config: `nordic-zephyr` did read it
that way, and an unprovisioned board reported itself as provisioned-with-nothing.

### 2.3 The auth-required shape is documented backwards

**verified** — `opendisplay_protocol.h:190` and `:222` both state `[0xFE][cmd_echo]`. All three
implementations ship `[0x00][echo][0xFE]`, with `0xFE` as *data* in a 3-byte frame. Already
recorded as DIVERGENCE §1.1's resolution ("correct the spec, do not change firmware"); repeated
here only because §2.1 above is the same class of defect and the two should be fixed in one
pass.

---

## 3. This repo

### 3.1 `MAX_CONFIG_SIZE` 4096 is the absolute ceiling — `MAX_CONFIG_CHUNKS` must become 21

**DECIDED 2026-07-25: 4096 is the absolute config ceiling — storable *and* transferable.**
Option 2 of the three previously listed here. The two numbers must agree, so the chunk bound
moves to meet the size bound rather than the size bound being quietly reinterpreted as storage-
only.

> **DO NOT LAND THIS WITHOUT § 3.8's BOUND.** `od_config_asm_start()` bounds a declared total
> against `MAX_CONFIG_CHUNKS * CONFIG_CHUNK_SIZE` and never against the buffer. At 20 chunks that
> ceiling is 4000, below the 4096 buffer, so it is the tighter bound and the module is safe. Raising
> the count to 21 makes it **4200 — larger than the buffer** — so a declared total of 4097..4200
> would be accepted and overflow, at the DEFAULT cap, on every target. The fix below and the clause
> in § 3.8 must land together, in that order or the same commit.

**The required change:** `MAX_CONFIG_CHUNKS` `20` → **`21`**. That is exact, not padded —
the header specifies `expectedChunks = ceil(total / 200)` (`opendisplay_protocol.h:320`), and
`ceil(4096 / 200) = 21`. The count includes the START frame's 200-byte payload, which is why 20
yields 4000 and not 4200.

This is a **canonical-header change and the headers are frozen**, so it is queued, not done.
It is a *relaxation* (a device accepts one more chunk), so under the header's own policy it is
a MINOR bump: old hosts sending ≤ 20 chunks are unaffected.

**verified — the bound is enforced on every target**, so this is a real barrier and not a
documentation artifact:

| Target | Enforcement |
|---|---|
| `Firmware` | `communication.cpp:557` — `receivedChunks >= MAX_CONFIG_CHUNKS` → NACK |
| `Firmware_NRF54` | `opendisplay_pipe.c:1018`; and `opendisplay_config_storage.h:13` documents the cap in prose as `MAX_CONFIG_CHUNKS(20)*CONFIG_CHUNK_SIZE(200)` |
| `Firmware_Silabs` | `opendisplay_pipe.c:889` |

**The consequence that does not go away with the header fix.** Deployed devices enforce 20 and
will keep doing so until they are reflashed — which for ESP32-C6 and nRF52840 means a bench
visit, and for ESP32-S3 means an OTA path that is not built. So the 4001–4096 band is
undeliverable to the existing fleet regardless of what the canonical header says, and stays that
way for as long as those units are in service.

That reintroduces, in a narrow band, exactly the discoverability problem the fleet-wide 4096
decision was taken to remove. It is much smaller than the original 2048-vs-4096 split, but it is
the same shape, and it should be handled the way ARCHITECTURE § "Capabilities are discovered by
interrogation" already prescribes: attempt-and-degrade on the NACK, never a timeout.

**Interim rule until the header change lands:** a host must not build a config above **4000**
bytes. Failing at chunk 21 wastes a full transfer and leaves the operator with a mid-stream
NACK rather than a refusal up front. This is cheap to honour — real configs are far below the
ceiling, and the whole 4096 figure is headroom rather than a live constraint.

**Still to do:** add the `MAX_CONFIG_CHUNKS = 21` change to the queue of canonical-header edits
that land when the freeze lifts (DIVERGENCE_MATRIX §8), sequenced with the other pending ones.

### 3.8 `od_config_asm` never bounds the declared total against its own buffer — *latent; becomes high severity the moment any target lowers the cap*

**Not exploitable today, and safe only by coincidence.** `od_config_asm_start()` rejects a declared
total above `OD_CONFIG_ASM_MAX_TRANSFERABLE` — `MAX_CONFIG_CHUNKS * CONFIG_CHUNK_SIZE` = 20 x 200 =
**4000** — and against nothing else (`shared/core/od_config_asm.c:63`). The buffer it fills is
`uint8_t buffer[OD_CONFIG_MAX_SIZE]`, currently **4096** (`od_config_asm.h:67,85`). The two numbers
are never compared. Because 4096 > 4000, the transferable ceiling happens to be the tighter bound
and the chunk copy at `:110` cannot overrun.

**The module therefore depends on an invariant nothing states or asserts:**

```
OD_CONFIG_MAX_SIZE >= OD_CONFIG_ASM_MAX_TRANSFERABLE
```

Lower `OD_CONFIG_MAX_SIZE` below 4000 and a declared total between the new cap and 4000 is accepted
at the start frame, then written past the end of the buffer at `:110` — driven by a two-byte
little-endian length field in the first `CONFIG_WRITE` frame, i.e. remote and pre-authentication on
any target whose gate admits `0x0040`.

**This is not hypothetical.** `plans/PLAN_SILABS_C13_2026-08-16.md` § 2.5 proposes exactly that for
the EFR32BG22 (cap 2048, 32 KB of RAM), and justified it by stating that `od_config_asm` already
refuses oversize declarations — the plan has been corrected, and C13.0a now lands the bound before
the cap moves. Filed here as well because the hole is in **shared** code and outlives that plan: the
next person to touch `OD_CONFIG_MAX_SIZE` for any reason meets it.

**The fix, and it is cheap:**

```c
/* od_config_asm.c -- the buffer bound the transferable ceiling only implied. */
if (total <= CONFIG_CHUNK_SIZE || total > OD_CONFIG_ASM_MAX_TRANSFERABLE ||
    total > OD_CONFIG_MAX_SIZE) {
    return OD_CONFIG_ASM_REJECTED;
}
```

At 4096 the added clause is unreachable, so ESP32 and Nordic images should come out
**byte-identical** — which is how the change is shown to be inert until a cap actually moves. A host
test at cap+1 must be shown failing on the pre-fix tree; note § 3.1 above separately raises
`MAX_CONFIG_CHUNKS` to 21, which lifts the transferable ceiling to 4200 and **inverts the
relationship**, making the missing bound live at the default 4096 cap. Whichever lands first, the
other must not be merged without this clause.

Found 2026-08-17 reviewing the C13 plan.

### 3.2 Corpus schema gaps — **ALL DECIDED, C12.0, 2026-08-16**

Found by authoring the first 23 vectors, and taken before the corpus grew: the decisions are in
`plans/PLAN_OD_DISPATCH_C12_2026-08-16.md` § 2.3 and implemented by `tests/host/vectors_tool.py`.
`forbids` is adopted; `expect.parsed` is blessed and validated but its EXECUTION is deferred, with
the reason recorded, because `od_dispatch_frame()` exposes replies rather than a parsed
`struct od_config`; ordered `expect.replies` are adopted with `expect.reply` kept as the
zero-or-one shorthand. A fourth gap surfaced during the work and is decided with them: `origin` was
implicit even though shared dispatch applies a 244-byte ceiling to BLE, 4094 to LAN, and exempts
LAN-TLS from the session gate.

The original text is kept below because each gap explains a field that now exists.

- **`requires` is positive-only.** The "a disabled subsystem still answers" vectors need the
  opposite — *this flag must be OFF*. Worked around with `state.cap_*` booleans. A `forbids`
  key (or a negation syntax) is the honest fix.
- **No post-parse assertion.** §2.2's whole contract is "the `0x27` still loads", which is
  invisible in a reply frame. An optional `expect.parsed` object (dotted path → value) was
  added as an extension. **Bless it or reject it** — without it the corpus's most valuable
  vector asserts nothing.
- **One frame per vector.** Cannot express ack-then-notification (`0x72` → `0x73`/`0x74`, where
  §3.3 says Silabs diverges), multi-frame sequences, or the chunk-ceiling case in 3.1 above
  (which needs 21 frames).

### 3.3 The corpus is authored, not captured — **THE CLAIM WAS FALSE; CORRECTED C12.0**

**This section was wrong, and the correction matters more than the original point.** Five vectors
do not read their bytes off source at all -- they copy real captures from
`py-opendisplay/tests/fixtures/real_protocol_data/`, and say so in their own notes
(`02_read_firmware_response.bin`, `03_upload_start_uncompressed_command.bin`,
`04_data_chunk_command.bin`, `05_upload_end_command.bin`). The files exist. What does not exist,
in either repository, is the target, firmware SHA, panel, host version or date behind them.

So the corpus holds three kinds of evidence, not one, and C12.0 makes the distinction
machine-readable rather than leaving it in prose: `authored`, `captured` (provenance-complete per
`TEST_OWNERSHIP.md` § "Capture plan"), and `captured-unattributed` for exactly those legacy
fixtures -- which names the source file, carries a limitation string, may not be used by a new
vector, and never counts as a provenance-complete regression baseline.

The original concern still stands for everything else, and its deadline has now passed for two
targets. The text follows.

Every OTHER `expect.reply` in `tests/vectors/` is read off source and specification. **Nothing has
been observed on hardware.** [TEST_OWNERSHIP.md](TEST_OWNERSHIP.md) § "Capture is
time-sensitive" is the item with a real deadline: once `shared/core` starts replacing a
target's logic, there is no longer an untouched reference to capture from, and the corpus
becomes a description of what the unified firmware does rather than a regression baseline for
what the fleet did. Bench time is the scarce input, not engineering effort.

### 3.5 FastEPD writes no pixels to an IT8951 panel — *high severity, S3-only*

All three `it8951WriteFramebuffer{1,2,4}Bit` functions in `third_party/FastEPD/src/FastEPD.inl`
have `#ifdef ARDUINO` guards that fall through to **nothing** under ESP-IDF — 9 sites: `:2199`,
`:2225`, `:2235`, `:2263`, `:2286`, `:2296`, `:2323`, `:2348`, `:2359`. The data preamble is
never sent and the row loop builds each line into `d[]` and discards it. Commands,
`LD_IMG_END` and `it8951DisplayArea*()` all still run, so the panel refreshes whatever was
already in its controller RAM and every call reports success.

`third_party/NOTICE.md` § "FastEPD has no ESP-IDF IO backend" claims **"Every affected guard
becomes `#if defined(ARDUINO) || defined(OD_FASTEPD_IDF_SPI)`"** — six were, these nine were
not, and that claim is what let them persist. The census was taken against what
`bbepInitIT8951`'s probe exercises; the framebuffer path needs a real panel attached.

Fix is the same one-line substitution already applied six times. Correct the NOTICE.md claim in
the same change. Lower-severity siblings, also unpatched and also silent: `bbepInitLights:1906`
(front-light never initialised), `Inkplate10IOInit:1581` (`Wire.setClock(400000)` skipped),
`bbepConvertPrevBuffer:3260` (unread).

### 3.6 `bbepWaitBusy` blocks the loop task for the length of a refresh

`bb_ep.inl:3995` polls BUSY with a bare `delay(20)`, so `serviceBleTx()` cannot run while a
refresh is in flight and queued BLE responses sit in the TX ring. This was the mechanism behind
acks arriving 16 s late while the panel was faulty (device log 2026-08-03: `ETX 0x0080` queued
in 2 ms, notified 17 s later) and it **survives the panel fix** — a legitimate multi-second
refresh has the same effect.

It is the shape `shared/` is designed to forbid: SHARED_API_DESIGN § `od_hal_time` says
`od_hal_delay_ms` "must never be used to wait out a panel refresh — that is the pump's job".
`bbepLightSleep()` is the injection point. Note it is wrong on every target for a different
reason — on the Silabs superloop there is no scheduler at all, so a blocking wait stops
everything.

### 3.9 FastEPD boot rendering and refresh can fail silently

**Status:** open; explicitly deferred by owner decision, 2026-08-18. Do not add this to the
boot-screen consolidation. **Evidence:** `verified` against the implemented FastEPD boot path.

The shared boot renderer now has a fallible frame/plane/row contract, but the FastEPD adapter
cannot currently honor it:

- `targets/esp32-idf/src/boot_screen.cpp` calls `fastepd_boot_write_row()` and always returns
  success from `od_boot_app_write_row()`;
- `targets/esp32-idf/src/display_fastepd.cpp:fastepd_boot_write_row()` returns `void` and silently
  drops a row when the framebuffer or source is null or the supplied pitch is short;
- `fastepd_full_refresh_impl()` discards the `int` returned by `FASTEPD::fullUpdate()`;
- `fastepd_wait_refresh()` returns only `!s_init_failed`, which reports the earlier initialization
  result rather than the refresh that just ran; and
- FastEPD's IT8951 ready/LUT wait helpers time out by breaking their loops and return `void`, so a
  controller timeout is not surfaced even through `fullUpdate()`.

The boot caller compounds the first two: `display_service.cpp` ignores the `bool` returned by
`writeBootScreenWithQr()` on the FastEPD branch, then unconditionally runs `fastepd_full_update()`.
A structurally incomplete framebuffer can therefore be refreshed and reported as successful.

**Recommended later boundary:** make framebuffer construction verifiable without trying to
detect whether `memcpy()` itself failed. Frame begin validates that the framebuffer exists and
the configured/native dimensions agree. Each plane tracks the exact expected pitch and strictly
sequential `y`; plane/frame end reject short, excess, duplicated or faulted output.
`fastepd_boot_write_row()` returns `bool`, and the boot seam propagates it. Separately,
`fastepd_full_update()` returns the vendor `fullUpdate()` status instead of discarding it. If real
IT8951 timeout detection is required, the ready/LUT waits must also return status or an equivalent
target-owned timed readiness check must be added.

On any render or refresh failure, skip the physical refresh where it has not started, call
`epdSessionForceOff()` (not only `pwrmgm(false)`), restore the watchdog phase to idle, and continue
running the device. Do not retry or transmit a white fallback in this path: renderer contract
failures are deterministic, and FastEPD's RAM framebuffer means a pre-refresh failure can leave
the existing image on glass unchanged.

### 3.7 ~~`esp_generic.inl` is dead code carrying four patches~~ — resolved 2026-08-03

Reverted to pristine upstream: byte-identical to `~/bb_epaper/esp_idf/esp_generic.inl`, 295
lines, zero `OD-PATCH` sites. Still present in the tree and still not compiled — reverted rather
than deleted, because a re-vendor would restore it and deleting makes the vendored copy diverge
from upstream *by omission*, which a `diff` cannot confirm the way "pristine and unused" can.

`third_party/NOTICE.md` now says so, and records what the four patches were, since that history
is the argument for owning the backend. bb_epaper is down to 3 `OD-PATCH` sites, both remaining
files genuinely compiled: `bb_ep.inl` (the `epd42yr2_init_full` duplicate-symbol fix, and the
BUSY-wait timeout warning from 3.6) and `bb_epaper.h` (`delay(int)` → `delay(long)`).

### 3.10 ~~Compressed legacy direct write and PIPE disagree on the streaming-decompression capability~~ — resolved 2026-08-20

The capability is advisory. Shared PIPE admits compression without consulting the target config,
matching shared legacy direct write. The host may use the bit to choose compression, but firmware
does not treat its absence as a refusal condition. Transfer Phase 3 pins that decision in the
shared production-machine tests; the Nordic hardware row remains open until compressed PIPE is run
on a config without the bit.

### 3.11 Nordic POWER_OFF comment disagrees with the handler-NACK activity policy

**Status:** open. **Evidence:** `verified` by reading the command and frame-policy paths,
2026-08-20; no hardware run needed to establish the mismatch.

`targets/nordic-zephyr/src/od_cmd_device.c` says the recognized-but-unsupported `0x0052`
POWER_OFF NACK must not stamp activity and that `od_frame_policy()` gives
`OD_FRAME_HANDLER_NACK` no stamp. The implementation says the opposite:
`od_cmd_app_power_off()` returns `OD_CMD_NACK`, dispatch maps that result to
`OD_FRAME_HANDLER_NACK`, and the policy stamps activity and resets the authentication-abuse run.

The runtime path is internally consistent; the comment and its stated intent are not. Decide
whether recognized unsupported commands should retain the current handler-NACK policy. If so,
correct the comment. If not, make the broader frame-policy change with tests for every handler
NACK consumer. Transfer Phase 3 preserves the current policy and does not resolve this item.

### 3.4 Already tracked elsewhere — pointers only

- **ESP32 does not parse `0x2A`**, and skip-to-CRC then discards the rest of the blob —
  DIVERGENCE §2.1 and ARCHITECTURE § "The 'old parsers skip unknown packets' escape hatch".
  Item 1.3 above is the host-side twin of this.
- **Security-hotfix deferral revisit trigger is met** — BG22 is shipped *and* field-updatable,
  so two of the three deferred defects sit on reachable hardware. MIGRATION § "Risks to watch".
- **The three-toolchain CI build matrix has no design** — DESIGN_REVIEW F8. Now the largest
  gap in CI, since the host compile has landed. Note that all three toolchains being installed
  locally (TOOLCHAINS § "All three toolchains are installed on this dev box") makes this less
  urgent, not less necessary: one machine with one set of versions is not a matrix.
- **`\bString\b` grep scoping** — done. **Host compile** — done, this phase.

---

## 4. `Firmware_Unified` / ESP32 — an authentication refusal with no stated reason

**Status:** open. **Evidence:** `verified` — reproduced from a device capture
(`s3-n16r8-extuart-debug`, reTerminal E1001, firmware `0.1.0-45-gc5119eb-dirty`, 2026-08-05)
and traced to source. Found while investigating an unrelated BLE connect defect.

### Symptom

Roughly twenty consecutive BLE sessions over ~60 s, each lasting about 3 seconds:

```
=== BLE CLIENT CONNECTED (ESP32) h=1 e=36 [owner] ===
BLE notify subscription h=1: enabled
[BLE][Q:0] URX 0x0050 (3 B): 00 50 00        <- AUTHENTICATE, step 1
[BLE][Q:0] UTX 0x0050 (3 B): 00 50 03        <- AUTH_STATUS_NOT_CONFIG
BLE notify subscription h=1: disabled
=== BLE CLIENT DISCONNECTED (ESP32) h=1 reason=0x213 ===
```

repeating through epochs `e36`–`e54`. Every gated command was refused in the same window —
`0x0040` READ CONFIG answered `00 40 FE` (`RESP_AUTH_REQUIRED`), `0x000F` REBOOT answered
`00 0F FE`. The device was, from the client's point of view, permanently unusable.

It stopped the instant an **unauthenticated** `0x0041`/`0x0042` WRITE CONFIG landed, and has
worked since, including across a subsequent power-on reset.

### What is actually happening

**The firmware is behaving correctly.** `AUTH_STATUS_NOT_CONFIG` (`0x03`) is returned by
`handleAuthenticate()` (`targets/esp32-idf/src/encryption.cpp:604`) when `isEncryptionEnabled()`
is false, and that predicate (`encryption.cpp:231`) is:

```c
return (securityConfig.encryption_enabled == 1) &&
       (encryption_key is not all-zero);
```

`config_parser.cpp:684` independently forces `encryption_enabled = 0` when the stored key is all
zeros. So the device had **no usable encryption key**, said so, and refused to authenticate. That
is the specified behaviour, not a defect.

**The defect is that it never says why.** Compare the two refusals in the same capture:

| Path | Log output |
|---|---|
| gated command without a session | `ERROR: [BLE] Command requires authentication (encryption enabled)` |
| `0x0050` with no key configured | *(nothing)* |

`handleAuthenticate()` sends the three-byte refusal and returns, with no `od_log_*` call on that
branch. The only clue is a **debug-level** line emitted once at boot
(`Security config: Encryption disabled (key is all zeros)`), which is absent from any capture
that does not include the boot, and absent entirely on a non-debug build. A field engineer sees a
client reconnecting every three seconds against a device that logs a healthy connect, a healthy
subscribe, and nothing else.

### Contributing factors, each separable

1. **Silent refusal** — `encryption.cpp:604`. The one-line fix, and the only part that is
   unambiguously this repo's to make.
2. **The condition is permanent but the client retries forever** — ~20 attempts in 60 s with no
   backoff. `AUTH_STATUS_NOT_CONFIG` cannot become true without a config write, so a client that
   understands the code should stop and surface it. This belongs to the client, not here.
3. **The state is reachable at all.** A device with `encryption_enabled` set and no key is
   inert to every authenticated command. How this unit got there is not established — the
   capture begins mid-session and does not include the boot that parsed that config.
4. **Recovery depends on an unauthenticated write.** `0x0041`/`0x0042` is accepted with no
   session, which is what rescued this device — and is simultaneously the asymmetry already
   noted in `targets/esp32-idf/README.md`: config can be *written* by anyone in radio range but
   not *read*. Whether that is deliberate provisioning policy or an oversight is an open
   question for the wire contract, and it should be answered before either half is changed:
   closing the write without providing another recovery path would make this state
   unrecoverable over BLE.

### Recommendation

Fix (1) here — log the refusal with its reason, at a level that survives a non-debug build.
Raise (2) with the client. Answer (4) in `DIVERGENCE_MATRIX.md` / the protocol before touching
it. (3) needs a reproduction before it is chaseable.

**Not fixed by the commit that found it**, because the useful part is the wire-contract question
in (4), and because the symptom was encountered while chasing an unrelated BLE connect defect.


---

## 5. `opendisplay-protocol` — one CCM nonce is used in BOTH directions under one key

**Status:** open, **needs a protocol revision — no firmware change can fix it.**
**Evidence:** `verified` — read from all four firmware repos and from the promoted
`shared/core/od_session.c`, which reproduces it deliberately. **Severity:** high, and unusually
easy to overlook because every implementation agrees, so nothing looks wrong anywhere.

Filed 2026-08-15 with the `od_session` promotion (plan C7). It is recorded here rather than left
in an API comment because a note beside the code that causes it reads as an explanation; the next
reader has to see it as a defect with an owner.

### The defect

A session has ONE `session_id` and TWO counters — `tx_counter` for device→host and `rx_last` for
host→device — and **both start at 0**. The CCM nonce is `session_id ‖ BE64(counter)`, taken from
the same 16-byte envelope nonce in both directions. So the device's first sealed response and the
host's first sealed command are encrypted under the **same key with the same nonce**.

That is the one thing CCM must never do. Nonce reuse in a counter-mode AEAD leaks the XOR of the
two plaintexts directly, and — worse for an authenticated mode — it exposes the authentication
subkey structure, which can permit forgery rather than merely disclosure. This is not a
theoretical sharp edge; it is the classic catastrophic failure of CTR-based AEAD.

### Why it has not bitten yet

The two directions are decrypted by different code holding different expectations, and
`py-opendisplay` never attempts to decrypt a device→host frame with the host→device counter
space. The exposure is real but currently unexercised: it needs an attacker who captures both
directions of one session, which BLE sniffing makes entirely practical.

### Why `od_session` did not fix it

Fixing it changes the wire. Any of the three fixes below makes new firmware and old hosts
mutually undecipherable, so it belongs to a protocol revision with a version negotiation, not to
a refactor whose whole contract was "byte-identical on the wire". `shared/core/od_session.h`
states the flaw at `od_session_seal()` and is explicit that TX-side non-reuse is all it
guarantees.

### Options, cheapest first

1. **A nonce-domain bit.** Reserve one bit of the counter (or one byte of the 16-byte envelope
   nonce) as a direction flag: 0 for host→device, 1 for device→host. One-line change on both
   sides, costs one bit of counter space, and no extra key material or handshake round.
2. **Directional key separation.** Derive `k_h2d` and `k_d2h` from the session key with two
   distinct CMAC labels. Cleanest cryptographically, and it also stops a captured frame being
   replayed back at the sender. Costs one extra HAL key slot per session — the slot API added in
   C1 already supports this, `OD_HAL_CRYPTO_KEY_SLOTS` just needs to be 2.
3. **Split the counter space** — device seals from `1 << 63`, host from 0. Zero protocol
   ceremony, but it halves the usable counter space and is the easiest of the three to get
   silently wrong in a third implementation.

**Recommendation: (2)**, with (1) as the fallback if a key slot is unaffordable on EFR32BG22.
Whichever is chosen, it must be settled in `opendisplay-protocol` first — four firmware repos and
`py-opendisplay` all encode this nonce, and a unilateral change in any one of them is a silent
interop break rather than a fixable bug.

### Carry-forward for `efr32bg22-slc`

The Silabs target still open-codes the whole session and is unaffected by the `od_session`
promotion, so when it swaps it inherits every row of `DIVERGENCE_MATRIX` § 6.5–6.9 at once. Two
of those are behaviour changes on that target specifically, not merely code motion:

- **§ 6.2, timeout basis.** Silabs expires on *idle*, so a continuously active session there
  never expires at all. Adopting `od_session`'s absolute basis will start expiring sessions that
  currently live forever — visible to any host that holds a long-running connection.
- **§ 6.3, hand-rolled CCM.** It still carries the RFC 3610 implementation over PSA ECB, with a
  `psa_import_key`/`psa_destroy_key` **per 16-byte block**. Its PSA has CCM too, so the same
  `CONFIG_PSA_WANT_ALG_CCM`-equivalent fix applies — but measure the flash delta against a
  32 KB part before assuming it lands.

Also still live on Silabs and closed everywhere else: the constant-time auth-proof compare
downgraded to `memcmp` (`targets/efr32bg22-slc/opendisplay_pipe.c:646`), first recorded in
`AUDIT_NORDIC_ZEPHYR_2026-08-14.md:259` as an NRF54 defect and fixed there, never fixed here.

---

## 6. `py-opendisplay` — `deep_sleep()` sends the POWER_OFF opcode

**Status:** open, **host-side, pre-dates the Firmware_Unified work.** **Evidence:** `verified` —
read from both the canonical header and the host enum. **Severity:** high on latch hardware, where
the observable result is a device that does not come back.

Found 2026-08-15 by an independent review of an unrelated firmware refactor.

### The defect

`py-opendisplay/src/opendisplay/protocol/commands.py:41` declares:

```python
DEEP_SLEEP = 0x0052  # Enter deep sleep now (ESP32 timer-wake / Silabs EM4; nRF unsupported)
```

The canonical header disagrees, and the firmware implements the header:

| opcode | canonical meaning | firmware handler |
|---|---|---|
| `0x0052` | `CMD_POWER_OFF` | `handlePowerOffCommand()` — releases the D-FF latch, cutting the rail |
| `0x0053` | `CMD_DEEP_SLEEP` | `handleDeepSleepCommand()` — timer-wake sleep |

So a host calling `deep_sleep()` sends **POWER_OFF**. `0x0053` appears nowhere in the host, which
means the real deep-sleep command is currently **unreachable from py-opendisplay at all**.

### Consequences, by hardware

- **D-FF latch boards** (`DEVICE_FLAG_PWR_LATCH_DFF`): the rail is cut. A caller that asked to
  sleep for ten minutes and wake gets a device that stays off until someone presses a button. This
  is the bad one — it looks like a crash or a dead battery.
- **Non-latch boards**: the device answers `OD_ERR_POWER_OFF_UNSUPPORTED` and stays awake, so
  `deep_sleep()` silently does nothing — on a device that supports `0x0053` perfectly well.

### Fix

Host-side, and it is two changes rather than one, because the name is also wrong:

1. `DEEP_SLEEP = 0x0053`, matching the header.
2. Add `POWER_OFF = 0x0052` and a corresponding call, since the capability exists and currently
   has no host API.

Both are host-only; no firmware or protocol change is required, and the firmware is already
correct against the canonical header. Worth checking `Home_Assistant_Integration` for a caller of
`deep_sleep()` before releasing, since the failure mode on latch hardware is a device that appears
to have died.

---

## 6. `Firmware_NRF54` — every PIPE-partial START is refused by the device's own flag check

**Status:** open upstream; **fixed in this repo** 2026-08-19. **Evidence:** `verified` by reading
both trees, and by a host test that fails without the fix
(`tests/host/pipe_write_test.c`, "a partial START reaches the partial machine with the transport
selector stripped").

`opendisplay_pipe_write.cpp` passes the PIPE START **flags word** to
`opendisplay_display_pipe_partial_arm()`, which validates it against the **0x76 partial** flag set
— `PARTIAL_ALLOWED_FLAGS`, bit0 (compression) only. A partial START necessarily carries
`PIPE_FLAG_PARTIAL` (bit1), so the check always trips and the device answers
`[FF][80][OD_ERR_PIPE_START_UNKNOWN_FLAG][00]`. PIPE-partial has therefore never worked on either
Nordic tree; it is not a migration regression.

- upstream: `Firmware_NRF54/src/opendisplay_display.cpp:1077` against
  `src/opendisplay_pipe_write.cpp:260`
- here (fixed): `targets/nordic-zephyr/src/opendisplay_pipe_write.cpp` now passes
  `flags & ~PIPE_FLAG_PARTIAL`

**The error code makes the failure worse than a refusal.** `OD_ERR_PIPE_START_UNKNOWN_FLAG` is
`0x02`, which `py-opendisplay` names `PIPE_START_NACK_COMPRESSION` — "compression unsupported,
retry uncompressed" (`protocol/responses.py:366`). So the host answers a partial-flag rejection by
re-sending the same partial transfer **uncompressed**, gets `0x02` again, caches
`_pipe_partial_supported = False` for the connection, and drops to the legacy `0x76` flow. One
wrong mask thus costs both the partial path and, for that retry, compression.

ESP32 validates the same fields inline in `display_service.cpp` and is unaffected; BG22 has no
PIPE.

---

## 7. `Firmware_Unified` / BG22 — uptime wraps in the hardware-tick domain

**Status:** open, pre-existing target defect; not caused by the shared logging/time-HAL plan.
**Evidence:** `verified` against all current BG22 call sites and Simplicity SDK 2025.12.2's
sleeptimer implementation. **Severity:** latent correctness failure on long-running devices.

### The defect

Ten shipped BG22 call sites compute uptime as:

```c
sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count())
```

`sl_sleeptimer_get_tick_count()` is a 32-bit hardware-tick count. Converting that value to
milliseconds makes the public time value wrap when the tick counter wraps, rather than modulo
`2^32` milliseconds. The exact interval depends on the configured timer frequency; at 32.768 kHz
it is about 36 hours. More generally, the defect exists whenever the configured tick rollover does
not map to an integral `2^32`-millisecond wrap; ordinary kHz-range low-frequency clocks above
1 kHz have that property.

Unsigned `now_ms - then_ms` timeout arithmetic is safe only when both values wrap in their declared
`uint32_t` millisecond domain. Across the earlier tick-domain wrap it produces one huge elapsed
interval, which can spuriously expire a session, trip a transfer deadline or age connection/NFC
state.

Affected consumers:

- `targets/efr32bg22-slc/od_session_app.c` — session liveness and activity time;
- `targets/efr32bg22-slc/opendisplay_pipe.c` — the two-second END barrier deadline;
- `targets/efr32bg22-slc/opendisplay_display.cpp` — transfer `started_ms`;
- `targets/efr32bg22-slc/opendisplay_ble.c` — six connection-age, NFC and timeout uses; and
- `targets/efr32bg22-slc/panel/od_bbep_efr32_io.inl` — the `bb_epaper` `millis()` adapter.

### Fix

Create one target uptime function that reads `sl_sleeptimer_get_tick_count64()`, converts with
`sl_sleeptimer_tick64_to_ms()`, requires `SL_STATUS_OK`, then narrows the millisecond result to
`uint32_t`. Route all ten call sites through it. The narrowing must occur after conversion so the
observable clock wraps modulo `2^32` milliseconds.

Keep this as an independently reviewable defect fix. The shared logging plan creates the correct
`od_hal_uptime_ms()` implementation but deliberately does not sweep these existing target callers.
Required proof: a fake-sleeptimer adapter test across the underlying 32-bit tick rollover, a known-
interval hardware check on BG22, and the complete BG22 target gate.

## 8. `Firmware_Unified` / nordic-zephyr — board identity is derived from SoC

### The defect

There is no board axis. `zephyr/CMakeLists.txt` derives `OD_BOARD_NRF54L15` from
`CONFIG_SOC_NRF54L15` and `OD_BOARD_NRF54LM20A` from `CONFIG_SOC_NRF54LM20A`, and the platform
sources follow: `src/platform/nrf54/od_board_nrf54l15.c` guards itself with
`BUILD_ASSERT(IS_ENABLED(CONFIG_SOC_NRF54L15))`, returns the literal `"XIAO nRF54L15"` from
`od_board_name()`, and drives `P2.03` / `P2.05` / `P2.10` from `od_board_early_init()` — the XIAO's
antenna-switch power, antenna-switch select and boost-select pins.

Those are board facts wearing a SoC label. A second nRF54L15 board that is not the XIAO inherits
all three GPIO writes and the wrong board name, with nothing in the build to object.

### Why it is latent rather than theoretical

The comment above the CMake block records the same failure already happening once: an nRF52840
build compiled as L15 and drove P2.03/P2.05/P2.10, which do not exist on that part. It was silent
because `od_pin_decode()` rejects a port whose GPIO node is not `okay` and `od_gpio_write()` then
returns without faulting, so every write was a no-op.

On a second real nRF54L15 board those pins **do** exist. The same bug stops being a no-op and
starts driving three arbitrary pads during early init, before anything else is up.

### Fix

Introduce `CONFIG_OD_BOARD_*`, set by the board fragment rather than defaulted from `SOC_*`, and
gate the XIAO-specific `od_board_*.c` on the board rather than the SoC. `od_board_name()` becomes a
board property. The SoC-keyed `OD_PLATFORM_NRF52840` / `OD_PLATFORM_NRF54` split stays as it is —
it genuinely is a SoC contract (pin encoding, bootloader) — so this adds an axis rather than
replacing one.

### Blocks

The Nordic panel SPIM promotion (`plans/PLAN_NORDIC_SPIM_8MHZ_2026-08-19.md`) selects its SPIM
instance at compile time. With no board axis, that selection is keyed on SoC too, and two nRF54L15
boards whose panels sit on different ports cannot both be right. The plan's § 4.6 records the
runtime pin-reachability half; this is the compile-time half, and it is the one that must land
first. `docs/PRESET_PIN_MATRIX.md` § "Adding a Nordic board" has the per-SoC instance rules a second
board would need.

Required proof: two boards on the same SoC building and selecting different panel instances, with
an unstated board failing the build rather than inheriting a default.
