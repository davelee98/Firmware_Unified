# Test ownership

Where the unit tests, fuzz harnesses, and wire-vector corpus for `shared/` live, and who is on
the hook when one goes red. Resolves DESIGN_REVIEW_2026-07-25.md § F4 and supplies the missing
mechanism behind MIGRATION.md § "Verification bar per subsystem" Gate 1.

> **DECIDED 2026-07-25 — Option A. Tests live in this repo.** Dave accepted the recommendation
> below; it is no longer a proposal. `Firmware_Unified` owns every test artifact, the corpus has
> exactly one copy and is never synced outward, and no other repo acquires a C toolchain or a
> test-data obligation on this project's behalf. The sections after "Recommendation" record the
> argument that decided it and the mechanism to build — read them as the specification, not as
> options still open. What remains open is *mechanism detail* (corpus schema fields, fuzz-time
> budget, whether PIPE vectors extract cleanly), not the ownership question.

## Decision

**Option A. `Firmware_Unified` owns every test artifact — the C unit tests, the fuzz harnesses,
*and* the shared wire-vector corpus — in `tests/` at the repo root. The corpus is never copied
anywhere. `py-opendisplay` participates by being *pulled in* as a pinned PyPI dependency of a CI
job that runs here, not by hosting anything.**

The corpus has exactly one copy, in this repo. The second implementation of the wire protocol is
brought to it (`pip install py-opendisplay==7.14.0`), rather than the corpus being pushed out to
the second implementation. That single inversion is what makes A work where C and D do not: it
gets the dual-stack contract test the design review asked for, with **zero files added to a repo
this project does not own** and **zero new copies to drift**.

| Artifact | Home | Runner | Owner |
|---|---|---|---|
| Wire-vector corpus (`tests/vectors/*.json`) | `Firmware_Unified` | both runners below | firmware |
| C unit tests for `shared/core`, `shared/compress` | `Firmware_Unified/tests/host/` | CTest | firmware |
| libFuzzer harnesses (config TLV, frame dispatch) | `Firmware_Unified/tests/fuzz/` | clang + CI | firmware |
| Corpus replay against the Python host | `Firmware_Unified/tests/host/replay_vectors.py` | pytest-free script, pinned `py-opendisplay` from PyPI | firmware |
| Capability-permutation builds, `.bss` ceilings, `_Static_assert`s | `Firmware_Unified/tests/` | CTest + CI matrix | firmware |
| `py-opendisplay`'s own 811 unit tests | `py-opendisplay` — **untouched** | its existing `test.yml` | @g4bri3lDev |
| Hardware Gate 2 | unchanged, manual | — | firmware |

Nothing in this recommendation requires a PR to `py-opendisplay`, `opendisplay-protocol`, or
`Home_Assistant_Integration`. That is deliberate; see § Ownership.

## The premise that does not survive contact

