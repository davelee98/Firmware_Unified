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

### 2.2 The auth-required shape is documented backwards

**verified** — `opendisplay_protocol.h:190` and `:222` both state `[0xFE][cmd_echo]`. All three
implementations ship `[0x00][echo][0xFE]`, with `0xFE` as *data* in a 3-byte frame. Already
recorded as DIVERGENCE §1.1's resolution ("correct the spec, do not change firmware"); repeated
here only because §2.1 above is the same class of defect and the two should be fixed in one
pass.

---

## 3. This repo

### 3.1 `MAX_CONFIG_SIZE` is 4096 stored but 4000 transferable

**verified** — `MAX_CONFIG_CHUNKS` is `20` (`opendisplay_protocol.h:889`) and each chunk
carries at most `CONFIG_CHUNK_SIZE` = 200 data bytes, so the chunked `CONFIG_WRITE` path tops
out at **20 × 200 = 4000 bytes**.

The fleet-wide 4096 decision (DIVERGENCE §2.7, MEMORY_CONSTRAINTS item 3) is not wrong, but it
is incomplete as stated: a 4001–4096-byte config is storable and **not writable over the
chunked path** — it NACKs mid-transfer. Raising `MAX_CONFIG_CHUNKS` to 21 is a canonical-header
change, and the headers are frozen.

Decide which of these is intended, and record it where the 4096 decision lives:

1. **The effective ceiling is 4000** and 4096 is only a storage bound. Cheapest; costs 96 bytes
   of schema headroom that nobody is using yet.
2. **Raise `MAX_CONFIG_CHUNKS` to 21** when the freeze lifts, making the two numbers agree.
3. **Leave the gap and document it** as a host obligation — `py-opendisplay` must refuse to
   build a config above 4000 bytes rather than failing partway through a transfer.

Until this is settled, prefer (1) in anything written, since it is what the wire does today.

### 3.2 Corpus schema gaps

Found by authoring the first 23 vectors. All three are recorded in the `_meta` of the vector
files; they need a decision before the corpus grows much further, because retrofitting a schema
across a large corpus is the expensive version of this.

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

### 3.3 The corpus is authored, not captured

Every `expect.reply` in `tests/vectors/` is read off source and specification. **Nothing has
been observed on hardware.** [TEST_OWNERSHIP.md](TEST_OWNERSHIP.md) § "Capture is
time-sensitive" is the item with a real deadline: once `shared/core` starts replacing a
target's logic, there is no longer an untouched reference to capture from, and the corpus
becomes a description of what the unified firmware does rather than a regression baseline for
what the fleet did. Bench time is the scarce input, not engineering effort.

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
