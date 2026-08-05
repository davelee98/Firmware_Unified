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
# compatibility layer -- it is a vendored Arduino library whose IT8951 transport is written
# against the Arduino SPI object, and there is no version of it that is not. That surviving
# piece is the VENDOR ADAPTER, and it is deliberately not called a shim: "shim" in this repo
# means scheduled demolition, and using the word for something permanent poisons it for the
# thing this check actually polices.
#
# THE ADAPTER IS SMALLER THAN THIS FILE USED TO CLAIM, AND IT NO LONGER LIVES HERE.
# Corrected 2026-08-04, after panel/od_bbep_idf_io.inl landed:
#
#   - It is ONE header plus its storage: targets/esp32-idf/vendor/fastepd/{SPI.h,
#     fastepd_adapter.cpp}. Earlier text here said the adapter would own delay(),
#     delayMicroseconds(), millis() and ledc_compat.h. That was written when bb_epaper still
#     used an Arduino IO backend. It does not: panel/od_bbep.cpp replaces the vendored
#     bb_epaper.cpp with our own IDF backend, and it drops the unused BBEPAPER C++ class that
#     was the only consumer of pinMode() and millis(). bb_epaper needs NOTHING from compat/.
#   - FastEPD borrows exactly one loose symbol, millis(), for arduino_io.inl'"'"'s 19 call sites.
#     It defines delay() and delayMicroseconds() itself against ets_delay_us. millis() is still
#     in compat/ because app code also still calls it; it moves to the adapter when the last
#     app caller goes to od_hal_uptime_ms().
#   - vendor/fastepd/ IS NOT ON THE COMPONENT'"'"'S INCLUDE PATH. main/CMakeLists.txt grants it to
#     a named list of translation units, so no new file can acquire an Arduino SPI dependency
#     by typing one include. That containment is the reason the adapter can be permanent
#     without being a leak.
#
# So the terminal state has two halves, and only the first is what this budget measures:
#
#   1. APP-CODE COUNT -> 4. Every file under targets/esp32-idf/ stops including a SHIM header.
#      The floor is 4, not 0: main.cpp, buzzer_hw.cpp, device_control.cpp and encryption.cpp are
#      counted only for their TARGET_NRF arms, which cannot be compiled or verified on this
#      target and leave with MIGRATION step 4.
#   2. compat/ IS DELETED, and vendor/fastepd/ IS NOT. They are separate directories precisely
#      so that "delete compat/" is an unambiguous instruction rather than a judgement call.
#
# When the count reaches 4: confirm the remainder is nRF-only, delete compat/, and retire this
# check with it. vendor/fastepd/ stays.
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
# vendor/ is excluded for a DIFFERENT reason, and the distinction matters: compat/ is skipped
# because it IS the thing being measured, while vendor/fastepd/ is skipped because it is not a
# shim at all. It is the permanent FastEPD adapter, it owns <SPI.h> rather than borrowing it,
# and counting it would make the budget rise every time the adapter gains a file -- turning a
# ratchet on temporary code into a tax on permanent code.
#
# Widened 2026-07-25 to every shim header, not just arduino_compat.h/Arduino.h. Phase B added
# Wire.h, SPI.h, WiFi.h, ledc_compat.h and esp32-hal-gpio.h, and a file that includes Wire.h
# but not Arduino.h is just as much a shim user. Measured both ways at the time: 20 under the
# old pattern, 21 under this one. The shim did NOT grow -- the METRIC did, by one file. That
# distinction is recorded here because "we widened the definition" is otherwise
# indistinguishable from "we let it grow", and a ratchet that cannot tell them apart is not
# a ratchet.
#
# SPI.h WAS REMOVED FROM THIS PATTERN on 2026-08-04, and it is the only header ever removed.
# It is no longer a shim header: it is targets/esp32-idf/vendor/fastepd/SPI.h, the PERMANENT
# FastEPD vendor adapter. Counting it would have pinned the floor one file above where the
# migration can actually finish -- display_fastepd.cpp includes it and always will, so the
# budget could never reach the point where compat/ is deletable, which is the single question
# this check exists to answer.
#
# THIS IS A DEFINITION CHANGE, NOT PROGRESS, and the same rule applies as when the pattern was
# WIDENED in phase B: say so, or a later reader cannot tell it apart from work. Measured both
# ways at the time -- 6 under the old pattern, 6 under this one. THE NUMBER DID NOT MOVE,
# because the one file affected (display_fastepd.cpp) is still counted for its <Arduino.h>.
# What changed is the FLOOR: 6 before, 5 after, reachable once that include goes.
#
# The obvious risk is that a genuine shim SPI header could now reappear in compat/ and slip
# past unnoticed. The check below closes that: compat/SPI.h existing is a hard failure.
#
# NOTE the `|| true`: grep exits 1 when it matches nothing, which under `set -e` + `pipefail`
# aborts the script on the healthiest possible state -- zero shim users. Without it this check
# fails hardest exactly when the port has succeeded.
shim_users() {
    grep -rlE '#[[:space:]]*include[[:space:]]*[<"][[:space:]]*(arduino_compat|Arduino|Wire|WiFi|ledc_compat|esp32-hal-gpio|HardwareSerial)\.h' \
         targets/esp32-idf --include='*.c' --include='*.cpp' --include='*.h' \
         --include='*.hpp' --include='*.inl' 2>/dev/null \
        | grep -v '^targets/esp32-idf/compat/' \
        | grep -v '^targets/esp32-idf/vendor/' || true
}

