# Report: code unification, de-duplication, and the target-specific end state

**Date:** 2026-08-17

**Snapshot:** `codex/silabs-c13`, including the current C13 working tree

**Scope:** source ownership and migration direction only; this is not an implementation plan and
does not modify any acceptance gate.

## Executive assessment

The repository is already receiving meaningful de-duplication benefits, but it is currently in
the migration hump: shared replacements, target adapters, extensive centralized tests, and the
still-unpromoted transfer implementations coexist.

The command/control plane is largely unified. ESP32 and Nordic consume the landed shared command
path, and the current C13 working tree connects BG22 to the same config, session, egress, gate and
dispatch modules. The largest remaining protocol duplication is the transfer plane: compression,
direct write, partial write, PIPE and NFC.

Full source-unification benefits arrive incrementally as those five subsystems move into
`shared/`. There is no detailed transfer execution plan or scheduled commit series yet; the
endgame roadmap defines only their order and acceptance boundaries.

The repository will not become small merely because it is unified. It supports three SDKs,
multiple bootloaders, several silicon families, different storage systems, two network models and
multiple panel backends. The correct goal is to eliminate target-specific **protocol policy and
protocol state**, not to eliminate target code that genuinely controls different hardware.

## 1. Benefits already realized

### 1.1 C13 produces net deletion on BG22

The C13 software candidate demonstrates actual deletion rather than only adding abstractions.
Measured raw source lines in the current working tree:

| Silabs source slice | Before C13 | Current | Change |
|---|---:|---:|---:|
| `opendisplay_pipe.c` | 1,303 | 274 | -1,029 |
| `opendisplay_config_parser.c` | 529 | 63 | -466 |
| Two former monoliths | 1,832 | 337 | **-1,495 (-82%)** |
| Broader protocol slice, including storage and new command/HAL adapters | 1,971 | 1,257 | **-714 (-36%)** |

The broader current slice includes:

- `opendisplay_pipe.c` and the retained config-parser compatibility surface;
- config storage;
- `od_cmd_silabs.c`;
- the crypto and radio HALs; and
- the target session-app seam.

The shared implementations those adapters consume already existed for ESP32 and Nordic, so BG22
adoption does not add a third protocol implementation. The result is a smaller target protocol
slice even after the required SDK seams are counted.

C13 also improves the constrained target's memory shape. The measured elastic heap grew by about
1 KiB relative to the pre-C13 map, which means non-heap static allocation fell by about 1 KiB.
That remains software evidence until BG22 Gate 2 runs.

### 1.2 Logic already owned once

The shared source list now owns:

- config TLV walking, validation and assembly;
- parsed configuration normalization;
- session authentication, KDF, replay protection and encryption policy;
- reply sealing and the TX queue;
- the command gate and opcode dispatch;
- config-read production;
- advertisement payload encoding; and
- watchdog and advertising policy on the targets that implement those HAL tiers.

`shared/sources.cmake` is the one source list consumed by the three targets and host tests. A new
shared translation unit therefore cannot silently reach only one target.

## 2. Why the tree still looks large

Raw line counts are useful only when separated by ownership. Current measurements are:

| Shared/test category | Raw LOC |
|---|---:|
| Shared executable C | 2,979 |
| Shared core headers | 2,578 |
| Shared HAL headers | 366 |
| Synced canonical protocol headers | 2,233 |
| Host tests, fuzz harnesses and vectors | 15,443 |

Only about 2,979 lines are executable shared C. The canonical protocol headers are a synchronized
contract rather than another implementation. The test tree is larger than the shared executable
core, but those tests do not ship in any firmware image.

For scale, handwritten target C/C++ currently measures approximately:

| Target | Raw LOC |
|---|---:|
| ESP32-IDF | 38,873 |
| Nordic/Zephyr | 14,333 |
| EFR32BG22/Silabs | 6,910 |

These target figures exclude build output, generated config, autogen and third-party directories.
They still include board support, transports, display backends, networking and device drivers, so
they are not counts of duplicated protocol logic. ESP32 will remain much larger than BG22 because
it has WiFi/LAN, more panel backends and more board variants.

