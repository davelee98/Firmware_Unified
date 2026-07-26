# Architecture

## The one rule

`shared/` must compile for **every** target. It may use the C/C++ standard library and
`shared/hal` interfaces, and nothing else.

Forbidden in `shared/`: `esp_*.h`, `driver/*.h`, `soc/*.h`, `hal/*.h`, `freertos/*.h`,
`nrf_*.h`, `sl_*.h`, `zephyr/*.h`, `Arduino.h`, `bluefruit.h`, `NimBLEDevice.h`,
`bb_epaper.h`, `TFT_eSPI.h` — any vendor or framework header. If a file needs one, it belongs
in `targets/`.

`shared/` must also be **plain C**, not C++. Two targets are C-only, and the Zephyr sources
that are the best donor for `shared/core` are already C. In particular, Arduino's `String`
must not appear anywhere in `shared/` — see docs/TOOLCHAINS.md, where removing its 575 call
sites is the largest single line item in the ESP32 port.

Enforce it mechanically (CI grep is enough) rather than by review habit. The moment one
vendor include slips into `shared/`, that file stops being shareable and the repo quietly
reverts to four codebases in one directory.

## Layers

```
        ┌───────────────────────────────────────────┐
        │  targets/<t>/    chip drivers, build sys, │
        │                  HAL implementation       │
        └───────────────┬───────────────────────────┘
                        │ implements
        ┌───────────────▼───────────────────────────┐
        │  shared/hal/     abstract interfaces      │
        └───────────────▲───────────────────────────┘
                        │ calls
        ┌───────────────┴───────────────────────────┐
        │  shared/core/    dispatch, config, xfers  │
        │  shared/compress/ inflate engines         │
        │  shared/protocol/ wire contract (#defines)│
        └───────────────────────────────────────────┘
```

Dependencies point **inward only**. `shared/core` never calls into a target; a target calls
`shared/core` and supplies HAL implementations.

## The host is the other half — `py-opendisplay` is the primary consumer

The diagram above stops at the antenna, and that is misleading. `shared/protocol` is not a
firmware-internal header; it is **one side of a two-party contract**, and the counterparty is
real, versioned software with its own release cycle:

```
  ┌────────────────────────────────────────────────────────┐
  │  Home_Assistant_Integration   PRIMARY CONSUMER         │
  │  branch: feat/clean-port (upstream/OpenDisplay)        │
  │  drawcustom service, BLE discovery, entities, OTA      │
  └─────────┬──────────────────────────────────────────────┘
            │  calls
  odl-renderer ──► epaper-dithering  ODL -> image -> device palette
            │  hands bytes to
  ┌─────────▼──────────────────────────────────────────────┐
  │  py-opendisplay   PRIMARY HOST / CONSUMER              │
  │  BLE + LAN transport, framing, chunking, session auth, │
  │  compression encoding, config TLV build/parse          │
  └─────────┬──────────────────────────────────────────────┘
            │  wire: shared/protocol  ◄── the contract
  ┌─────────▼──────────────────────────────────────────────┐
  │  shared/core on a target                               │
  └────────────────────────────────────────────────────────┘
```

**`Home_Assistant_Integration` is the primary consumer of these services, and it reaches
devices exclusively through `py-opendisplay`.** Track the **`feat/clean-port`** branch on the
`OpenDisplay` upstream remote — that is the line this migration must stay compatible with, not
whatever a local fork happens to be checked out at. It consumes firmware behaviour in three
distinct ways, and all three are affected by decisions in this repo:

- **Content delivery** — `drawcustom` renders ODL, dithers to the device palette, and pushes via
  `py-opendisplay`'s transfer paths. Transfer-mode capability (PIPE vs direct write, compressed
  vs not) is chosen here.
- **Device configuration** — the config-flow round trip, which is also the *primary capability
  discovery mechanism* (below). Its read-modify-write cycle is why capability fields must be
  clamped on write rather than read.
