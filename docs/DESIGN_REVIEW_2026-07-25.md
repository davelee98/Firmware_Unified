# Design review — 2026-07-25

Adversarial review of the unified-firmware design before first import, grounded in the scaffold
docs, the four source repos, and — per the host-is-the-other-half rule now in ARCHITECTURE.md —
`py-opendisplay` read as the primary consumer. Citations are `repo-relative path:line`.
Verdict up front: **the layering is sound and the boundary rules are right; the plan's biggest
exposures are outside the firmware tree** — capability discovery the host cannot actually
perform, a deployed-device config-migration hole on ESP32, an ordering that maximizes the
plan's own top risk, and a complete absence of a test story for the one artifact
(`shared/core`) that is perfectly testable off-target.

## The findings that matter most, ranked

### F1 — The capability-gating story does not reach the host, and its namespace is nearly full

The design's answer to permanent divergence is `OD_*_ENABLE` compile flags plus
NACK-on-disabled-subsystem (SHARED_API_DESIGN.md § capability gating). Read against the actual
host implementation, the chain is broken in four places:

1. **The host's capability source is the device's own stored config, not the firmware.**
   `py-opendisplay` gates every feature decision on the `transmission_modes` bitfield of the
   DisplayConfig TLV (`py-opendisplay/src/opendisplay/models/config.py:305-371`) — a value the
   *host or factory wrote to the device* and the firmware merely echoes back. Its own docstring
   states the failure mode: "a device flashed with pipe-capable firmware but an older config
   will not set the bit" (`config.py:357-364`). Capability truth is provisioning-time, and
   nothing anywhere ties an `OD_*_ENABLE` build flag to the bits the factory config carries.
   A `OD_PIPE_ENABLE=0` build provisioned with bit `0x10` set costs every upload a
   `TIMEOUT_PIPE_START` probe (`device.py:2195-2207, 2380-2391`); the inverse silently disables
   a working feature.
2. **The NACK the design mandates cannot be sent.** A non-PIPE target is supposed to NACK the
   `0x80` START with an "unsupported" code — which does not exist in the
   `OD_ERR_PIPE_START_*` namespace (DIVERGENCE_MATRIX §1 opcode table; host codes stop at
   `0x07`, `py-opendisplay/src/opendisplay/protocol/responses.py:278-284`). The headers are
   frozen. So "a disabled subsystem still answers" is, today, unimplementable as specified —
   the design's central discoverability obligation bottoms out in a spec gap it cannot close.
3. **The capability bitfield has two free bits.** `transmission_modes` is a `uint8` with
   `0x01/0x02/0x04/0x08/0x10/0x80` assigned (`config.py:321-371`). The divergences this repo
   commits to as permanent — zlib window bits, `MAX_CONFIG_SIZE`, LAN transport, NFC, Channel
   Sounding, partial support (currently a separate field) — need more expressive room than
   `0x20` and `0x40`. The capability *mechanism* is at end-of-life exactly as the design starts
   leaning on it.
4. **Nothing on the wire identifies the target.** `CMD_FIRMWARE_VERSION` returns
   maj/min/sha and `MsdAdvertisement` carries no hardware-type field
   (`shared/protocol/opendisplay_structs.h:1233-1240`), so the DIVERGENCE_MATRIX fallback
   "the host must infer capability from firmware version" is impossible once four version
   namespaces merge (see F6) — version does not name a target, and the advert cannot either.

**Consequence:** "feature parity is not a goal" is only safe if divergence is discoverable
(ARCHITECTURE.md consequence 2), and it is not. ARCHITECTURE.md § "Capabilities are discovered
by interrogation, not assumed" (added mid-review) now proposes the fix — clamped capability
read-back plus a read-only `DeviceCapabilities` TLV; see § "Adversarial read: the
capability-interrogation proposal" below for where that proposal holds and where it breaks.
Items 2 (the frozen-out *unsupported* code) and 4 (no target identity on the wire) are **not**
addressed by it, and the `py-opendisplay` counterpart remains unbudgeted either way. The whole
cluster still blocks the `MAX_CONFIG_SIZE` decision, the window-bits contract, and the
NACK-on-disabled model.

### F2 — Deployed-device continuity is unexamined for the target migrated first