An earlier draft of ARCHITECTURE.md said `py-opendisplay` "already ships captured wire frames"
that "should become a shared corpus" (since corrected there, 2026-07-25 — its § "Shared vectors
with `py-opendisplay`" now records the same measurement as below). Measured, the corpus is:

```
tests/fixtures/real_protocol_data/   5 files, 284 bytes total
  02_read_firmware_command.bin       2 B   0043
  02_read_firmware_response.bin     45 B   004300412865...   <- the only real capture
  03_upload_start_uncompressed…bin   2 B   0070
  04_data_chunk_command.bin        232 B   0071 + 'A'*230    <- filler payload
  05_upload_end_command.bin          3 B   007200
```

Three of the five are bare opcodes. One is `0x71` followed by 230 `A`s. And the wiring is worse
than the contents:

- `01_read_config_command.bin` and `01_read_config_response.bin` **do not exist**, so
  `tests/conftest.py:96-106` returns fallbacks. The command fallback is `b"\x00\x40"`, which
  makes `tests/unit/test_protocol_commands.py:39-46` assert equality against the literal it
  already asserted one line earlier — a tautology dressed as a capture test.
- The response fallback is `b""`, which is falsy, so every `if real_*_response` body in
  `tests/unit/test_protocol_responses.py:43-158` is unreachable. Four tests, silently green,
  asserting nothing.

The real protocol vectors in that repo are **inline byte literals inside test bodies** —
`tests/unit/test_pipe_write.py` alone has 59 of them, `test_auth_protocol.py` 19,
`test_protocol_error_frames.py` 10 — across 811 test functions. They are excellent tests and
they are not extractable as data without rewriting them.

**So there is no corpus to share. There is a corpus to build.** Whoever builds it owns it, and
the party that needs it is the one promoting four divergent C implementations into one. That is
this repo. Everything below follows from that.

Two smaller corrections while here: `hypothesis>=6.148.8` is declared in
`py-opendisplay/pyproject.toml:52` and used **zero** times in `tests/`; and the differential
inflate fuzzer that F4 cites as precedent, `Firmware/tools/test_zlib_stream.c`, is referenced
only by `Firmware/tools/README.md:14` and runs in no CI anywhere.

## Why not the others

### D — corpus owned by `opendisplay-protocol`, synced out

The tempting one, and the one to reject hardest. D proposes reusing `sync_protocol_header.py`
for test data. That mechanism is **not solved; it is the unsolved problem**. Run it:

```
$ tools/sync_protocol_header.py --check
check: 1 in sync, 5 drifted, 2 missing.
```

One of eight vendored copies is correct. `Firmware_NRF54/src/opendisplay_protocol.h` differs from
canonical by 1060 diff lines; `Firmware_Silabs/opendisplay_protocol.h` is **absent entirely**
while `Firmware_Silabs/include/opendisplay_protocol.h` sits stale at protocol 2.1 and compiles
only by include-path accident (DIVERGENCE_MATRIX.md § 8.4).

The reason is structural, not neglect. The workspace `CLAUDE.md` describes `--check` as "used in
CI/pre-commit". It is not: `opendisplay-protocol` has **no `.github/` directory and no
`.pre-commit-config.yaml`**, and a grep for `sync_protocol_header` across all eleven repos hits
only `.md` files. The tool is correct, well-documented, and wired to nothing — so it reports
drift to whoever remembers to type it, which is a description of how the drift got there. This
repo is already the ninth copy: `shared/protocol/*.h` is byte-identical to canonical today,
outside the copy map, unpoliced (CLAUDE.md § "Protocol header — do not hand-edit", known gap).

Adding a test corpus to that pipeline multiplies a mechanism with a 12 % success rate over a
much larger, much more frequently edited artifact. The corpus is also the wrong shape for it:
headers are one file with one authority; vectors accrete one per bug, from whoever hit the bug.

D also puts the gate in a **single-author repo with no CI at all** (65 commits, all David Lee, no
`upstream` remote). A test that runs nowhere is not a test.

### B — everything in `py-opendisplay/tests/`

Requires a C toolchain, a CMake build, CTest, and libFuzzer in a repo whose CI is two workflows
of `uv sync` + `pytest` + `prek` (`.github/workflows/test.yml`, `lint.yml`), and whose
`CODEOWNERS` is `* @g4bri3lDev` — a maintainer with 259 commits there to this project's 18.
Firmware regressions would land as red CI in someone else's release pipeline, gating a package
that `Home_Assistant_Integration/custom_components/opendisplay/manifest.json` pins exactly
(`py-opendisplay[silabs-ota]==7.14.0`). That is the textbook setup for a test getting
`@pytest.mark.skip`-ed by a maintainer who did not cause the break and cannot fix it.

It is also backwards on substance: the tests exist to verify **C code that lives here**.

### C — split, with the corpus duplicated

C is A plus the drift. Every argument against D applies, minus D's one virtue (a designated
authority). Rejected on the premise the whole repo rests on.

### E — a new conformance repo

