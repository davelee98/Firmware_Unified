#!/usr/bin/env python3
"""vectors_tool.py -- validate tests/vectors/*.json, and generate the C table the corpus runner
replays.

TWO JOBS, ONE TOOL, because they must agree about the schema. A validator that accepts what the
generator cannot emit is a gate that passes and then fails the build; keeping the field list in one
file is what stops that.

    vectors_tool.py --check  <dir>              schema gate for one directory
    vectors_tool.py --audit  <dir>              schema gate PLUS corpus-wide integrity
    vectors_tool.py --emit   <dir> <out.inc>    generate the C table (implies --audit)

TWO LEVELS, because they answer different questions. `--check` asks whether each vector is
well-formed, which is what a fixture directory of deliberately malformed shapes needs. `--audit`
additionally asks whether THE corpus is intact -- the exact counts, and the pairings whose whole
purpose is that two vectors cannot drift apart. Applying corpus invariants to a two-vector fixture
would fail for reasons that say nothing about the fixture.

WHY GENERATION RATHER THAN A PARSER IN THE RUNNER. JSON is test data, not a product input. A
vendored C parser would add code, a licence and malformed-input behaviour to the test
infrastructure, and would make the runner partly a test OF that parser; a hand-rolled one would put
a bespoke parser's correctness in front of every corpus failure. The generated table is never
committed -- a checked-in copy is the second corpus TEST_OWNERSHIP.md exists to forbid, and it
permits a stale green.

STANDARD LIBRARY ONLY. The host suite's whole point is that it needs no toolchain beyond a C
compiler and CMake; a pip dependency in the gate would undo that.
"""

from __future__ import annotations

import json
import pathlib
import sys

SCHEMA = "opendisplay-wire-vectors/1"

# Capability predicates a vector may name in `requires` / `forbids`. A closed set on purpose: an
# unknown name is a typo that would otherwise silently make a vector unreachable, which reads as
# coverage. Adding one here is a deliberate act.
CAPABILITIES = {
    "cap_partial",       # 0x76 partial-region support compiled in / panel is 1bpp
    "cap_buzzer",        # 0x77 buzzer support compiled in
    "cap_power_latch",   # D-FF power-latch hardware present (0x52 POWER_OFF)
    "cap_pipe",          # 0x80-0x82 sliding-window PIPE compiled in
    "cap_nfc",           # 0x83 NFC endpoint implemented
}

# Which transport a frame arrives on. NOT cosmetic: shared dispatch applies a 244-byte ceiling to
# BLE and 4094 to LAN, and exempts LAN-TLS from the session gate entirely
# (shared/core/od_dispatch.c). A vector that does not say cannot be replayed deterministically.
ORIGINS = {"ble", "lan-plain", "lan-tls"}

# What the runner is entitled to CLAIM from a passing vector.
#   shared             -- the reply comes from shared dispatch itself (gate, refusal, unknown)
#   target-production  -- the reply comes from a target's production command code
#   historical-fixture -- the reply is a shape current production does not emit; a behaviour
#                         fixture reproduces it, and it may never be counted as target coverage
PROOFS = {"shared", "target-production", "historical-fixture"}

# How the bytes on each side came to be. See the C12 plan section 2.4: the corpus is NOT uniformly
# authored, whatever FOLLOWUPS.md said -- several vectors copy real captures whose provenance was
# never recorded, and pretending otherwise would let unattributed bytes pass as a regression
# baseline.
PROVENANCE = {"authored", "captured", "captured-unattributed"}

# What a `captured` side must carry to count as a provenance-complete regression baseline
# (docs/TEST_OWNERSHIP.md, "Capture plan"). Prefixed per side: frame_target, reply_target, ...
CAPTURE_FIELDS = ("target", "firmware_sha", "protocol_version", "panel", "host_version",
                  "transport", "date")
PROVENANCE_KEYS = ("frame", "reply", "limitation")

