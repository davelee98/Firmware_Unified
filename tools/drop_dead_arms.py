#!/usr/bin/env python3
"""drop_dead_arms.py -- delete preprocessor branches guarded by a macro that is never defined.

A restricted unifdef: it resolves conditions that mention ONLY the named symbols and leaves every
other condition untouched. That restriction is the safety property -- a condition it cannot fully
evaluate is not guessed at, it is preserved verbatim, so the worst outcome is that a dead branch
survives rather than a live one being removed.

    drop_dead_arms.py --undef TARGET_NRF --define TARGET_ESP32 FILE...

`--undef X` deletes branches guarded by X. `--define X` marks X as always present, which makes the
#else after `#ifdef X` unreachable and removes it, while KEEPING the guard. `--collapse X` is
`--define X` plus removing the guard as well, splicing its body into the surrounding text: for a
symbol every build of the target defines, `#ifdef X` is an always-true conditional, and one of
those is worse than none -- it tells a reader there is another configuration when there is not.

Which of the two you want depends on whether the symbol still distinguishes anything. A compound
condition is NEITHER: `#if defined(X) && defined(Y)` is left alone even under --collapse, because
the answer still depends on Y. Simplifying it to `#if defined(Y)` is an expression rewrite rather
than a deletion, and this tool does not do those.

USED ONCE, and kept because the next target migration will want it. It is not part of any build.

WHAT IT DOES NOT DO. It does not evaluate `#if` arithmetic, macro VALUES, or nested expansion. It
understands `#ifdef X`, `#ifndef X`, and `#if`/`#elif` built only from `defined(X)` joined by `&&`,
`||`, `!` and parentheses. Anything else -- a comparison, a number, a bare identifier -- is UNKNOWN
and its whole chain is left alone.

VERIFY THE RESULT BY BUILDING AND COMPARING SECTION SIZES against the previous build. Removing code
that never compiled cannot move `text` or `data` by one byte, so any movement is the tool having
resolved something it should not have. That check is not optional: it is what caught this tool
deleting the arm that sizes the inflate window.
"""

from __future__ import annotations

import re
import sys

DIRECTIVE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")

# One arm of an #if chain: its directive keyword, the text after it, its body, and the directive
# line exactly as it was written.
Branch = tuple[str, str, list[str], str]



class Unknown:
    """A condition this tool declines to evaluate. Distinct from False on purpose."""

    def __bool__(self) -> bool:                      # pragma: no cover - guard against misuse
        raise TypeError("Unknown must be tested explicitly, never used as a truth value")


UNKNOWN = Unknown()


def evaluate(expr: str, undefined: set[str], defined: set[str] = frozenset()) -> bool | Unknown:
    """True/False when the expression is built ONLY from defined() over symbols we know about.

    THE GRAMMAR IS THE SAFETY PROPERTY, and it is narrow on purpose. An earlier version tokenized
    loosely and substituted 0 or 1 for every unresolved name, which is right for `defined(X)` and
    WRONG for a value: it read `#if OPENDISPLAY_ZLIB_WINDOW_BITS == 9` as false under both guesses,
    concluded the branch was dead, and deleted the arm that sizes the inflate window -- a wire
    contract with the host encoder, silently cut from 4096 to 512 bytes. Nothing but a
    section-size comparison against the previous build caught it.

    So: anything that is not defined()/!/&&/||/parentheses is UNKNOWN. No numbers, no comparisons,
    no bare identifiers, no arithmetic.
    """
    e = expr.strip()
    if not e:
        return UNKNOWN

    # Replace each defined() test with a slot index, then require what is left to be pure boolean
    # connective. A bare identifier surviving this pass means the condition reads a macro's VALUE.
    names: list[str] = []

    def slot(m: re.Match[str]) -> str:
        names.append(m.group(1) or m.group(2))
        return f" _{len(names) - 1} "

    body = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)", slot, e)
    if not re.fullmatch(r"[\s!&|()]*(?:_\d+[\s!&|()]*)*", body) or "defined" in body:
        return UNKNOWN

    unresolved = sorted({n for n in names if n not in undefined and n not in defined})
    if len(unresolved) > 8:
        return UNKNOWN                              # too wide to enumerate; do not guess

    # TRY EVERY ASSIGNMENT of the symbols we do not know. `defined(TARGET_NRF) && defined(X)` is
    # false whatever X is, and a conservative "all names must be known" rule would miss that -- it
    # is the commonest shape in this tree. If every assignment agrees, the answer is certain; if
    # they disagree, the condition genuinely depends on something else and is left alone.
    py = body.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    results = set()
    for bits in range(1 << len(unresolved)):
        env = {}
        for i, n in enumerate(names):
            if n in undefined:
                env[f"_{i}"] = 0
            elif n in defined:
                env[f"_{i}"] = 1
            else:
                env[f"_{i}"] = (bits >> unresolved.index(n)) & 1
        try:
            results.add(bool(eval(py, {"__builtins__": {}}, env)))   # noqa: S307
        except Exception:
            return UNKNOWN
        if len(results) > 1:
            return UNKNOWN
    return results.pop() if results else UNKNOWN


