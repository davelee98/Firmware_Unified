#!/usr/bin/env bash
# sdkconfig drift gate: the EFFECTIVE configuration of every board is reviewed and checked in,
# and any change to it has to be approved as a diff.
#
#     tools/sdkconfig_baseline.sh            # check every board with a build (default)
#     tools/sdkconfig_baseline.sh --update   # re-record after an approved change
#     tools/sdkconfig_baseline.sh s3-n16r8   # check one board
#
# WHY THIS EXISTS, and why a "translation table" in docs was not enough.
#
# The port carried 77 behaviour-relevant sdkconfig differences from the Arduino build it is
# meant to reproduce, of which SIX were declared. The other 71 were IDF defaults inherited by
# never naming the symbol -- invisible, because a setting you did not write does not appear in
# any file you can review. Among them: the CPU running at two-thirds clock, a debug-level
# optimisation build, the task watchdog no longer resetting a wedged device, and a tick rate
# that silently turned every sub-10 ms delay into no delay at all.
#
# docs/TOOLCHAINS.md has a PlatformIO-knob -> sdkconfig translation table for exactly this
# purpose, maintained by hand. It caught two of those four. Hand-maintained lists of things to
# remember do not survive contact with a toolchain upgrade -- which is the same argument
# compat/ratchet.sh makes about the Arduino shim, and the same answer: give it a mechanical
# gate rather than good intentions.
#
# WHAT IS RECORDED: the whole effective sdkconfig, not a curated subset. A subset can only
# catch drift in settings someone already thought of, and every one of the four above was
# missed precisely because nobody thought of it. The cost is a noisy diff on an IDF upgrade;
# that noise IS the review -- an IDF bump changes defaults under you, and this is the only
# place that becomes visible before hardware does it for you.
#
# The header block is stripped: it carries the IDF version, so keeping it would make every
# baseline conflict on a toolchain bump for a reason that is not a configuration change.

set -euo pipefail
cd "$(dirname "$0")/.."

BASELINE_DIR="sdkconfig.baselines"
UPDATE=0
BOARDS=()

for arg in "$@"; do
    case "$arg" in
        --update) UPDATE=1 ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *)  BOARDS+=("$arg") ;;
    esac
done

# Default to every board that has been built. Deliberately NOT every board fragment: a board
# nobody built has no effective config to compare, and inventing one by building it here would
# make a check command take twenty minutes.
#
# A build/ subdirectory is only a board if boards/<name>.cmake exists. build/ also holds
# helper trees -- build/_warncheck is one -- which have an sdkconfig but are not boards, and
# baselining them would gate the project on the config of a scratch build.
if [ ${#BOARDS[@]} -eq 0 ]; then
    for d in build/*/sdkconfig; do
        [ -f "$d" ] || continue
        name="$(basename "$(dirname "$d")")"
        [ -f "boards/$name.cmake" ] || continue
        BOARDS+=("$name")
    done
fi

if [ ${#BOARDS[@]} -eq 0 ]; then
    echo "::error::no built boards found under build/. Run ./build.sh first." >&2
    exit 1
fi

mkdir -p "$BASELINE_DIR"

# Strip the generated header comment block (everything up to and including the blank line that
# follows it) and any Espressif version banner lines.
normalise() {
    grep -vE '^#( Espressif| Automatically generated| Project Configuration|$)' "$1" \
        | grep -vE '^CONFIG_IDF_(INIT_VERSION|FIRMWARE_CHIP_ID)='
}

rc=0
for b in "${BOARDS[@]}"; do
    live="build/$b/sdkconfig"
    base="$BASELINE_DIR/$b.sdkconfig"
    if [ ! -f "$live" ]; then
        echo "::error::$b has no build/$b/sdkconfig -- build it first" >&2
        rc=1
        continue
    fi
    if [ "$UPDATE" = 1 ]; then
        normalise "$live" > "$base"
        echo "recorded  $b"
        continue
    fi
    if [ ! -f "$base" ]; then
        echo "::error::$b has no recorded baseline. Review its config, then:" >&2
        echo "          tools/sdkconfig_baseline.sh --update $b" >&2
        rc=1
        continue
    fi
    if diff -u "$base" <(normalise "$live") > /tmp/od_sdkdiff.$$ 2>&1; then
        echo "OK        $b"
    else
        echo
        echo "::error::$b: effective sdkconfig differs from its recorded baseline."
        cat /tmp/od_sdkdiff.$$
        echo
        echo "If the change is intended, record it in the SAME commit that causes it:"
        echo "    tools/sdkconfig_baseline.sh --update $b"
        echo "An unreviewed diff here is a behaviour change nobody chose -- see the note at the"
        echo "top of this script for the four that reached hardware before it existed."
        rc=1
    fi
    rm -f /tmp/od_sdkdiff.$$
done

exit $rc