# Semantic knobs a vector may set before its steps run. Deliberately a closed set: an unrecognised
# key is a vector asking for a precondition no runner establishes, which would otherwise pass while
# asserting less than it claims. The chunk_* group belongs to the config corpus, which this tool
# validates but does not replay.
STATE_KEYS = {
    "sec_enabled", "session", "xfer_active", "storage_ok", "fw_patch_byte", "fw_sha",
    "chunk_active", "chunk_count", "chunk_expected", "chunk_received", "chunk_total",
}


class Bad(Exception):
    """A schema violation, reported with the vector id already attached."""


def _hex(field: str, value: str) -> bytes:
    if not isinstance(value, str):
        raise Bad(f"{field}: expected a hex string, got {type(value).__name__}")
    if value != value.lower():
        raise Bad(f"{field}: hex must be lowercase")
    if len(value) % 2:
        raise Bad(f"{field}: odd-length hex ({len(value)} chars)")
    try:
        return bytes.fromhex(value)
    except ValueError as exc:
        raise Bad(f"{field}: not hex ({exc})") from None


def _steps(vec: dict) -> list[dict]:
    """Normalise a vector to a step list. A legacy {dir,frame,expect} IS one step; `steps` is the
    long form. Mixing them in one vector is ambiguous rather than convenient, so it is refused."""
    has_legacy = "frame" in vec or "expect" in vec
    if "steps" in vec:
        if has_legacy:
            raise Bad("has both `steps` and a top-level frame/expect -- pick one")
        steps = vec["steps"]
        if not isinstance(steps, list) or not steps:
            raise Bad("`steps` must be a non-empty array")
        return steps
    if not has_legacy:
        raise Bad("no `frame`/`expect` and no `steps`")
    return [{k: vec[k] for k in ("dir", "frame", "expect", "origin") if k in vec}]


def _replies(expect: dict, where: str) -> list[bytes] | None:
    """None means SILENCE -- the device emits nothing at all, which is an assertion in its own
    right and the reason `expect.reply: null` is not the same as an absent key."""
    if "reply" in expect and "replies" in expect:
        raise Bad(f"{where}: has both `reply` and `replies`")
    if "replies" in expect:
        seq = expect["replies"]
        if not isinstance(seq, list) or not seq:
            raise Bad(f"{where}: `replies` must be a non-empty array; use `reply: null` for silence")
        return [_hex(f"{where}.replies[{i}]", r) for i, r in enumerate(seq)]
    parsed = expect.get("parsed")
    if parsed is not None:
        # Validated here, EXECUTED later: od_dispatch_frame() exposes an outcome and replies, not a
        # parsed struct od_config. Checking the shape now is what stops the field rotting before an
        # adapter exists to run it.
        if not isinstance(parsed, dict) or not parsed:
            raise Bad(f"{where}: `expect.parsed` must be a non-empty object")
        for path, want in parsed.items():
            if not isinstance(path, str) or not path:
                raise Bad(f"{where}: `expect.parsed` key must be a dotted path")
            if any(seg == "" for seg in path.split(".")):
                raise Bad(f"{where}: `expect.parsed` path {path!r} has an empty segment")
            if not isinstance(want, (str, int, float, bool)) and want is not None:
                raise Bad(f"{where}: `expect.parsed[{path}]` must be a JSON scalar")
    if "reply" not in expect:
        raise Bad(f"{where}: no `reply` or `replies`")
    if expect["reply"] is None:
        return None
    return [_hex(f"{where}.reply", expect["reply"])]


def check_file(path: pathlib.Path) -> list[dict]:
    doc = json.loads(path.read_text())
    meta = doc.get("_meta", {})
    if meta.get("schema") != SCHEMA:
        raise Bad(f"{path.name}: _meta.schema must be {SCHEMA!r}, got {meta.get('schema')!r}")
    vectors = doc.get("vectors")
    if not isinstance(vectors, list) or not vectors:
        raise Bad(f"{path.name}: `vectors` must be a non-empty array")

    out: list[dict] = []
    for raw in vectors:
        vid = raw.get("id")
        if not isinstance(vid, str) or not vid:
            raise Bad(f"{path.name}: a vector has no id")
        try:
            out.append(_check_vector(raw))
        except Bad as exc:
            raise Bad(f"{vid}: {exc}") from None
    return out


