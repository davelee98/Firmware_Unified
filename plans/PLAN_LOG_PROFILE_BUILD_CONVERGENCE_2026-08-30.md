# Converge ESP32 and Nordic log-profile build selection

Updated: 2026-09-03.

## 1. Objective

Give the ESP32-IDF and Nordic-Zephyr build front doors one way to select whether OpenDisplay DEBUG
logging is compiled in, without otherwise unifying their build systems. Logging verbosity is a
build profile, not a board identity and, on Nordic, not a radio transport, power or debugger
profile. **The selector has exactly one firmware effect: it chooses the compile-time
`OD_LOG_LEVEL`.** It does not select a Zephyr configuration overlay, enable thread metadata, change
Zephyr subsystem verbosity, resize a log buffer or alter a boot message.

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
- Nordic's current `prj_debug.conf` is not a log-level profile: besides Zephyr subsystem logging
  and deferred-log buffer tuning, it enables `CONFIG_DEBUG_THREAD_INFO` and the NCS boot banner.
  `OD_DEBUG_BUILD` also changes the application boot line. Those effects must not move under
  `OD_LOG_PROFILE`; the GDB support gets its own explicit input, while the subsystem/buffering
  choices remain separate work.
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
- Calling `od_select_log_profile()` is the whole effect. In particular, `debug` does not define
  `OD_DEBUG_BUILD`, append `prj_debug.conf`, change `CONFIG_LOG_DEFAULT_LEVEL`, enable
  `CONFIG_DEBUG_THREAD_INFO`, or select different log transport/buffering.
- Keep all unrelated differences between the scripts: board discovery, ordinary no-argument
  behavior, cleaning, toolchain activation, flashing and target-specific packaging remain owned by
  each target unless a requirement below names them explicitly.

## 4. ESP32 shape

- Remove the logging-only `*-debug.cmake` pseudo-boards. Select the same physical board in INFO and
  DEBUG modes.
- INFO keeps the shipping build-directory and artifact names. DEBUG adds `-debug` to the physical
  board tag in both the build-directory key and artifact stem, preventing CMake-cache reuse and
  output collision while preserving the naming convention Nordic already publishes. For example,
  `opendisplay-s3-n16r8-extuart-debug-merged.bin`, not
  `opendisplay-s3-n16r8-extuart-merged-debug.bin`.
- Preserve ordinary explicit-board and `--release` behavior except where the new log-profile input
  necessarily replaces a pseudo-board name. Compatibility wrappers may translate an old debug
  pseudo-board spelling during one transition, but the board registry must contain physical boards
  only when the migration closes.

## 5. Nordic axes and compatibility

- `PROFILE` selects `normal`, `uart` or `quiet` transport/power behavior. `normal` is the default:
  it keeps the shipping-safe nRF54 configuration (serial off, RTT available) while board-specific
  configuration continues to give the nRF52840 its USB CDC console. The name describes its role as
  the ordinary shipping mode without implying that it changes radio or battery policy.
- `OD_LOG_PROFILE` independently selects `info` or `debug`. Every combination is valid, including:

```text
PROFILE=uart  OD_LOG_PROFILE=debug ./build.sh
PROFILE=quiet OD_LOG_PROFILE=debug ./build.sh
```

- Absence of `OD_LOG_PROFILE` means `info`, regardless of `PROFILE`.
- Retain legacy `PROFILE=battery` temporarily as an alias for `PROFILE=normal`. Normalize it before
  toolchain activation, print `normal` in build summaries, migrate every checked-in caller, and
  remove the alias only after `rg` finds no remaining use.
- Retain legacy `PROFILE=debug` temporarily as an alias for
  `PROFILE=normal OD_LOG_PROFILE=debug`. Combining that legacy spelling with explicit
  `OD_LOG_PROFILE=info` is contradictory and must fail. `PROFILE=uart OD_LOG_PROFILE=debug` is not
  a conflict; it is the intended composition. Compatibility is for the old spelling, not the
  accidental `prj_debug.conf` bundle: the alias changes the OpenDisplay log ceiling only.