MIGRATION.md raises OTA/flash-layout compatibility for exactly one target (nRF52840, step 4).
The ESP32 — step 1, the deployed reference fleet — has the same class of problem and no
mention:

- **Config storage migration.** TOOLCHAINS.md recommends "converge on NVS" for config. Deployed
  ESP32 units store the config blob in LittleFS (`Firmware/src/config_parser.cpp:67-157`). An
  IDF build that reads NVS finds nothing: the device comes up unprovisioned — panel type, pin
  map, security config gone. The doc's caveat ("verify LittleFS holds nothing else") checks the
  wrong direction; the missing piece is a **read-LittleFS-migrate-to-NVS-once** path and a
  partition table that keeps the old data partition addressable. Neither is planned anywhere.
- **Whether ESP32 field update exists at all is unrecorded.** `CMD_ENTER_DFU` on ESP32 just
  reboots (`Firmware/src/device_control.cpp:869-874`, "OTA typically handled via WiFi" — but
  no `esp_ota_*`/`Update` call exists anywhere in `src/`). If deployed ESP32 units are
  serial-flash-only, the migration constraint is milder and should be *stated*; if any field
  path exists outside this tree, it constrains the partition layout and must be documented
  before Phase B fixes one. I could not determine which is true from the repos — this is an
  unverified fact a human must supply.
- Related, smaller: NimBLE-Arduino → NimBLE C-API loses whatever bond/CCCD persistence the
  Arduino wrapper kept in NVS under its own namespaces; nobody has checked whether anything
  depends on it.

nRF54 (MCUboot, `Firmware_NRF54/zephyr/prj.conf:78-79`) and Silabs (.gbl apploader, toolchain
unchanged) look genuinely low-risk here — but that conclusion should be written down as a
checked decision, not left implicit.

### F3 — The migration order maximizes exposure to its own top-ranked risk