A ninth repo, no CI, no owner, no license, and it inherits the bootstrap problem twice: it can
neither build `shared/` (which lives here) nor release the host (which lives there). The only
version of E worth revisiting is publishing the corpus *as a package generated from this repo* —
a distribution question, deferred until someone asks for it. See § Deferred.

## The mechanism

One corpus, two runners, both in this repo.

```
tests/
  vectors/                 the corpus — JSON, hex-inline, one file per subsystem
    dispatch.json          framing, ack/nack shapes, auth gate, unknown opcode
    config_tlv.json        TLV walks, chunked assembly, CRC, unknown-type skip
    xfer_direct.json       0x70/0x71/0x72
    xfer_partial.json      0x76
    pipe.json              0x80-0x82, reorder + SACK      [requires OD_PIPE_ENABLE]
    session.json           auth handshake, nonce/replay
    advert.json            16-byte MSD encode
  host/
    CMakeLists.txt         host build of shared/ + unit tests, CTest
    test_vectors.c         C runner: feeds each vector to od_core_rx / od_config_parse
    replay_vectors.py      Python runner: same vectors through py-opendisplay's public API
    doubles/               HAL test doubles (link-time, per SHARED_API_DESIGN.md)
  fuzz/
    fuzz_config_tlv.c      libFuzzer
    fuzz_dispatch.c        libFuzzer
    corpus/                seed inputs + crash regressions (checked in)
```

A vector is data, not code:

```json
{
  "id": "dispatch/auth-required-shape",
  "dir": "h2d",
  "frame": "0070",
  "state": {"sec_enabled": true, "session": null},
  "expect": {"reply": "0070fe", "note": "DIVERGENCE 1.1 — [00][echo][0xFE]"},
  "min_protocol": "2.0",
  "requires": []
}
```

`requires` names `OD_*_ENABLE` flags; the C runner skips a vector whose flags are compiled out,
and asserts the compiled-out subsystem **NACKs rather than drops** (ARCHITECTURE.md:193-196) —
which is itself a vector class the Python side cannot express.

`replay_vectors.py` is ~150 lines of stdlib plus `py-opendisplay`, and calls its **public API
only** — `opendisplay.protocol.commands`, `.responses`, `.config_parser`. It must never
reimplement encode logic; a vector both runners derive from the same local helper proves nothing.

### Build system: CMake, sharing one source list

ARCHITECTURE.md:337-343 already commits to all three target builds being CMake so `shared/` can
be "a single library … from one source list." Make that list a real file:

```
shared/sources.cmake        set(OD_SHARED_SOURCES core/od_dispatch.c core/od_config.c ...)
```

included by `targets/esp32-idf` (`idf_component_register`), `targets/nordic-zephyr`
(`target_sources(app ...)`), `targets/efr32bg22-slc` (SLC-declared set), **and**
`tests/host/CMakeLists.txt`. Four consumers, one list — a file added to `shared/` that is not
tested is then a build error in three places, not an omission nobody notices.

Not Meson, not plain make: a second build language for the one directory every target build
already consumes is a straight loss. CTest as the runner. No test framework at first — plain
`assert()` in `.c` files registered with `add_test()`; adopt Unity or greatest only when the
absence hurts.

Host compile flags: `-std=c99 -Wall -Wextra -Werror`, under **both gcc and clang**. This is F4's
"convert the boundary check from grep to compiler" and it closes three of F8's four holes
(`extern`-declared vendor symbols, leaked target macros, freestanding-unfriendly libc) that
`shared-boundary.yml`'s grep structurally cannot see.

### Fuzzing: libFuzzer here, hypothesis there, atheris nowhere

| | Tool | Where | Cost |
|---|---|---|---|
| C parsers (config TLV, dispatch) | clang libFuzzer + ASan + UBSan | `Firmware_Unified` CI | 60 s/harness/PR + nightly 15 min |
| Python parser | `hypothesis` — already a declared dep, currently unused | `py-opendisplay`, if its maintainer wants it | not ours |
| — | atheris | **nowhere** | — |