- **Firmware update — Silabs only, today.** The manifest pulls `py-opendisplay[silabs-ota]`, so
  the integration is the field-update path for EFR32BG22 (`.gbl` via the Silabs AppLoader) and
  for **nothing else**. `py-opendisplay/src/opendisplay/ota.py` also implements
  `perform_nrf_dfu` (legacy Nordic DFU, `.zip`, via the `nrf-ota` extra), but HA does not pin
  that extra, so it is library capability rather than shipped product behaviour. ESP32 has no
  OTA path at all — `ENTER_DFU` merely reboots.

  **Open item: can OTA be extended to more targets?** This is an investigation, not a decision,
  and it is load-bearing for the migration. If a target has no field-update path, every
  flash-layout or bootloader change it undergoes is a bench-reflash for every deployed unit —
  which sets a hard limit on what the ESP32 framework change may alter for units already in the
  field. Answer it before Phase B, not after. See MIGRATION.md § "Risks to watch".

**`py-opendisplay` is to the host side what `Firmware`/ESP32 is to the firmware side: the
reference implementation.** It is the only complete client of the wire protocol — everything
above it (the HA integration, the CLI, `odl-renderer` output) reaches devices *through* it. A
protocol behaviour that `py-opendisplay` cannot produce or parse does not exist in practice,
however many firmware targets implement it.

Three consequences for work in this repo:

1. **A `shared/core` change that alters observable wire behaviour is not done when the
   firmware builds.** It is done when the host counterpart is decided — even if the host change
   itself lands in another repo on another schedule. Encoder-side coupling is already load
   bearing: the zlib window (host must cap `windowBits` at the smallest target it addresses),
   `MAX_CONFIG_SIZE`, chunk sizing, and every NACK code a client must distinguish from a
   timeout.
2. **Capability divergence must be *expressible to the host*, not merely tolerated by it.**
   Feature parity across targets is explicitly not a goal (SHARED_API_DESIGN.md), which is only
   safe if a host can discover what a given device supports. Where it cannot — the 2048-vs-4096
   `MAX_CONFIG_SIZE` split is the live case — the divergence is a defect on the host side even
   though both firmwares are individually correct.
3. **Version coupling is exact and already enforced.** `Home_Assistant_Integration`'s
   `manifest.json` pins exact versions — as of 2026-07-25,
   `py-opendisplay[silabs-ota]==7.14.0` and `odl-renderer==0.5.12` — and `py-opendisplay` pins
   an exact `epaper-dithering==`. Firmware is the one link in that chain with no pin: deployed
   devices are whatever they were flashed with. So the host must remain backward-compatible
   with firmware it cannot upgrade, which is the real constraint behind the conservative-opcode
   rule. Note the `[silabs-ota]` extra — the integration ships the Silabs OTA path, so a
   firmware change that alters the update mechanism lands in HA's dependency set.

Treat `py-opendisplay` as a stakeholder in every protocol-affecting decision recorded here, and
name the host-side consequence explicitly rather than leaving it implied.

## `opendisplay-protocol` is the canonical home for macros

Decided 2026-07-25, resolving DESIGN_REVIEW_2026-07-25.md's "canonical macro registry" item.
**Wire-contract macros live in the canonical headers and nowhere else** — no firmware repo, and
not `shared/core`, defines its own copy of a value the wire depends on.

This is largely already true and worth recognising rather than re-inventing: the canonical
header carries nine sections of them — opcodes (§1), response/status bytes (§2), auth status
(§3), opcode-scoped NACK namespaces (§4), NFC sub-protocol (§5), PIPE wire constants (§6),
chunk/size budgets (§7: `OD_BLE_MAX_FRAME`, `CONFIG_CHUNK_SIZE`, `MAX_RESPONSE_DATA_SIZE`, …),
encryption envelope sizes (§8), and LAN transport (§9). New wire constants go there.

### The line: contract versus build selection

Not every `OD_*` macro is contract, and putting build configuration in the canonical header
would force one value on targets that legitimately differ.

| Kind | Owner | Examples |
|---|---|---|
| Wire constants | canonical header | opcodes, error codes, frame/chunk sizes, envelope sizes, ports |
| **Fleet floors and limits** | canonical header | the minimum zlib `windowBits` every device must accept; the smallest `MAX_CONFIG_SIZE` a host may assume |
| Per-target selection | `targets/<t>/` build files | `OPENDISPLAY_ZLIB_WINDOW_BITS` for *this* build, this target's `MAX_CONFIG_SIZE`, buffer sizes |
| Capability gating | `targets/<t>/` build files | `OD_PIPE_ENABLE`, `OD_PARTIAL_ENABLE`, `OD_NFC_ENABLE`, … |

