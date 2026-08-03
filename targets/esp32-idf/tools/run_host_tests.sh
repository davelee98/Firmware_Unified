#!/usr/bin/env bash
# Host tests for this target's portable logic. Run from anywhere:
#
#     targets/esp32-idf/tools/run_host_tests.sh
#
# Today that is src/link_owner.cpp, the connection-ownership arbiter. It is target code
# rather than shared/ code, so it is not covered by tests/host/ -- but it is the one file in
# the target whose bugs are genuinely cross-task (BLE claims run on the NimBLE host task, LAN
# claims and every release on the loop task) and therefore unreachable by inspection. It is
# also the clearest shared/ promotion candidate here, so it is worth gating before the
# promotion rather than after.
#
# Two sanitizer builds, matching the reference firmware's: ASan+UBSan, then TSan+UBSan as a
# SEPARATE binary, because TSan and ASan cannot be combined.
#
# -I tools/hostshim supplies a fake clock -- Arduino.h and esp_timer.h over one backing
# variable -- so the REAL src/link_owner.cpp and src/od_log.h compile unmodified. The test
# drives that clock directly, which is the only way to park on the ~49.7-day wrap boundary the
# wrap cases need; waiting is not an option.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/od-host-tests"
mkdir -p "$OUT"

CXXFLAGS=(-std=c++17 -Wall -Wextra -Werror -O1 -I tools/hostshim -I src)
SRCS=(tools/test_link_owner.cpp src/link_owner.cpp)

# ThreadSanitizer aborts at startup with "unexpected memory mapping" under some kernels' ASLR
# layouts -- WSL2 in particular. setarch -R disables ASLR for the child only.
#
# PROBED WITH A THROWAWAY BINARY, not by running the real test and retrying on failure. The
# retry form was tried first and is actively misleading: TSan's abort does not stop the test
# process cleanly, so the first attempt prints a spurious "FAIL ... 1 failures" line before the
# retry succeeds, and the log then shows a failure that did not happen. Deciding up front means
# a FAIL line in this script's output is always a real one.
TSAN_RUN=()
probe_tsan() {
    local probe="$OUT/tsan_probe"
    printf 'int main(void){return 0;}\n' > "$OUT/tsan_probe.c"
    if ! gcc -fsanitize=thread "$OUT/tsan_probe.c" -o "$probe" 2>/dev/null; then
        echo "note: no ThreadSanitizer in this toolchain -- TSan pass skipped" >&2
        return 1
    fi
    if "$probe" >/dev/null 2>&1; then
        return 0
    fi
    if command -v setarch >/dev/null 2>&1 && setarch -R "$probe" >/dev/null 2>&1; then
        echo "note: TSan needs ASLR disabled here (kernel mapping layout); using setarch -R"
        TSAN_RUN=(setarch -R)
        return 0
    fi
    echo "note: ThreadSanitizer cannot start on this kernel -- TSan pass skipped" >&2
    return 1
}

echo "==> link_owner: ASan + UBSan"
g++ "${CXXFLAGS[@]}" -fsanitize=undefined,address "${SRCS[@]}" -o "$OUT/test_link_owner" -pthread
"$OUT/test_link_owner"

if probe_tsan; then
    echo "==> link_owner: TSan + UBSan"
    g++ "${CXXFLAGS[@]}" -fsanitize=thread,undefined "${SRCS[@]}" \
        -o "$OUT/test_link_owner_tsan" -pthread
    "${TSAN_RUN[@]}" "$OUT/test_link_owner_tsan"
else
    echo "==> link_owner: TSan pass SKIPPED (see note above)"
fi

echo "==> all host tests passed"