MIGRATION.md names concurrent development on `Firmware` as "the risk most likely to derail the
migration" — and then schedules the multi-week ESP32 framework port *first*, serializing the
entire plan behind the one step that requires freezing (or racing) the most active repo.
`Firmware` merged **76 commits in the last 30 days** including a 2000-line feature (#124); the
DIVERGENCE_MATRIX went stale against it within hours of being written.

The stated rationale — ESP32 is "the reference for what `shared/core` must expose" — is weaker
than it looks, because the docs themselves undercut it twice: DIVERGENCE_MATRIX already
*documents* the full surface (a reference can be consulted without being ported first), and
MIGRATION.md's own donor analysis says to take "coverage from `Firmware`, shape from
`Firmware_NRF54`" — i.e. the structural donor for `shared/core` is the *Zephyr* repo, which is
already C, already CMake, and nearly import-clean.

**The unweighed alternative:** import `Firmware_NRF54` first. It bootstraps `shared/core` +
HAL + the boundary + host tests in a fraction of the time, against a repo with low churn,
promoting the config parser / dispatch / session code that DIVERGENCE_MATRIX says is the better
structural donor anyway. The ESP32 port then happens *against a stabilized `shared/core`*,
shrinking the `Firmware` freeze window from "the whole extraction" to "Phase B/C of one port."
The costs are real but bounded: PIPE, LAN, and tinfl exist only in `Firmware`, so `shared/core`
v1 would lack its optional subsystems — but they are compile-gated options by design and can be
promoted during the ESP32 step. This deserves an explicit decision with the trade-off written
down; right now the doc asserts the order without ever pricing the alternative, and the cost of
reordering is zero until Phase A lands.

### F4 — There is no test story, for the one component that is trivially testable

`shared/` is plain C with no vendor headers — it compiles with host gcc by construction. Yet
across all seven docs the only verification mechanisms are a grep (shared-boundary.yml) and
on-hardware manual checks (MIGRATION.md verification bar). Missing entirely:

- **A host build of `shared/` in CI.** Even `gcc -c -std=c99 -Wall -Werror` over `shared/`
  catches what the grep cannot: transitive vendor usage, C++isms in `.c` files, implicit
  function declarations, accidental libc dependencies. It is the cheapest possible real
  enforcement of the one rule, and it is absent.
- **Unit tests for the promoted subsystems.** The config TLV parser is attacker-reachable over
  BLE *pre-auth*; its bugs are security bugs, and it is about to become the single
  implementation on every product (see F7). The transfer state machines and the session/replay
  logic are pure functions over byte streams — the ideal unit under test. A precedent exists
  and was never generalized: `Firmware/tools/test_zlib_stream.c` differentially fuzzes the
  inflater against zlib on the host.
- **Golden vectors from the primary consumer.** `py-opendisplay` ships wire-level fixtures and
  protocol tests (`py-opendisplay/tests/unit/` — auth proof, config CRC, upload flows;
  `tests/fixtures/real_protocol_data`). A dual-stack loopback — py-opendisplay encoder driving
  `od_core` compiled for the host — would verify every DIVERGENCE resolution (NRF54 chunk
  validation, session-clear-on-save, mid-session plaintext rejection) *before* any hardware
  flash, and would catch host/firmware contract drift permanently. Nothing like it is planned.

Without this, "replace the target's copy with the shared implementation" is verified only by
manual hardware smoke tests on boards two of which cannot even be built on the primary dev box.

### F5 — Host-side counterparts of recorded decisions are unfiled, and one "open" question is already answered

- **`MAX_CONFIG_SIZE`:** the host hardcodes 4096 with no per-device limit
  (`py-opendisplay/src/opendisplay/protocol/config_serializer.py:672-675`). A 2.5 KB config
  sent to a BG22 truncates silently at 2048 *today*. The firmware side is recorded
  (DIVERGENCE 2.7); the host-side work item exists nowhere.
- **The zlib window "unresolved contract" (MEMORY_CONSTRAINTS.md open question) is resolved de
  facto:** the host encodes `window_bits = FIRMWARE_ZLIB_WINDOW_BITS = 9` unconditionally
  (`py-opendisplay/src/opendisplay/encoding/compression.py:14`, `device.py:389`) and rejects
  its own output if the header advertises anything else (`device.py:1842`). Everything is
  9 bits; the E1004's 32 KB window build is **host-unreachable dead capability**. The real
  remaining question is not "what does the host send" but "does anyone want >9 bits ever, and
  where does the per-device bit live" — which is F1's capability problem, not a compression
  problem. MEMORY_CONSTRAINTS should be corrected to say so.
- Generalized rule the docs should adopt: **every DIVERGENCE resolution with wire effect gets a
  named `py-opendisplay` work item in the same row.** Currently zero rows have one.

### F6 — Versioning and release identity are undefined, and the current namespaces already collide

Four independent version schemes are about to share one repo and one wire query:
ESP32 `BUILD_VERSION "1.0.0"` default (`Firmware/src/main.h:12-13`), nRF54
`OD_APP_VERSION=0x0100` (`Firmware_NRF54/zephyr/CMakeLists.txt:74`), Silabs
`OD_APP_VERSION=0x0019` (`Firmware_Silabs/cmake_gcc/CMakeLists.txt:17`). `CMD_FIRMWARE_VERSION`
returns maj/min/sha with no target identifier (F1.4). Undecided: whether a repo tag means "all
targets released" or per-target tags (`esp32/v2.3.0`); whether version numbers unify or stay
per-target; how the host expresses "this fix exists on nRF54 ≥ X but ESP32 ≥ Y". This blocks
the first release from the unified repo and blocks any host-side compatibility matrix. It is
mentioned in no document.

### F7 — Security: known shipped vulnerabilities are queued behind a multi-month migration

The Silabs <31-byte plaintext auth bypass (`Firmware_Silabs/opendisplay_pipe.c:1238-1251` —
REBOOT/DEEP_SLEEP/DIRECT_WRITE_END execute unencrypted mid-session), the
session-survives-key-change bug (DIVERGENCE 2.4), the `diff==0` replay acceptance
(NRF54 + Silabs), and the idle-based session timeout (Silabs, 6.2) are all recorded as "fix on
adoption" — i.e., at migration step 3, months out. **These are fixes to shipping firmware and
should be hotfixed in the source repos now, out-of-band from the migration.** The migration
docs are becoming the place vulnerabilities go to wait.

The structural point is bigger: the repo's value proposition — "fixes land once" — has a dual
the docs never state: **bugs land everywhere.** `shared/core` becomes a single point of failure
across every product. That is still the right trade, but it obliges exactly the things
currently missing: host-side fuzzing of the parser (F4), a review policy for `shared/`
changes (F9), and staged rollout across targets rather than simultaneous release (F6).

### F8 — The boundary CI is weaker than the weight placed on it

`.github/workflows/shared-boundary.yml` is the only mechanical enforcement, and:

- The `od_hal_` exemption is a **line filter** (`grep -vE 'od_hal_'`), so any vendor include
  sharing a line with the string passes: `#include <esp_timer.h> /* backs od_hal_time */` is
  invisible. Fix by exempting the specific include form (`"od_hal_[a-z]+\.h"`), or drop the
  `hal/` include style so no exemption is needed.
- A grep sees only `#include` lines. `extern`-declared vendor symbols, macros leaked through a
  target-supplied config header, and freestanding-unfriendly libc calls all pass. The honest
  enforcement is F4's host compile, which the docs never schedule despite admitting the grep is
  "nowhere near sufficient."
- `\bString\b` will fire on prose in any future `shared/**/README.md` or comment; noisy checks
  get weakened. Scope it to `*.c`/`*.h`.
- The promised three-toolchain CI matrix (the thing that actually carries the "no dev box
  builds everything" load) has no design: no runner/caching plan for NCS (~GBs), IDF, or
  `slt`-delivered Simplicity SDK (2.4 GB), no cost estimate, no statement of what blocks merge.
  For a repo whose README says "CI carries more weight here than usual," CI is one grep.

### F9 — Ownership, review, and repo-lifecycle mechanics are missing

No CODEOWNERS, no named reviewer model for `shared/` changes (which touch four products at
once — who signs off for the BG22 RAM budget on an ESP32-motivated change?), no plan for the
four source repos' open issues/PRs, no statement of when a source repo becomes archived vs.
mirror, no policy for the `third_party/bb_epaper` **assembled fork** (upstream base + two
downstream-only backends — that is a fork with no upstream-sync owner, whatever the docs hope
about upstreaming). The DIVERGENCE_MATRIX claims to be "maintained, not a one-time survey" but
has no mechanism — no PR-checklist hook in the source repos, nothing in CI — and it already
went stale against #124 the day it was written. A doc that must be manually remembered during a
months-long migration of an actively developed codebase will not stay current.

### F10 — Licensing: the repo has no license, and its imports are GPL-3

All four source repos are GPL-3.0; `bb_epaper` is GPL-3.0; FastEPD is Apache-2.0.
`Firmware_Unified` has **no LICENSE file**. The first import commit publishes GPL code in an
unlicensed repo. One-file fix; must precede import. (No incompatibility issue — Apache-2.0
vendored inside a GPL-3 work is fine — but the combined-work license should be stated.)

## Big-picture soundness

**The `shared/core` + `shared/hal` cut is right.** Dependencies-inward, link-time HAL binding,
the RX-enqueue/pump execution model, and the panel ops-table exception are all well-argued and
correctly shaped by the BG22 floor. Two doc-level contradictions to fix before they confuse an
implementer: ARCHITECTURE.md/CLAUDE.md still describe the HAL as "a plain C struct of function
pointers **or** compile-time-selected functions" while SHARED_API_DESIGN.md (correctly) mandates
link-time `extern` functions with one deliberate vtable exception — align on the latter. And
README.md:70 permits "the C/C++ standard library" in `shared/` while CI rejects `.cpp` — say
plain C everywhere.

**The value proposition is real but bundled with a cost that is not dedup.** The duplicated
logic is roughly 2-4 k lines per target (parsers, state machines, session); eliminating three
copies is the genuine win. The ESP32 IDF port — 575 `String` sites, a ~450-line BLE rewrite,
the Seeed_GFX port, ten board configs — is *framework migration*, not deduplication, and it is
the largest line item in the plan. The docs justify bundling them ("the `String` removal was
going to happen anyway") but that argument only covers files destined for `shared/`; the rest
of the app's Arduino surface is being ported because the repo policy says no Arduino, not
because unification requires it. **The severable cheaper design:** `shared/` as plain C99 is
consumable by PlatformIO today (`lib_extra_dirs`/private lib — PIO compiles C sources fine);
unify the shared logic first under the existing toolchains, and do the IDF port second, on its
own merits (which are real: community-fork removal, Kconfig, native OTA tooling). TOOLCHAINS.md
rejects this as "two dependency models," which is a maintenance annoyance, not an architectural
impossibility. The current plan may still be the right call — but it should be made knowing
that most of the dedup benefit is available without the port, and the port is where the risk
lives (F3).

**One structural premise is already outdated on the reference target.** SHARED_API_DESIGN
consequence 2 asserts "single-connection is a given on all four repos," so core state is
file-static singletons with no per-connection context. Since #124 the ESP32 has two concurrent
transports; processing is serialized on one task (`Firmware/src/communication.cpp:27-45`), so
this holds *for now*, but a BLE client and a LAN client interleaving against one global
session/transfer state is a live coexistence question the survey tables (which predate #124)
never examined. Also missing from the HAL for the same execution model: a critical-section /
irq-lock primitive — `od_core_rx` runs in BT-thread/BGAPI/ISR-adjacent context while
`od_core_process` drains on the main loop, and the ring between them needs *some* documented
synchronization story (C11 atomics, or an `od_hal_irq_lock`), which no interface provides.

**The HAL surface is missing what the dispatcher's own opcode list requires.** `shared/core`
dispatches REBOOT, POWER_OFF (latch hardware), DEEP_SLEEP, and ENTER_DFU — and the HAL has no
reset, power-latch, sleep-entry, or DFU-entry interface. It builds the MSD advert from voltage
and temperature — and has no battery/sensor source interface. Either these opcodes' *actions*
stay in targets (then say so and define the callback seam) or the HAL grows
`od_hal_power`/`od_hal_dfu`/`od_hal_sensors`. Currently undecided by omission.

**The protocol-header sync mechanism survives its own critique.** README.md's "why unify" lists
"kept in sync only by a script" as a cost of the old world — and the new world keeps exactly
that mechanism between `opendisplay-protocol` and `shared/protocol/` (currently not even wired
up). With one firmware repo, a git submodule — or making `Firmware_Unified` the canonical home
and syncing *outward* to `py-opendisplay`'s constants — would eliminate the drift class rather
than police it. Worth an explicit decision instead of inheriting the status quo.

## Adversarial read: the capability-interrogation proposal

ARCHITECTURE.md § "Capabilities are discovered by interrogation, not assumed" proposes
(1) device-clamped capability read-back, (2) a read-only `DeviceCapabilities` config TLV,
(3) a `CMD_CAPABILITIES` opcode as last resort, plus the silence-never-means-unsupported
invariant. The invariant is right and is endorsed without reservation — with the delivery-order
note that it depends on an *unsupported* error code the frozen headers do not have, so the spec
addition sequences first. The rest, attacked as requested:

### Proposal 1 (clamped read-back): right instinct, wrong half — clamp on WRITE, not on READ

The write side is sound: **clamp, never reject.** Rejecting unsupported bits breaks fleet
provisioning with shared templates — a factory template carrying `0x10` would NACK on every
BG22 and fail provisioning in the field, which is strictly worse than today. Clamp-and-report
fits existing wire surface: the 4-byte config ack carries two reserved zero bytes
(DIVERGENCE_MATRIX 1.4); one bit meaning "accepted with modifications — re-read to see what
held" stays inside tier 2 of the conservative ladder. Clamping must also apply at NVS *load*,
or devices provisioned by pre-clamp firmware keep serving unsupported bits until their next
write.

The read side breaks three real things:

1. **It makes config export lossy in the direction users will not notice.** The supported
   round-trip is interrogate → `export_config_json` ("Open Display Config Builder format") →
   edit → `write_config` to any device (`py-opendisplay/src/opendisplay/device.py:1572-1607`,
   `cli.py:853-878`). A config exported from a clamped BG22 is a template silently stripped of
   PIPE/partial/every capability the small device lacks; written to an ESP32, it disables
   working features with no error anywhere. Today's mirror semantics keep templates portable
   across the fleet; clamp-on-read converts every export into a least-capable-device ratchet.
2. **It collapses "can't" and "don't" into one bit, permanently.** `transmission_modes` mixes
   capability bits with policy bits (`0x80` no-boot-text; `0x02` compression preference —
   `models/config.py:305-371`). After clamping, a cleared `0x10` means either "operator
   disabled PIPE" or "device cannot PIPE" and no reader can tell which — a config UI showing
   "supported but turned off" becomes unimplementable. Requested-vs-effective needs two
   representations, and a clamped read-write field provides one. That second representation is
   exactly what proposal 2's read-only packet is — **and once it exists, clamp-on-read is
   redundant**: stored config stays a faithful mirror (always valid by construction, because
   writes were clamped), and capability truth lives in the packet.
3. **Read-back stops being byte-stable across firmware updates.** HA caches the read blob in
   its config-entry store and replays it to skip interrogation
   (`Home_Assistant_Integration/custom_components/opendisplay/delivery.py:323-336`,
   `__init__.py:132,315`), re-reading only on an explicit resync trigger
   (`delivery.py:363-381`). Under clamp-on-read, capability truth changes with every firmware
   update while the cache does not — the cache-staleness window becomes a capability-staleness
   window. (Proposal 2 shares this exposure; either way the rule "firmware-version change
   invalidates the cached config" must be added to the host, and is another unbudgeted
   counterpart item.)

Mechanical note either way: firmware serving a modified blob must recompute the outer CRC16.
The host currently *ignores* the CRC on read (`protocol/config_parser.py:81-83`), so a lazy
implementation would appear to work — recompute anyway, or the still-open advisory-CRC decision
(DIVERGENCE 2.8, MEMORY_CONSTRAINTS) gets made by accident in the wrong place.

**Recommended reshape:** clamp on `CONFIG_WRITE` + clamp at NVS load + "modified" bit in the
ack reserved byte; leave `CONFIG_READ` a faithful mirror; put capability truth in the read-only
packet. This keeps every property proposal 1 wanted (read-back reflects what the device will
do — via the packet), avoids the template ratchet, and preserves the can't/don't distinction.

### The implemented-opcode bitmap: drop it

Three shape problems, independent of size:

- **Capability is not opcode-shaped.** PIPE is three opcodes and one capability; compressed
  direct-write is one opcode and two capabilities; the actual gaps are *values* (window bits,
  `MAX_CONFIG_SIZE`, max frame) which no bitmap expresses. Worst: `0x76` support is
  **panel-conditional at runtime** — `od_panel_caps_t.supports_partial` depends on which panel
  the current config selects (SHARED_API_DESIGN §panel), so a compile-time bitmap either lies
  for some configs or must be recomputed per config load, at which point it is not an opcode
  bitmap any more.
- **It creates a second source of truth that can disagree with the authoritative one.** The
  invariant already obliges hosts to handle the *unsupported* NACK on every attempt (bitmap
  says yes, panel-conditional NACK says no). A discovery surface that must always be
  double-checked by the mechanism it was meant to replace adds maintenance on every target
  forever and saves the host nothing.
- **It freezes an opcode-numbered worldview into the wire.** Capabilities that have no opcode
  (G5 decode, measured-palette support, 9-vs-15-bit window) still need named fields, so the
  packet needs the named-field mechanism anyway; the bitmap is then dead weight.

Keep the `DeviceCapabilities` packet to **named versioned fields**: protocol version,
`MAX_CONFIG_SIZE`, max accepted `windowBits`, max frame. Booleans stay in the existing mode
fields (clamped on write); everything else is the NACK's job.

### Host consumption cost — the genuinely unbudgeted part

`py-opendisplay` has nowhere to put any of this today. `DeviceCapabilities` (the host class) is
geometry-only — width/height/color-scheme/rotation (`models/capabilities.py:11-17`). PIPE probe
results are connection-scoped and reset on every connect (`device.py:587-591, 743-744`), so
even successful discovery is forgotten and timeouts are repaid per reconnect. Feature gates
read the cached config TLV directly (`device.py:2195-2207`). Consuming the proposal therefore
means: a TLV parser + model extension (cheap — the format is skippable by design), rethreading
the upload gates from raw `transmission_modes` to a capability object (moderate), making the
hardcoded 4096 in `config_serializer.py:672-675` per-device (the `MAX_CONFIG_SIZE` fix),
persisting capability across connections, and a HA config-entry schema bump plus the
resync-on-firmware-change rule. None of this appears in any plan or pin-bump schedule. The
firmware half of the proposal is the easy half.

### BG22 affordability: yes, with one sizing trap

The write-clamp mask is a compile-time constant and a few ANDs — zero RAM, negligible flash.
CRC16 recompute over ≤2048 B is trivial. The trap is the read path: BG22's config-read scratch
is exactly `MAX_CONFIG_SIZE`-shaped (`opendisplay_config_buf()`, NVM3 record 2064 B —
DIVERGENCE 2.5/2.7), so a synthetic read-only packet appended at `CONFIG_READ` time needs
either a scratch of stored+N bytes, or the packet counted against the 2048 stored budget, or
emit-only-when-room — and the read-chunk-count derivation (DIVERGENCE 2.6) shifts with it. It
is ~20-40 bytes and affordable, but size it deliberately: the failure mode is the smallest
target truncating the very packet that exists to report its limits. (A 32-byte opcode bitmap
would also fit; the objection above is shape, not RAM.)

## Open decisions not yet recorded (or recorded but under-specified)

| Decision | Status | Blocks |
|---|---|---|
| Feature freeze (or rebase policy) on `Firmware` during the port | **DECIDED 2026-07-25 — frozen** for the duration of the port (MIGRATION.md § Risks) | ~~Phase A start~~ — unblocked |
| Capability-discovery design (F1; ARCHITECTURE § interrogation) | **Mostly settled 2026-07-25**: clamp on **write** not read (your argument accepted); clamp firmware-authoritative bits only — never panel-descriptive fields, since the device cannot interrogate the panel; device self-determines the firmware half. **Still open**: whether the opcode bitmap survives (you argue drop it), and the `py-opendisplay` counterpart is unbudgeted | `MAX_CONFIG_SIZE` reporting; host-side capability storage |
| ESP32 config-storage migration for deployed units (F2) | **DECIDED 2026-07-25 — flash and reconfigure.** No LittleFS→NVS migration path is built; deployed units are reflashed and reconfigured from the host. Frees the partition layout and removes the Phase B migration work | Resolved (MIGRATION.md § Deployed fleet status) |
| Does ESP32 field OTA exist at all | **ANSWERED 2026-07-25.** No OTA implemented on any ESP32. But S3 (shipped) already has `app0`+`app1` partitions, so it is implementable without touching units; C6 (shipped) is `huge_app.csv`, single-slot — not implementable without a physical reflash | Resolved; C6 now bounds Phase B (MIGRATION.md § Deployed fleet status) |
| Versioning/tag/release scheme across targets (F6) | Absent | First unified release; host compat matrix |
| Host test harness + golden vectors (F4) | **DECIDED 2026-07-25** — this repo owns tests, harnesses and corpus; mechanism, versioning and first step specified in TEST_OWNERSHIP.md. **Not yet built** — corpus is new construction, and wire captures have their own deadline | Trusting any subsystem swap (until built) |
| LICENSE + provenance policy | **DECIDED 2026-07-25 — GPL-3.0**, byte-identical to all three source repos; `LICENSE` added to the repo root (README § License). Vendored third-party trees keep their own licence and notice | ~~First import commit~~ — unblocked |
| CODEOWNERS / cross-target review policy (F9) | Absent | First `shared/` PR |
| Where bootloader builds/configs live (Gecko bootloader, MCUboot sysbuild, UF2, ESP 2nd-stage) | Only nRF52840's flagged | Per-target import completeness |
| `shared/` consumption mechanics: Zephyr module (`module.yml`) vs `target_sources`; `.slcp` source registration; `idf_component_register` | Asserted as easy, never specified | First cross-target build |
| Canonical macro registry (`OD_*_ENABLE`, sizes, floors) — one header or per-target convention | **DECIDED 2026-07-25** — `opendisplay-protocol` owns wire constants *and fleet floors*; per-target selection and `OD_*_ENABLE` stay in `targets/<t>/` build files (ARCHITECTURE.md § canonical macros) | Resolved — but now depends on the header-sync mechanism, which is 1-in-sync/5-drifted/2-missing and unenforced |
| Submodule vs script-sync for `shared/protocol` | Inherited without decision | Header-drift class |
| Hotfix policy for security bugs found during survey (F7) | **DECIDED 2026-07-25 — deferred.** Fixed on promotion to `shared/core`, not hotfixed in source repos. Exposure accepted and tabulated in MIGRATION.md § Risks | Nothing — accepted risk, revisit if the timeline slips |
| Fleet status: which targets are shipped | **ANSWERED 2026-07-25.** Shipped: ESP32-S3, ESP32-C6, nRF52840, and legacy nRF52 (`Firmware_NRF`, a handful of low-capability units). Not shipped: nRF54L15. `Firmware_NRF` is maintained in place, never migrated, and sets the host's backward-compat floor | Resolved (MIGRATION.md § Deployed fleet status) |

## Likely pitfalls, ranked, with triggers

1. **Deployed ESP32 unit updated to a Phase-B/C build loses its config** (F2). Trigger: first
   reflash of a provisioned unit with NVS-only firmware. Data loss, device blind.
2. **Host sends a 2049-4096 B config to a BG22 → silent truncation → partially applied config**
   (F5). Trigger exists today; probability grows with every TLV added (NFC did).
3. **Stale `0x10` bit + `OD_PIPE_ENABLE=0` → per-connection probe timeout; the designed NACK
   cannot be emitted because its error code is frozen out** (F1.2). Trigger: any non-PIPE
   target provisioned from a pipe-era config template.
4. **The arduino_compat ratchet does not exist.** MIGRATION.md presents it as the mechanical
   mitigation for the plan's second-biggest risk, but no such CI check is in the scaffold —
   only shared-boundary.yml. If Phase B starts before the ratchet is built, it will be built
   never. Same dynamic threatens `--allow-multiple-definition` if it ships "temporarily"
   (MEMORY_CONSTRAINTS Group5 policy — adopt option 1 before Phase B, not during).
5. **DIVERGENCE_MATRIX rots silently** (F9). Trigger: any `Firmware` merge during the port —
   already happened (#124: `wifi_service.cpp`, `communication.cpp`, `od_inflate_tinfl.cpp`
   rows stale). Consequence: `shared/core` promotes superseded semantics believing them
   verified.
6. **BG22 link failure on a subsystem swap.** Claimed mitigated ("measure after every swap") —
   but a habit, not a mechanism. Make it a CI size gate (`.bss`/`.text` thresholds per target)
   or it will be skipped under schedule pressure.
7. **A `--push` of the canonical header reverts upstream PR #120's structs wording, or the
   first import lands `Firmware` upstream's stale 2.1 header** (DIVERGENCE §8). Both recorded;
   both unowned; the copy-map gap means `--check` will not catch the second.
8. **Cross-transport state interleaving on ESP32** (single global session/transfer state, two
   live transports). Trigger: LAN client acts while a BLE transfer is in flight. Survey
   predates the feature; behavior unverified.

## Improvements to make before the first import (cost of change ≈ 0 now)

1. **Add LICENSE** (GPL-3.0 to match sources) and a `NOTICE`-style provenance convention for
   `third_party/`.
2. **Stand up the host build + first tests now.** An empty `shared/` compiling under
   `gcc -Wall -Werror` in CI costs an afternoon; promote the config parser as the first
   `shared/core` unit *with tests and py-opendisplay golden vectors* before any target consumes
   it. This also converts the boundary check from grep to compiler.
3. **Build the two missing ratchets** — `arduino_compat.h` include-count and BG22 size budget —
   before the phases that need them.
4. **Fix shared-boundary.yml**: exempt `od_hal_*.h` by include form not by line, scope the
   `String` grep to C sources, and record that the grep is a stopgap for the host compile.
5. **Do the opendisplay-protocol PR** now: copy-map entry for `Firmware_Unified/shared/protocol/`
   plus the PR-#120 wording applied to canonical — sequenced, small, and it closes pitfall 7.
6. **Hotfix F7's security bugs in the shipping repos**, decoupled from migration.
7. **Settle the capability-discovery design** (F1; now drafted in ARCHITECTURE.md) with the
   reshape argued above — clamp on write not read, named fields not an opcode bitmap — plus the
   host counterpart work items, and fold the `MAX_CONFIG_SIZE` and window-bits questions into it
   rather than keeping three separate opens. The *unsupported* error code is its spec
   prerequisite and lands first.
8. **Record the versioning/tagging scheme** (F6) and add target identity to the discovery
   design — the wire currently cannot express it.
9. **Re-decide the migration order with the NRF54-first alternative priced** (F3). If ESP32-first
   survives the comparison, write down why; the current rationale does not engage with it.
10. **Answer the two deployed-fleet questions** (ESP32 field-update path; nRF52840 product
    status) — both are facts, not designs, and both gate irreversible choices.