def condition_of(kind: str, rest: str, undefined: set[str], defined: set[str]) -> bool | Unknown:
    if kind == "ifdef":
        name = rest.strip().split()[0] if rest.strip() else ""
        if name in undefined:
            return False
        return True if name in defined else UNKNOWN
    if kind == "ifndef":
        name = rest.strip().split()[0] if rest.strip() else ""
        if name in undefined:
            return True
        return False if name in defined else UNKNOWN
    return evaluate(rest, undefined, defined)


def process(lines: list[str], undefined: set[str], defined: set[str] = frozenset(),
            collapse: set[str] = frozenset()) -> list[str]:
    out: list[str] = []
    i = 0
    while i < len(lines):
        m = DIRECTIVE.match(lines[i])
        if not m or m.group(1) not in ("ifdef", "ifndef", "if"):
            out.append(lines[i])
            i += 1
            continue
        chain, endif, end = collect(lines, i)
        out.extend(rewrite(chain, endif, undefined, defined, collapse))
        i = end
    return out


def collect(lines: list[str], start: int) -> tuple[list[Branch], str, int]:
    """Split one #if..#endif into [(kind, rest, body, line)], its closing #endif line, and the index
    just past it -- tracking nesting so an inner chain stays inside its parent's body and is
    processed recursively later.

    EVERY DIRECTIVE LINE COMES BACK VERBATIM and is re-emitted unchanged wherever a branch survives.
    Reconstructing them looks harmless and is not: it drops the indentation of a nested `#  if` and
    the trailing comment naming an include guard on its `#endif`, so a deletion arrives carrying
    reformatting nobody asked for. Only an #elif promoted to the head of its chain is rewritten,
    and even then it keeps its original indent."""
    chain: list[Branch] = []
    m = DIRECTIVE.match(lines[start])
    kind, rest, line = m.group(1), m.group(2), lines[start]
    body: list[str] = []
    depth = 0
    i = start + 1
    while i < len(lines):
        d = DIRECTIVE.match(lines[i])
        if d:
            k = d.group(1)
            if k in ("ifdef", "ifndef", "if"):
                depth += 1
            elif k == "endif":
                if depth == 0:
                    chain.append((kind, rest, body, line))
                    return chain, lines[i], i + 1
                depth -= 1
            elif k in ("elif", "else") and depth == 0:
                chain.append((kind, rest, body, line))
                kind, rest, body, line = k, d.group(2), [], lines[i]
                i += 1
                continue
        body.append(lines[i])
        i += 1
    chain.append((kind, rest, body, line))          # unterminated; caller's problem, not ours
    return chain, "#endif\n", i


def collapses(branch: Branch, collapse: set[str]) -> bool:
    """Is this branch guarded by nothing but a bare test of a collapse symbol?

    `#ifdef X` and `#if defined(X)` qualify. `#if defined(X) && defined(Y)` does NOT, however
    certainly X is defined: the surviving condition is `defined(Y)`, and producing it would be an
    expression rewrite. The guard stays and reads slightly redundantly, which is the cheaper error.
    """
    kind, rest, _, _ = branch
    if kind == "ifdef":
        name = rest.strip().split()[0] if rest.strip() else ""
        return name in collapse
    if kind in ("if", "elif"):
        m = re.fullmatch(r"\s*defined\s*\(\s*(\w+)\s*\)\s*|\s*defined\s+(\w+)\s*", rest)
        return bool(m) and (m.group(1) or m.group(2)) in collapse
    return False