- Existing debug **build** convenience scripts become thin wrappers that set
  `OD_LOG_PROFILE=debug`. Debug flash scripts do not select build inputs: they continue resolving
  the distinct `-debug` build directory through their existing `BUILD_DIR` /
  `OD_FLASH_PROFILE=debug` mechanisms. Update checked-in callers together and remove an old
  spelling only after `rg` proves it has no remaining caller.
- Remove `OD_DEBUG_BUILD` from the log-profile path and remove the different `DEBUG starting` boot
  line. A compile-command check for the selected `OD_LOG_LEVEL` is the build's evidence; changing a
  user-visible boot message is not part of choosing a logging ceiling.
- Preserve nRF54 thread-aware debugging behind a separate, Nordic-only
  `OD_NORDIC_GDB=0|1` input, default `0`. `OD_NORDIC_GDB=1` appends a narrowly named GDB overlay
  containing `CONFIG_DEBUG_THREAD_INFO=y` and the NCS boot-banner setting. It composes with every
  transport and log profile, and `debug-nrf54.sh` must tell the user to request it when the selected
  build lacks thread metadata. It must not imply `OD_LOG_PROFILE=debug`.
- Delete `prj_debug.conf` after its responsibilities are separated. Its Zephyr subsystem-level and
  deferred-buffer settings do not move into the new GDB overlay and are not selected by
  `OD_LOG_PROFILE`; whether to retain or retune them is gated on the timestamped serial capture in
  `PLAN_NORDIC_LOG_NOISE_TXPOWER_2026-09-03.md` section 3.
- **`PROFILE=quiet OD_LOG_PROFILE=debug` is accepted but is a deliberately silent log-debug
  build, and this must be documented at the point of use, not just here.** `prj_quiet.conf` sets
  `CONFIG_LOG=n`, so Zephyr's logging subsystem is compiled out entirely under `quiet` regardless
  of `OD_LOG_PROFILE`; `od_log_*` calls compiled in by `OD_LOG_LEVEL=OD_LOG_DEBUG` have nowhere to
  go and produce no output. Do not re-enable `CONFIG_LOG` for this combination — `quiet`'s purpose
  is µA power measurement (see `prj_quiet.conf`'s own comment), and turning logging back on for it
  would defeat that. The combination stays valid because rejecting it would require `OD_LOG_PROFILE`
  to know about Nordic's transport/power axis, which § 5's whole point is to avoid; instead, the
  build's own summary output (already itemized in `build.sh`'s per-build banner) must state
  plainly when `PROFILE=quiet` makes a requested `OD_LOG_PROFILE=debug` a no-op, so this isn't
  discovered only by an empty log capture.

## 6. Build-all and release behavior

**`--all`'s existing meaning does not change, and a new `--all-profiles` is added for the
cross-product.** Conflating the two was an error in an earlier draft of this plan: root
`build-release.sh` already invokes Nordic as `./build.sh --all` (`build-release.sh`, the
`nordic) echo "./build.sh --all"` case), meaning "every board, at whichever `PROFILE` is in
effect" — today that's `PROFILE=battery` by default, and under this plan it's
`PROFILE=normal OD_LOG_PROFILE=info` by default. If `--all` were redefined to always mean "every
board × both log profiles," root's
existing unmodified call would silently start producing DEBUG artifacts on every release build,
directly contradicting § 3's "`--release` and root `build-release.sh` remain shipping-only and
force `OD_LOG_PROFILE=info`" requirement below. So:

- **`./build.sh --all`** (both targets) keeps its current meaning: every supported physical board,
  built once, at whichever `OD_LOG_PROFILE` is currently selected (default `info`). This is
  exactly Nordic's existing behavior and ESP32's newly-added equivalent — root `build-release.sh`'s
  existing `./build.sh --all` call for Nordic needs **no change** under this plan.
