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
# NOTE the `|| true`: grep exits 1 when it matches nothing, which under `set -e` + `pipefail`
# aborts the script on the healthiest possible state -- zero shim users. Without it this check
# fails hardest exactly when the port has succeeded.
shim_users() {
    grep -rlE '#[[:space:]]*include[[:space:]]*[<"][^">]*arduino_compat\.h' \
         targets/esp32-idf --include='*.c' --include='*.cpp' --include='*.h' \
         --include='*.hpp' --include='*.inl' 2>/dev/null || true
}

if [ -d targets/esp32-idf ]; then
    actual=$(shim_users | wc -l | tr -d '[:space:]')
else
    actual=0
fi

echo "arduino_compat.h: $actual file(s) include it; budget is $budget"

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
fi

echo "OK: shim usage is at its recorded budget."
