#!/usr/bin/env python3
"""bench_dispatch_gate_test.py -- the two bench helpers, against a fake BLE client.

WHY THESE ARE TESTED AT ALL. `dispatch-gate` exists to produce hardware evidence, and a tool that
drives its stimulus wrongly produces evidence for something else while looking identical in a log.
The two failure modes are specific:

  * sealing twice instead of resending retained bytes. The second frame then carries a FRESH nonce,
    so the device sees an ordinary duplicate sequence number rather than a replay, answers normally,
    and the run is recorded as "replay drew a response" -- the opposite conclusion.
  * writing after re-enabling notifications instead of before. Nothing is ever withheld, the device
    never reports RETRY, and the backpressure row passes without exercising anything.

Neither is visible on a board. Both are visible here, which is why this runs before either goes
near hardware.

THESE DRIVE THE PRODUCTION WORKFLOWS. An earlier version reproduced by hand the sequence the
workflows were supposed to perform, and so proved the helpers and nothing about `_bench_replay()`
or `_bench_withhold()` -- a review showed that rewriting either to seal twice, or to return an
unconditional pass, went undetected. They are now run end to end against a scripted device peer
(fake_device.py), and every case asserts on the workflow's own PASS logic.

bleak is stubbed rather than installed: nothing here touches a radio, and requiring the BLE stack
would make the gate skip on any machine without it -- and a check that skips is a check that
eventually means nothing.
"""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import pathlib
import sys
import tempfile
import types

# Stub bleak BEFORE importing the CLI. The module imports it at top level for BleakClient, which no
# case here reaches.
sys.modules.setdefault("bleak", types.SimpleNamespace(BleakClient=object))

# The CLI's filename is not an identifier, so it is loaded by path rather than imported.
_CLI = pathlib.Path(__file__).resolve().parents[2] / "targets" / "esp32-idf" / "tools"
_spec = importlib.util.spec_from_file_location("od_device_cli", _CLI / "od-device-cli.py")
assert _spec is not None and _spec.loader is not None
cli = importlib.util.module_from_spec(_spec)
# Registered BEFORE execution: @dataclass resolves its class's module through sys.modules, so a
# module that is not there yet fails at import rather than at use.
sys.modules["od_device_cli"] = cli
_spec.loader.exec_module(cli)

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fake_device import FakeDevice  # noqa: E402

# The tool narrates to stderr for an operator at a bench. In a test run that is noise around the
# assertions, so it is silenced here rather than in the tool.
cli._status = lambda _message: None

FAILURES: list[str] = []
CHECKS = 0


def check(cond: bool, what: str) -> None:
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILURES.append(what)
        print(f"FAIL {what}")


class FakeClient:
    """Records every write and every subscription change, in order.

    The ORDER is the whole point for the withhold case: it is not enough that a write happened and
    a subscribe happened, they have to have happened in that sequence."""

    def __init__(self) -> None:
        self.events: list[tuple[str, object]] = []
        self.subscribed = False

    async def write_gatt_char(self, _uuid: str, data: bytes) -> None:
        self.events.append(("write", bytes(data)))

    async def start_notify(self, _uuid: str, _cb) -> None:
        self.subscribed = True
        self.events.append(("subscribe", None))

    async def stop_notify(self, _uuid: str) -> None:
        self.subscribed = False
        self.events.append(("unsubscribe", None))

    @property
    def writes(self) -> list[bytes]:
        return [d for (k, d) in self.events if k == "write"]


SESSION_KEY_FOR_ORDER = bytes(range(16))


def make_ctx() -> tuple[cli._BleCtx, FakeClient]:
    """An authenticated context with a deterministic session, so sealed bytes are reproducible."""
    session = cli.BleSession(master_key=bytes(range(16)))
    session.session_key = SESSION_KEY_FOR_ORDER
    session.session_id = bytes(range(16, 24))   # 8 bytes: id + 8-byte counter = the 16-byte nonce
    session.counter = 0
    session.authenticated = True
    client = FakeClient()
    ctx = cli._BleCtx(client, session)
    ctx.notify_enabled = True
    return ctx, client


# ------------------------------------------------------------------------------------ cases ---

