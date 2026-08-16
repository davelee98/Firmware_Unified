"""fake_device.py -- a scripted BLE peer that behaves like the firmware under test.

WHY A DEVICE MODEL RATHER THAN A FAKE CLIENT. The first version of the bench tests drove the
low-level helpers -- seal, send-raw, CCCD -- and reproduced by hand the sequence the real workflow
was supposed to perform. That proved the helpers and nothing about `_bench_replay()` or
`_bench_withhold()`, so a workflow rewritten to seal twice, or to return an unconditional pass,
went undetected. A review found exactly that.

This peer answers writes the way the firmware does, so the workflows can be run end to end and
their PASS logic probed:

  * it holds the session key and VERIFIES the CCM tag, so a corrupted frame is refused for the same
    reason the device refuses it -- [00][cmd][FF] from the shared gate;
  * it tracks nonces, so a byte-identical resend is a replay and draws silence, while a re-sealed
    frame carries a fresh nonce and is answered normally. That distinction is the whole OD-S1 row,
    and a peer that ignored it would let the mistake pass;
  * it QUEUES notifications while unsubscribed and delivers them on re-enable, which is the
    behaviour the backpressure row is looking for -- and it can be told to drop them instead, so the
    failing case is reachable too.

It is a model, not the firmware. It cannot prove radio timing, and a green run here says the tool
drives the right stimulus -- never that a device passed.
"""

from __future__ import annotations

import asyncio
from typing import Any, Callable


