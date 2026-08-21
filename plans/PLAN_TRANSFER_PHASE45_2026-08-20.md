# Transfer Phases 4–5 — promote NFC and close the transfer plane

**Date:** 2026-08-20

**Source snapshot:** Phase 3 closed at `b3ab8bb`, merged to `main` as `9e691a3` (shared PIPE
landed at `3ca5e57`). The earlier `aba251b` cited here was `docs: add Claude Codex mailboxes`,
not a Phase 3 implementation commit at all.

**Owns:** Phases 4 (NFC) and 5 (cleanup and release evidence) of
[`PLAN_TRANSFER_PROMOTION_2026-08-17.md`](PLAN_TRANSFER_PROMOTION_2026-08-17.md), which is
superseded in full by this document and
[`PLAN_TRANSFER_PHASE3_2026-08-20.md`](PLAN_TRANSFER_PHASE3_2026-08-20.md).

**Inherits, and does not re-decide:** § 4.1's common handler rules, § 4.5's NFC wire freeze,
§ 5.1–§ 5.4's architecture (one shared machine, link-time seam, no second vtable or registry,
replies through the existing seal-or-plain sites), § 7's commit discipline, § 8's software and
hardware gates, § 9's measurements, § 10's stop conditions and § 11's definition of done. Where
this plan and the superseded text disagree, this plan wins.

---

## 1. Outcome

Phase 4 makes `shared/core/od_nfc.{c,h}` the only `0x0083` state machine. Dispatch routes the
opcode straight to it at the unchanged reservation budget of `1`. Nordic and BG22 keep NDEF
encode/decode and tag I/O behind the two-function `od_nfc_app.h` seam; ESP32 compiles the
capability-off arm, which keeps the routed entry point and a no-op reset — dispatch names them —
and links no assembler, no response buffer and no seam reference.

Phase 5 deletes what the promotion orphaned, converts the remaining transitional ratchets into
permanent structural ones, and produces the release evidence: per-unit source and memory deltas,
link maps for every target, and a hardware matrix in which every row is either passed or
explicitly recorded as open release debt.

At the end of Phase 5 the transfer plane — pump, direct, partial, PIPE, NFC — is shared in full,
and no target owns wire parsing, byte accounting, chunk assembly, SACK construction, etag policy
or transfer ownership.

---

## 2. Entry preconditions

### 2.1 Blocking

1. **Phase 3's software candidate has landed and its full software gate is clean with no skip.**
   Its hardware rows do not block Phase 4: NFC has no dependency on PIPE hardware. Those rows must
   instead be passed or explicitly recorded as open release debt before step 13 can close Phase 5.
2. **The Nordic single-shot-write overflow (§ 3.4, N3) is resolved by step 0's standalone
   commit.** It must be fixed before the reference fixtures are frozen and must not be silently
   absorbed into the shared-machine or target-cutover commits.

   The regression test drives the handler against a **fake sink** and records that the historical
   pre-fix code forwarded the wrapped length. Do not execute the real NDEF encoder to "capture a
   fixture" — that is the overflow. Step 1's fixture for this input is historical and cites the
   step-0 fix rather than promising to capture today's behaviour.

### 2.2 Not blocking, and deliberately so

Phase 4 has no dependency on the Nordic panel SPIM work, on ESP32 hardware, or on the `OD-S1`
replay stimulus. NFC shares no state, no buffer and no code path with the image transfer plane;
its only coupling to Phase 3 is `od_core_reset()` ordering, which is already established.

---

## 3. Ground truth at authoring

### 3.1 Where NFC lives today

| Target | File | Shape |
|---|---|---|
| `nordic-zephyr` | `src/od_cmd_nfc.c` (206 lines) | full handler; 512-byte assembler + 244-byte static response buffer |
| `efr32bg22-slc` | `od_cmd_silabs.c:253-348` | full handler; 512-byte static assembler, response buffer on the stack |
| `esp32-idf` | `src/od_cmd_app.cpp:116-123` | `OD_CMD_UNKNOWN`, silent, no state |

Reset wiring already exists and is correct on both implementing targets:
`targets/nordic-zephyr/src/opendisplay_pipe.c:117` calls `od_cmd_nfc_reset()`, and
`targets/efr32bg22-slc/opendisplay_pipe.c:128` calls `od_cmd_silabs_reset()`, which clears the
config assembler and the NFC counters together.

### 3.2 The target seam already matches

Both targets declare byte-identical I/O, so `od_nfc_app.h` is a rename, not a redesign:

```c
bool opendisplay_ble_nfc_read(uint8_t *type_out, uint8_t *data_out, uint16_t *data_len_io,
                              uint16_t max_len);
bool opendisplay_ble_nfc_write(uint8_t type, const uint8_t *data, uint16_t data_len);
```

`targets/nordic-zephyr/src/opendisplay_ble.h:49-51`,
`targets/efr32bg22-slc/opendisplay_ble.h:29-30`. NDEF encode/decode, the 512-byte `s_ndef`
staging buffer and the TNB132M I2C work stay target-side and are untouched by this plan.

### 3.3 What the client actually does

From `../py-opendisplay/src/opendisplay/protocol/commands.py:102-110` and `device.py:1469-1540`:

- **`NFC_SUB_READ` has no client implementation** — `commands.py:103` says "not built here", so
  every BLE read row needs a bespoke sender/decoder that must be built before those rows can run.
  An independent NFC reader is **not** a substitute: it talks to the tag directly and bypasses
  `CMD_NFC_ENDPOINT`, shared dispatch, the seam and response framing, so it proves nothing about
  the path under test. Its role is stimulus and oracle — provisioning a record, or confirming what
  a WRITE left on the tag (§ 9).
- Inline write is used for payloads ≤ 120 bytes; above that the client sends START, 120-byte DATA
  chunks and END. Total is capped client-side at 512.
- The client raises `NfcNotSupportedError` when the **first** frame draws no response. ESP32's
  silent `OD_CMD_UNKNOWN` is what that detection rests on; it is wire behaviour, not an omission.

### 3.4 Divergences found by diffing the donors — all four are decided in § 4

The donors are `../Firmware_NRF54/src/opendisplay_pipe.c:1047-1200` and
`../Firmware_Silabs/opendisplay_pipe.c:916-1060`. `../Firmware` has no NFC, so the usual
"Firmware is the authority" rule has no subject here; the two donors are the authority and where
they disagree the safer form wins, with the reason written down.

| # | Divergence | Donors | Firmware_Unified |
|---|---|---|---|
| 1 | Chunk assembler owner binding | both bind `connection` and reject foreign DATA/END with `0x07` | **both ports dropped it** — any connection can extend or commit another's assembly |
| 2 | READ tag-data cap | 238 (`OD_PIPE_MAX_PAYLOAD - 6`) | 218 (`OD_SESSION_PAYLOAD_MAX - 4`) on both |
| 3 | Inline-write length bound | Silabs widens to `uint32_t` with a CWE-190 comment; NRF54 does not | Nordic kept the **16-bit form**; BG22 widened |
| 4 | Inline-write check order | length-fit before rec_type | Nordic matches; **BG22 checks rec_type first** |