Fuzzing is mandatory for the two pre-auth entry points (MIGRATION.md:153). Those are C. Fuzzing
the *Python* config parser is a legitimate but separate exercise that belongs to the repo that
owns that parser; proposing atheris here would put a second fuzzing toolchain in a repo that
cannot merge it. Check the seed corpus and every crash-reproducer into `tests/fuzz/corpus/` so a
found crash becomes a permanent unit test — that, not the fuzzer runtime, is where the value
compounds.

oss-fuzz is **not** recommended now: it wants a public, multi-maintainer project with a
maintained build, and this repo has one author and no build (the no-license gap F10 flagged
was closed 2026-07-25 — GPL-3.0, README § License). Revisit after the first two targets land.

## Ownership

The arrangement puts every failure in the repo of the person who caused it:

- A `shared/core` change that breaks a vector → red in `Firmware_Unified`, on the firmware
  author's PR, next to the diff that caused it.
- A `py-opendisplay` change that breaks the contract → *not* caught on their PR, because the
  runner pins a version. Caught by a **scheduled nightly job here** that runs the corpus against
  the latest PyPI release and opens an issue in this repo. Detection is ≤24 h late and lands on
  us, who then file it upstream with a failing vector attached.

That asymmetry is the honest cost of not owning `py-opendisplay`, and it is strictly better than
the alternative, which is a test in their CI that they will disable. It also produces the artifact
that makes an upstream conversation productive: a named vector, a byte string, and two decodes.

`CODEOWNERS` for this repo should route `shared/**` and `tests/**` to the same reviewer set —
F9's unaddressed point, and cheap to do in the same PR.

## Everything here is new construction

**Neither the tests nor the vectors exist. Both must be built.** `Firmware_Unified` has zero
tests. `py-opendisplay` has 811 test functions but no extractable corpus — its real vectors are
inline byte literals in test bodies, and its `tests/fixtures/real_protocol_data/` is 5 files and
284 bytes of stubs. Budget this as authoring work, not as a transcription pass over something
that already exists. That is the single most likely way this plan gets underestimated.

## Vector sources: authored and captured

Two sources, deliberately complementary. Neither is sufficient alone.

**Authored vectors** — written from the canonical header and the DIVERGENCE_MATRIX rows. These
own the error space: malformed TLVs, truncated frames, bad CRCs, replayed nonces, oversize
configs, reordered and duplicated PIPE chunks, short-frame auth attempts. **You cannot capture
these**, because a working host does not emit them. This is where the fuzz corpus seeds come
from, and where bugs like the Silabs `<31 byte` bypass would have been caught.

**Captured wire data** — recorded from live host↔device sessions. This is ground truth, and it
answers a question authored vectors cannot: *does the corpus match what the fleet actually
does?* It also supplies realistic compressed payloads and real config blobs, which are tedious
and error-prone to synthesize by hand.

### Capture is time-sensitive — do it before the migration moves code

The four existing repos are the shipping implementations. Once their logic starts being replaced
by `shared/core`, **there is no longer an untouched reference to compare against.** Capture now,
from each target as it stands today, and the corpus becomes a regression baseline the unified
firmware must reproduce — rather than a description of whatever the unified firmware happens to
do. This is the one piece of test work with a deadline, and it does not depend on any import.

### Capture plan

One session per target — ESP32, nRF54L15, EFR32BG22, nRF52840 — covering the Gate 2 scenarios
so host and hardware verification exercise the same paths:

| Scenario | Why it must be real, not authored |
|---|---|
| Full image push, uncompressed | Baseline framing and chunk cadence |
| Full image push, compressed | Real zlib streams at the 9-bit window; synthesizing these invites encoder mismatch |
| Config read/write round-trip | Real TLV blobs, including whatever fields the fleet actually sets |
| Auth handshake | The undocumented 16-byte server proof in STEP-2 (DIVERGENCE_MATRIX) |
| Partial region `0x76` | Real 17-byte BE headers and etag behaviour |
| PIPE `0x80`-`0x82` | Real window/SACK interleaving — the hardest thing to author correctly |
| Interrupted transfer + retry | Real disconnect timing and recovery state |