class FakeDevice:
    """A BLE peer with just enough protocol to exercise the bench workflows."""

    def __init__(self, cli: Any, session_key: bytes) -> None:
        self.cli = cli
        self.session_key = session_key
        self.subscribed = False
        self.events: list[tuple[str, object]] = []
        self.seen_nonces: set[bytes] = set()
        self.log_lines: list[str] = []

        # --- knobs, so the failing cases are reachable -------------------------------------
        self.start_nacks = False          # PIPE START refuses (e.g. a size mismatch)
        self.data_nacks = False           # the first DATA frame refuses
        self.answer_replays = False       # the defect OD-S1 exists to detect
        self.drop_while_unsubscribed = False   # queued frames are lost rather than held
        self.dispatch_while_unsubscribed = True  # the device processes writes even when it cannot send
        self.refresh_status = 0x73        # 0x73 complete, 0x74 timed out, None to send neither
        # What a bad tag draws. The device answers [00][cmd][FF]; setting anything else models a
        # device -- or a race -- that puts SOMETHING in the control window without it being the
        # refusal. The control must reject that, or silence in the step before is unfalsifiable.
        self.bad_tag_answer: bytes | None = None
        self.config_blob = self._valid_packet(300)

        self._queued: list[bytes] = []
        self._notify: Callable[[bytes], None] | None = None
        self._pipe_open = False
        self.session_id = bytes(range(16, 24))
        self._tx_counter = 0

    def _valid_packet(self, size: int) -> bytes:
        """A well-formed config packet: [2B LE length][body][2B LE CRC]. The withhold row compares
        the withheld read against a baseline read byte for byte, and both go through the CLI's own
        validator -- so a blob that fails validation would fail the phase for the wrong reason."""
        body = bytearray(size - 2)
        body[0] = size & 0xFF
        body[1] = (size >> 8) & 0xFF
        body[2] = 0x01                                   # version
        for i in range(3, len(body)):
            body[i] = (i * 7) & 0xFF
        crc = self.cli.outer_packet_crc(bytes(body))
        return bytes(body) + bytes([crc & 0xFF, (crc >> 8) & 0xFF])

    # ---------------------------------------------------------------- the BLE client surface ---

    async def write_gatt_char(self, _uuid: str, data: bytes) -> None:
        self.events.append(("write", bytes(data)))
        if not self.subscribed and not self.dispatch_while_unsubscribed:
            # A device that defers processing entirely. The withhold row must NOT pass against
            # this: nothing entered the RETRY arm, so nothing was proven about backpressure.
            self._queued_writes_deferred = True
            self._deferred = getattr(self, "_deferred", [])
            self._deferred.append(bytes(data))
            return
        for reply in self._handle(bytes(data)):
            self._emit(reply if self._is_plaintext(reply) else self._seal(reply))

    async def start_notify(self, _uuid: str, cb: Callable[[Any, bytearray], None]) -> None:
        self.subscribed = True
        self.events.append(("subscribe", None))
        self._notify = lambda b: cb(None, bytearray(b))
        for pending in getattr(self, "_deferred", []):
            for reply in self._handle(pending):
                self._emit(reply if self._is_plaintext(reply) else self._seal(reply))
        self._deferred = []
        queued, self._queued = self._queued, []
        for frame in queued:
            self._notify(frame)

    async def stop_notify(self, _uuid: str) -> None:
        self.subscribed = False
        self.events.append(("unsubscribe", None))

    # ------------------------------------------------------------------------------ internals ---

    def _seal(self, frame: bytes) -> bytes:
        """Seal an application response, as od_cmd_reply() does.

        WHICH FRAMES ARE SEALED IS THE PROTOCOL, not a detail: application responses are protected
        and control/error frames are plaintext, chosen at the call site. A fake that sent everything
        in the clear would have its config chunks silently discarded by the host's own decrypt path,
        which is how this model first failed."""
        wire, self._tx_counter = self.cli.encrypt_command(
            self.session_key, self.session_id, self._tx_counter, frame[0], frame[1], frame[2:])
        return wire

    def _emit(self, frame: bytes) -> None:
        if self.subscribed and self._notify:
            self._notify(frame)
        elif not self.drop_while_unsubscribed:
            self._queued.append(frame)      # od_txq holds it: RETRY, not a drop

    def _handle(self, wire: bytes) -> list[bytes]:
        if len(wire) < 2:
            return []
        cmd_hi, cmd_lo = wire[0], wire[1]

        # An encrypted frame is [cmd:2][nonce:16][ct][tag:12]; anything shorter is plaintext.
        if len(wire) >= 2 + 16 + 1 + 12:
            nonce = wire[2:18]
            try:
                self.cli.decrypt_response(self.session_key, wire)
            except Exception:
                # A tag failure is tamper evidence and keeps its NACK (od_gate.c).
                if self.bad_tag_answer is not None:
                    return [self.bad_tag_answer]
                return [bytes([0x00, cmd_lo, 0xFF])]
            if nonce in self.seen_nonces:
                self.log_lines.append(f"decrypt failed (0x{cmd_hi:02x}{cmd_lo:02x}, nonce_reason=2)")
                if not self.answer_replays:
                    return []               # OD-S1: a replayed PIPE DATA frame draws silence
                return [bytes([0x00, cmd_lo, 0xFF])]
            self.seen_nonces.add(nonce)
            return self._dispatch(cmd_lo)
        return self._dispatch(cmd_lo)

    # A hard NACK and the gate's [00][cmd][FF] never go sealed: a client whose session just failed
    # cannot read a protected frame. Everything else is an application response and is.
    @staticmethod
    def _is_plaintext(frame: bytes) -> bool:
        return frame[0] == 0xFF or (len(frame) == 3 and frame[2] == 0xFF)

    def _dispatch(self, cmd_lo: int) -> list[bytes]:
        if cmd_lo == 0x80:
            if self.start_nacks:
                return [bytes([0xFF, 0x80, 0x01, 0x00])]
            self._pipe_open = True
            return [bytes([0x00, 0x80, 0x01, 0x20, 0x20, 0xF4, 0x00, 0x01])]
        if cmd_lo == 0x81:
            if self.data_nacks:
                return [bytes([0xFF, 0x81, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00])]
            return [bytes([0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00])]
        if cmd_lo == 0x82:
            out = [bytes([0x00, 0x82])]
            if self.refresh_status is not None:
                out.append(bytes([0x00, self.refresh_status]))
            self._pipe_open = False
            return out
        if cmd_lo == 0x40:
            return self._config_chunks()
        if cmd_lo == 0x60:
            # Unknown opcode: silent on the wire, and visible only in the device's own log. That
            # asymmetry is exactly why the withhold row needs a log file.
            self.log_lines.append("unknown cmd 0x0060")
            return []
        return []

    def _config_chunks(self) -> list[bytes]:
        blob = self.config_blob
        out: list[bytes] = []
        first = blob[:180]
        out.append(bytes([0x00, 0x40, 0x00, 0x00,
                          len(blob) & 0xFF, (len(blob) >> 8) & 0xFF]) + first)
        rest = blob[180:]
        n = 1
        while rest:
            part, rest = rest[:180], rest[180:]
            out.append(bytes([0x00, 0x40, n & 0xFF, (n >> 8) & 0xFF]) + part)
            n += 1
        return out

    def write_log(self, path: str) -> None:
        with open(path, "w") as fh:
            fh.write("\n".join(self.log_lines) + "\n")