# Vendored libraries include shim headers too, and the grep above never saw them: it is scoped
# to targets/esp32-idf, so third_party/ was a blind spot.
#
# THIS LIST IS NOT THE ADAPTER'"'"'S SPECIFICATION, whatever the previous version of this comment
# said. It is a TEXT grep that does not evaluate #ifdef, so most of what it prints is
# unreachable in this build and specifies nothing:
#
#   bb_epaper/src/bb_epaper.h        <Arduino.h> is behind #ifdef ARDUINO. Not defined here --
#                                    the build takes the __LINUX__ branch. Nothing is included.
#   bb_epaper/src/arduino_io.inl     NEVER COMPILED. panel/od_bbep.cpp replaces bb_epaper.cpp
#   bb_epaper/src/esphome_io.inl     and includes panel/od_bbep_idf_io.inl instead.
#   FastEPD/src/FastEPD.h            <Arduino.h> behind #ifdef ARDUINO. Not defined here.
#   FastEPD/src/FastEPD.cpp          REAL: includes <SPI.h> under OD_FASTEPD_IDF_SPI (OD-PATCH).
#   FastEPD/src/FastEPD.inl          REAL: the IT8951 transport, same OD_FASTEPD_IDF_SPI branch.
#
# So the adapter exists for TWO files, both FastEPD'"'"'s, and they are served by
# vendor/fastepd/SPI.h rather than by anything under compat/. Printed for visibility, never
# enforced, because there is no number here that is supposed to fall.
third_party_shim_users() {
    grep -rlE '#[[:space:]]*include[[:space:]]*[<"][[:space:]]*(arduino_compat|Arduino|Wire|SPI|WiFi|ledc_compat|esp32-hal-gpio|HardwareSerial)\.h' \
         third_party --include='*.c' --include='*.cpp' --include='*.h' \
         --include='*.hpp' --include='*.inl' 2>/dev/null || true
}

# See the pattern note above: <SPI.h> is deliberately no longer counted, because it resolves to
# the permanent vendor adapter. That is only safe while no SHIM SPI header exists -- if one
# reappears under compat/, every file including it would silently drop off the count.
if [ -f "$COMPAT_DIR/SPI.h" ]; then
    echo "::error::$COMPAT_DIR/SPI.h exists, but <SPI.h> is no longer counted by this check."
    echo "SPI.h was removed from the pattern because it is the FastEPD vendor adapter"
    echo "(targets/esp32-idf/vendor/fastepd/SPI.h), which is permanent and must not be"
    echo "ratcheted. A shim SPI header under compat/ would therefore be invisible here."
    echo "Either delete it, or put SPI back in the pattern above and raise the budget."
    exit 1
fi

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
        echo "      NOT COUNTED, and NOT a specification: this is a text grep that does not"
        echo "      evaluate #ifdef. Only FastEPD.cpp and FastEPD.inl actually include anything"
        echo "      here (<SPI.h>, under OD_FASTEPD_IDF_SPI). The bb_epaper entries are dead --"
        echo "      arduino_io.inl and esphome_io.inl are never compiled (panel/od_bbep.cpp"
        echo "      supplies our own IDF backend) and the two *.h hits sit behind an"
        echo "      #ifdef ARDUINO this build does not define. The adapter that serves the two"
        echo "      real files is targets/esp32-idf/vendor/fastepd/, not compat/."
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

if [ "$actual" -le 4 ]; then
    echo
    echo "Count is at the FLOOR (4): the only files left include a shim header for their"
    echo "TARGET_NRF arms -- main.cpp, buzzer_hw.cpp, device_control.cpp, encryption.cpp."
    echo "None of those arms compiles on this target, so none can be verified here; they leave"
    echo "with the nRF target at MIGRATION step 4."
    echo
    echo "That is the whole of what this check measures, and it is NOT a licence to delete"
    echo "the FastEPD vendor adapter. vendor/fastepd/{SPI.h,fastepd_adapter.cpp} is PERMANENT"
    echo "and lives outside $COMPAT_DIR precisely so that 'delete compat/' stays unambiguous."
    echo
    echo "Next: confirm the remainder is nRF-only, delete $COMPAT_DIR, and retire this check"
    echo "and .github/workflows/esp32-shim-ratchet.yml together. Leave vendor/fastepd alone."
fi

echo "OK: shim usage is at its recorded budget."