The distinction that matters: **a floor is contract, a selection is not.** "Every device accepts
a 512-byte window" is a promise a host relies on and belongs in the header. "This build uses
512 bytes" is a target's business. The same split applies to `MAX_CONFIG_SIZE` — the fleet
minimum a host must respect is contract; BG22's 2048 and ESP32's 4096 are build values, which
is exactly why capability discovery has to report the per-device number.

`OD_*_ENABLE` never goes canonical. A capability macro in a shared header would either impose a
feature on targets that cannot implement it or default to off and silently disable it
everywhere — both failure modes the capability-gating design exists to avoid.

### This raises the stakes on a mechanism that is currently broken

Canonical ownership only means something if the copies stay in sync, and they do not:
`sync_protocol_header.py --check` reports **1 in sync, 5 drifted, 2 missing** (2026-07-25), and
`opendisplay-protocol` has **no `.github/` and no pre-commit config** — nothing runs the check
in any repo, despite the workspace `CLAUDE.md` describing it as used in CI. Declaring the header
canonical for *more* macros increases dependence on a mechanism that is presently 1-for-8 and
unenforced. Fixing that — and settling submodule-versus-script-sync — is now a prerequisite,
not a background cleanup.

## The config binary is backward compatible, and changes to it are minimized

Two rules, the second following from the first.

**1. The config binary format is backward compatible. This is not negotiable.** A config blob
written by any firmware or host version must still parse correctly on any other. Deployed
devices hold stored config in NVS/NVM3/settings that survives a firmware update, factory
provisioning writes blobs that must remain readable, and `Home_Assistant_Integration` caches and
replays configs. Firmware is the one unpinned link in the dependency chain — so a device
flashed a year ago must still be configurable by today's host, and a config exported today must
still apply to that device.

Concretely, this forbids:

- changing the **size** of an existing packet, or the **offset** of any field within it,
- **repurposing** a field, or narrowing its accepted range,
- **renumbering** packet types, and
- making a previously optional packet required.

And it permits exactly two moves: **consume reserved bits or reserved bytes** (already declared
"must be 0", so old writers already emit a valid value), or **add a new packet type** that old
parsers skip.

**2. Minimize changes to config.** Even permitted changes have costs that land in five places at
once — every firmware parser, `py-opendisplay`'s build and parse paths, the HA config flow,
factory provisioning, and the size budget. That budget is real: BG22 caps a config at **2048
bytes** while every other target allows 4096, so bytes added to the schema are spent against the
smallest device in the fleet, not the largest.

So prefer, in order: **reserved bits** in an existing bitfield → **reserved bytes** in an
existing packet → **a new packet type** → a schema change of any other kind, which the backward
compatibility rule mostly forbids anyway.

### The "old parsers skip unknown packets" escape hatch is not currently safe

The second permitted move relies on unknown packet types being skipped cleanly. **Today that is
true on `Firmware_NRF54` only.** Its size-table parser steps over known-size packets and
falls back to skip-to-CRC only for genuinely unknown IDs; `Firmware` and `Firmware_Silabs`
force skip-to-CRC on any unrecognised type, which silently discards everything after it —
including the `0x27` security packet if the new type is ordered ahead of it
(DIVERGENCE_MATRIX §2.2).

The consequence is concrete: **adding a config packet type today would break security config on
two of three targets in the field**, and those units cannot be fixed retroactively. So until the
NRF54 size-table parser is the one in `shared/core`, treat "add a new packet type" as *not
actually available* for anything that must work on deployed hardware — which pushes the
capability-reporting values into `SystemConfig.reserved[15]`, as § "The gap, and a proposed fix"
does. Adopting NRF54's parser is what re-opens that option for future firmware.

## Secrets are never logged verbatim — presence and length only

**Rule.** No code in `shared/` logs credential or key material at any log level. Report that a
value is *set* and how long it is; never its content.

```c
od_log_debug("SSID: (set, %u chars)", ssid_len);      /* yes */
od_log_debug("Password: %s", pw_len ? "(set)" : "(empty)");
od_log_debug("SSID: %s", ssid);                        /* NO */
```

`Firmware` already does exactly this in its config parser (`#124`), with the comment *"Do NOT
log the SSID or password (credentials). Presence/length only."* This promotes that from one
repo's convention to a rule for all of them — the other targets are unverified and must be
checked during promotion.