def _check_vector(raw: dict) -> dict:
    req = raw.get("requires", [])
    forb = raw.get("forbids", [])
    for name, seq in (("requires", req), ("forbids", forb)):
        if not isinstance(seq, list):
            raise Bad(f"`{name}` must be an array")
        if len(set(seq)) != len(seq):
            raise Bad(f"`{name}` has a duplicate")
        for cap in seq:
            if cap not in CAPABILITIES:
                raise Bad(f"`{name}` names an unknown capability {cap!r}")
    both = set(req) & set(forb)
    if both:
        raise Bad(f"{sorted(both)} appears in both `requires` and `forbids`")

    proof = raw.get("proof")
    if proof not in PROOFS:
        raise Bad(f"`proof` must be one of {sorted(PROOFS)}, got {proof!r}")

    prov = raw.get("provenance", {})
    if not isinstance(prov, dict):
        raise Bad("`provenance` must be an object")
    known = set(PROVENANCE_KEYS)
    for side in ("frame", "reply"):
        known |= {f"{side}_source"} | {f"{side}_{f}" for f in CAPTURE_FIELDS}
    unknown = set(prov) - known
    if unknown:
        # A typo in a provenance key is a claim nobody checks. Rejecting the key set is what makes
        # `captured` mean the same thing in every vector.
        raise Bad(f"`provenance` has unknown field(s) {sorted(unknown)}")
    for side in ("frame", "reply"):
        kind = prov.get(side)
        if kind is None:
            continue
        if kind not in PROVENANCE:
            raise Bad(f"`provenance.{side}` must be one of {sorted(PROVENANCE)}, got {kind!r}")
        if kind == "captured":
            # TEST_OWNERSHIP.md's capture plan lists what a capture must carry to be a regression
            # baseline. A `captured` label without them is an unattributed capture wearing a better
            # name, which is precisely the confusion this field exists to remove.
            missing = [f for f in CAPTURE_FIELDS if not prov.get(f"{side}_{f}")]
            if missing:
                raise Bad(f"`provenance.{side}` is captured but lacks {missing}")
        if kind == "captured-unattributed":
            # The whole point of this kind is that it names what it cannot attribute.
            if not prov.get(f"{side}_source"):
                raise Bad(f"`provenance.{side}` is captured-unattributed but has no {side}_source")
            if not prov.get("limitation"):
                raise Bad(f"`provenance.{side}` is captured-unattributed but has no limitation")

    for key in raw.get("state", {}):
        if key not in STATE_KEYS:
            raise Bad(f"unknown state key {key!r} (capabilities belong in requires/forbids)")

    steps = _steps(raw)
    norm = []
    for i, step in enumerate(steps):
        where = f"step[{i}]"
        direction = step.get("dir", raw.get("dir"))
        if direction not in ("h2d", "d2h"):
            raise Bad(f"{where}: `dir` must be h2d or d2h, got {direction!r}")
        frame = _hex(f"{where}.frame", step["frame"]) if "frame" in step else None
        if frame is None:
            raise Bad(f"{where}: no `frame`")
        origin = step.get("origin", raw.get("origin"))
        if direction == "h2d":
            # A d2h observation is not dispatched, so it needs no origin; an h2d one always does.
            if origin not in ORIGINS:
                raise Bad(f"{where}: h2d needs an `origin` in {sorted(ORIGINS)}, got {origin!r}")
            if len(frame) < 2:
                raise Bad(f"{where}: an h2d frame needs at least the two opcode bytes")
        elif origin is not None and origin not in ORIGINS:
            raise Bad(f"{where}: unknown origin {origin!r}")
        if direction == "h2d" and "expect" not in step:
            # Silence is `expect.reply: null` and must be written down. Inferring it from an absent
            # key turns a vector somebody forgot to finish into an assertion that the device says
            # nothing -- which is a real, load-bearing property, and not one to acquire by accident.
            raise Bad(f"{where}: an h2d step needs an `expect` (use `reply: null` for silence)")
        replies = _replies(step["expect"], where) if "expect" in step else None
        if direction == "d2h" and replies:
            raise Bad(f"{where}: a d2h observation cannot expect a reply")
        norm.append({"dir": direction, "frame": frame, "origin": origin, "replies": replies})

    return {
        "id": raw["id"],
        "requires": sorted(req),
        "forbids": sorted(forb),
        "proof": proof,
        "state": raw.get("state", {}),
        "steps": norm,
    }


