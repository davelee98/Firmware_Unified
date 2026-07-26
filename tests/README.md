# tests

Host-runnable tests for `shared/`. No hardware, no target toolchain.

Ownership, rationale, and the full mechanism are in [docs/TEST_OWNERSHIP.md](../docs/TEST_OWNERSHIP.md);
this file is just how to run things.

```bash
cmake -S tests/host -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Add `-DCMAKE_C_COMPILER=clang` for the second compiler. Neither compiler is required
locally — CI runs both, and gcc alone is enough to work here.

## Layout

```
vectors/     the wire-vector corpus — JSON, one file per subsystem
host/
  CMakeLists.txt     host build of shared/ + CTest wiring
  canary.c           compiles the protocol headers under the strict flags (scaffold)
  replay_vectors.py  replays the corpus through py-opendisplay's public API
```

## Why this exists before any target code

`shared/` is plain C with no vendor headers, so it compiles for the host **by
construction** — the boundary rule and host testability are the same property seen twice.
That makes this the cheapest real enforcement available, and the reason it is standing up
first: every later import lands as a test-first change into a build that already enforces
the contract, instead of having to stand the contract up afterwards.

It is deliberately green against an empty `shared/`. Nothing here is waiting on a target.

## The two runners

The corpus is data. It is replayed by **two** implementations of the wire protocol — the C
core (once `shared/` has one) and `py-opendisplay`. A vector both sides agree on is a
contract test; a vector they disagree on is exactly the host/firmware divergence this repo
exists to eliminate, caught in CI instead of on a bench.

Today only the Python side exists, so this is a one-sided test. It still earns its place:
it forces the vector schema to be expressible against a real implementation before there is
a second one to argue with, and it makes every vector added later half-verified on arrival.

`replay_vectors.py` calls py-opendisplay's **public API only** and must never reimplement
encode logic — a vector both runners derive from the same local helper proves nothing.
Vectors it cannot express are reported as `SKIP` with a reason, never quietly passed.

## The compiler is the boundary check

[.github/workflows/shared-boundary.yml](../.github/workflows/shared-boundary.yml) greps for
vendor includes. A grep sees `#include` lines and literal tokens; it cannot see an
`extern`-declared vendor symbol, a macro leaked through a target config header, or a libc
call that does not exist on a freestanding target. `-std=c99 -Wall -Wextra -Werror` with
`CMAKE_C_EXTENSIONS OFF` does. Treat the grep as the fast pre-filter and this as the real
gate.

## Not here yet

- **The C vector runner** (`test_vectors.c`) — arrives with the first `shared/core` source.
  There is nothing for it to run against yet.
- **Fuzz harnesses** (`tests/fuzz/`) — libFuzzer over the config TLV parser and frame
  dispatch, both reachable pre-authentication. Mandatory before those subsystems are
  promoted, not after.
- **HAL test doubles** (`host/doubles/`) — one per interface, arriving with the interfaces.
- **Captured wire vectors.** The corpus is authored today. Captures from real sessions
  have their own deadline: once `shared/core` starts replacing a target's logic, there is
  no longer an untouched reference to capture from. See TEST_OWNERSHIP § "Capture is
  time-sensitive".