**What counts as secret:** WiFi SSID and password (`WifiConfig`, packet `0x26`), everything in
`security_config` (`0x27`) including the shared secret, session keys and derived keys, CCM
nonces and MACs, and **raw config blobs or TLV payloads** — a parser that dumps its input has
logged a credential whether or not it meant to.

Three reasons this is a rule and not a style preference:

1. **Log level is not a control.** Debug logging gets enabled in the field precisely when
   something is wrong, and RTT/UART capture is routine during support. A secret behind
   `od_log_debug` is a secret that leaks on the day you most need logs.
2. **It escapes the device.** Logs are pasted into issues and shared with support. The wire
   captures planned for the test corpus have the same exposure, which is why TEST_OWNERSHIP.md
   already requires scrubbing them — same rule, different artifact.
3. **The failure is silent and permanent.** Nothing breaks when a credential is logged; it is
   only discovered by reading the log, usually much later.

**Enforce it mechanically where possible.** A CI grep over `shared/` for log calls taking
credential-named symbols (`ssid`, `password`, `psk`, `secret`, `key`, `nonce`, `mac`) catches
the obvious cases; the rest is a review obligation on `shared/core` log sites. As with the
vendor-include check, a rule that lives only in reviewers' heads decays.

## Capabilities are discovered by interrogation, not assumed

A host must be able to learn what a device supports by **asking it**, never by inferring from
firmware version, device name, or trial and error. This is what makes "feature parity is not a
goal" safe rather than reckless: divergence is fine, *undiscoverable* divergence is a defect.

### A device should determine its own capabilities, as far as it can

Before a host can interrogate a device, the device has to *know the answer*. It largely does,
and the design should exploit that rather than treat capability as something told to the device
from outside.

**The firmware half is self-knowable, and the firmware is authoritative over it.** Whether PIPE,
partial region, NFC, LAN, or a given inflate engine is present is fixed at build time by the
`OD_*_ENABLE` set. So is `MAX_CONFIG_SIZE`, the maximum accepted zlib `windowBits`, the maximum
frame size, and the protocol version. A device never has to be *told* any of this and should
never be trusted to have been told it correctly — a config blob asserting PIPE support on a
build compiled without PIPE is simply wrong, and the firmware is the party that knows.

**The peripheral half is not self-knowable, and the host is authoritative over it.** E-paper
panels generally cannot be interrogated: no reliable ID readback, no capability register. The
same applies to most attached hardware — sensors, PMIC, buttons, buzzer. The device cannot
discover that it is wired to a 1872×1404 4-gray ED103TC2 rather than a 2.9-inch B/W panel; it
knows only what `DisplayConfig` was written into it. That is why `panel_ic_type`, geometry,
pin assignments, and `partial_update_support` are *configuration*, and must stay so.

**Effective capability is the AND of the two**, and the distinction is not cosmetic:

```
  supports_partial_refresh  =  OD_PARTIAL_ENABLE          (firmware — device knows)
                            &&  panel does partial         (peripheral — device is told)
```

Report the conjunction where a host needs one answer to act on, but **keep the two
distinguishable**, because they fail differently and want different fixes. A firmware gap is
permanent for that build and the host must degrade around it. A peripheral gap may just be a
wrong `panel_ic_type` — an operator error that is *fixable by rewriting config*. Collapsing
both into a single "no" makes a misconfigured device look like an incapable one, and sends
whoever is debugging it to the wrong repo.

The practical consequence for the proposal below: **clamp only the bits the firmware is
authoritative over.** Never clamp panel-descriptive fields against what the firmware thinks the
panel can do — the firmware does not know, and a device that "corrects" its own panel
configuration will silently discard a valid setup.

Two mechanisms, in priority order.

**Primary — the config flow.** `CONFIG_READ` returns the device's TLV blob, and the schema
already carries capability fields:

| Field | Declares |
|---|---|
| `SystemConfig.communication_modes` | BLE / OEPL / WiFi (bits 3-7 reserved) |
| `SystemConfig.device_flags` | power-latch and init quirks (bits 5-7 reserved) |
| `DisplayConfig.transmission_modes` | streaming-inflate, ZIP, G5, DIRECT_WRITE, **PIPE_WRITE** (bits 5-6 reserved) |
| `DisplayConfig.partial_update_support` | panel partial-update capability |

This is the right mechanism and it is **under-used, not missing** — `transmission_modes`
already has a bit for the exact capability that diverges most (PIPE is ESP32-only), and there
are reserved bits in three of the four fields. Prefer extending these over anything new.