def rewrite(chain: list[Branch], endif: str, undefined: set[str],
            defined: set[str] = frozenset(), collapse: set[str] = frozenset()) -> list[str]:
    conds = [condition_of(k, r, undefined, defined | collapse) if k != "else" else True
             for k, r, _, _ in chain]

    # A branch whose condition is definitely TRUE makes every later branch unreachable. Dropping
    # them removes the dead #else -- typically the other target's arm -- while LEAVING the guard
    # itself, which still says "this is target-specific" to whoever ports the next one. Collapsing
    # the guard as well would be a different and much larger change.
    for n, cond in enumerate(conds):
        if cond is True:
            chain, conds = chain[: n + 1], conds[: n + 1]
            break

    # Drop every definitely-false branch, wherever it sits. A false #elif in the middle of a chain
    # can never be taken, so removing it changes nothing -- and leaving it was the first version's
    # bug: it dropped only LEADING false branches, so `#ifdef TARGET_ESP32 ... #elif TARGET_NRF`
    # kept its dead tail because the head was merely unknown.
    kept = [c for c, cond in zip(chain, conds) if cond is not False]
    if not kept:
        return []                                    # every branch was false: the chain vanishes

    # A chain whose only survivor is its #else has no condition left to test. Under --collapse the
    # same is true of a lone survivor whose condition is a bare test of a collapse symbol: the
    # directives carry no information, so the body is spliced in and they go.
    if len(kept) == 1 and (kept[0][0] == "else"
                           or (conds[0] is True and collapses(kept[0], collapse))):
        return process(kept[0][2], undefined, defined, collapse)

    out: list[str] = []
    for n, (kind, rest, body, line) in enumerate(kept):
        if n == 0 and kind == "elif":
            # The only line this tool ever rewrites: an #elif promoted to the head of its chain has
            # to become an #if. Its indentation is part of the surrounding nesting, so it is kept.
            indent = line[: len(line) - len(line.lstrip())]
            out.append(f"{indent}#if{rest}".rstrip() + "\n")
        else:
            out.append(line)
        out.extend(process(body, undefined, defined, collapse))
    out.append(endif)
    return out


COLLAPSE_SELFTEST = [
    # (why, source, expected) -- run with --collapse TARGET_ESP32
    (
        "A bare always-true guard carries no information: body spliced, directives gone.",
        "before\n#ifdef TARGET_ESP32\nbody\n#endif\nafter\n",
        "before\nbody\nafter\n",
    ),
    (
        "The defined() spelling collapses the same way.",
        "#if defined(TARGET_ESP32)\nbody\n#endif\n",
        "body\n",
    ),
    (
        "A COMPOUND condition is left alone. The answer still depends on Y, and rewriting the "
        "expression to `#if defined(Y)` is not a deletion. This is the case that must not move.",
        "#if defined(TARGET_ESP32) && defined(Y)\na\n#else\nb\n#endif\n",
        "#if defined(TARGET_ESP32) && defined(Y)\na\n#else\nb\n#endif\n",
    ),
    (
        "A guard with a dead #else collapses to its body -- the #else is unreachable either way.",
        "#ifdef TARGET_ESP32\nesp\n#else\nother\n#endif\n",
        "esp\n",
    ),
    (
        "Nested guards collapse at every level, and an inner chain that stays UNKNOWN survives "
        "with its indentation and trailing comments untouched.",
        "#ifdef TARGET_ESP32\n#  if defined(Y)\na\n#  endif  // Y\n#endif  // TARGET_ESP32\n",
        "#  if defined(Y)\na\n#  endif  // Y\n",
    ),
    (
        "#ifndef of a collapse symbol is false, so its body goes and nothing is spliced.",
        "#ifndef TARGET_ESP32\ndead\n#endif\nkept\n",
        "kept\n",
    ),
]


