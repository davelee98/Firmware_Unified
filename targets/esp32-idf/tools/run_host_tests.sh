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
# -I tools/hostshim supplies a fake clock (esp_timer.h) and the two FreeRTOS types od_log.h
# names, so the REAL src/link_owner.cpp and src/od_log.h compile unmodified. The test
# drives that clock directly, which is the only way to park on the ~49.7-day wrap boundary the
# wrap cases need; waiting is not an option.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/od-host-tests"
mkdir -p "$OUT"

# -DTARGET_ESP32 so od_log.h takes the same branch the firmware does. It is not a new
# assumption: src/link_owner.cpp already includes "esp_timer.h" unconditionally (its OD-PORT off
# Arduino millis()), and tools/hostshim/esp_timer.h is what satisfies it. Without the define,
# od_log.h took its nRF arm and looked for the Adafruit core's <FreeRTOS.h> -- a header this
# shim has no business providing, for a target this test does not compile.
CXXFLAGS=(-std=c++17 -Wall -Wextra -Werror -O1 -DTARGET_ESP32 -I tools/hostshim -I src)
SRCS=(tools/test_link_owner.cpp src/link_owner.cpp)

# ThreadSanitizer aborts at startup with "unexpected memory mapping" under some kernels' ASLR
# layouts -- WSL2 in particular. setarch -R disables ASLR for the child only.
#
# ALWAYS use setarch when it exists. Do not probe.
#
# Two earlier designs failed, and both failure modes are worth knowing:
#
#   * Run the real binary and retry on failure. TSan's abort does not stop the test process, so
#     the first attempt prints a spurious "FAIL ... 1 failures" line before the retry succeeds.
#     The log then shows a failure that did not happen.
#   * Probe with a throwaway binary and use setarch only if the probe fails. This is what was
#     shipped, and it is WRONG: a trivial probe has different memory mappings from the real
#     test binary, so a passing probe does not predict it. Measured 2026-08-03 -- roughly 1 run
#     in 6 had the probe succeed and the real binary then abort, producing the same spurious
#     FAIL line plus an intermittent non-zero exit (66, TSan's default error code) from a suite
#     that is actually clean. An intermittently red gate is worse than no gate: it trains people
#     to re-run until green, which is exactly how a real race would get ignored.
#
# Disabling ASLR for a short single-purpose test child costs nothing and removes the variable
# entirely, so it is not conditional on detecting a problem first.
TSAN_RUN=()
if command -v setarch >/dev/null 2>&1 && setarch -R true >/dev/null 2>&1; then
    TSAN_RUN=(setarch -R)
else
    echo "note: setarch -R unavailable; if TSan aborts with 'unexpected memory mapping' that is" >&2
    echo "      this kernel's ASLR layout, not a test failure" >&2
fi

have_tsan() {
    printf 'int main(void){return 0;}\n' > "$OUT/tsan_probe.c"
    gcc -fsanitize=thread "$OUT/tsan_probe.c" -o "$OUT/tsan_probe" 2>/dev/null
}

echo "==> link_owner: ASan + UBSan"
g++ "${CXXFLAGS[@]}" -fsanitize=undefined,address "${SRCS[@]}" -o "$OUT/test_link_owner" -pthread
"$OUT/test_link_owner"

if have_tsan; then
    echo "==> link_owner: TSan + UBSan"
    g++ "${CXXFLAGS[@]}" -fsanitize=thread,undefined "${SRCS[@]}" \
        -o "$OUT/test_link_owner_tsan" -pthread
    "${TSAN_RUN[@]}" "$OUT/test_link_owner_tsan"
else
    echo "==> link_owner: TSan pass SKIPPED -- no ThreadSanitizer in this toolchain" >&2
fi

echo "==> all host tests passed"
