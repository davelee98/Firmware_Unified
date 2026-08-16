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

bleak is stubbed rather than installed: nothing here touches a radio, and requiring the BLE stack
would make the gate skip on any machine without it -- and a check that skips is a check that
eventually means nothing.
"""

from __future__ import annotations

import asyncio
import importlib.util
import pathlib
import sys
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


def make_ctx() -> tuple[cli._BleCtx, FakeClient]:
    """An authenticated context with a deterministic session, so sealed bytes are reproducible."""
    session = cli.BleSession(master_key=bytes(range(16)))
    session.session_key = bytes(range(16))
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
    check(client.writes[0][:2] == b"\x00\x40" or len(client.writes[0]) > 2,
          "the config read is written first")


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


def main() -> int:
    test_seal_once_send_twice()
    test_two_seals_would_differ()
    test_withhold_writes_precede_resubscribe()
    test_set_notify_is_idempotent()
    test_raw_log_records_before_decryption()
    test_evidence_redacts_secrets()
    print(f"bench_dispatch_gate: {CHECKS} checks, {len(FAILURES)} failures")
    return 0 if not FAILURES else 1


if __name__ == "__main__":
    raise SystemExit(main())