- **`./build.sh --all-profiles`** (both targets, new) is the explicit complete log-profile matrix:
  build every supported physical board once with `OD_LOG_PROFILE=info` and once with
  `OD_LOG_PROFILE=debug`;
  - publish both successful artifact sets into the repository's top-level `release/` directory;
  - use the shipping filename for INFO and add `-debug` to the board tag in each DEBUG artifact
    stem (`opendisplay-<board>-debug-merged.hex`, `-ota.bin`, `-dfu.zip` or `.uf2` as applicable);
  - use distinct build directories for every `(board, log profile)` pair;
  - validate the complete requested matrix before starting a vendor build;
  - continue after a pair fails, then exit nonzero after attempting the remaining pairs;
  - remove the exact expected artifact for a pair before building, so a failed pair cannot leave a
    stale artifact presented as current.

This standardizes only `--all-profiles`'s log-profile expansion; `--all` and every other
no-argument, explicit-board and clean semantic remain target-local and unchanged. Because ESP32
does not currently have `--all`, add it with Nordic's existing single-profile meaning, not
`--all-profiles`'s meaning.

`--all-profiles` owns the log-profile axis. Supplying `OD_LOG_PROFILE` explicitly with it is an
error rather than an ignored or partly honoured input. Legacy `PROFILE=debug --all-profiles` is
also an error; use `PROFILE=normal --all-profiles`. Nordic's canonical
`PROFILE=normal|uart|quiet`, temporary `PROFILE=battery` alias, and `OD_NORDIC_GDB=0|1` still
compose with the matrix. Validate these conflicts before toolchain activation or the first
recursive build.

`--release` and root `build-release.sh` remain shipping-only and force `OD_LOG_PROFILE=info`; an
ambient environment value must not turn a release build into diagnostic firmware. A separately
requested diagnostic operation may call target `--all-profiles`, but it is not a release-mode
override, and root `build-release.sh` must not gain a code path that calls `--all-profiles`.

## 7. Manifest contract

The checked-in reader audit was repeated 2026-09-03. Root `build-release.sh` checks only whether
the expected per-target manifest filename exists when writing its summary; no checked-in code
parses either target's header or rows. The remaining references are producer code or prose.

Preserve each target's current columns. Add a header line defining the stable mapping:

```text
log      unsuffixed=info/OD_LOG_INFO  -debug=debug/OD_LOG_DEBUG
```

The artifact and row tag then record physical board plus log profile unambiguously without a
schema migration: an unsuffixed row is INFO and a `<board>-debug` row is DEBUG. Keep the existing
artifact path and size columns. Replace the manifest atomically for an `--all-profiles` run;
failed pairs get no success row and no stale artifact.

Re-run the reader grep during implementation in case a consumer lands after this plan update. If a
parser has appeared, stop and either prove the added header is compatible or update/version the
contract atomically. Do not treat a manifest schema change as an incidental logging edit.

## 8. Implementation stages

1. Inventory current ESP32/Nordic build and manifest invocations, including root release, debug
   wrappers, flash helpers and validation scripts. Add behavior-characterization shell fixtures.
2. Consume the shared CMake selector and add validated `OD_LOG_PROFILE` handling to each target
   without changing matrix behavior.
3. Rename Nordic's default transport mode from `battery` to `normal`, retain the temporary
   `battery` alias, then separate transport/power, log-level and GDB axes. Retain and test the
   legacy `PROFILE=debug` translation, remove `OD_DEBUG_BUILD`, split out the narrow GDB overlay,
   and leave subsystem verbosity/buffer policy outside the log selector.
4. Remove ESP32 logging-only pseudo-boards and migrate checked-in callers to physical board plus
   `OD_LOG_PROFILE=debug`. Remove their stale entries from source-selection lists and update active
   board-count claims; preserve historical hardware-evidence names as historical labels.
