#!/usr/bin/env bash
# Arduino-shim ratchet: the number of files including arduino_compat.h may only DECREASE.
#
# docs/MIGRATION.md calls the shim "the biggest risk in this plan", for a reason that is not
# technical: temporary compatibility layers become permanent ones. The mitigation it specifies
# is this check -- "give it a mechanical ratchet rather than good intentions" -- and
# DESIGN_REVIEW § "Likely pitfalls" #4 predicts that if the ratchet is not built before Phase B
# starts, it will be built never. So it exists before the shim does, with a budget of 0.
#
# The shim is scaffolding with a scheduled demolition, not a portability layer. When the budget
# reaches 0 and arduino_compat.h is gone, DELETE this whole directory and the workflow that
# calls it -- the check is finished, and a passing check nobody can fail is just noise.
#
#   ./targets/esp32-idf/compat/ratchet.sh
#
# Exit 0 only when the live count exactly matches SHIM_BUDGET.

set -euo pipefail

cd "$(dirname "$0")/../../.."   # repo root
COMPAT_DIR="targets/esp32-idf/compat"
BUDGET_FILE="$COMPAT_DIR/SHIM_BUDGET"

if [ ! -f "$BUDGET_FILE" ]; then
    echo "::error::$BUDGET_FILE is missing. The ratchet cannot pass without a recorded budget."
    exit 1
fi

budget=$(grep -vE '^\s*(#|$)' "$BUDGET_FILE" | head -1 | tr -d '[:space:]')
if ! [[ "$budget" =~ ^[0-9]+$ ]]; then
    echo "::error::$BUDGET_FILE must contain a single integer, got: '$budget'"
    exit 1
fi

# Count FILES (not occurrences) that include the shim, anywhere under the target.
# -l so a file including it twice still counts once; that is the unit the budget is in.
# Counts users of EITHER arduino_compat.h OR Arduino.h.
#
# MIGRATION.md specifies "the number of files including arduino_compat.h". Taken literally
# that would measure nothing: the imported sources write `#include <Arduino.h>`, and
# compat/Arduino.h forwards to the real shim, so a literal count would sit at 1 forever
# regardless of how many files actually depend on the Arduino surface. The forwarder would
# launder every dependency past the ratchet.
#
# What the check is *for* is "how many files still need Arduino", so that is what it counts.
# compat/ itself is excluded -- the shim including its own header is not a user of it.
#
# Widened 2026-07-25 to every shim header, not just arduino_compat.h/Arduino.h. Phase B added
# Wire.h, SPI.h, WiFi.h, ledc_compat.h and esp32-hal-gpio.h, and a file that includes Wire.h
# but not Arduino.h is just as much a shim user. Measured both ways at the time: 20 under the
# old pattern, 21 under this one. The shim did NOT grow -- the METRIC did, by one file. That
# distinction is recorded here because "we widened the definition" is otherwise
# indistinguishable from "we let it grow", and a ratchet that cannot tell them apart is not
# a ratchet.
#
# NOTE the `|| true`: grep exits 1 when it matches nothing, which under `set -e` + `pipefail`
# aborts the script on the healthiest possible state -- zero shim users. Without it this check
# fails hardest exactly when the port has succeeded.
shim_users() {
    grep -rlE '#[[:space:]]*include[[:space:]]*[<"][[:space:]]*(arduino_compat|Arduino|Wire|SPI|WiFi|ledc_compat|esp32-hal-gpio)\.h' \
         targets/esp32-idf --include='*.c' --include='*.cpp' --include='*.h' \
         --include='*.hpp' --include='*.inl' 2>/dev/null \
        | grep -v '^targets/esp32-idf/compat/' || true
}

# Vendored libraries include the shim too, and the grep above never saw them: it is scoped to
# targets/esp32-idf, so third_party/ was a blind spot. bb_epaper and FastEPD ARE Arduino
# libraries -- depending on Arduino.h/Wire.h/SPI.h is their normal state, not creeping shim
# usage -- so they do NOT belong in the app-code budget, which measures OUR migration. But they
# must not be invisible either: when the budget reaches 0 the instruction is to delete compat/,
# and doing that with third_party still including those headers breaks the build.
#
# Reported separately and never enforced. The real fix for these is upstream IO backends
# (see third_party/NOTICE.md), not a number that has to fall.
third_party_shim_users() {
    grep -rlE '#[[:space:]]*include[[:space:]]*[<"][[:space:]]*(arduino_compat|Arduino|Wire|SPI|WiFi|ledc_compat|esp32-hal-gpio)\.h' \
         third_party --include='*.c' --include='*.cpp' --include='*.h' \
         --include='*.hpp' --include='*.inl' 2>/dev/null || true
}

if [ -d targets/esp32-idf ]; then
    actual=$(shim_users | wc -l | tr -d '[:space:]')
else
    actual=0
fi

echo "arduino_compat.h: $actual file(s) include it; budget is $budget"

if [ -d third_party ]; then
    tp_count=$(third_party_shim_users | wc -l | tr -d '[:space:]')
    if [ "$tp_count" -gt 0 ]; then
        echo
        echo "note: $tp_count vendored file(s) under third_party/ also include shim headers."
        third_party_shim_users | sed 's/^/    /'
        echo "      Not counted in the budget (vendored Arduino libraries), but compat/ cannot"
        echo "      be deleted until they have real ESP-IDF IO backends. See third_party/NOTICE.md."
    fi
fi

if [ "$actual" -gt "$budget" ]; then
    echo
    shim_users | sed 's/^/    /'
    echo
    echo "::error::the Arduino shim GREW ($budget -> $actual)."
    echo "The shim may only shrink. It is a demolition schedule, not a portability layer:"
    echo "if it is still present when the last subsystem lands, the port is not done."
    echo "Route the dependency through shared/hal or write the IDF call directly."
    echo "See docs/MIGRATION.md § 'The ESP32 import is different', phase C."
    exit 1
fi

if [ "$actual" -lt "$budget" ]; then
    echo
    echo "::error::the shim shrank ($budget -> $actual) but $BUDGET_FILE was not updated."
    echo "Lower it to $actual in the same commit. The budget must track reality, or the"
    echo "ratchet silently develops slack and stops preventing the thing it exists to prevent."
    exit 1
fi

if [ "$actual" -eq 0 ]; then
    echo
    echo "Budget is 0. If targets/esp32-idf/compat/arduino_compat.h no longer exists, phase C"
    echo "is complete: delete $COMPAT_DIR and .github/workflows/esp32-shim-ratchet.yml together."
    if [ -d third_party ] && [ "$(third_party_shim_users | wc -l | tr -d '[:space:]')" -gt 0 ]; then
        echo
        echo "NOT YET: the vendored files listed above still include shim headers. Deleting"
        echo "$COMPAT_DIR now would break the build. Give them ESP-IDF IO backends first."
    fi
fi

echo "OK: shim usage is at its recorded budget."
