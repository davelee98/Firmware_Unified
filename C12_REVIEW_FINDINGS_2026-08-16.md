# C12 implementation review findings

**Reviewed:** 2026-08-16
**Reviewed HEAD:** `51ac382` (`Merge pull request #32 from davelee98/feat/od-dispatch-c12-bench`)
**Scope:** C12.0-C12.2 corpus and bench-tool implementation, checked against
`plans/PLAN_OD_DISPATCH_C12_2026-08-16.md` and its definition of done.

## Conclusion

C12 is not ready to sign off. The current corpus and bench tests pass, but the new bench tool can
produce false `PASS` evidence without exercising the hardware properties it claims to prove. The
previous corpus-integrity findings also remain unresolved.

The repository documentation correctly retains the hardware state as `ACCEPTED-UNRUN`. Under the
plan's definition of done, C12.0-C12.2 constitute the landed software half; full C12 remains open
until the required hardware evidence exists.

## Findings

### P1 — Replay phase can pass when PIPE never opened

`targets/esp32-idf/tools/od-device-cli.py:_bench_replay()` records the number of replies to PIPE
START and the first PIPE DATA frame, but does not validate either response. Its final result requires
only:

- silence after resending the retained encrypted frame;
- at least one response after the bad-tag control; and
- a client counter delta of one for the retained frame.

This is insufficient because nonce-replay handling occurs in the shared session gate before PIPE
handler state matters. A replayed `0x0081` frame is therefore silent even when PIPE START was
rejected and no transfer is active.

The command-line default makes this path directly reachable: `--total-size` defaults to zero, while
production PIPE START rejects a total that does not match the panel's expected transfer size.

A review probe simulated:

- PIPE START returning a NACK;
- the first PIPE DATA frame returning a NACK;
- the retained replay drawing silence; and
- an arbitrary response arriving during the bad-tag control window.

The workflow still returned:

```text
start_replies=1
first_send_replies=1
replay_drew_silence=True
control_was_answered=True
pass=True
```

The bad-tag control also accepts any notification rather than requiring the expected plaintext
decrypt-failure response. A delayed or unrelated notification can therefore satisfy it.

Finally, the phase does not continue with fresh counters through PIPE END or verify successful
completion/rendering. This omits the plan's required proof that the replay left the transfer alive.

Relevant locations:

- `targets/esp32-idf/tools/od-device-cli.py:1097-1143`
- `targets/esp32-idf/tools/od-device-cli.py:1530-1531`
- `shared/core/od_gate.c:111-125`
- `targets/nordic-zephyr/src/opendisplay_pipe_write.cpp:315-321`

### P1 — Withhold phase does not prove backpressure occurred

The plan requires the tool to observe Nordic's consumer-side `unknown cmd 0x0060` log before
re-enabling notifications. Because RX is FIFO, that observation proves the preceding CONFIG_READ
reached dispatch while the notification path was unwritable.

The implementation sends CONFIG_READ and the canary while unsubscribed, sleeps, and then
re-enables notifications. It does not capture, accept, or require the device log. Its
`frames_during_withhold == 0` condition only proves that the unsubscribed host received no
notifications; it does not prove the device attempted a send or entered the HAL's `OD_RADIO_RETRY`
path.

A review probe simulated a device that accepted the GATT writes but delayed command processing and
config delivery until notifications were re-enabled. The workflow still returned:

```text
frames_during_withhold=0
config_reassembled=True
contiguous=True
pass=True
```

The phase also checks only that chunk indexes are contiguous and their accumulated length reaches
the advertised total. It does not compare the reconstructed configuration with a known pre-withhold
read, expected bytes, or a digest, so it does not verify exact reassembly.

Relevant locations:

- `targets/esp32-idf/tools/od-device-cli.py:1146-1199`
- `targets/nordic-zephyr/src/opendisplay_pipe.c:167-179`
- `targets/nordic-zephyr/src/od_hal_radio.c:42-47`
- `plans/PLAN_OD_DISPATCH_C12_2026-08-16.md:343-352`

### P1 — Bench tests do not execute the production bench workflows

`tests/host/bench_dispatch_gate_test.py` tests lower-level operations by manually reproducing the
intended sequences. It never calls `_bench_replay()`, `_bench_withhold()`, or
`_do_dispatch_gate()`.

Consequently:

- changing `_bench_replay()` to seal twice is not detected by `test_seal_once_send_twice()`;
- moving the actual `_bench_withhold()` writes after re-enable is not detected by
  `test_withhold_writes_precede_resubscribe()`;
- weakening either production workflow to return an unconditional `pass=True` is not detected; and
- the two mutations the commit says were caught were not applied to the code paths used on a board.

The test intended to establish that CONFIG_READ is first is also vacuous for authenticated traffic:

```python
client.writes[0][:2] == b"\x00\x40" or len(client.writes[0]) > 2
```

Every encrypted command is longer than two bytes, so the assertion passes regardless of which
command was sent.

Relevant locations:

- `tests/host/bench_dispatch_gate_test.py:97-132`
- `tests/host/bench_dispatch_gate_test.py:134-158`
- `tests/host/bench_dispatch_gate_test.py:211-217`

### P1 — Previous corpus-integrity blockers remain unresolved

The C12.2 commit does not address the corpus findings from the preceding review.

#### Schema validation remains incomplete

The validator still:

- accepts an H2D step with no `expect` and converts it to an assertion of silence;
- ignores and does not validate `expect.parsed`;
- accepts `captured` provenance without complete capture metadata;
- accepts `captured-unattributed` provenance without a limitation;
- accepts unknown provenance fields; and
- has no positive/negative schema fixture suite.

Direct probes confirmed all five malformed forms were accepted.

Relevant locations:

- `tests/host/vectors_tool.py:141-208`
- `tests/host/CMakeLists.txt:352-355`

#### Generated expectation isolation is not enforced

The generated include directory is attached to each whole executable target. As a result,
`corpus_profile_portable.c`, `corpus_profile_nordic.c`, and the Nordic fakes compile with the
generated directory on their include path. They can include `dispatch_vectors.inc` and consult the
expected replies, despite documentation claiming only the runner can see it.

Relevant locations:

- `tests/host/CMakeLists.txt:364-370`
- `tests/host/CMakeLists.txt:378-393`

#### Exact corpus coverage is not enforced

The generator selects dispatch vectors by the `dispatch/` ID prefix. The runner only fails when
zero vectors are discovered or zero H2D steps execute. It does not pin:

- 24 total vectors;
- 17 dispatch vectors;
- 14 H2D dispatch vectors; or
- 3 D2H direction-only vectors.

The JSON dependency uses `file(GLOB ...)` without `CONFIGURE_DEPENDS`. A new JSON file added after
configuration can therefore be seen by the runtime schema check while remaining absent from the
already-generated C table.

Relevant locations:

- `tests/host/vectors_tool.py:301-303`
- `tests/host/corpus_runner.c:254-279`
- `tests/host/CMakeLists.txt:343-350`

#### Firmware-version pairing assertion is absent

`dispatch.json` states that the generator asserts byte equality between the current H2D
firmware-version reply and the corresponding D2H frame. No such generator assertion exists. D2H
steps are counted but not byte-compared by the C runner, and py-opendisplay 7.14.0 ignores the patch
byte. The D2H vector can therefore drift while both halves remain green.

Relevant locations:

- `tests/vectors/dispatch.json:95-107`
- `tests/vectors/dispatch.json:139-154`
- `tests/host/vectors_tool.py:217-268`
- `tests/host/corpus_runner.c:203-207`

#### Missing Python silently removes required tests

When Python is unavailable, CMake emits warnings and successfully configures without the schema
gate, both dispatch-corpus executables, or the bench test. This permits a green host build with the
entire C12 firmware-reply and bench-tool coverage absent.

Relevant locations:

- `tests/host/CMakeLists.txt:338-407`
- `tests/host/CMakeLists.txt:419-425`

### P2 — Evidence redaction does not recurse through lists

`_redact()` recursively processes dictionaries only. The evidence record's `phases` and `frames`
fields are lists, so their nested dictionaries bypass redaction.

A direct probe confirmed:

```python
_redact({"frames": [{"session_nonce": "deadbeef", "hex": "001122"}]})
```

returns the nonce unchanged. The raw encrypted frames also contain session identifiers/nonces by
construction, despite the function's statement that nonces never enter evidence files.

Relevant locations:

- `targets/esp32-idf/tools/od-device-cli.py:1202-1214`
- `targets/esp32-idf/tools/od-device-cli.py:1228-1240`
- `tests/host/bench_dispatch_gate_test.py:196-208`

### P2 — A generated Python bytecode file was committed

C12.2 adds:

```text
targets/esp32-idf/tools/__pycache__/od-device-cli.cpython-312.pyc
```

The file is generated, interpreter-specific, and modified by ordinary imports of the CLI. The
working tree was already dirty at the start of this review because this tracked bytecode file had
changed. It should be removed from version control and covered by an appropriate ignore rule.

## Verification performed

Focused gcc host verification passed:

```text
28/28 tests passed
bench_dispatch_gate: 18 checks, 0 failures
dispatch_corpus_portable: 17 discovered, 14 H2D, 3 D2H, 0 failures
dispatch_corpus_nordic: 17 discovered, 11 H2D, 2 predicate exclusions,
                        2 historical exclusions, 0 failures
```

The passing bench test does not invalidate the findings above because it does not invoke the two
production workflows whose PASS logic was probed.

The full `tools/check.sh --targets` result recorded in the C12.2 commit was not independently
reproduced during this review. Previous attempts in this environment were blocked by LeakSanitizer
under ptrace and by sandbox restrictions on the uv and external Zephyr caches.

## Required closure before sign-off

At minimum:

1. Make replay PASS require a valid START ACK, a valid first DATA response, the exact bad-tag NACK,
   and successful continuation through END/completion. Reject zero or otherwise invalid transfer
   parameters before connecting.
2. Make withhold PASS require evidence that the `0x0060` canary reached device dispatch before CCCD
   re-enable, and compare exact reconstructed config bytes with a known baseline.
3. Test `_bench_replay()`, `_bench_withhold()`, and `_do_dispatch_gate()` directly with a scripted
   fake BLE/device peer. Re-run the plan's mutations against those functions.
4. Repair list-aware evidence redaction or explicitly define which raw-wire fields are permitted in
   evidence and make the implementation and documentation agree.
5. Close the schema, expectation-isolation, exact-accounting, paired-vector, and missing-Python
   corpus findings.
6. Remove the tracked `.pyc`, restore a clean tree, and rerun the clean 13/0/0 software gate.
7. Keep C12 hardware status `ACCEPTED-UNRUN` until H1, H2, H3, and OD-S1 recovery evidence exists as
   required by the plan.