def test_seal_once_send_twice() -> None:
    """The replay must be byte-identical and must cost exactly one counter value."""
    ctx, client = make_ctx()

    async def run() -> None:
        before = ctx.session.counter
        wire = await ctx.seal_command(0x00, 0x81, b"\x00" + b"A" * 32)
        after = ctx.session.counter
        await ctx.send_raw(wire)
        await ctx.send_raw(wire)
        check(after - before == 1, "sealing one frame advances the counter exactly once")
        check(ctx.session.counter == after, "resending retained bytes does not move the counter")

    asyncio.run(run())
    check(len(client.writes) == 2, "two writes reached the transport")
    check(client.writes[0] == client.writes[1],
          "the replay is BYTE-IDENTICAL to the frame the device accepted")


def test_two_seals_would_differ() -> None:
    """The mutation this guards against, stated as its own case.

    If the tool ever seals twice, the two frames differ and the counter moves twice -- so the device
    sees a duplicate sequence number under a fresh nonce, not a replay. Asserting the difference
    here means the distinction is pinned even though no production path takes it."""
    ctx, _ = make_ctx()

    async def run() -> tuple[bytes, bytes, int]:
        a = await ctx.seal_command(0x00, 0x81, b"\x00" + b"A" * 32)
        b = await ctx.seal_command(0x00, 0x81, b"\x00" + b"A" * 32)
        return a, b, ctx.session.counter

    a, b, counter = asyncio.run(run())
    check(a != b, "two seals of the same payload differ -- a fresh nonce each time")
    check(counter == 2, "two seals cost two counter values")


def test_withhold_writes_precede_resubscribe() -> None:
    """Both commands must be written while unsubscribed, and delivery may only begin after."""
    ctx, client = make_ctx()

    async def run() -> None:
        await ctx.set_notify(False)
        await ctx.send_command(0x00, 0x40)      # the config read that will queue
        await ctx.send_command(0x00, 0x60)      # the unknown-opcode canary, FIFO behind it
        await ctx.set_notify(True)

    asyncio.run(run())
    kinds = [k for (k, _) in client.events]
    check(kinds == ["unsubscribe", "write", "write", "subscribe"],
          f"unsubscribe, both writes, then subscribe -- got {kinds}")

    unsub = kinds.index("unsubscribe")
    resub = kinds.index("subscribe")
    writes = [i for i, k in enumerate(kinds) if k == "write"]
    check(all(unsub < i < resub for i in writes),
          "every command was written inside the withheld window")

    # The canary is SECOND on purpose: RX is FIFO, so a device log naming 0x0060 proves the config
    # read ahead of it had already been dispatched with the notify path unwritable.
    #
    # ASSERTED ON THE DECRYPTED OPCODE, not the frame length. The earlier form was
    # `writes[0][:2] == b"\x00\x40" or len(writes[0]) > 2`, and every encrypted command is longer
    # than two bytes -- so the `or` made it unconditionally true. A test that reads as an ordering
    # check and checks nothing is worse than no test.
    opcodes = [cli.decrypt_response(SESSION_KEY_FOR_ORDER, w)[:2] for w in client.writes]
    check(opcodes == [b"\x00\x40", b"\x00\x60"],
          f"config read first, canary second -- got {[o.hex() for o in opcodes]}")


def test_set_notify_is_idempotent() -> None:
    """Asking for a state the link is already in must not emit a spurious CCCD write, or the
    ordering assertion above becomes noise on a real link."""
    ctx, client = make_ctx()

    async def run() -> None:
        await ctx.set_notify(True)              # already enabled
        await ctx.set_notify(False)
        await ctx.set_notify(False)             # already disabled
        await ctx.set_notify(True)

    asyncio.run(run())
    kinds = [k for (k, _) in client.events]
    check(kinds == ["unsubscribe", "subscribe"], f"one change each way -- got {kinds}")


def test_raw_log_records_before_decryption() -> None:
    """The evidence has to be the bytes on air. A decrypted view cannot show that a replay was
    identical, which is the one thing the replay phase must demonstrate."""
    ctx, _ = make_ctx()
    log = cli._FrameLog()
    ctx.raw_log = log

    async def run() -> None:
        wire = await ctx.seal_command(0x00, 0x81, b"\x00" + b"A" * 8)
        await ctx.send_raw(wire)
        await ctx.send_raw(wire)

    asyncio.run(run())
    h2d = [r for r in log.records if r["dir"] == "h2d"]
    check(len(h2d) == 2, "both writes were logged")
    check(h2d[0]["hex"] == h2d[1]["hex"], "the log shows the two frames were identical")
    check(all(r["t_ms"] >= 0 for r in h2d), "timestamps are monotonic from the run's start")