**Secondary — protocol responses.** An explicit error code meaning *unsupported*, distinct from
malformed-request codes. Today there is no such code and `OD_ERR_PIPE_START_BAD_HEADER` is
misused for it (DIVERGENCE_MATRIX.md §1). Adding one is a new error code in an existing
namespace — tier 3 of the conservative-opcode ladder, and cheap.

**The invariant that makes the secondary mechanism usable at all: silence must never mean
unsupported.** A compiled-out subsystem always NACKs; it never drops the frame. Silabs today
silently drops `0x45` and `0x80`-`0x82`, which is indistinguishable to a host from a lost
frame, a busy device, or a dead link — so the host must burn a timeout and still cannot tell
"never supported" from "temporarily unavailable". Every `OD_*_ENABLE=0` subsystem answers.

**Host fallback is expected and should be deliberate.** Read capabilities, choose the best path,
and degrade on `unsupported`: PIPE → direct write, partial → full refresh, compressed →
uncompressed. A *timeout* is a transport failure, not a capability signal, and must never be
treated as one.

### The gap, and a proposed fix

The config fields above describe **configuration the host wrote**, not firmware capability.
Writing `PIPE_WRITE` into a BG22's config does not make BG22 implement PIPE, so reading it back
tells the host what it wrote, not what the device can do. As it stands the primary mechanism is
a mirror, not an interrogation. Three changes fix that, in increasing cost:

1. **Device-clamped capability fields — firmware-authoritative bits only.** The firmware masks
   `transmission_modes` and `communication_modes` against a compile-time capability mask
   derived from its `OD_*_ENABLE` set, rejecting or clamping bits it does not implement and
   reporting what it kept. **Zero new wire surface** — no opcode, no field, no version bump.

   Two constraints on this, both from the self-knowledge split above. First, **do not clamp
   `partial_update_support`, `panel_ic_type`, geometry, or pins** — those describe the attached
   panel, which the firmware cannot interrogate and must not overrule. Second, clamp on
   **write**, not on read: `Home_Assistant_Integration` does read-modify-write on config, so
   clamping the read makes an exported config lossy — a BG22 export would silently strip PIPE,
   and applying that template to a capable device would then disable it. Clamp on write, report
   what was kept, and leave read byte-stable.
2. **Report capability *values* in reserved bytes of an existing packet — not a new one.**
   `SystemConfig` already carries `uint8_t reserved[15]` (and `DeviceConfig` another
   `reserved[6]`), at fixed offsets, declared "must be 0". Spending ~7 of those 15 bytes covers
   everything the values case needs:

   | Value | Bytes | Why it cannot be a reserved *bit* |
   |---|---|---|
   | `MAX_CONFIG_SIZE` | 2 (u16 LE) | a size, not a flag — the 2048-vs-4096 split |
   | max accepted zlib `windowBits` | 1 | range 9..15 |
   | max frame size | 2 | differs by transport |
   | protocol version major/minor | 2 | |

   **`0` already means "not reported", for free.** Existing firmware zeroes those bytes, so a
   host reading zero knows the device predates capability reporting and falls back to
   attempt-and-degrade. No new packet, no size change, no new packet-type registration, and no
   change to the TLV walk — only meaning assigned to bytes already allocated and already
   transmitted.

   Reach for a genuinely new read-only `DeviceCapabilities` packet only if the reserved space
   runs out. A new TLV packet type is tier 4 of the ladder — versioned and skippable by design,
   so firmware that does not emit it degrades
   gracefully and the host falls back to (3).
3. **A new `CMD_CAPABILITIES` opcode** — last resort, and probably unnecessary if 1 and 2 land.
   Record the argument before reaching for it.

Until 1 and 2 exist, a host meeting an unknown device must attempt-and-degrade, which is why
the *unsupported* error code and the no-silent-drop invariant are prerequisites rather than
nice-to-haves.

## Everything on the wire is testable without hardware

**Every wire protocol path and every function in `shared/` has host-runnable unit tests,
including fuzz and stress tests for error conditions.** Hardware verification stays as the
final gate (MIGRATION.md § "Verification bar per subsystem") — it does not move earlier, and it
does not substitute for this. A subsystem that can only be exercised by flashing a board is
tested serially, by one person, on one of four toolchains, and its error paths are never
exercised at all because they are hard to provoke on real hardware. That is how the four repos
accumulated divergent error handling in the first place.