Shared source is also compiled independently into every target image. Unification removes source
and behavior forks; it does not provide cross-device binary de-duplication. Binary flash may stay
flat or grow slightly even when maintenance cost and target-owned policy fall substantially.

## 3. When the remaining benefits arrive

### 3.1 C13 Gate 2 and landing

The current C13 working tree gives all three target families the shared command path. Once the
BG22 hardware gate passes and C13 lands, the command/control plane has one implementation across
all three target families.

This is the next concrete unification boundary. Until the hardware gate runs, it is a software
candidate rather than a release-qualified replacement.

### 3.2 Five transfer promotions

The largest remaining protocol-state duplication is the transfer plane. The intended order is:

1. compression stream/backend;
2. direct write;
3. partial write;
4. PIPE; and
5. NFC.

Each must be an independent, revertible promotion. A unit should:

1. define its shared state and behavioral HAL boundary;
2. add differential, malformed-sequence, interruption and capability-off tests;
3. promote only that state machine;
4. delete its old target state and corresponding command hooks in the same series;
5. build every enabled and disabled capability permutation; and
6. pass Gate 2 on every capable hardware class before the next unit begins.

The benefit should not be deferred until the fifth unit. Compression code is deleted when
compression is accepted; direct state is deleted when direct is accepted, and so on. Retaining
parallel paths until the end would turn a controlled migration into a permanent compatibility
layer.

### 3.3 Final hardware matrix and donor retirement

After C13, all five transfer units and the final silicon/bootloader/storage/transport matrix pass,
the migrated `Firmware`, `Firmware_NRF54` and `Firmware_Silabs` repositories can be retired.
`Firmware_NRF` remains the maintenance repository and host-compatibility floor for the shipped
legacy nRF52 fleet.

Donor retirement removes cross-repository duplication and authority ambiguity. It does not remove
the unified repository's target drivers.

## 4. Largest remaining duplication hotspots

The remaining transfer policy and state are spread through several large target files:

- ESP32 routes through the 241-line `od_cmd_app.cpp` into the 3,305-line
  `display_service.cpp`;
- Nordic has a 595-line `opendisplay_pipe_write.cpp`, a 127-line direct adapter and a 206-line NFC
  adapter; and
- BG22 still carries transfer reply/sequencing policy in the 443-line `od_cmd_silabs.c`, with
  panel mechanics in its 921-line display implementation.

These entire files cannot be copied into `shared/`. The portable portion is:

- transfer state and legal transitions;
- start/data/end validation;
- reply and error selection;
- byte counts, offsets and completion rules;
- compression orchestration;
- PIPE window, reorder and SACK policy;
- interruption/reset behavior; and
- shared deadlines where the semantics are transport-independent.

The target-owned remainder is panel begin/write/end, refresh, transport transmission, storage,
NFC-controller IO and other physical-device operations.

## 5. Target-specific end state

The intended ownership boundary is:

```text
shared/core
    wire policy + state machines + validation + replies + recovery

shared/hal
    small behavioral interfaces

targets
    SDK startup + transport ingress + hardware drivers + HAL implementations
```

### 5.1 What should remain target-specific

- SDK initialization, event callbacks and build/linker integration;
- BLE, LAN and vendor transport ingress/egress;
- bootloader entry and firmware-update mechanics;
- NVS/NVM3/flash drivers;
- crypto-engine bindings;
- GPIO, I2C, SPI, timers and power control;
- panel drivers and refresh mechanics;
- NFC-controller drivers and record storage;
- board-specific LED, button, buzzer, touch and sensor control; and
- explicit capability and resource constants.

### 5.2 What should not remain target-specific

- opcode maps;
- wire reply construction and error selection;
- config TLV parsing, CRC and chunk assembly;
- authentication, replay and nonce policy;
- response sealing decisions;
- queue reservation and retry policy;
- transfer sequencing and reset rules;
- compression protocol state;
- PIPE windows, reorder buffers and acknowledgments; and
- protocol constants already defined by the canonical contract.

## 6. Rules that minimize target glue

### 6.1 Move policy, not vendor calls