def test_evidence_redacts_secrets() -> None:
    """A JSON pasted into a ticket is exactly how a key escapes."""
    record = cli._redact({
        "master_key": "00112233445566778899aabbccddeeff",
        "address": "AA:BB:CC:DD:EE:FF",
        "nested": {"session_nonce": "deadbeef", "harmless": 1},
        "phases": "kept",
    })
    check(record["master_key"] == "<redacted>", "a key is redacted")
    check(record["address"] == "<redacted>", "an address is redacted")
    check(record["nested"]["session_nonce"] == "<redacted>", "redaction reaches nested objects")
    check(record["nested"]["harmless"] == 1, "ordinary fields survive")
    check(record["phases"] == "kept", "the result itself survives")


# ------------------------------------------------------- the production workflows, end to end ---

SESSION_KEY = bytes(range(16))


def make_device_ctx() -> tuple[cli._BleCtx, FakeDevice]:
    """A context wired to a peer that answers like the firmware, with a live session."""
    session = cli.BleSession(master_key=bytes(range(16)))
    session.session_key = SESSION_KEY
    session.session_id = bytes(range(16, 24))
    session.counter = 0
    session.authenticated = True
    dev = FakeDevice(cli, SESSION_KEY)
    ctx = cli._BleCtx(dev, session)
    ctx.notify_enabled = True
    dev.subscribed = True
    dev._notify = lambda b: ctx._on_notify(None, bytearray(b))
    return ctx, dev


def gate_args(**over: object) -> argparse.Namespace:
    base = dict(phase="all", total_size=192, window=4, max_frame=244, chunk_bytes=64,
                settle=0.0, observe=0.0, withhold=0.0, timeout=1.0, frame_gap=0.0,
                refresh_wait=0.0, device_log=None, target=None, firmware_sha=None, output=None)
    base.update(over)
    return argparse.Namespace(**base)


def run_replay(dev_setup=None, **over) -> dict:
    ctx, dev = make_device_ctx()
    if dev_setup:
        dev_setup(dev)
    log = cli._FrameLog()
    ctx.raw_log = log
    return asyncio.run(cli._bench_replay(ctx, log, gate_args(**over)))


def test_replay_workflow_passes_against_a_conforming_device() -> None:
    out = run_replay()
    check(out["start_acked"], "START was acked")
    check(out["first_frame_acked"], "the first DATA frame was acked")
    check(out["replay_drew_silence"], "the replay drew silence")
    check(out["control_answered_exactly"], "the control drew exactly 0081ff")
    check(out["end_acked"] and out["refresh_reported"], "the transfer completed through refresh")
    check(out["pass"], "a conforming device passes the replay phase")


def test_replay_fails_when_pipe_never_opened() -> None:
    """THE FINDING THAT MOTIVATED THE REWRITE. A replayed nonce is dropped by the session gate
    before PIPE state matters, so a transfer that never opened is silent too -- and the phase used
    to pass on that silence alone."""
    out = run_replay(lambda d: setattr(d, "start_nacks", True))
    check(not out["pass"], "a NACKed START cannot pass the replay phase")
    check(out.get("stopped_at") == "start-not-acked", "it stops at the START, and says so")


def test_replay_fails_when_first_frame_refused() -> None:
    out = run_replay(lambda d: setattr(d, "data_nacks", True))
    check(not out["pass"], "a refused first DATA frame cannot pass")
    check(out.get("stopped_at") == "first-data-not-acked", "it stops there, and says so")


def test_replay_fails_when_the_device_answers_a_replay() -> None:
    """The defect the row exists to catch, seen from the tool's side."""
    out = run_replay(lambda d: setattr(d, "answer_replays", True))
    check(not out["replay_drew_silence"], "an answered replay is observed")
    check(not out["pass"], "and it fails the phase")


def test_replay_fails_when_the_transfer_dies() -> None:
    """Silence is the required behaviour only if the upload then completes. A device that went
    quiet because the transfer died satisfies the silence and control steps."""
    out = run_replay(lambda d: setattr(d, "refresh_status", None))
    check(out["replay_drew_silence"], "the replay was still silent")
    check(out["control_answered_exactly"], "the control still answered")
    check(not out["refresh_reported"], "but no refresh was reported")
    check(not out["pass"], "so the phase fails -- silence without survival is not a pass")