Divergence 3 is a live defect, not a style difference. `od_cmd_nfc.c:90` evaluates
`(uint16_t)(4u + text_len) > payload_len`. The six-byte frame `00 83 01 00 FF FD`, whose four-byte
body is `01 00 FF FD`, sets `text_len` to
`0xFFFD`. `rec_type` `0` is `OD_NFC_REC_TEXT`, so the frame **clears** the rec_type gate rather
than being stopped by it; the sum wraps to `1`, the bound passes, and `nfc_encode_ndef()`'s own
guard also passes because its `payload_len` wraps to `0` — while the `memcpy` that follows uses the
unwrapped `data_len` (`targets/nordic-zephyr/src/opendisplay_nfc.c:309-322`).

**It is an out-of-bounds read as well as a write, and both are ~65 KB.** The copy takes `0xFFFD`
bytes *from* a dispatch body living in a 256-byte RX slot and puts them *into* the 512-byte `.bss`
array `s_ndef`. Neither is a near miss of a bound; each overruns its object by two orders of
magnitude. BG22 is not affected — `opendisplay_ble.c:1392` already bounds `data_len` against its
write buffer before the record builder does any arithmetic.

"Post-authentication" is not a sufficient mitigation: dispatch gates on `od_session` only when
security is enabled, so on a device with security disabled the frame reaches the handler with no
authentication at all.

### 3.5 Test and corpus coverage — partial, not absent

BG22 already has production-source NFC coverage: `tests/host/silabs_fault_test.c:198-234` drives
the real `od_cmd_app_nfc()` over a fake tag, asserting that a 218-byte record produces the
224-byte maximum frame and that **219 is refused with `NFC_ERR_READ_FAILED` rather than
truncated**; further read cases sit at lines 479-543. Its fakes and context helpers are reused
rather than re-invented.

What is genuinely absent: any Nordic NFC host test, any shared-machine suite, any capability-off
proof, and any corpus vector — `grep -c NFC tests/vectors/dispatch.json` returns 0.

### 3.6 Sizing facts

- `OD_SESSION_PAYLOAD_MAX` is 222 (`shared/core/od_session.h:97`), so a read response of
  `[status][cmd][0x80][type][len:2][data]` caps tag data at **218**.
- The 512-byte assembly limit is a **wire** constant, not a target tunable: the canonical header
  documents `NFC_ERR_BAD_TOTAL_LEN` as "total_len == 0 or > 512"
  (`shared/protocol/opendisplay_protocol.h:867`) and the client enforces the same number. Unlike
  `OD_CONFIG_MAX_SIZE` it must **not** become per-target.
- BG22 at Phase 3 HEAD: 250,292 B flash, 32,284 B static RAM, 480 B headroom. That is the
  recaptured baseline for both phases.
- Nordic carries a 244-byte static `s_nfc_rsp_buf` for a response that can never exceed 224.
- **The two controller adapters disagree above the cap, and neither is the shared machine's to
  decide.** BG22 stages tag reads in a 128-byte `s_od_nfc_read_data`
  (`targets/efr32bg22-slc/opendisplay_ble.c:238`) and **refuses** any record exceeding `out_max`
  (`:1271`, `:1296`, `:1310`, `:1353`, `:1367`), so a 219-byte record cannot exist end to end on
  real BG22 hardware at all. Nordic **truncates** to `out_max`
  (`targets/nordic-zephyr/src/opendisplay_nfc.c`, the `*io_len = out_max` arms). See N2b.

---

## 4. Frozen decisions

### N1 — Ownership is the full `{origin, tag}`, restoring donor behaviour

The shared assembler binds to the full immutable reply identity. DATA or END from any other owner
answers `[FF][83][FF][07]` and mutates nothing.

This is a **restoration**, not an invention: both donors bind `connection`, both unified ports
lost it in the import, and the canonical header already documents `0x07` as "DATA/END without
active START (**or wrong connection**)" (`opendisplay_protocol.h:868`). The wire is unchanged;
`{origin, tag}` is strictly stronger than the donors' connection byte because it also separates
transports. Consistent with § 4.1 and with Phase 3's D7.

It is nonetheless an **observable change against current unified firmware**, which accepts foreign
DATA/END: those inputs now answer `0x07` and mutate nothing. It is listed as such in § 12.

### N2 — 218 is the cap the shared handler requests from the seam

Both donors pass 238 as the `max_len` **argument**; both unified targets already narrowed that
argument to 218 so a sealed response still fits one BLE frame, applied in **both** plaintext and
encrypted sessions so the bound does not depend on whether the session happens to be encrypted.
That reasoning is correct and the cap is kept.

The claim is scoped to the handler's request. "The donors expose 238" is true of the argument
only, not of either target end to end — what a client actually receives is the adapter's answer
under that cap (N2b). The narrowing is nonetheless a divergence from the deployed donors that no
host can interrogate, so it gets a row in `docs/DIVERGENCE_MATRIX.md` in the same commit that
promotes it. The constant is derived, not literal:

```c
#define OD_NFC_READ_MAX  ((uint16_t)(OD_SESSION_PAYLOAD_MAX - 4u))   /* 218 */
OD_STATIC_ASSERT(OD_NFC_READ_MAX == 218u, "NFC read cap is wire-visible");
```

`OD_STATIC_ASSERT`, not a raw `_Static_assert`: the macro's three arms exist
(`shared/protocol/opendisplay_structs.h:259-265`) because headers like this one are pulled into
ESP32's C++ translation units, where `_Static_assert` is a compiler extension rather than standard
C++. `od_session.h` already follows the macro throughout.

Restoring 238 is out of scope and would need a protocol decision, not a firmware one.

### N2b — Over-cap adapter behaviour stays target-owned and is recorded, not normalised

Above the cap the two adapters answer differently, and § 3.6 shows the difference is structural
rather than incidental: BG22 refuses (its staging buffer is 128 bytes, below the cap), Nordic
truncates. The shared machine passes `OD_NFC_READ_MAX` down and reports whatever comes back — a
`false` becomes `0x02`, a short answer is sent at its true length. It does **not** impose a
uniform truncate-or-refuse rule.

Normalising the two adapters is a deliberate behaviour change to controller code on two targets,
with a hardware gate of its own; it is explicitly out of scope here. Both behaviours get a
`DIVERGENCE_MATRIX` row in the promoting commit. Consequences that this plan then honours:

- truncate-versus-refuse is a **shared-machine policy test over the fake**, not a claim about
  either target's hardware (§ 7);
- the existing BG22 219-byte expectation (`NFC_ERR_READ_FAILED`) is deployed behaviour that stays
  green and must not be "fixed" to match Nordic; and
- BG22's hardware row exercises its real 128-byte controller limit, not 218/219 (§ 9).

### N3 — Every length bound is evaluated in 32 bits

The shared machine widens before comparing, in every arm:

```c
if ((uint32_t)declared_len + 4u > (uint32_t)body.n) { /* 0x01 */ }
```