Reply formats, timeout semantics, persistence ordering, transfer sequencing and recovery belong in
shared code. A target hook that still selects a protocol error or constructs a response frame is
usually carrying policy that has not yet been promoted.

### 6.2 Keep HALs behavioral

Prefer interfaces such as `panel_begin`, `panel_write`, `panel_finish` and `panel_refresh` over a
one-for-one wrapper around every vendor call. Thin vendor-wrapper forests increase total target
LOC without establishing a shared policy owner.

HAL definitions should remain link-time C functions. A vtable costs RAM and indirection that BG22
does not need; the panel backend is the deliberate exception because one target can select among
multiple panel implementations at runtime.

### 6.3 Delete the old path in the promotion series

Every transfer promotion should carry a deletion budget. Acceptance should require removal of the
old target state, response construction and hooks, not merely successful linkage of a new shared
module.

### 6.4 Compile unsupported features completely out

Capability metadata should generate the unsupported response without requiring hand-written BG22
PIPE stubs or ESP32 NFC stubs. An unsupported target should link no state machine and pay no RAM or
code cost for that subsystem.

### 6.5 Remove target-side wire construction mechanically

After a subsystem is promoted, structural checks should reject its `RESP_*`, `CMD_*`, `OD_ERR_*`,
window bookkeeping and mutable protocol state under `targets/`. These tokens provide a more useful
metric than total target line count.

### 6.6 Revisit config-command orchestration after transfers

Config parsing is shared, but substantial target command adapters remain—Nordic's config command
file is 388 lines. Persistence and hardware reload are target mechanisms; the ordering of persist,
success reply, reload and session invalidation is shared policy. A small store/apply seam could
remove another layer of command glue after the transfer work without hiding hardware behavior.

### 6.7 Do not confuse file splitting with de-duplication

Splitting the 2,000-plus-line Silabs BLE file or the 3,305-line ESP32 display service may improve
ownership and reviewability, but it does not reduce target-specific code unless policy moves into
`shared/` and the original implementation is deleted.

## 7. Recommended measurements and gates

Track the following after every promotion:

- target-owned protocol-policy LOC;
- mutable transfer state outside `shared/`;
- target references to the promoted subsystem's wire constants and response codes;
- number and size of target command hooks remaining;
- shared `.text` impact per target;
- fixed RAM and elastic heap per target;
- capability-off `.text` and RAM cost; and
- target LOC deleted versus HAL glue added.

The desired trend is zero target-owned protocol policy, not equal-sized target directories.

## 8. Roadmap discrepancies to resolve before transfer planning

The endgame roadmap's sequence remains useful, but its current status and capability table are
stale relative to the C13 working tree:

1. It says Silabs consumes only pure shared sources, defines no `od_cmd_app_*` hook and still owns
   a 1,303-line pipe. The current target consumes the crypto, radio and app-session tiers, defines
   the complete hook set and has a 274-line pipe.
2. It marks NFC supported on ESP32, while the current ESP32 `od_cmd_app_nfc()` returns
   `OD_CMD_UNKNOWN` and explicitly states that the target implements no NFC.
3. `shared/sources.cmake` still contains historical commentary about target-local Nordic protocol
   headers that have already been deleted.

The detailed transfer plan must remeasure capabilities from source before choosing shared state
or HAL shapes. Otherwise it can create dead code for unsupported targets—the opposite of the
de-duplication goal.

## 9. Conclusion

The unification has already paid off: the current C13 protocol slice is about 36% smaller after
its new adapters are counted, and the two former Silabs monoliths are about 82% smaller. The source
tree looks larger because the project is carrying centralized tests and is only halfway through
removing target transfer state.

The next major reduction begins after C13 hardware qualification. Compression, direct, partial,
PIPE and NFC should then be promoted independently, with deletion required at each boundary.

The appropriate final target is not a repository with little target code. It is a repository with:

- one owner for every wire policy;
- one owner for every protocol state machine;
- thin, behavioral hardware seams;
- no code or RAM cost for unsupported capabilities; and
- target directories containing only the mechanisms that genuinely differ.