# ------------------------------------------------------------------------------ generation ---

def _c_bytes(b: bytes) -> str:
    return "{" + ", ".join(f"0x{x:02x}" for x in b) + "}" if b else "{0}"


# Vectors that must carry byte-identical replies. dispatch.json states in its own note that the
# generator asserts this; before C12.3 it did not, which made the note a promise of protection the
# reader did not have. The two firmware-version vectors record the same answer from opposite
# directions -- one h2d, one d2h -- and if they can drift the stale one becomes a false witness.
PAIRED_REPLIES = (
    ("dispatch/firmware-version-current-with-patch",
     "dispatch/firmware-version-response-with-patch"),
)


def check_pairings(vectors: list[dict]) -> None:
    by_id = {v["id"]: v for v in vectors}
    for a, b in PAIRED_REPLIES:
        if a not in by_id or b not in by_id:
            raise Bad(f"paired vectors {a} / {b}: one is missing, so the pairing is unchecked")
        # The h2d side's reply is compared with the d2h side's FRAME: a d2h vector is the
        # observation itself, so its bytes live in `frame`.
        ra = by_id[a]["steps"][0]["replies"]
        rb = by_id[b]["steps"][0]["frame"]
        if not ra or len(ra) != 1 or ra[0] != rb:
            raise Bad(f"{a} and {b} must carry identical bytes: "
                      f"{(ra[0].hex() if ra else None)} != {rb.hex()}")


def emit(vectors: list[dict], out: pathlib.Path) -> None:
    """One .inc, included by exactly one translation unit. Anything a fixture could reach is
    deliberately absent: the fakes must not be able to see an expected reply, or the corpus becomes
    its own oracle."""
    L: list[str] = []
    L.append("/* GENERATED by tests/host/vectors_tool.py -- do not edit, do not commit.")
    L.append(" * Regenerated whenever any vector file under tests/vectors changes. */")
    L.append("")
    for vi, v in enumerate(vectors):
        for si, s in enumerate(v["steps"]):
            L.append(f"static const uint8_t k_frame_{vi}_{si}[] = {_c_bytes(s['frame'])};")
            if s["replies"]:
                for ri, r in enumerate(s["replies"]):
                    L.append(f"static const uint8_t k_reply_{vi}_{si}_{ri}[] = {_c_bytes(r)};")
                items = ", ".join(
                    f"{{k_reply_{vi}_{si}_{ri}, sizeof k_reply_{vi}_{si}_{ri}}}"
                    for ri in range(len(s["replies"])))
                L.append(f"static const od_vec_reply_t k_replies_{vi}_{si}[] = {{{items}}};")
    L.append("")
    L.append("static const od_vec_step_t k_steps[] = {")
    step_base: list[int] = []
    n = 0
    for vi, v in enumerate(vectors):
        step_base.append(n)
        for si, s in enumerate(v["steps"]):
            replies = f"k_replies_{vi}_{si}" if s["replies"] else "NULL"
            nrep = len(s["replies"]) if s["replies"] else 0
            silent = "1" if s["replies"] is None else "0"
            origin = {"ble": "OD_ORIGIN_BLE", "lan-plain": "OD_ORIGIN_LAN_PLAIN",
                      "lan-tls": "OD_ORIGIN_LAN_TLS", None: "OD_ORIGIN_BLE"}[s["origin"]]
            d2h = "1" if s["dir"] == "d2h" else "0"
            L.append(f"    {{k_frame_{vi}_{si}, sizeof k_frame_{vi}_{si}, {replies}, {nrep}, "
                     f"{silent}, {origin}, {d2h}}},")
            n += 1
    L.append("};")
    L.append("")
    L.append("static const od_vec_t k_vectors[] = {")
    for vi, v in enumerate(vectors):
        st = v["state"]
        sess = "1" if st.get("session") == "live" else "0"
        caps_req = " | ".join(f"OD_VEC_{c.upper()}" for c in v["requires"]) or "0u"
        caps_forb = " | ".join(f"OD_VEC_{c.upper()}" for c in v["forbids"]) or "0u"
        L.append(
            f'    {{"{v["id"]}", &k_steps[{step_base[vi]}], {len(v["steps"])}, '
            f'OD_PROOF_{v["proof"].upper().replace("-", "_")}, {caps_req}, {caps_forb}, '
            f'{int(bool(st.get("sec_enabled")))}, {sess}, '
            f'{int(bool(st.get("xfer_active")))}, {int(st.get("storage_ok", True))}, '
            f'{int(bool(st.get("fw_patch_byte", True)))}, '
            f'"{st.get("fw_sha", "")}"}},')
    L.append("};")
    L.append("")
    out.write_text("\n".join(L) + "\n")