**Mechanism.** `py-opendisplay` is the host on every path, so record at its transport boundary
— a debug hook or env-var frame dump is far cheaper and less lossy than a BLE sniffer, and it
sees both directions with framing already resolved. The LAN transport is TCP, so `tcpdump`
works trivially there. A sniffer (nRF52 / Ubertooth) is the fallback only if the host cannot be
instrumented.

**Metadata is mandatory.** A capture without provenance is unreplayable. Each one records:
target, firmware commit SHA, protocol version, panel type, `py-opendisplay` version, transport,
and date. A vector that cannot be attributed to a known build cannot be trusted when it later
disagrees with the C core.

**Scrub secrets before anything is committed.** Captures of the authenticated path contain
session keys, nonces, MACs, and device addresses. Capture against a throwaway shared secret on a
device that will be re-provisioned, and scrub addresses. This is a public repo; a captured
session key is a real leak, not a theoretical one.

**Keep a decoded sidecar.** A `.bin` is unreviewable in a diff. Each capture ships with a
human-readable decode so a reviewer can see what changed when a vector is updated — and so a
wrong expectation is visible rather than authoritative.

## Versioning the corpus

The corpus is versioned with the repo. Cross-version testing is expressed in the vectors, not in
branches:

| Field | Expresses |
|---|---|
| `min_protocol` | the `OD_PROTOCOL_VERSION` at which the vector became valid (canonical is 2.2, `opendisplay_protocol.h:265-267`) |
| `requires` | `OD_*_ENABLE` predicate — firmware capability, skipped when compiled out |
| `deprecated_after` | for shapes the spec later corrected but shipped devices still emit |

The Python runner runs a **matrix of pinned hosts**: the version HA pins today (7.14.0), plus
`latest`. "Old firmware against a new host" is then the concrete pair (vector with low
`min_protocol`, host at `latest`) — which is the only case that actually matters in the field,
because firmware is the one unpinned link in the chain (ARCHITECTURE.md:114-121): deployed
devices are whatever they were flashed with, forever, and the host must keep decoding them.

Vectors are **append-only in meaning**. Deleting one asserts that no deployed device produces
that shape any more — a claim nobody can make about this fleet. Mark `deprecated_after` and keep
running it.

## What it costs, and what it rules out

Costs:

- One CI workflow with a Python step in a firmware repo, and a `py-opendisplay` version pin to
  bump periodically. Small, and it is the price of the dual-stack check existing at all.
- Transcribing vectors out of `py-opendisplay`'s inline byte literals is manual, one subsystem
  at a time. Budget it as part of each `shared/core` promotion, not as one bulk task.
- Host drift detected up to 24 h late (above).
- A second `shared/` consumer that must keep compiling: every HAL interface needs a test double.
  This is the intended pressure — an interface awkward to double is an interface that has leaked
  target assumptions — but it is real work at each promotion.

Ruled out:

- **`py-opendisplay` never grows a C toolchain.** No CMake, no CTest, no libFuzzer, no vendored
  `shared/` sources. If a future proposal needs one, it is a signal the split is wrong, not that
  the constraint should bend.
- **No test data ever enters `sync_protocol_header.py`'s copy map.** That tool stays scoped to
  the two headers — where its byte-for-byte rule is correct — and the correct next PR against it
  is still the one already queued: add `Firmware_Unified/shared/protocol/` as a destination,
  after applying PR #120's wording to canonical (DIVERGENCE_MATRIX.md § 8.3).
- **No ninth repo.**
- **No corpus copy in any repo.** One file tree, one authority. If a second consumer ever needs
  it, it gets a generated artifact, not a copy.

## First step — ownable today, before any target code

One PR to this repo. It is green on merge with an empty `shared/`, and it converts every
subsequent import into a test-first change.