**This costs nearly nothing, because `shared/` is already host-compilable by construction.**
Plain C, no vendor headers, no heap, no kernel — the boundary rule and host testability are the
same property seen twice. The HAL is link-time bound (SHARED_API_DESIGN.md), so a host test
binary links test doubles in place of the target implementations. Do not introduce a
function-pointer vtable "for testability"; link-time substitution already provides it, at zero
RAM cost on the BG22.

Put the harness in `tests/` at the repo root, **not** under `shared/` — the boundary check
scans `shared/` and would flag test-framework includes.

**Ownership is decided: the tests live in this repo** (2026-07-25). `Firmware_Unified` owns the
C unit tests, the fuzz harnesses, and the wire-vector corpus; the corpus has exactly one copy
and is never synced outward; `py-opendisplay` is consumed as a pinned dependency of a replay job
running in this repo's CI rather than being asked to host anything. Rationale, mechanism, and
the first step are in TEST_OWNERSHIP.md.

### What must be covered

| Area | Why it needs host tests |
|---|---|
| Config TLV parse / chunked assembly | Pre-auth attack surface; unknown-type skip logic already caused a real bug (DIVERGENCE_MATRIX §2.2) |
| Frame dispatch + encryption gate | Where the Silabs `<31 byte` auth bypass lives — a table-driven test would have caught it |
| Direct-write / partial / PIPE state machines | Pure bookkeeping over byte streams; no panel needed. PIPE reorder + SACK especially |
| Session auth, nonce/replay window | Arithmetic with load-bearing off-by-ones (`PIPE_MAX_W ≤ ±32` replay window) |
| Advertisement assembly | Pure encode; trivially testable, currently untested |
| Inflate (`shared/compress`) | Window-size rejection, truncated and corrupt streams |

**Fuzzing is not optional for the parsers.** Config TLV parsing and frame dispatch are reachable
*before authentication*, so a malformed frame from an unpaired peer reaches them. Fuzz those two
entry points; stress-test the transfer state machines with reordered, duplicated, truncated,
and interleaved chunks.

### Shared vectors with `py-opendisplay`, and what cannot be shared

The goal is a shared corpus — the same encoded frames and expected decodes, run against both the
C core and the Python host. A vector both sides agree on is a contract test; a vector they
disagree on is precisely the host/firmware divergence this repo exists to eliminate, caught in
CI instead of on a bench.

**That corpus does not exist yet and must be built.** An earlier draft of this section claimed
`py-opendisplay` already ships usable captured frames at `tests/fixtures/real_protocol_data/`.
Measured 2026-07-25, that directory is **5 files totalling 284 bytes** — two are bare 2-byte
opcodes, one is 3 bytes. It is a stub, not a corpus. The real wire vectors in that repo are
inline byte literals inside test bodies, which are not extractable as data without rewriting the
tests. So this is work to do, not an asset to adopt — and whoever builds it owns it. See
TEST_OWNERSHIP.md.

Some tests are firmware-only and have no host counterpart — do not force them into the shared
corpus:

- **Capability-permutation builds.** Every `OD_*_ENABLE` combination must compile and behave;
  the host has no equivalent of a compiled-out subsystem.
- **Memory bounds.** Static buffer sizing, absence of heap use, and the BG22 `.bss` ceiling.
- **Non-blocking behaviour.** That a handler returns without blocking, which is a property of
  the C implementation and meaningless in Python.
- **Compile-time invariants.** The `_Static_assert`s coupling `PIPE_MAX_W`, the replay window,
  and reorder-slot count.

## What belongs in `shared/core`

Target-agnostic logic that today exists in near-duplicate across the four repos:

- **Command dispatch** — the `CMD_*` opcode switch, response framing, error codes.
- **Config-packet parsing** — TLV walk into the `opendisplay_structs.h` types, validation,
  CRC, the chunked `CONFIG_WRITE` assembler.
- **Transfer state machines** — direct-write (`0x70`/`0x71`/`0x72`), partial-region (`0x76`),
  and the PIPE sliding window (`0x80`-`0x82`) including the reorder queue. These are pure
  bookkeeping over byte streams; only the final "write these bytes to the panel" call is
  target-specific.
