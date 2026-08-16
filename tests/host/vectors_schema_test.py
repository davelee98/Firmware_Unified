#!/usr/bin/env python3
"""vectors_schema_test.py -- positive and negative fixtures for the corpus schema.

WHY NEGATIVE FIXTURES ARE THE POINT. A validator is only worth its acceptance decisions, and a
suite that feeds it the real corpus proves it accepts what already exists -- which it must, or the
tree would be red. What it does not show is that anything is REJECTED. The C12 review found five
malformed shapes the validator accepted, and every one of them would have made a vector claim more
than it checked:

  * an h2d step with no `expect` became an assertion that the device says nothing;
  * `expect.parsed` was ignored entirely, so the highest-value config vector asserted nothing;
  * `captured` provenance needed no metadata, so an unattributed capture could wear the better name;
  * `captured-unattributed` needed no limitation string;
  * unknown provenance fields passed, so a typo was a claim nobody checked.

Each has a case below. They are built in memory rather than committed as broken JSON files, so a
future `--check` over tests/vectors cannot trip over them.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile

TOOL = pathlib.Path(__file__).resolve().parent / "vectors_tool.py"
CORPUS = pathlib.Path(__file__).resolve().parents[1] / "vectors"

FAILURES: list[str] = []
CHECKS = 0


def check(cond: bool, what: str) -> None:
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILURES.append(what)
        print(f"FAIL {what}")


def run_check(doc: dict) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as d:
        (pathlib.Path(d) / "t.json").write_text(json.dumps(doc))
        p = subprocess.run([sys.executable, str(TOOL), "--check", d],
                           capture_output=True, text=True)
        return p.returncode, (p.stdout + p.stderr)


def base_vector(**over) -> dict:
    v = {
        "id": "dispatch/fixture",
        "dir": "h2d",
        "origin": "ble",
        "frame": "0070",
        "proof": "shared",
        "expect": {"reply": "007000"},
        "provenance": {"frame": "authored", "reply": "authored"},
    }
    v.update(over)
    return v


def doc(*vectors: dict) -> dict:
    return {"_meta": {"schema": "opendisplay-wire-vectors/1"}, "vectors": list(vectors)}


def accepts(v: dict, what: str) -> None:
    rc, out = run_check(doc(v))
    check(rc == 0, f"{what} (rejected: {out.strip()[:120]})")


def rejects(v: dict, what: str) -> None:
    rc, _ = run_check(doc(v))
    check(rc != 0, f"REJECTS {what}")


# ----------------------------------------------------------------------------------- cases ---

def test_the_real_corpus_validates() -> None:
    p = subprocess.run([sys.executable, str(TOOL), "--check", str(CORPUS)],
                       capture_output=True, text=True)
    check(p.returncode == 0, f"the committed corpus validates ({p.stderr.strip()[:160]})")


def test_positive_shapes() -> None:
    accepts(base_vector(), "a minimal h2d vector")
    accepts(base_vector(expect={"reply": None}), "silence, written down as reply: null")
    accepts(base_vector(expect={"replies": ["0072", "0073"]}), "an ordered reply list")
    accepts(base_vector(forbids=["cap_buzzer"]), "a negative capability predicate")
    accepts(base_vector(requires=["cap_pipe"]), "a positive capability predicate")
    accepts({"id": "dispatch/seq", "proof": "shared",
             "provenance": {"frame": "authored", "reply": "authored"},
             "steps": [{"dir": "h2d", "origin": "ble", "frame": "0070",
                        "expect": {"reply": "0070"}},
                       {"dir": "h2d", "origin": "ble", "frame": "0072",
                        "expect": {"replies": ["0072", "0073"]}}]},
            "a multi-step vector")
    accepts(base_vector(expect={"reply": "007000",
                                "parsed": {"security.encryption_enabled": 1}}),
            "expect.parsed with a dotted path and a scalar")


def test_the_five_the_review_found() -> None:
    v = base_vector()
    del v["expect"]
    rejects(v, "an h2d step with no `expect` (silence must be written down)")

    rejects(base_vector(expect={"reply": "007000", "parsed": {}}),
            "an empty expect.parsed")
    rejects(base_vector(expect={"reply": "007000", "parsed": {"a..b": 1}}),
            "an expect.parsed path with an empty segment")
    rejects(base_vector(expect={"reply": "007000", "parsed": {"a.b": {"nested": 1}}}),
            "an expect.parsed value that is not a scalar")

    rejects(base_vector(provenance={"frame": "captured", "reply": "authored"}),
            "`captured` with no capture metadata")
    rejects(base_vector(provenance={"frame": "captured-unattributed",
                                    "frame_source": "x.bin", "reply": "authored"}),
            "`captured-unattributed` with no limitation")
    rejects(base_vector(provenance={"frame": "captured-unattributed",
                                    "limitation": "y", "reply": "authored"}),
            "`captured-unattributed` with no source")
    rejects(base_vector(provenance={"frame": "authored", "reply": "authored", "bogus": 1}),
            "an unknown provenance field")

    accepts(base_vector(provenance={
        "frame": "captured", "frame_target": "xiao_nrf52840", "frame_firmware_sha": "abc1234",
        "frame_protocol_version": "2.2", "frame_panel": "GDEY075", "frame_host_version": "7.14.0",
        "frame_transport": "ble", "frame_date": "2026-08-16", "reply": "authored"}),
        "`captured` WITH complete metadata")


def test_other_structural_rules() -> None:
    rejects(base_vector(frame="007"), "odd-length hex")
    rejects(base_vector(frame="00AA"), "uppercase hex")
    rejects(base_vector(origin="usb"), "an unknown origin")
    v = base_vector()
    del v["origin"]
    rejects(v, "an h2d step with no origin")
    rejects(base_vector(proof="wishful"), "an unknown proof class")
    v = base_vector()
    del v["proof"]
    rejects(v, "a vector with no proof class")
    rejects(base_vector(requires=["cap_pipe"], forbids=["cap_pipe"]),
            "the same capability required and forbidden")
    rejects(base_vector(forbids=["cap_nonsense"]), "an unknown capability name")
    rejects(base_vector(forbids=["cap_pipe", "cap_pipe"]), "a duplicated predicate")
    rejects(base_vector(expect={"reply": "0070", "replies": ["0070"]}),
            "both `reply` and `replies`")
    rejects(base_vector(expect={"replies": []}), "an empty `replies` array")
    rejects(base_vector(dir="d2h", expect={"reply": "0070"}),
            "a d2h observation that expects a reply")
    rejects({"id": "dispatch/x", "proof": "shared", "dir": "h2d", "origin": "ble",
             "frame": "0070", "expect": {"reply": None},
             "provenance": {"frame": "authored", "reply": "authored"},
             "steps": [{"dir": "h2d", "frame": "0070", "expect": {"reply": None}}]},
            "a vector with both `steps` and a top-level frame")
    rejects(base_vector(state={"nonsense": 1}), "an unknown state key")
    rejects(base_vector(frame="00"), "an h2d frame shorter than an opcode")

    rc, _ = run_check({"_meta": {"schema": "wrong/9"}, "vectors": [base_vector()]})
    check(rc != 0, "REJECTS a wrong schema id")
    rc, _ = run_check({"_meta": {"schema": "opendisplay-wire-vectors/1"}, "vectors": []})
    check(rc != 0, "REJECTS an empty vector list")

    a, b = base_vector(), base_vector()
    rc, _ = run_check(doc(a, b))
    check(rc != 0, "REJECTS a duplicate vector id")


def main() -> int:
    test_the_real_corpus_validates()
    test_positive_shapes()
    test_the_five_the_review_found()
    test_other_structural_rules()
    print(f"vectors_schema: {CHECKS} checks, {len(FAILURES)} failures")
    return 0 if not FAILURES else 1


if __name__ == "__main__":
    raise SystemExit(main())