1. **`tests/vectors/dispatch.json` and `config_tlv.json`**, seeded with ~20 vectors: the five
   `real_protocol_data` files transcribed to hex with real expectations attached, plus the
   DIVERGENCE_MATRIX § 1 framing rows (1.1 auth-required `[00][echo][FE]`, 1.2 decrypt-failure
   `[00][echo][FF]`, 1.4 the 4-byte ack padding, 1.9 unknown-opcode) and § 2.2's unknown-TLV skip
   — the case whose mishandling already loses the 0x27 security packet on two targets.
2. **`tests/host/replay_vectors.py`** — runs those vectors through `py-opendisplay==7.14.0`.
   With no C in the repo, this is a one-sided test; it still earns its place, because it forces
   the vector schema to be expressible against a real implementation before there is a second one
   to argue with, and it makes every vector added later half-verified on arrival.
3. **`shared/sources.cmake` (empty list) + `tests/host/CMakeLists.txt`** compiling `shared/` with
   gcc and clang at `-std=c99 -Wall -Wextra -Werror`, wired to CTest.
4. ~~**`.github/workflows/host-tests.yml`** running 2 and 3 on every push, plus the nightly
   latest-`py-opendisplay` job.~~ **SUPERSEDED 2026-08-15: this repo has no CI.** All four
   workflows were deleted and their checks moved to `tools/check.sh`, run by hand. Two
   consequences the rest of this document still assumes away: nothing is enforced on push, and
   the nightly latest-`py-opendisplay` job — the mechanism this document relies on to catch
   upstream wire drift within 24 h — is now `tools/check.sh --latest`, which only runs when
   someone asks. Run it before a release; drift is otherwise found whenever it is next run.
5. **`CODEOWNERS`** covering `shared/**` and `tests/**`.

Step 3 is the item DESIGN_REVIEW § "Improvements to make before the first import" #2 costs at "an
afternoon". Steps 1-2 are the part that has to happen before the config parser is promoted,
because the promotion is exactly when its error handling gets rewritten (MIGRATION.md:146-147).

### Run the captures in parallel — they have their own deadline

Capture does not wait for the PR above, and it should not queue behind the migration. Every
session recorded against a shipping firmware is a baseline that stops being obtainable once
`shared/core` starts replacing that firmware's logic (§ "Capture is time-sensitive"). Bench time
with each of the four targets is the scarce input, not engineering effort.

Order by how soon the target's code moves: **ESP32 first** (step 1 of the migration, and the
reference implementation), then nRF54L15, then EFR32BG22, then nRF52840. A target captured after
its logic has been replaced still yields a useful vector — it just no longer proves the unified
firmware did not change behaviour, which is the whole point.

## Uncertainties, stated

- **Whether `py-opendisplay`'s maintainer wants any of this.** Not asked. The recommendation is
  deliberately built to need no answer — but if the answer is later "yes, ship us the vectors,"
  the generated-artifact path (§ Deferred) opens and this doc should be revised, not worked
  around.
- **Vector coverage of the PIPE path.** `py-opendisplay/tests/unit/test_pipe_write.py` has by far
  the densest byte literals (59), and PIPE is ESP32-only firmware-side. Whether the reorder/SACK
  vectors are cleanly extractable from those tests is unverified; assume the PIPE corpus is the
  expensive one.
- **Fuzz budget.** 60 s/harness/PR is a guess with no measurement behind it. Set it after the
  first harness exists and the first corpus has a shape.
- **oss-fuzz eligibility.** Not checked against current acceptance criteria; treated as out of
  scope rather than rejected on evidence.

## Deferred

Publishing the corpus as a versioned artifact (`opendisplay-vectors` wheel, or a release asset)
so other repos can consume it read-only, generated from `tests/vectors/` by CI. One-directional,
no copies, no drift. Do not build it until a second consumer asks — until then it is a
distribution mechanism for one consumer, which is the definition of premature.