- **Session auth / AES-CCM** — nonce handling, replay window, session timeout. The AES
  primitive comes from the HAL (each SDK has its own accelerated implementation).
- **Advertisement payload assembly** — the manufacturer-specific data layout.

## What belongs in `shared/hal`

Keep the surface small and behavioural, not a thin wrapper over one vendor's API. Proposed
starting set:

| Interface | Purpose |
|---|---|
| `od_hal_time` | monotonic ms, delay, deadline helpers |
| `od_hal_gpio` | pin mode, read/write, interrupt attach |
| `od_hal_spi` | panel bus: begin, write bytes, speed |
| `od_hal_i2c` | sensors, PMIC |
| `od_hal_nvs` | config blob load/save/erase |
| `od_hal_radio` | transport-agnostic send/receive of framed payloads (BLE GATT, LAN TCP) |
| `od_hal_crypto` | AES-CCM, CMAC, RNG |
| `od_hal_log` | line-oriented log sink |
| `od_hal_panel` | row/plane writes + refresh, the one genuinely display-specific call |

Design each as a plain C struct of function pointers or a compile-time-selected set of
functions — not C++ virtual classes, since two targets are C-only.

Two of these already exist in embryo and should be promoted rather than designed from scratch:
`Firmware_NRF54/src/nrf54_zephyr_compat.h` is `od_hal_time` plus device-id
(`od_msleep`, `od_uptime_get_32`, `od_busy_wait`, `od_hwinfo_get_device_id` — already
`od_`-prefixed), and `nrf54_gpio.c` is `od_hal_gpio`. For `od_hal_panel`/`od_hal_spi`, the
working model is `bb_epaper`'s own `*_io.inl` backends, which already abstract exactly this
surface across Arduino, Zephyr, ESP-IDF, Silabs, Linux, and memory-only.

## Non-goals

- **One build system.** Three build systems remain three: ESP-IDF, west/Zephyr, and Simplicity
  SDK + SLC each own their target directory. This is *three*, chosen per silicon vendor — not
  one, and not one per repo of origin. See docs/TOOLCHAINS.md.

  All three are CMake, which is deliberate and load-bearing: it is what lets `shared/` be a
  single library — an `idf_component_register()` component, a `target_sources(app ...)` list,
  and an SLC-declared source set — from one source list. It is not an invitation to unify the
  three builds.
- **Lowest-common-denominator features.** Targets legitimately differ (PSRAM, ROM inflate,
  panel families). Capability differences are expressed through config and `#if`, not by
  removing features.
- **Rewriting working drivers.** Import them as-is into `targets/`; only the shared logic is
  refactored.

## Memory-sensitivity note

Targets differ enormously in RAM (nRF52840 256 KB, EFR32BG22 32 KB, ESP32-S3 512 KB +
PSRAM). `shared/` code must therefore avoid:

- large static buffers sized for the biggest target,
- unbounded heap allocation, and
- assuming a heap exists at all.

Buffer sizes belong behind compile-time constants the target sets, with a documented floor.
The ESP32-S3 work that motivated this repo (window-sized inflate dictionaries, pre-reserved
TLS record buffers, de-triple-buffered config) is exactly the class of tuning that must stay
target-parameterised rather than hardcoded in `shared/`.

**`shared/` takes these as plain preprocessor constants — do not design its configuration
surface around Kconfig.** ESP-IDF and Zephyr both have Kconfig, and on those two targets an
option should get a type, default, range, and help text rather than a raw `-D`. But the Silabs
target has no Kconfig at all (SLC + `target_compile_definitions`), so Kconfig is merely *how
two of the three targets set* these constants, never the interface `shared/` presents.

The knobs that exist today: `OPENDISPLAY_ZLIB_WINDOW_BITS`, `OPENDISPLAY_ZLIB_USE_HEAP_WINDOW`,
and `PIPE_SMALL_DRAM_WINDOW` (the classic ESP32's 320 KB of RAM cannot link the full 33-slot
PIPE_WRITE reorder queue).

The zlib window is the worked example of why this must be a parameter and not a constant: the
EFR32BG22 pins a **512-byte** window (`OPENDISPLAY_ZLIB_WINDOW_BITS=9`) because 32 KB is a third
of the whole chip's RAM, while other targets pin 32 KB for legacy-client compatibility. 512 B is
the documented floor, and the encoder side must know which devices are 9-bit.