def test_replay_control_requires_the_exact_nack() -> None:
    """Accepting any notification would let a late SACK stand in for the control, and then the
    silence assertion in the step before is unfalsifiable."""
    out = run_replay()
    hexes = [r["hex"] for r in out["control_replies"]]
    check("0081ff" in hexes, f"the control frame is matched byte-for-byte, got {hexes}")

    # A device that answers the bad tag with SOMETHING ELSE must not satisfy the control. Without
    # this case, "any notification" and "exactly 0081ff" are indistinguishable -- the conforming
    # peer sends only the right frame, so the weaker condition passes too.
    wrong = run_replay(lambda d: setattr(d, "bad_tag_answer",
                                         bytes([0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00])))
    check(len(wrong["control_replies"]) > 0, "the wrong answer did arrive")
    check(not wrong["control_answered_exactly"], "but it does not satisfy the control")
    check(not wrong["pass"], "so the phase fails")


def run_withhold(dev_setup=None, **over) -> dict:
    ctx, dev = make_device_ctx()
    if dev_setup:
        dev_setup(dev)
    log = cli._FrameLog()
    ctx.raw_log = log
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as fh:
        path = fh.name

    # The device's log grows as it dispatches, exactly as an operator's RTT capture would during a
    # run. Flushing it on every write keeps the file current when the phase reads it.
    original = dev.write_gatt_char

    async def logging_write(uuid: str, data: bytes) -> None:
        await original(uuid, data)
        dev.write_log(path)

    dev.write_gatt_char = logging_write
    return asyncio.run(cli._bench_withhold(ctx, log, gate_args(device_log=path, **over)))


def test_withhold_workflow_passes_against_a_conforming_device() -> None:
    out = run_withhold()
    check(out.get("frames_during_withhold") == 0, "nothing arrived while unsubscribed")
    check(out.get("canary", {}).get("found"), "the canary line was found in the device log")
    check(out.get("bytes_match_baseline"), "the withheld read matches the baseline byte for byte")
    check(out.get("pass"), "a conforming device passes the withhold phase")


def test_withhold_fails_without_the_device_log() -> None:
    """frames_during_withhold == 0 is true by construction -- an unsubscribed host cannot receive
    anything -- so without the log the phase would pass having proved nothing."""
    ctx, dev = make_device_ctx()
    log = cli._FrameLog()
    ctx.raw_log = log
    out = asyncio.run(cli._bench_withhold(ctx, log, gate_args(device_log=None)))
    check(not out["pass"], "no device log means no pass")
    check(not out.get("canary", {}).get("found"), "and the canary is reported as unfound")


def test_withhold_fails_when_the_device_defers_processing() -> None:
    """The case the old assertion could not see: a device that accepts the writes but processes
    nothing until re-enable never enters the RETRY arm, so nothing about backpressure was shown."""
    out = run_withhold(lambda d: setattr(d, "dispatch_while_unsubscribed", False))
    check(not out.get("canary", {}).get("found"),
          "a deferred dispatch produces no canary line before re-enable")
    check(not out.get("pass"), "so the phase fails")


def test_withhold_fails_when_queued_frames_are_dropped() -> None:
    out = run_withhold(lambda d: setattr(d, "drop_while_unsubscribed", True))
    check(not out.get("pass"), "dropped frames fail the phase")


def main() -> int:
    test_seal_once_send_twice()
    test_two_seals_would_differ()
    test_withhold_writes_precede_resubscribe()
    test_set_notify_is_idempotent()
    test_raw_log_records_before_decryption()
    test_evidence_redacts_secrets()
    test_replay_workflow_passes_against_a_conforming_device()
    test_replay_fails_when_pipe_never_opened()
    test_replay_fails_when_first_frame_refused()
    test_replay_fails_when_the_device_answers_a_replay()
    test_replay_fails_when_the_transfer_dies()
    test_replay_control_requires_the_exact_nack()
    test_withhold_workflow_passes_against_a_conforming_device()
    test_withhold_fails_without_the_device_log()
    test_withhold_fails_when_the_device_defers_processing()
    test_withhold_fails_when_queued_frames_are_dropped()
    print(f"bench_dispatch_gate: {CHECKS} checks, {len(FAILURES)} failures")
    return 0 if not FAILURES else 1


if __name__ == "__main__":
    raise SystemExit(main())
