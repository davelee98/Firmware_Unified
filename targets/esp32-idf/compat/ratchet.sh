#!/usr/bin/env bash
# Arduino-shim ratchet: the number of files including arduino_compat.h may only DECREASE.
#
# docs/MIGRATION.md calls the shim "the biggest risk in this plan", for a reason that is not
# technical: temporary compatibility layers become permanent ones. The mitigation it specifies
# is this check -- "give it a mechanical ratchet rather than good intentions" -- and
# DESIGN_REVIEW § "Likely pitfalls" #4 predicts that if the ratchet is not built before Phase B
# starts, it will be built never. So it exists before the shim does, with a budget of 0.
#
# The shim is scaffolding with a scheduled demolition, not a portability layer.
#
# THE ENDSTATE IS NOT AN EMPTY DIRECTORY (decided 2026-08-04). FastEPD needs a PERMANENT
# compatibility layer -- it is a vendored Arduino library and there is no version of it that
# does not want Arduino primitives -- so some of what currently lives here never leaves. That
# surviving piece is the VENDOR ADAPTER, and it is deliberately not called a shim: "shim" in
# this repo means scheduled demolition, and using the word for something permanent poisons it
# for the thing this check actually polices.
#
# So the terminal state has two halves, and only the first is what this budget measures:
#
#   1. APP-CODE COUNT -> 0. Every file under targets/esp32-idf/ stops including a shim header.
#      Reachable: the last three (buzzer_hw.cpp, device_control.cpp, encryption.cpp) are
#      counted only for their TARGET_NRF arms, which leave at MIGRATION step 4.
#   2. THIS DIRECTORY IS NOT DELETED. The Arduino primitives the vendored libraries link
#      against -- delay(), delayMicroseconds(), millis() with C++ linkage, and ledc_compat.h --
#      are extracted into the vendor adapter and OWNED there, rather than borrowed from a
#      directory that was supposed to die.
#
# When the count reaches 0: extract the vendor adapter, delete what is left, and retire this
# check with it. Do NOT read "budget is 0" as "delete compat/" -- the build needs what the
# vendored libraries link against, and a passing check that tells you to break the build is
# worse than no check.
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
    grep -rlE '#[[:space:]]*include[[:space:]]*[<"][[:space:]]*(arduino_compat|Arduino|Wire|SPI|WiFi|ledc_compat|esp32-hal-gpio|HardwareSerial)\.h' \
         targets/esp32-idf --include='*.c' --include='*.cpp' --include='*.h' \
         --include='*.hpp' --include='*.inl' 2>/dev/null \
        | grep -v '^targets/esp32-idf/compat/' || true
}

# Vendored libraries include the shim too, and the grep above never saw them: it is scoped to
# targets/esp32-idf, so third_party/ was a blind spot. bb_epaper and FastEPD ARE Arduino
# libraries -- depending on Arduino.h/Wire.h/SPI.h is their normal state, not creeping shim
# usage -- so they do NOT belong in the app-code budget, which measures OUR migration.
#
# This list is therefore not a backlog. It is the SPECIFICATION OF THE VENDOR ADAPTER: the set
# of Arduino primitives that must still exist after phase C, because FastEPD needs a permanent
# compatibility layer (decided 2026-08-04). Reported so the set is visible and reviewable, and
# never enforced, because there is no number here that is supposed to fall.
third_party_shim_users() {
    grep -rlE '#[[:space:]]*include[[:space:]]*[<"][[:space:]]*(arduino_compat|Arduino|Wire|SPI|WiFi|ledc_compat|esp32-hal-gpio|HardwareSerial)\.h' \
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
        echo "      Not counted: these are vendored Arduino libraries, and this list is the"
        echo "      VENDOR ADAPTER's specification rather than a backlog -- FastEPD needs a"
        echo "      permanent compatibility layer. See third_party/NOTICE.md and the note at"
        echo "      the top of this script."
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
    echo "Budget is 0: no app code under targets/esp32-idf/ includes a shim header."
    echo
    echo "That is the whole of what this check measures, and it is NOT a licence to delete"
    echo "$COMPAT_DIR. FastEPD needs a permanent compatibility layer, so the Arduino"
    echo "primitives the vendored libraries link against -- delay(), delayMicroseconds(),"
    echo "millis() with C++ linkage, ledc_compat.h -- outlive phase C by decision, not by"
    echo "omission. See the note at the top of this script."
    echo
    echo "Next: extract those into the VENDOR ADAPTER, owned there rather than borrowed from"
    echo "here; delete what remains of $COMPAT_DIR; retire this check and"
    echo ".github/workflows/esp32-shim-ratchet.yml together."
    if [ -d third_party ] && [ "$(third_party_shim_users | wc -l | tr -d '[:space:]')" -gt 0 ]; then
        echo
        echo "Still linked from third_party/ (the files listed above) -- this is the set the"
        echo "vendor adapter has to satisfy, not a backlog to clear."
    fi
fi

echo "OK: shim usage is at its recorded budget."