SELFTEST = [
    # (source, expected) -- run with --undef TARGET_NRF --undef ARDUINO_ARCH_NRF52
    #                                --define TARGET_ESP32
    (
        "THE BUG THAT SHRANK A WIRE CONTRACT. An unresolved macro in an arithmetic comparison is "
        "not 0 and not 1, and guessing both is not a proof. This condition selects the inflate "
        "dictionary size, and reading it as false cut a 4096-byte window to 512.",
        "#if OPENDISPLAY_ZLIB_WINDOW_BITS == 9\n#  define D 4096u\n#else\n#  define D W\n#endif\n",
        "#if OPENDISPLAY_ZLIB_WINDOW_BITS == 9\n#  define D 4096u\n#else\n#  define D W\n#endif\n",
    ),
    (
        "A bare identifier reads a macro's VALUE, so the chain is left alone.",
        "#if MAX_LEN\nkeep\n#endif\n",
        "#if MAX_LEN\nkeep\n#endif\n",
    ),
    (
        "A dead #else after a guard that is definitely true. The guard SURVIVES -- it still says "
        "'target-specific' to whoever ports the next target.",
        "#ifdef TARGET_ESP32\nesp\n#else\nnrf\n#endif\n",
        "#ifdef TARGET_ESP32\nesp\n#endif\n",
    ),
    (
        "A false #elif in the MIDDLE of a chain, which an earlier version kept because it only "
        "dropped leading branches.",
        "#if defined(A)\na\n#elif defined(TARGET_NRF)\ndead\n#else\nz\n#endif\n",
        "#if defined(A)\na\n#else\nz\n#endif\n",
    ),
    (
        "An #elif promoted to the head of its chain -- the ONLY line this tool rewrites.",
        "#if defined(TARGET_NRF)\ndead\n#elif defined(TARGET_ESP32)\nlive\n#endif\n",
        "#if defined(TARGET_ESP32)\nlive\n#endif\n",
    ),
    (
        "Every arm false: the chain vanishes entirely.",
        "#ifdef TARGET_NRF\na\n#elif defined(ARDUINO_ARCH_NRF52)\nb\n#endif\n",
        "",
    ),
    (
        "Nested indentation and trailing comments are preserved verbatim. Reformatting is not "
        "this tool's job, and a deletion that also reflows directives cannot be reviewed.",
        "#ifdef TARGET_ESP32\n#  if defined(X)\na\n#  else\nb\n#  endif\n"
        "#else  // not ESP32\ndead\n#endif  // TARGET_ESP32\n",
        "#ifdef TARGET_ESP32\n#  if defined(X)\na\n#  else\nb\n#  endif\n#endif  // TARGET_ESP32\n",
    ),
    (
        "An include guard is an #ifndef over a symbol we know nothing about: untouched.",
        "#ifndef G_H\n#define G_H\nx\n#endif  // G_H\n",
        "#ifndef G_H\n#define G_H\nx\n#endif  // G_H\n",
    ),
    (
        "#ifndef of a symbol we are told IS defined is false, so its body goes.",
        "#ifndef TARGET_ESP32\n#include <Adafruit_TinyUSB.h>\n#endif\nafter\n",
        "after\n",
    ),
]


def selftest() -> int:
    """Cases this tool got WRONG at least once. Both failures produced a green build.

    A tool that edits source by resolving preprocessor conditions is only as good as the
    conditions it declines to resolve, and neither of its two soundness bugs was visible in
    anything but a section-size comparison against the previous build. Run this before trusting
    it on a new target, and keep comparing sizes anyway.
    """
    failures = 0
    cases = ([(w, s, e, {"TARGET_ESP32"}, set()) for w, s, e in SELFTEST]
             + [(w, s, e, set(), {"TARGET_ESP32"}) for w, s, e in COLLAPSE_SELFTEST])
    for why, src, want, defd, coll in cases:
        got = "".join(process(src.splitlines(keepends=True),
                              {"TARGET_NRF", "ARDUINO_ARCH_NRF52"}, defd, coll))
        if got != want:
            failures += 1
            print(f"FAIL: {why}\n  want: {want!r}\n  got:  {got!r}")
    print(f"drop_dead_arms selftest: {len(cases)} cases, {failures} failures")
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    if "--selftest" in argv:
        return selftest()

    undefined: set[str] = set()
    defined: set[str] = set()
    collapse: set[str] = set()
    files: list[str] = []
    i = 1
    while i < len(argv):
        if argv[i] == "--undef":
            undefined.add(argv[i + 1])
            i += 2
        elif argv[i] == "--define":
            defined.add(argv[i + 1])
            i += 2
        elif argv[i] == "--collapse":
            collapse.add(argv[i + 1])
            i += 2
        else:
            files.append(argv[i])
            i += 1
    if not (undefined or defined or collapse) or not files:
        print(__doc__, file=sys.stderr)
        return 64
    for path in files:
        with open(path) as fh:
            lines = fh.readlines()
        new = process(lines, undefined, defined, collapse)
        if new != lines:
            with open(path, "w") as fh:
                fh.writelines(new)
            print(f"{path}: {len(lines)} -> {len(new)} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