This adopts the Silabs donor form and fixes § 3.4's divergence 3 by construction. The change is
demonstrated against a fake sink, which records the length the pre-fix handler forwarded; the real
NDEF encoder is never run with a wrapped length (§ 2.1). `od_span_split()` is used wherever a cut
is taken, per architectural decision 1.

The sibling `Firmware_NRF54` carries the same defect and is **not** fixed from here; it is filed
in `docs/FOLLOWUPS.md` as external work (§ 13).

### N4 — Validation order: length before record type, as its own arm

Both donors and the Nordic port reject a malformed length with `0x01` before testing rec_type.
BG22's port both reordered the checks and **conflated the length test with the sink call** —
`if ((uint32_t)len + 4u > n || !opendisplay_ble_nfc_write(...)) err = NFC_ERR_TAG_WRITE_FAILED`
(`od_cmd_silabs.c:290-297`) — so a bad length there answers `0x03`, not `0x01`. The donor order
wins, and the length check becomes its own arm.

**Two BG22 input classes change**, not one:

| Input | Today | After |
|---|---|---|
| valid rec_type, declared length beyond the body | `0x03` | `0x01` |
| invalid rec_type, declared length beyond the body | `0x05` | `0x01` |

Unchanged and pinned so they stay that way: invalid rec_type with an exactly-fitting length still
answers `0x05`, and a body with trailing bytes beyond the declared length is still **accepted**.
Both changed classes are named in the cutover commit message, the test oracle, the
`DIVERGENCE_MATRIX` row and § 12's exception list.

### N5 — Which failures clear, retain or replace the logical assembler state

Frozen from the donors, which both targets already match:

| Event | Reply | Assembler |
|---|---|---|
| DATA without active START, or wrong owner | `FF 83 FF 07` | untouched |
| DATA would exceed `total_len` | `FF 83 FF 08` | **cleared** |
| END with `received != total_len` | `FF 83 FF 09` | **retained — retryable** |
| END, tag write failed | `FF 83 FF 03` | **cleared** |
| END committed | `00 83 81` | cleared |
| Replacement START, from the same or a foreign owner | `00 83 82` | cleared, then re-armed for the new owner |
| `od_core_reset()` | none | cleared |

The retryable short END is deployed behaviour on both targets and both donors. It is pinned by
test and is not simplified away for cleanup symmetry (§ 4.5 of the superseded plan says so
explicitly).

In this table **cleared is a logical state**, not a byte-for-byte promise about inactive storage:
no assembly is active, no old bytes can be committed, and the next valid START behaves as it would
after boot. Nordic currently zeroes its whole object; BG22 leaves `rec_type`, `received_len` and
staged bytes stale on some inactive paths. The shared implementation may zero the whole object.
That is an unobservable internal normalisation, not a wire divergence, and tests pin it through the
next public transition rather than requiring stale bytes to survive.

### N6 — Reply failure unwinds the arming, never the tag

Extending § 4.1's rule to this endpoint:

- **READ / inline WRITE / END** — the tag operation has already happened when the ACK is queued,
  because the ACK's meaning *is* "committed". A failed reply is reported in the handler verdict; it
  does not and cannot revert the tag. This is the opposite ordering from Phase 3's D2 (ACK before
  hardware) and deliberately so: there the ACK means "armed", here it means "done".
- **START / DATA** — a failed ACK **clears the assembler**. The client cannot know whether its
  frame landed, and its only recovery is a fresh START; leaving bytes staged would guarantee a
  later `0x08` or `0x09` on a transfer the client believes never began. This is a deliberate
  divergence from the donors, which ignore the send result entirely.

In all cases the verdict is truthful — no arm returns `OD_CMD_OK` after a reply it could not
queue — which is what keeps a failed frame out of the session activity stamp.

Reservation makes a full queue impossible at this point, so the reachable failures are a dead
transport, an invariant or reset race, and a seal failure that substitutes a hard NACK. The
START/DATA clearing is an **observable sequence-state change** from both donors and both current
ports, and is listed in § 12.

### N7 — The response buffer is a stack local, not shared static

The 224-byte read response is built in a local of the shared frame handler, sized exactly
(`6 + OD_NFC_READ_MAX`) rather than at `OD_PIPE_MAX_PAYLOAD`. The 512-byte assembler is the only
NFC object that outlives a dispatch and the only NFC static in the shared module.

The two targets are affected differently and neither figure is asserted here:

- **BG22** already builds this response on the stack, so its stack profile is unchanged.
- **Nordic** trades a 244-byte static for stack. That is a net static saving, reduced by the
  larger shared state object (N1's owner fields, X3) — measure it at the cutover rather than
  quoting a number. It also adds the response to peak main-stack use on the READ path, nested
  with `od_reply()`'s sealed buffer, so **stack high-water evidence is mandatory in Nordic's read
  hardware row** (§ 9).

### N8 — Capability-off is byte-exact silence, and carries no state

`OD_CAP_NFC=0` compiles an arm that returns `OD_CMD_UNKNOWN`, sends nothing, stamps no activity,
and references neither the assembler nor the seam. This preserves ESP32's current wire behaviour
exactly, including the client's `NfcNotSupportedError` detection (§ 3.3).

The entry point and the reset **remain as symbols** — dispatch and `od_core_reset()` name them, so
they must link, exactly as `od_pipe` retains its three entry symbols on BG22. The proof is
therefore not "no NFC symbol": `tests/host/nfc_off_test.c` plus a map/symbol check show that the
512-byte assembler, the state object and both seam references are absent from the ESP32 image.

Manufacturing an "NFC unsupported" NACK is rejected for the reason ESP32's comment already gives —
it would invent a wire meaning, and it would let an unauthenticated probe of `0x0083` hold the
exclusive link open.

### N9 — Reservation budget stays 1; the row promotes

Every arm emits exactly one frame, so `od_dispatch_ops.h`'s budget of `1` is unchanged. A budget
change would be a separate wire-policy decision.

At step 8 the row names the shared entry point directly and `od_cmd_app_nfc` leaves
`shared/core/od_cmd_app.h`. Per CLAUDE.md, a promoted opcode leaves the link-error surface that
forces every target to state an answer; the ratchet in § 8 replaces that enforcement for this
opcode.

### X1 — Phase 5 deletes on "last caller disappeared", per file

No speculative deletion and no deletion of anything a target still calls. The inventory is fixed
in step 10 and each entry names the commit that orphaned it.

### X2 — Ratchets are retired only when their subject cannot return

A transitional ratchet is either converted to a permanent structural one or deleted with a stated
reason. "The code it guarded is gone" is not sufficient — the question is whether re-introducing
it would be caught by something else.

### X3 — BG22's 32,284 B / 480 B headroom is the floor

No regression against it without an explicitly approved trade-off recorded here. Phase 4 has one,
and it is approved in advance rather than gated on an impossibility:

**BG22 static RAM will grow.** Its current NFC statics are the 512-byte buffer plus three scalars;
the shared state adds N1's owner — one `od_reply_t`, itself an `od_origin_t` plus a `uint32_t` tag
(`shared/core/od_txq.h:69-72`) — plus the active flag, and alignment. A dozen-odd bytes against
480 B of headroom is an acceptable price for closing the foreign-owner defect, and the alternative
— a narrower owner representation than the rest of the repo uses — trades a repo-wide invariant
for a rounding error.

So the gate is: measure the delta on the flashed image at the BG22 cutover, record it, and stop if
it exceeds **64 bytes** or leaves headroom below **400 bytes**. Either outcome means the state
object is wrong, not that the budget needs raising. Nordic's saving is measured the same way and
is not quoted in advance (N7).

### X4 — An open hardware row is release debt, never a pass

A build, a link map or a host suite result is never reported as hardware verification. An
unavailable board leaves its row open and named.

---

## 5. Architecture

### 5.1 New files

```
shared/core/od_nfc.h          state, od_nfc_frame(), od_nfc_reset()
shared/core/od_nfc.c          the whole 0x83 machine, both capability arms
shared/core/od_nfc_app.h      the two-function target seam
tests/host/nfc_test.c         shared-machine suite over a fake tag
tests/host/nfc_off_test.c     capability-off link proof
```

The seam lives in `shared/core/` beside `od_cmd_app.h`, `od_session_app.h`, `od_inflate_app.h`,
`od_xfer_app.h` and `od_boot_app.h`. `shared/hal/` is reserved for `od_hal_*` driver interfaces,
and this is not one: it names a target *function*, not a driver, which is the same distinction
that makes the APP tiers APP tiers.

**The shared command entry point is `od_nfc_frame()`**, and it is named here because three
different places have to stub or route it:

```c
/* shared/core/od_nfc.h */
od_cmd_result_t od_nfc_frame(const od_cmd_ctx_t *ctx, od_span_t body);
void            od_nfc_reset(void);
```

Both symbols exist under either capability arm (N8). `od_dispatch_ops.h` names `od_nfc_frame` at
step 8, `od_core_reset()` names `od_nfc_reset` at step 3, and the dispatch-only fixtures stub
`od_nfc_frame` rather than the seam — see § 5.1's exception and step 8.

`od_nfc.c` joins a new `OD_SHARED_SOURCES_APP_NFC` tier in `shared/sources.cmake`. Nordic and BG22
take the tier explicitly; ESP32 receives it through the aggregate and compiles it capability-off,
exactly as BG22 receives `od_pipe.c` through `APP_XFER` with `OD_CAP_PIPE=0`.

Adding `od_nfc_reset()` to `od_core.c` makes this a source-list dependency at step 3:
**every consumer that takes `APP_SESSION` with `od_core.c` also takes `APP_NFC`**, even before
dispatch is rerouted. Record that dependency beside `APP_SESSION`'s existing `APP_XFER` dependency
in `shared/sources.cmake`. The dispatch-only fixture archives remain the deliberate exception: they
filter out `od_core.c` and provide a route stub for the shared NFC entry point instead of linking the
production machine.

**The aggregate is also what `tests/host/` consumes, and the existing library split has no slot for
ESP32's profile.** Settle both points at step 3 rather than discovering them there.

- `od_shared` takes `OD_SHARED_SOURCES` with NFC **on** (`tests/host/CMakeLists.txt:43`). It is a
  static archive, so an executable pulls `od_nfc.o` only when another pulled object references it.
  The first such reference is `od_core_reset()` at step 3, not dispatch at step 8:
  `od_core_reset_test` (`tests/host/CMakeLists.txt:430`) therefore gains
  `od_nfc_app_test_stub.c` beside `od_xfer_app_test_stub.c` in step 3 and asserts that a mid-assembly
  core reset leaves the next DATA/END at `0x07`. Step 8 then updates the dispatch fixtures and
  production corpus profiles described below. Do not give the seam weak defaults; a stub visible
  in an explicit source list keeps a missing fake a link error rather than a silent no-op.
- **Neither existing variant is the capability-off proof.** `od_shared_silabs` is BG22's ABI
  (`:47-58`), and BG22 has NFC — so it is `OD_CAP_NFC=1` like the base library. ESP32's profile is
  a genuinely new combination, and `:46` states outright that "executable-local definitions cannot
  retroactively change `od_shared`". `nfc_off_test.c` therefore needs either a real additional
  variant or `od_nfc.c` compiled into the executable at `OD_CAP_NFC=0`. Choose in step 3 and say
  which; an `OD_CAP_NFC=0` define hung on an executable that links a capability-on archive proves
  nothing and will still pass.

### 5.2 The seam

```c
/* shared/core/od_nfc_app.h */
bool od_nfc_app_read(uint8_t *type, uint8_t *data, uint16_t *len_io, uint16_t cap);
bool od_nfc_app_write(uint8_t type, const uint8_t *data, uint16_t len);
```

`*len_io` is **output-only, and `cap` is the sole bound.** An implementation must not read it on
entry. Both current callers happen to pre-set it to the cap (`od_cmd_nfc.c:65`, and BG22 passes
`out_len` as both arguments), and neither adapter reads it back — every arm assigns — so the
in-value is dead today. It is pinned here precisely because § 3.2 calls the seam a rename: a
signature that looks byte-identical is an invitation for a future adapter to start depending on
what the caller left there, and the shared machine does not promise it.

Nothing else crosses it. The seam performs no parsing, builds no reply, and knows no opcode,
sub-command, error code or length field — if an implementation starts to, the seam is wrong
(§ 11).

### 5.3 Shared state

```c
struct od_nfc {
    od_reply_t  owner;       /* the reply identity entire, per N1 -- od_txq.h:69-72 */
    uint16_t    total_len;
    uint16_t    received_len;
    uint8_t     rec_type;
    bool        active;
    uint8_t     data[OD_NFC_ASSEMBLY_MAX];   /* 512, wire-frozen per § 3.6 */
};
```

**The owner is `od_reply_t` itself, not a hand-copied `{origin, tag}` pair** — matching
`od_xfer`, whose `s_xfer.owner` is already one (`od_xfer.c:31-36`).

**But C cannot compare structs, so storing the aggregate buys nothing on its own.** An earlier
draft of this section claimed the identity check would hold "by construction"; it would not. There
is no `==` for aggregates, and `memcmp` over a struct with padding is a bug waiting on a compiler.
What actually makes it hold is a **canonical equality helper declared beside the type**:

```c
/* shared/core/od_txq.h, beside od_reply_t */
static inline bool od_reply_same(const od_reply_t *a, const od_reply_t *b)
{
    return a->origin == b->origin && a->tag == b->tag;
}
```

Step 2 converts `od_xfer_owner_matches()` (`od_xfer.c:49-51`) — today the only hand-written
field-wise comparison in `shared/` — to this helper before NFC calls it in step 3. That is what
makes a future third field a single-site edit instead of a silent divergence between two
subsystems.

Field order is `owner` first, then the 16-bit pair, then the two bytes, then the buffer: `owner` is
4-aligned, so leading it avoids the 3 bytes of padding a leading `bool` forces. Do not claim the
scalars are "packed" without checking — the resulting `sizeof` is what X3 measures, and X3's
64-byte ceiling is small enough that padding is a material fraction of it.

One instance, private to the translation unit, reached through `od_nfc_reset()` and the frame
entry point — the `od_session` precedent. Under `OD_CAP_NFC=0` the struct is not defined and the
instance does not exist, while both entry symbols remain (N8).

### 5.4 Compile-time surface

Added: `OD_CAP_NFC` (Nordic 1, BG22 1, ESP32 0). Derived and static-asserted: `OD_NFC_READ_MAX`
(218, N2) and `OD_NFC_ASSEMBLY_MAX` (512, uniform, § 3.6). No new per-target tunable.

### 5.5 Reset

`od_nfc_reset()` joins `od_core_reset()` ahead of the target's hardware teardown, beside the
transfer reset the Phase 3 ratchet already orders. `od_cmd_nfc_reset()` and the NFC half of
`od_cmd_silabs_reset()` disappear at their cutovers.

**This reorders a teardown, and the reorder is stated rather than absorbed.** `od_core_reset()` is
`od_xfer_reset → od_config_read_cancel → od_txq_reset → od_session_clear` (`od_core.c:21-27`), and
Nordic today runs it *before* its own `od_cmd_config_reset()` and `od_cmd_nfc_reset()`
(`opendisplay_pipe.c:115-117`) — so NFC currently clears last. It takes the **first** slot, beside
`od_xfer_reset()`: both are producers of staged bytes, and neither reads egress or session state,
so nothing in the existing chain can observe the move. Harmless is not the same as unremarked —
CLAUDE.md describes this function as "producer, egress, session — in that order", and that sentence
is updated in the same commit rather than left to describe a chain it no longer matches.

---

## 6. Staging

Each step is one reviewable commit. No step leaves two callable implementations of one arm.

### Step 0 — Fix the Nordic single-shot-write overflow

Land N3 as a standalone production fix before freezing fixtures. Its regression test drives the
current handler over a fake sink, proves every `0xFFFC`–`0xFFFF` declared length is refused without
a sink call, and records the unsafe pre-fix forwarding result as a historical fixture without ever
calling the real NDEF encoder. Record the sibling `Firmware_NRF54` defect in `docs/FOLLOWUPS.md` in
this commit; do not modify the sibling.

### Step 1 — Freeze the reference behaviour before touching it

Capture, as executable fixtures, what each target answers **today** for every input in § 7 — from
the production source, before any change, including the § 3.4 divergences. Reuse
`silabs_fault_test.c`'s fakes for the BG22 side (§ 3.5) rather than building a second set. Diff
both donors in the same commit message and classify every difference as adaptation or drift.

This is the step that makes N4's two changed classes demonstrable rather than asserted. The
overflow input is the exception: N3 landed in step 0, so its fixture is historical and cites that
fix commit because the unsafe pre-fix behaviour is no longer executable here.

**The fixtures record observable post-condition assembler state, not only reply bytes.** N5's table
is half about state, and that half is precisely what a reply capture cannot see. Drive a follow-up
DATA, END or replacement START as appropriate and prove whether the prior assembly is retained,
cleared or replaced. Raw internal fields may also be captured for diagnosis, but they are not the
oracle once the assembler is inactive: BG22's committed-END path zeroes `s_nfc_total` while leaving
other fields stale, whereas Nordic zeroes its whole object. Per N5, the frozen contract is that no
inactive byte can affect a later public transition, not that stale storage remains byte-identical.

### Step 2 — Land `od_nfc_app.h` and the host fake

Seam header, fake tag device (programmable read payload, programmable write failure, recorded
calls), no production caller yet. In the same commit, add `od_reply_same()` beside `od_reply_t`
and convert `od_xfer_owner_matches()` to use it. Run the existing transfer suites here so the live
shared-subsystem edit is proven before step 3 introduces the helper's NFC caller.

### Step 3 — Land `od_nfc.c` dormant, both capability arms in one commit

The complete machine, including the capability-off arm, compiled and tested but routed by nothing.
Landing the off-arm separately would let a capability-on build ship with no proof that the off
build links nothing.

In the same commit, add `APP_SESSION` → `APP_NFC` to the documented source-tier dependency, add
`od_nfc_app_test_stub.c` to `od_core_reset_test`, and extend that suite with a mid-assembly reset.
This is required now because `od_core.c` names `od_nfc_reset()` now; it is not deferred to step 8's
dispatch reroute.

**`od_core_reset_test` is not the only executable that reference pulls `od_nfc.o` into, and the
others break unless this commit carries them.** `tests/host/fake_silabs/fake_silabs.c` calls
`od_core_reset()`, so every Silabs host executable — `silabs_fault_test`,
`od_dispatch_corpus_silabs_test`, the storage and lifecycle suites — links `od_core.o` and, from
step 3, `od_nfc.o` with it. Their fakes still define `opendisplay_ble_nfc_read`/`_write` under the
old names until step 6, so the seam goes unresolved and the link fails. Two obligations follow, and
neither is optional in this commit:

- **Temporary seam forwarders in production and tests.** Both capability-on production adapters
  and their associated host fakes gain `od_nfc_app_read`/`od_nfc_app_write`, forwarding to the
  `opendisplay_ble_nfc_*` functions they already implement. This keeps every intermediate target
  and host link independent of dead-section elimination. The forwarders are deleted at each
  target's cutover, when the production adapter and its fake take the seam names for real.
- **ESP32 sets `OD_CAP_NFC=0` here, not at step 7.** ESP32 consumes the aggregate and calls
  `od_core_reset()` (`session_guard.cpp:141`), so at step 3 it would compile `od_nfc.c`
  capability-**on** and link against adapters that do not exist on that target and never will.
  Step 7 keeps the capability-off *proof* — `nfc_off_test.c`, the map and symbol evidence — but the
  define itself has to land with the source that reads it.

The general rule this instance of: a shared file entering the aggregate is live at the first
**reference**, not at the first route. `od_core_reset()` is that reference, and it is three steps
ahead of dispatch.

### Step 4 — The full shared suite, still dormant

§ 7 in its entirety, including the mutation checks. The suite must fail against a machine with the
owner check removed, with the 32-bit widening reverted, with the retryable short END converted to
a reset, and with the reply-failure unwind removed.

**Dispatch still names `od_cmd_app_nfc` until step 8, so a target that deletes its definition
cannot link.** Each cutover therefore leaves a temporary hook — a wrapper whose whole body calls
the shared entry point — retired in step 8. This is Phase 2's pattern, and it is what buys the
per-target hardware gate: rerouting dispatch first would force all three targets to cut over in
one commit.

### Step 5 — Nordic cutover — **hardware gate before step 6**

`od_cmd_nfc.c`'s state and parsing are deleted, leaving the temporary hook above; `od_cmd_nfc.h`
goes; the two `opendisplay_ble_nfc_*` functions become the `od_nfc_app` implementation and the
step-3 forwarder for this target is dropped; the 244-byte static response buffer goes. Record the
static delta and the READ-path stack high-water (N7).

**Nordic's `opendisplay_pipe.c:117` call is deleted outright, not re-pointed at
`od_nfc_reset()`.** Step 3 already put that call inside `od_core_reset()`, which line 115 invokes
two lines earlier; re-pointing would reset the assembler twice per teardown and leave a target-side
reset list of exactly the kind `od_core.h` exists to abolish.

Three comments become false the moment step 3 lands and are corrected there, not here:
`od_core.c:3` and `:10` say "four calls" and "all four happen"; `od_core.h` says "Target display,
config and NFC state remains target-owned"; and `tests/host/core_reset_test.c:8` and its
`test_reset_clears_all_four()` (`:152`) name the same count. CLAUDE.md's "producer, egress,
session — in that order" sentence is the fourth (§ 5.5).

Nordic's § 9 rows are **opened in this step and updated as they run**, before BG22 starts — the
bespoke BLE READ tool is built here, since without it no READ row can be attempted at all.

### Step 6 — BG22 cutover — **hardware gate before step 8**

`od_cmd_silabs.c:253-348` is deleted down to the same temporary hook; `od_cmd_silabs_reset()`
reduces to the config assembler. Record flash and static RAM against X3's 64-byte / 400-byte stop
condition in the commit message, and open and update BG22's § 9 rows in this step as they run.

**`silabs_fault_test.c`'s NFC assertions stay green and unedited; its link plumbing does not, and
the distinction is the whole of the rule.** That file calls `od_cmd_app_nfc()` at five sites
(`:212`, `:227`, `:492`, `:514`, `:537`) and drives it through a fake `opendisplay_ble_nfc_read`
behind `fake_silabs_nfc_read_len`. This step renames the adapters onto the `od_nfc_app` seam and
step 8 removes `od_cmd_app_nfc` outright, so the fake is re-pointed at `od_nfc_app_read` and the
call sites at the shared entry point. What must not move is any expected byte or verdict — the
219-byte `NFC_ERR_READ_FAILED` refusal above all (N2b). Re-pointing a fake is plumbing; editing an
expectation is § 11's stop condition, and a commit doing the second under cover of the first is the
failure this paragraph exists to catch.

### Step 7 — ESP32 capability-off proof

Retain the step-3 `OD_CAP_NFC=0` definition in the ESP32 build and prove it here:
`nfc_off_test.c` green, map/symbol check added — absence of state and seam references, not of
symbols (N8). The stub in `od_cmd_app.cpp:116-123` becomes the same temporary hook and stays until
step 8. ESP32's § 9 row is opened and updated here.

### Step 8 — Reroute dispatch, delete the hook, install the ratchet

`od_dispatch_ops.h`'s row names the shared entry point at budget `1`; `od_cmd_app_nfc` leaves
`od_cmd_app.h`; **all three temporary hooks from steps 5-7** go; the § 8 ratchet lands in the same
commit.

**There are three host definitions and one build file**, and they do not migrate the same way:

- `tests/host/dispatch_route_test.c:148` and `tests/host/dispatch_test.c:165` — replace the target
  hook with the shared entry point's stub/route assertion.
- `tests/host/corpus_profile_portable.c:298-305` — delete `od_cmd_app_nfc` and define a route stub
  for **`od_nfc_frame`**. It must *not* gain `od_nfc_app_*` fakes: this executable links
  `od_session_fake_dispatch` → `od_shared_dispatch_fixture`, whose source list is PURE +
  HAL_CRYPTO + HAL_RADIO + APP_SESSION with `od_core.c` filtered out (`CMakeLists.txt:72-77`), so
  it contains no `od_nfc.o` for a seam fake to serve. Stubbing the seam there would leave the
  entry point unresolved while looking like the fix. Left alone entirely, its NFC vectors keep
  describing a hook dispatch no longer calls, and the § 8 ratchet cannot catch it because that
  ratchet scopes to `targets/**`.
- `tests/host/CMakeLists.txt` — `:622` compiles `targets/nordic-zephyr/src/od_cmd_nfc.c` into
  `corpus_profile_nordic`; `:647` and `:666` compile `targets/efr32bg22-slc/od_cmd_silabs.c` into
  two Silabs executables. **Only the Nordic entry is removed.** `od_cmd_nfc.c` is an NFC-only
  translation unit and goes with the cutover; `od_cmd_silabs.c` also owns the buzzer, config and
  device hooks (`:245` onward) and stays in both source lists, merely shorter. A stale entry is a
  build failure rather than a silent wrong answer, which is why this is listed last — but the
  inventory is only exhaustive with it in.

After this step no target-side NFC **handler, parser or assembler** symbol survives. The
`od_nfc_app_*` adapter symbols deliberately do — they are the seam.

### Step 9 — Phase 4 evidence

**The checklist rows are not created here.** Each target's rows are opened and updated in its own
cutover step (5, 6, 7), alongside the run, per the repository rule; creating them retroactively
after the gates they were supposed to gate is backwards. Step 9 consolidates.

Consolidated here: per-target link maps, § 10's measurements, the CLAUDE.md status bullet, and an
**exhaustive** `docs/DIVERGENCE_MATRIX.md` inventory — one row each for

| Decision | What the row records |
|---|---|
| N1 | foreign DATA/END now refused `0x07`; restores donor binding both ports lost |
| N2 | the handler requests 218, where both donors pass 238 |
| N2b | the adapters disagree above the cap — Nordic truncates, BG22 refuses |
| N3 | wrapped lengths refused by mandatory step 0; **referenced, not re-added** at step 9 |
| N4 | two BG22 input classes move to `0x01` |
| N6 | assembler cleared on a failed START/DATA ACK |

This inventory, § 4's decisions and § 12's DoD item 3 must agree; if they drift, the DoD wins.

### Step 10 — Phase 5 deletion inventory

One commit per orphan class, each naming the commit that orphaned it: obsolete transfer and NFC
declarations, structs, pump buffers, target constants, build entries. Nothing speculative (X1).

### Step 11 — Ratchet consolidation

Every transitional ratchet in `tools/check.sh` is converted or deleted with a reason (X2). The
permanent set at close rejects: a second transfer parser, a second pump, a second reorder state,
a second NFC assembler, a target-side response literal for a promoted opcode, and a shared file
including a vendor header. `shared/sources.cmake`'s arrival comments are brought current.

### Step 12 — Full gate and link-map inspection

`tools/check.sh --targets` clean, no skip, every target's map read rather than merely produced.

### Step 13 — Release evidence

§ 10's measurements per unit, not only for the final squash, and the § 9 matrix with every row
passed or explicitly open.

---

## 7. Tests

`tests/host/nfc_test.c` builds the production machine over the step-2 fake, at `OD_CAP_NFC=1`.
`tests/host/nfc_off_test.c` builds it at `0`.

**Sub-commands and errors.** Every arm of `0x00`, `0x01`, `0x10`, `0x11`, `0x12`; unknown
sub-command bytes including `0x02`, `0x13` and `0xFF` (`0x04`); empty body (`0x01`); every error
code `0x01`–`0x09` reached by its own input.

**Lengths.** 0, 1, 120, 121, 218, 219, 512, 513 for both read and assembled write. A 218-byte
record is answered whole in **both** plaintext and encrypted sessions. Assert equal decoded
application payloads, not equal wire lengths: the plaintext frame is 224 bytes and the sealed
frame is 253 bytes because the session envelope adds 29 bytes (N2). Above the cap the assertion is
on the *shared machine's* handling of whatever the seam returns (N2b) — a refusing fake yields
`0x02`, a truncating fake yields a short answer sent at its true length — never a claim that either
target does one or the other.

**The overflow class (N3).** Declared lengths `0xFFFC`, `0xFFFD`, `0xFFFE`, `0xFFFF` in a
four-byte body, each answered `0x01` with no seam call — the direct regression test for § 3.4's
divergence 3. Plus declared length exactly equal to and one greater than the body remainder.

**Record types and N4's order.** All five valid values, plus 5, 6, 0x80 and 0xFF rejected `0x05`.
Both of N4's changed classes get their own case — valid type with an over-declared length, and
invalid type with an over-declared length, each answering `0x01`. Both unchanged cases are pinned
too: invalid type with an exactly-fitting length still `0x05`, and a body carrying trailing bytes
beyond the declared length still accepted.

**Assembly.** Exact fill; single-byte chunks; a trailing one-byte extension; zero-length DATA
(`0x01`, assembler retained); overflow by one byte (`0x08`, cleared); short END retried and then
completed (N5); same-owner and foreign-owner replacement START each discarding the partial
assembly and binding the new START's owner; recycled tag value after the owner died.

**Ownership (N1).** DATA and END from a different tag, and from a different origin, each answered
`0x07` with the incumbent assembly bit-for-bit unchanged and no seam call.

**Failure injection.** Read failure `0x02`; write failure on inline and on END `0x03` with the
assembler cleared; reply failure on each of READ, inline WRITE, START, DATA and END with N6's
verdict and assembler outcome asserted for each.

**Reset.** `od_core_reset()` mid-assembly clears the state and precedes hardware teardown.

**Capability-off.** Every sub-command answers nothing, returns `OD_CMD_UNKNOWN`, stamps no
activity, calls no seam; `nm` shows no assembler symbol and no seam reference.

**Corpus.** `tests/vectors/dispatch.json` gains `target-production` NFC vectors driven through
`corpus_profile_nordic.c` and `corpus_profile_silabs.c`, and the ESP32 silence as its own vector.
This closes § 3.5's remaining gaps from the wire end as well as the unit end.

**Mutation checks.** Per step 4, each of: owner check removed, 32-bit widening reverted, short-END
retry converted to a reset, reply-failure unwind removed, capability-off arm allocating the
assembler. Each must turn the suite red.

---

## 8. Ratchet transitions in `tools/check.sh`

**Added at step 8** — `transfer: no target NFC assembler`, by symbol, mirroring
`pipe_target_machine_absent`:

```
od_nfc_write_chunk_t | s_nfc_write_chunk | s_nfc_data | s_nfc_rsp_buf |
nfc_rec_type_valid | nfc_type_valid | od_cmd_nfc_reset | od_cmd_app_nfc
```

over `targets/**` — with `opendisplay_nfc.c`'s NDEF encoder explicitly out of scope, because it is
controller adaptation and stays.

**Added at step 8** — `host: NFC capability-off link proof`, beside `pipe_off_link_proof` and
`log_off_link_proof`.

**Reviewed at step 11** — every existing transitional check, against X2. `transfer: single pump
owner` and `transfer: no target PIPE machine` are expected to become permanent structural
ratchets rather than be retired; each decision is recorded in the commit message.

---

## 9. Hardware gates

Rows are opened in the owning target cutover steps (5–7), updated alongside each run, and remain
open until passed or explicitly recorded as release debt. Step 9 only consolidates that evidence.

**Nordic (`xiao_nrf52840` mandatory; one nRF54-class board):** inline write ≤ 120 bytes read back
by an independent NFC reader; chunked 512-byte write **as `OD_NFC_REC_RAW_NDEF`** read back the
same way; a 218-byte read, and
a 219-byte tag truncated to 218 — **Nordic's adapter behaviour, asserted here and nowhere else**
(N2b) — both in plaintext and encrypted sessions; BLE disconnect
mid-assembly followed by a fresh START from a new connection; tag hardware absent or failing,
answering `0x02`/`0x03`; and the § 3.4 divergence-3 frame answered `0x01` with the device still
alive afterwards. **Stack high-water on the READ path is part of this row** (N7), not an optional
extra.

**The 512-byte row must be `RAW_NDEF`, and that is not a test-convenience choice.** Every other
record type is built as an NDEF *short record* whose payload length is a single byte: TEXT caps
host data at 252, URI at 254, and WELL_KNOWN_RAW and MIME at 255 payload bytes
(`targets/nordic-zephyr/src/opendisplay_nfc.c:311-393`, and identically
`targets/efr32bg22-slc/opendisplay_ble.c:1399-1466`). A 512-byte assembly under any of them is
accepted at START, accepted across every DATA frame, and only then refused at END with `0x03`.
Only `RAW_NDEF` passes the target encoder whole; that statement does not prove that a controller
adapter addresses and commits all 512 bytes.

That gap between the 512-byte wire limit and a ~255-byte encoder limit is **deployed donor
behaviour on both targets and is not this plan's to close** — the shared machine owns assembly and
the encoder stays target-side (§ 5.2). It is captured as a step-1 frozen fixture so the promotion
demonstrably preserves it, and named here so the hardware row does not spend five round-trips
proving a refusal it mistook for a regression. BG22 inherits the same constraint below.

**BG22 (`efr32bg22-slc`):** the same list **except the 218/219 pair**, which its adapter makes
unreachable — a 219-byte record cannot survive the 128-byte staging buffer (§ 3.6) — and with
**every write above 240 bytes**, the 512-byte RAW_NDEF row included, classified as an addressing
investigation rather than a presumed pass.

The current write loop computes block offsets as `(uint8_t)(0x10u + i * 0x10u)`
(`opendisplay_ble.c:1526-1530`), which truncates to `0x00` at `i == 15` and repeats from there.

**The boundary is 240 bytes, not 512, and the row is scoped to the boundary rather than to the
largest case.** `need_blocks` is `(record_len + 15) / 16`, and block 15 covers bytes 240-255, so
*every* BG22 tag write of a record of 241 bytes or more crosses the wrap — a 250-byte MIME record
exactly as much as a 512-byte RAW_NDEF one. Scoping the investigation to the 512-byte row alone
would leave chunked writes of 241-511 bytes presumed passing on the identical path. **Nordic is
unaffected and was checked**: it commits through `nfc_t2t_payload_set()`
(`opendisplay_nfc.c:132`), the SoC's own NFCT tag peripheral, with no I2C sub-addressing anywhere
in the path. BG22's *read* path shares the expression but caps `ln` at 128, so its block index
never reaches 8 and the 128/129 row below stays a clean boundary test.

An END ACK alone cannot pass any row above 240 bytes.

**Three cases, so the boundary is located rather than merely straddled.** A single 512-byte capture
proves only that something is wrong somewhere; it cannot separate a wrap from any other failure in
a 32-block write, and it cannot show the promotion left the working range working.

| Record | Expectation | What it settles |
|---|---|---|
| RAW_NDEF, **240 bytes** (`i` reaches 14) | passes: readback byte-identical | the shared machine drives the adapter correctly below the wrap — a clean pass here is what makes 241 a controller finding rather than a promotion regression |
| RAW_NDEF, **241 bytes** (`i` reaches 15, first wrapped offset) | **investigation** | the minimal reproducer, and one block of trace instead of seventeen |
| RAW_NDEF, **512 bytes** (`i` reaches 31) | investigation | the deployed-scale case, only meaningful once 241 is understood |

Capture the I2C block sequence for each and independently read back and compare every byte. If the
device's paging rules do not make the wrapped sub-addresses distinct and readback differs at 241,
leave the 241 and 512 rows open and record the controller defect in `docs/FOLLOWUPS.md` with the
240-byte pass beside it as the bound; controller addressing changes remain outside this
promotion. If readback is whole, attach the trace that explains why the apparent wrap is valid.

**This row is severable from step 6's gate, and it is the only one that is.** X4 governs how a row
is *reported* — never as a pass — and confers no permission to proceed past a gate; citing it for
that would be reading a rule that is not there. The permission is granted here instead, and on a
stated ground: the wrap predates this plan, lives in controller code the promotion does not touch,
and cannot be caused, worsened or fixed by moving the `0x83` state machine into `shared/`. Blocking
a cutover on it would hold the promotion hostage to an unrelated defect. So: open the row, file the
defect, proceed to step 8, and close or discharge it in Phase 5's matrix (§ 13, step 13).

Every other BG22 row gates step 8 as written. A row is severable only where this plan says the word
— a general "unrelated defect" escape would dissolve the gate it is carved out of.

Substitute an exact controller-limit row. The boundary is the **raw NDEF length read from the tag
attribute**, not the decoded host-payload length, so the stimulus is specified in raw terms:
RAW_NDEF records of 128 and 129 bytes, provisioned by WRITE or by an independent writer, then read
back through the bespoke BLE READ tool — 128 answered whole, 129 refused `0x02`. Confirm the exact
boundary against the adapter before running it; `sizeof(s_od_nfc_read_data)` is the constant, but
which length is compared to it is the thing the row proves.

`silabs_fault_test.c` does **not** already assert this. It drives the handler over a fake seam at
the 218 cap and never compiles the 128-byte buffer; it stays unchanged and keeps proving what it
proves (N2b).

Plus both of N4's changed input classes confirmed on the wire, and static RAM measured against
X3's stop condition on the flashed image.

**ESP32-S3:** `0x0083` probed in both sessions draws nothing, the link is not held open, and the
client raises `NfcNotSupportedError`; map/symbol evidence attached.

**Every READ row requires the bespoke BLE tool (§ 3.3), without exception.** A physical NFC
reader bypasses the entire path being tested and can never stand in for it; it appears in these
rows only as stimulus (provisioning a record) or as an oracle (confirming what a WRITE committed).
A read row backed by a reader alone is not a pass. Building that tool is the first task of the
Nordic hardware gate, not an assumed prerequisite.

---

## 10. Software gate and acceptance

`tools/check.sh --targets` clean with no skip, at every step boundary. Per § 9 of the superseded
plan, record after each unit: shared production LOC added and target LOC removed; remaining
target-owned protocol state structs and wire-response literals; `.text`, `.rodata`, `.data`,
`.bss`, and BG22's heap-inclusive figure; stack high-water where available; hardware rows
passed/open.

Phase 4 minimums: one NFC machine; zero target NFC assemblers or response literals; zero disabled
NFC state or seam references on ESP32; BG22 static RAM within X3's approved 64-byte growth and
400-byte headroom floor; Nordic static delta and READ-path stack high-water both measured and
recorded; net handwritten production deletion, with test growth reported separately.

Phase 5 minimums: § 11 of the superseded plan, in full, with every clause satisfied or its row
explicitly open.

---

## 11. Stop conditions

Inherit § 10 of the superseded plan, and add:

- the NFC seam begins parsing sub-commands, error codes, record types or length fields;
- the 512-byte assembly limit becomes per-target;
- the 218-byte read cap changes in either direction without a protocol decision;
- the shared machine imposes a uniform truncate-or-refuse rule above the cap, or either adapter's
  over-cap behaviour is changed inside this phase (N2b);
- an existing green NFC assertion is edited to match the new machine rather than the machine being
  fixed;
- capability-off retains the assembler or references the seam;
- a reply failure is followed by a tag mutation, or by a verdict claiming success;
- foreign DATA or END can mutate, commit or clear an active assembly (a valid replacement START
  deliberately displaces it and binds the new owner, per N5);
- any length bound is evaluated in fewer than 32 bits;
- a sibling repository is modified; or
- Phase 5 deletes something that still has a caller, or retires a ratchet whose subject could
  return unguarded.

---

## 12. Definition of done

1. `od_nfc.{c,h}` is the only `0x0083` machine, built as plain C on all three toolchains.
2. Target NFC code is controller adaptation only; both target handlers are deleted.
3. Shared replies match the step-1 frozen fixtures byte for byte, except the four deliberate
   changes — **N1** (foreign DATA/END now refused), **N3** (wrapped lengths refused by mandatory
   step 0), **N4** (two BG22 input classes) and **N6** (assembler cleared on a failed START/DATA
   ACK) — each recorded in `DIVERGENCE_MATRIX`.
4. ESP32 links no NFC state and its silence is proven by test and by map.
5. The corpus covers `0x0083` from both ends on both production profiles.
6. Every § 9 row is passed or explicitly open as named release debt.
7. Phase 5's deletions are complete, the ratchet set is permanent, and `shared/sources.cmake`'s
   comments are current.
8. Measurements show net deletion with no BG22 regression against X3.
9. `tools/check.sh --targets` reports no failure and no skip.

---

## 13. External follow-up work — not implemented here

- **`Firmware_NRF54` carries the § 3.4 divergence-3 overflow** in
  `src/opendisplay_pipe.c`'s `handle_nfc_endpoint()`, with the same wrapping 16-bit bound and the
  same unguarded `memcpy` in its NDEF encoder. Sibling repositories are read-only; file it in
  `docs/FOLLOWUPS.md` with the reproducing frame.
- **`Firmware_Silabs` binds the assembler to `connection` and `Firmware_NRF54` does too**; if
  either donor is ever re-imported, N1 must not be lost a second time.
- **`py-opendisplay` implements no `NFC_SUB_READ`**, so the read half of the wire has no client
  and no host-side validation. Worth a client-side issue independently of this plan.
- **The 218 vs 238 read cap (N2)** is a firmware-side narrowing of a deployed wire behaviour. If
  the fleet ever needs the full 238 bytes it is a protocol revision, not a firmware change.
- **The adapters disagree above the cap (N2b)** — Nordic truncates, BG22 refuses and cannot stage
  more than 128 bytes anyway. Normalising them is a controller-code change on two targets with a
  hardware gate of its own, and is the natural successor task to this phase.