def main(argv: list[str]) -> int:
    if len(argv) < 3 or argv[1] not in ("--check", "--audit", "--emit"):
        print(__doc__, file=sys.stderr)
        return 64
    root = pathlib.Path(argv[2])
    files = sorted(root.glob("*.json"))
    if not files:
        print(f"no vector files under {root}", file=sys.stderr)
        return 1
    every: list[dict] = []
    for f in files:
        try:
            every.extend(check_file(f))
        except Bad as exc:
            print(f"{f}: {exc}", file=sys.stderr)
            return 1
        except json.JSONDecodeError as exc:
            print(f"{f}: malformed JSON: {exc}", file=sys.stderr)
            return 1
    ids = [v["id"] for v in every]
    dup = {i for i in ids if ids.count(i) > 1}
    if dup:
        print(f"duplicate vector id(s): {sorted(dup)}", file=sys.stderr)
        return 1

    if argv[1] == "--check":
        return 0

    try:
        check_pairings([v for v in every if v["id"].startswith("dispatch/")])
    except Bad as exc:
        print(f"pairing: {exc}", file=sys.stderr)
        return 1

    # EXACT COUNTS, not a floor. The runner already fails on zero, but zero is not the failure that
    # happens: a vector quietly dropped by a bad glob, or added without a decision about its proof
    # class, leaves a green run with less coverage than the last one. Update these deliberately.
    dispatch = [v for v in every if v["id"].startswith("dispatch/")]
    h2d = [v for v in dispatch if any(s["dir"] == "h2d" for s in v["steps"])]
    d2h_only = [v for v in dispatch if all(s["dir"] == "d2h" for s in v["steps"])]
    for got, want, what in ((len(every), 24, "total vectors"),
                            (len(dispatch), 17, "dispatch vectors"),
                            (len(h2d), 14, "h2d dispatch vectors"),
                            (len(d2h_only), 3, "d2h-only dispatch vectors")):
        if got != want:
            print(f"corpus accounting: {what} is {got}, expected {want}. If this change is "
                  f"intended, update EXPECTED_COUNTS in {__file__}.", file=sys.stderr)
            return 1

    if argv[1] == "--audit":
        return 0
    if len(argv) < 4:
        print("--emit needs an output path", file=sys.stderr)
        return 64
    # Only dispatch vectors are replayed; config vectors are a different runner's subject and are
    # validated here but not emitted. Saying so in the tool beats a silent filter.
    emit([v for v in every if v["id"].startswith("dispatch/")], pathlib.Path(argv[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