5. Add target `--all-profiles` board × `{info,debug}` expansion, collision-free build/artifact
   naming, continue-on-failure behavior and stale-artifact removal, without changing `--all`'s
   existing single-profile meaning.
6. Reconfirm the manifest compatibility audit, add the suffix-mapping header, and wire both
   successful profile sets into `release/` without changing the row columns.
7. Update root release and validation paths, then run the fake-toolchain suite and full target gate.

## 9. Verification

Add Bash-3.2-compatible shell fixtures with fake `idf.py` and `west` executors proving:

- default INFO and explicit DEBUG;
- invalid `OD_LOG_PROFILE` rejection before vendor execution;
- exactly one emitted `-DOD_LOG_LEVEL`, with the expected value;
- Nordic legacy mapping and contradictory-legacy rejection;
- `normal|uart|quiet` × `info|debug` composition, the temporary `battery` → `normal` alias, and that
  `quiet`+`debug` compiles but
  the build's own output states plainly that Zephyr logging is off for it;
- `OD_LOG_PROFILE` changes only the one `OD_LOG_LEVEL` definition: no `OD_DEBUG_BUILD`, GDB thread
  metadata, boot-message variant, Zephyr subsystem-level change or debug overlay;
- `OD_NORDIC_GDB=1` composes independently and is the only new front-door path that enables the
  narrow GDB overlay;
- distinct build and artifact paths for INFO and DEBUG;
- `--all` still builds every board at one profile only (regression-proving it against today's
  Nordic behavior and root `build-release.sh`'s existing unmodified call);
- a two-board `--all-profiles` fixture invokes all four board/profile pairs;
- `--all-profiles` rejects explicit `OD_LOG_PROFILE` and legacy `PROFILE=debug` before vendor
  execution;
- both successful profile sets land in `release/` and are represented correctly in the manifest;
- one failed pair does not stop later pairs, yields a nonzero final status and retains no stale
  output or success row;
- `--release` and root `build-release.sh` force INFO and never invoke `--all-profiles`.

Then run `tools/check.sh --targets`. A skip is not a pass. Inspect representative ESP32 and Nordic
compile commands and artifacts rather than relying only on CMake source greps. The standard target
gate remains the shipping INFO matrix; separately run representative real DEBUG builds to prove
the selector and collision-free artifact paths with the vendor toolchains.

## 10. Out of scope

- Shared logging call-site ownership or wording.
- General convergence of target build CLIs, default board behavior, cleaning or flashing.
- Zephyr subsystem `CONFIG_LOG_DEFAULT_LEVEL`; it remains separate from OpenDisplay's compiled
  logging ceiling.
- Nordic deferred-log capacity/drain tuning. The timestamped-reader capture decides that work; this
  refactor neither carries those settings under `OD_LOG_PROFILE` nor substitutes a new policy.
- BG22, which retains `OD_CAP_LOG=0`.
- Any protocol, wire-format or hardware behavior change.

## 11. Definition of done

- Both targets accept `OD_LOG_PROFILE=info|debug` and emit exactly one matching compile definition.
- `OD_LOG_PROFILE` has no effect beyond that definition.
- Nordic transport/power, logging and optional GDB support compose independently.
- Nordic's canonical default transport mode is `normal`; `battery` is only a tested temporary
  compatibility alias and every checked-in caller uses `normal`.
- ESP32's logging-only pseudo-boards are gone without losing debug builds for those boards.
- `--all` still builds every supported physical board at one selected log profile only, matching
  today's Nordic behavior and requiring no change to root `build-release.sh`'s existing call.
- `--all-profiles` publishes successful INFO and DEBUG artifacts for every supported physical board
  into `release/`, with collision-free names and no failed pair represented by stale output.
- Release entry points force INFO and never invoke `--all-profiles`.
- Existing manifest columns are preserved, the profile-suffix mapping is explicit, and each
  requested successful pair appears exactly once.
- Fake-toolchain fixtures and `tools/check.sh --targets` pass with no skips.
