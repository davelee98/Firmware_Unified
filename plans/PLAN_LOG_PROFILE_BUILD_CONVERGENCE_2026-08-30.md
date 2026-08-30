# Converge ESP32 and Nordic log-profile build selection

## 1. Objective

Give the ESP32-IDF and Nordic-Zephyr build front doors one way to select whether OpenDisplay DEBUG
logging is compiled in, without otherwise unifying their build systems. Logging verbosity is a
build profile, not a board identity and, on Nordic, not a radio transport or power profile.

This plan owns build-script input, board/profile expansion, build-directory and artifact naming,
release publication and manifest consequences. It is deliberately separate from
`PLAN_LOGGING_CONVERGENCE_2026-08-30.md`, which owns shared logging call sites. Its L0 shared CMake
selector (`od_select_log_profile()`) is the prerequisite this plan consumes; neither plan blocks
the other's ready shared-core work.

## 2. Current state

- ESP32 injects `OD_LOG_LEVEL=OD_LOG_DEBUG` through two logging-only `*-debug.cmake` board
  fragments. Ordinary boards rely on `od_log.h`'s INFO fallback. Bare `./build.sh` builds every
  board fragment, including those two pseudo-boards; `--release` excludes them by suffix. ESP32
  has no `--all` option today.
- Nordic validates `PROFILE=battery|uart|debug|quiet`. That shell enum currently overlays three
  independent CMake choices: UART logging transport, quiet/power behavior and debug compilation.
  `./build.sh --all` builds every Nordic board for the one selected `PROFILE`.
- Only two ESP32 physical boards currently have debug variants. Expanding every supported board
  across INFO and DEBUG is therefore an intentional increase in build count, build time and
  `release/` contents, not a mechanical preservation of today's matrix.
- Per-target release manifests are an existing tooling surface. Adding profile metadata or changing
  their row count must be reviewed with every checked-in reader; do not assume they are human-only.

## 3. Selected interface

**Adopt Nordic's profile shape for this narrow concern**, separated from Nordic's other profile
axes:

```text
OD_LOG_PROFILE=info  ./build.sh ...   # default, compile through OD_LOG_INFO
OD_LOG_PROFILE=debug ./build.sh ...   # compile through OD_LOG_DEBUG
```

- `OD_LOG_PROFILE` accepts exactly `info` or `debug`, defaults to `info`, and is validated before
  either vendor build starts.
- Both scripts pass the value to logging convergence L0's `od_select_log_profile()` path. Neither
  script nor board fragment writes `OD_LOG_LEVEL` directly, and every firmware compile receives
  exactly one explicit definition.
- Keep all unrelated differences between the scripts: board discovery, ordinary no-argument
  behavior, cleaning, toolchain activation, flashing and target-specific packaging remain owned by
  each target unless a requirement below names them explicitly.

## 4. ESP32 shape

- Remove the logging-only `*-debug.cmake` pseudo-boards. Select the same physical board in INFO and
  DEBUG modes.
- INFO keeps the shipping build-directory and artifact names. DEBUG adds `-debug` before the
  artifact extension and to the build-directory key, preventing CMake-cache reuse and output
  collision.
- Preserve ordinary explicit-board and `--release` behavior except where the new log-profile input
  necessarily replaces a pseudo-board name. Compatibility wrappers may translate an old debug
  pseudo-board spelling during one transition, but the board registry must contain physical boards
  only when the migration closes.

## 5. Nordic axes and compatibility

- `PROFILE` selects `battery`, `uart` or `quiet` transport/power behavior.
- `OD_LOG_PROFILE` independently selects `info` or `debug`. Every combination is valid, including:

```text
PROFILE=uart  OD_LOG_PROFILE=debug ./build.sh
PROFILE=quiet OD_LOG_PROFILE=debug ./build.sh
```

- Absence of `OD_LOG_PROFILE` means `info`, regardless of `PROFILE`.
- Retain legacy `PROFILE=debug` temporarily as an alias for
  `PROFILE=battery OD_LOG_PROFILE=debug`. Combining that legacy spelling with explicit
  `OD_LOG_PROFILE=info` is contradictory and must fail. `PROFILE=uart OD_LOG_PROFILE=debug` is not
  a conflict; it is the intended composition.
- Existing debug convenience and flash scripts become thin wrappers that set
  `OD_LOG_PROFILE=debug`. Update checked-in callers together and remove an old spelling only after
  `rg` proves it has no remaining caller.

## 6. Build-all and release behavior

On both target front doors, `./build.sh --all` is the explicit complete log-profile matrix:

- build every supported physical board once with `OD_LOG_PROFILE=info` and once with
  `OD_LOG_PROFILE=debug`;
- publish both successful artifact sets into the repository's top-level `release/` directory;
- use the shipping filename for INFO and insert `-debug` before each DEBUG extension;
- use distinct build directories for every `(board, log profile)` pair;
- validate the complete requested matrix before starting a vendor build;
- continue after a pair fails, then exit nonzero after attempting the remaining pairs;
- remove the exact expected artifact for a pair before building, so a failed pair cannot leave a
  stale artifact presented as current.

This standardizes only `--all`'s log-profile expansion. Other no-argument, explicit-board and clean
semantics remain target-local. Because ESP32 does not currently have `--all`, add it explicitly;
do not silently redefine bare `./build.sh` as part of this work.

`--release` and root `build-release.sh` remain shipping-only and force `OD_LOG_PROFILE=info`; an
ambient environment value must not turn a release build into diagnostic firmware. A separately
requested diagnostic operation may call target `--all`, but it is not a release-mode override.

## 7. Manifest contract

The per-target manifest must distinguish both profiles for every published artifact. Before
changing its schema:

1. `rg` every checked-in reader and document the result.
2. Preserve the current header and columns if profile can be represented compatibly in the
   artifact name; otherwise version the manifest format or update every reader atomically.
3. Record, directly or unambiguously through a versioned filename, board, log profile, compiled
   `OD_LOG_LEVEL`, artifact path and size.
4. Replace the manifest atomically for an `--all` run. Failed pairs get no success row and no stale
   artifact.

Do not treat a manifest schema change as an incidental logging edit.

## 8. Implementation stages

1. Inventory current ESP32/Nordic build and manifest invocations, including root release, debug
   wrappers, flash helpers and validation scripts. Add behavior-characterization shell fixtures.
2. Consume the shared CMake selector and add validated `OD_LOG_PROFILE` handling to each target
   without changing matrix behavior.
3. Separate Nordic's transport/power and log axes; retain and test the legacy `PROFILE=debug`
   translation.
4. Remove ESP32 logging-only pseudo-boards and migrate checked-in callers to physical board plus
   `OD_LOG_PROFILE=debug`.
5. Add target `--all` board × `{info,debug}` expansion, collision-free build/artifact naming,
   continue-on-failure behavior and stale-artifact removal.
6. Resolve the manifest compatibility audit, update it only as required, and wire both successful
   profile sets into `release/`.
7. Update root release and validation paths, then run the fake-toolchain suite and full target gate.

## 9. Verification

Add Bash-3.2-compatible shell fixtures with fake `idf.py` and `west` executors proving:

- default INFO and explicit DEBUG;
- invalid `OD_LOG_PROFILE` rejection before vendor execution;
- exactly one emitted `-DOD_LOG_LEVEL`, with the expected value;
- Nordic legacy mapping and contradictory-legacy rejection;
- `battery|uart|quiet` × `info|debug` composition;
- distinct build and artifact paths for INFO and DEBUG;
- a two-board `--all` fixture invokes all four board/profile pairs;
- both successful profile sets land in `release/` and are represented correctly in the manifest;
- one failed pair does not stop later pairs, yields a nonzero final status and retains no stale
  output or success row;
- `--release` and root `build-release.sh` force INFO.

Then run `tools/check.sh --targets`. A skip is not a pass. Inspect representative ESP32 and Nordic
compile commands and artifacts rather than relying only on CMake source greps.

## 10. Out of scope

- Shared logging call-site ownership or wording.
- General convergence of target build CLIs, default board behavior, cleaning or flashing.
- Zephyr subsystem `CONFIG_LOG_DEFAULT_LEVEL`; it remains separate from OpenDisplay's compiled
  logging ceiling.
- BG22, which retains `OD_CAP_LOG=0`.
- Any protocol, wire-format or hardware behavior change.

## 11. Definition of done

- Both targets accept `OD_LOG_PROFILE=info|debug` and emit exactly one matching compile definition.
- Nordic transport/power and logging profiles compose independently.
- ESP32's logging-only pseudo-boards are gone without losing debug builds for those boards.
- `--all` publishes successful INFO and DEBUG artifacts for every supported physical board into
  `release/`, with collision-free names and no failed pair represented by stale output.
- Release entry points force INFO.
- Manifest compatibility is audited and any required reader/schema changes land atomically.
- Fake-toolchain fixtures and `tools/check.sh --targets` pass with no skips.
