#!/usr/bin/env bash
# check.sh -- every gate this repo has, run locally.
#
# There is no CI. The checks that used to live in .github/workflows/ are here instead, which
# means NOTHING RUNS UNLESS SOMEONE RUNS IT. Run this before you push.
#
#   tools/check.sh                 boundary + host suites + sanitizers + fuzz + arduino-free
#   tools/check.sh --targets       also builds ESP32, Nordic, and Silabs targets. Required before
#                                  merge.
#   tools/check.sh --esp32         ESP32 only, as --targets used to be
#   tools/check.sh --fuzz-time 300 longer fuzz budget per target (default 60 s)
#   tools/check.sh --latest        also replay the corpus against the NEWEST py-opendisplay
#   tools/check.sh --list          print the checks and exit
#
# A SKIP IS NOT A PASS. Several checks need a toolchain that may be absent -- clang, ESP-IDF,
# a Python that can install py-opendisplay. Those SKIP rather than fail, but they are counted
# and reprinted in the summary, and the exit status is 2 when anything was skipped. Silent
# skipping is how a suite comes to mean nothing: "all green" has to distinguish "checked" from
# "could not check".

set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

FUZZ_TIME=60
DO_ESP32=0
DO_NORDIC=0
DO_SILABS=0
DO_LATEST=0

while [ $# -gt 0 ]; do
    case "$1" in
        --esp32)      DO_ESP32=1 ;;
        --nordic)     DO_NORDIC=1 ;;
        --silabs)     DO_SILABS=1 ;;
        --targets)    DO_ESP32=1; DO_NORDIC=1; DO_SILABS=1 ;;
        --latest)     DO_LATEST=1 ;;
        --fuzz-time)  FUZZ_TIME="${2:?--fuzz-time needs a value}"; shift ;;
        --list)
            grep -oP '^\s*check "\K[^"]+' "$0" | sed 's/^/  /'
            exit 0 ;;
        -h|--help)    sed -n '2,20p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *)            echo "unknown argument: $1" >&2; exit 64 ;;
    esac
    shift
done

PASS=0; FAIL=0; SKIP=0
FAILED_NAMES=(); SKIPPED_NAMES=()
BUILD_ROOT="$REPO/build/check"
mkdir -p "$BUILD_ROOT"

say()  { printf '\n\033[1m== %s\033[0m\n' "$1"; }
ok()   { printf '\033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '\033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); FAILED_NAMES+=("$1"); }
skip() { printf '\033[33mSKIP\033[0m  %s -- %s\n' "$1" "$2"; SKIP=$((SKIP+1)); SKIPPED_NAMES+=("$1 -- $2"); }

# check <name> <command...>  -- runs the command, capturing output, reporting only on failure.
check() {
    local name="$1"; shift
    say "$name"
    local log="$BUILD_ROOT/$(echo "$name" | tr ' /' '__').log"
    if "$@" >"$log" 2>&1; then
        ok "$name"
    else
        bad "$name"
        echo "--- output ($log) ---"
        tail -40 "$log"
    fi
}

# ==================================================================================== boundary ==
# Ported verbatim from .github/workflows/shared-boundary.yml. A GREP ONLY: it sees #include
# lines and literal tokens, and cannot see an extern-declared vendor symbol, a macro leaked in
# through a target config header, or a libc call absent on a freestanding target. Compiling
# shared/ for the host under both compilers is the load-bearing check; this is the fast one.
boundary_includes() {
    # Header patterns that pin a file to one SDK. Extend as targets are imported. Note the
    # ESP-IDF families beyond esp_*: driver/, soc/, hal/, rom/ and freertos/ are all IDF-only.
    # miniz.h is the ROM tinfl header -- the portable inflate engine belongs in shared/core, the ROM
    # binding does not. Panel libraries are target-layer: shared/ reaches the panel only through
    # od_hal_panel.
    local pattern='#[[:space:]]*include[[:space:]]*[<"]([[:space:]]*)?(esp_|esp32|driver/|soc/|hal/|rom/|miniz|freertos/|nrf_|nrfx|nrf\.h|sl_|sli_|em_|zephyr/|Arduino\.h|WiFi\.h|ESPmDNS|bluefruit\.h|Adafruit|NimBLE|bb_epaper|FastEPD|TFT_eSPI|Seeed)'
    # shared/hal's own headers are od_hal_*.h and may legitimately be included as
    # "hal/od_hal_panel.h", which the IDF `hal/` family above would otherwise flag. Anchor the
    # exemption on the INCLUDE TARGET, not the line: a bare `grep -v od_hal_` would excuse
    # `#include <esp_wifi.h> /* od_hal_ */`.
    local exempt='#[[:space:]]*include[[:space:]]*[<"][[:space:]]*hal/od_hal_'
    # third_party/ is deliberately NOT scanned: bb_epaper selects its IO backend by #ifdef and
    # every backend includes vendor headers, so it can never satisfy this rule -- but it is one
    # vendored copy shared by all targets, not a per-target fork. Do not "fix" it by moving it
    # under shared/.
    local hits
    hits=$(grep -rInE "$pattern" shared/ | grep -vE "$exempt" || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo
        echo "vendor/framework include found under shared/."
        echo "shared/ must use only the C standard library and shared/hal interfaces."
        echo "Move the offending file to targets/<target>/, or route the dependency through a"
        echo "shared/hal interface. See docs/ARCHITECTURE.md."
        return 1
    fi
}

boundary_c_only() {
    # shared/ must be plain C: two targets are C-only, and the Zephyr sources that are the best
    # donor for shared/core are already C.
    if find shared/ \( -name '*.cpp' -o -name '*.cc' -o -name '*.hpp' \) | grep -q . ; then
        echo "C++ source found under shared/. shared/ is plain C."
        find shared/ \( -name '*.cpp' -o -name '*.cc' -o -name '*.hpp' \)
        return 1
    fi
    # Scoped to C sources on purpose: unscoped, this fires on prose in any shared/**/README.md
    # ("a length+ptr pair rather than a String"), and a check that cries wolf on documentation
    # is a check someone eventually deletes.
    if grep -rInE --include='*.c' --include='*.h' '\bString\b' shared/ ; then
        echo "Arduino String found under shared/. Use char[] or a length+ptr pair."
        return 1
    fi
}

boundary_protocol() {
    # shared/protocol/ is a byte-for-byte copy of the canonical opendisplay-protocol source: pure
    # constants and config structs, no behaviour. Change the canonical source and re-run
    # tools/sync_protocol_header.py --push instead of editing these.
    if grep -rInE '^[[:space:]]*(void|int|bool|uint[0-9]+_t|static)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' shared/protocol/ ; then
        echo
        echo "function declaration found in shared/protocol/ -- these files are synced, not written."
        return 1
    fi
}

check "shared boundary: no vendor includes"   boundary_includes
check "shared boundary: C only, no String"    boundary_c_only
check "shared boundary: protocol headers"     boundary_protocol

# od_session.c compares attacker-supplied bytes -- an auth proof and a session id -- and must do
# it in od_ct_equal, whose fixed-iteration volatile loop is the whole defence. A HOST TEST CANNOT
# CATCH THIS: replacing od_ct_equal with memcmp passes the entire suite, verified by mutation.
# It is exactly the regression AUDIT_NORDIC_ZEPHYR_2026-08-14.md:259 records shipping unnoticed on
# another target, so the enforcement has to be structural.
session_constant_time() {
    local hits
    hits=$(grep -nE '\bmemcmp[[:space:]]*\(' shared/core/od_session.c || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo
        echo "memcmp in od_session.c: comparisons of attacker-supplied bytes belong in"
        echo "od_ct_equal(), which is fixed-iteration with a volatile accumulator. If this is a"
        echo "comparison of non-secret data, move it behind a helper with a name that says so."
        return 1
    fi
}
check "shared boundary: od_session uses no memcmp"  session_constant_time

# ARCHITECTURE, RATCHETED BY SYMBOL. Each name below is one whose ABSENCE is the property: a
# second opcode map, an implicit frame context, an exported session singleton. Grepping for them
# is crude but it is what stops a well-meaning "just for now" reintroduction, and unlike a line
# count it says what is actually wrong when it fires.
#
# Scoped to targets/*/src and targets/*/hal so a build directory's .map files cannot trip it.
c11_structure() {
    local rc=0 hits
    scan() {
        local what="$1" pattern="$2" why="$3"
        hits=$(grep -rInE "$pattern" targets/*/src targets/*/hal shared/ \
               targets/efr32bg22-slc/*.[ch] targets/efr32bg22-slc/*.cpp 2>/dev/null || true)
        if [ -n "$hits" ]; then
            echo "$hits"; echo; echo "$what: $why"; echo
            rc=1
        fi
    }
    # The opcode map is od_dispatch.c's. A target definition of this is a second map, and two maps
    # is how one target answers an opcode the other treats as unknown.
    scan "od_cmd_dispatch" '\bod_cmd_dispatch[[:space:]]*\(' \
         "the per-command seam is od_cmd_app.h; the opcode map is shared."
    # A frame context stored in a global outlives its frame: any nested or later path can read it
    # after the caller has moved on, and no compiler can catch that. Both ESP32 ingresses build an
    # od_reply_t and pass it.
    scan "implicit frame context" '\b(g_commandOrigin|g_commandInstance|commandOrigin|imageDataWritten)\b' \
         "build an od_reply_t at the ingress and pass it to od_dispatch_app_frame()."
    # The session object is private to its od_session_app translation unit. A name reachable from
    # elsewhere is one a caller can memset -- and memset is not teardown here: the key lives in an
    # od_hal_crypto slot, so zeroing the struct strands a prepared key in a finite pool and loses
    # the slot index with it. od_session_clear() is the only teardown.
    scan "exported session singleton" '\b(g_session|od_pipe_session|od_pipe_device_id)\b' \
         "reach the session through od_session_app_state()."
    # Confidentiality is chosen at the CALL SITE. Inferring it from response bytes is how this
    # firmware once sealed its own rejection frames, which the host then validated as ACKs.
    scan "byte-inferred sealing" 'response\[(0|2)\][[:space:]]*==[[:space:]]*(RESP_NACK|0xFF)' \
         "call od_reply_plain() explicitly; never inspect the frame to decide."
    return $rc
}
check "structure: ownership ratchets"  c11_structure

od_color_structure() {
    local rc=0 hits count
    for path in \
        targets/nordic-zephyr/src/opendisplay_display_color.c \
        targets/nordic-zephyr/src/opendisplay_display_color.h \
        targets/efr32bg22-slc/opendisplay_display_color.c \
        targets/efr32bg22-slc/opendisplay_display_color.h; do
        if [ -e "$path" ]; then
            echo "retired target color helper returned: $path"
            rc=1
        fi
    done

    hits=$(grep -rInE '\bopendisplay_color_[A-Za-z0-9_]*\b|\bcalc_controller_plane_bytes\b|\b(getBitsPerPixel|getplane)[[:space:]]*\(' \
           targets/*/src targets/*/panel targets/efr32bg22-slc/*.[ch] \
           targets/efr32bg22-slc/*.cpp 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "legacy target-local direct-stream color geometry returned; use shared/core/od_color.h"
        rc=1
    fi

    # NAME THE FILE, NEVER THE DIRECTORY. cmake_gcc/ contains build/, whose generated ninja
    # files, compile_commands.json and linker map all mention this source -- so a recursive grep
    # counts 6 and this check fails IFF BG22 has been built, which --targets does in the same run.
    count=$(grep -RFl 'core/od_color.c' shared/sources.cmake targets/*/CMakeLists.txt \
            targets/*/*/CMakeLists.txt targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake \
            2>/dev/null | wc -l)
    if [ "$count" -ne 1 ] || ! grep -q 'core/od_color.c' shared/sources.cmake; then
        echo "od_color.c must be registered exactly once, in shared/sources.cmake (found $count)"
        rc=1
    fi
    return $rc
}
check "structure: od_color is the direct geometry authority"  od_color_structure

inflate_pump_structure() {
    local rc=0 hits count
    local target_sources=(
        targets/esp32-idf/src
        targets/nordic-zephyr/src
        targets/efr32bg22-slc/*.c
        targets/efr32bg22-slc/*.cpp
    )

    # Targets reach an inflater only through their selected od_inflate_app adapter. Direct engine
    # calls would restore a target-local push/poll loop and bypass the shared final/accounting
    # policy.
    hits=$(grep -rInE '\bod_zlib_stream_(reset|push|poll|error|output_count)[[:space:]]*\(' \
           "${target_sources[@]}" 2>/dev/null \
           | grep -vE '/od_inflate_app\.(c|cpp):' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "portable inflater bypassed od_inflate_app; target transfer code must call od_zlib_pump"
        rc=1
    fi

    hits=$(grep -rInE '\bod_inflate_tinfl_(reset|push|poll|error|output_count)[[:space:]]*\(' \
           "${target_sources[@]}" 2>/dev/null \
           | grep -vE '/od_inflate_(app|tinfl)\.(cpp|h):' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "tinfl bypassed od_inflate_app; target transfer code must call od_zlib_pump"
        rc=1
    fi

    hits=$(grep -rInE '\bod_inflate_app_(reset|push|poll|error|output_count)[[:space:]]*\(' \
           "${target_sources[@]}" 2>/dev/null \
           | grep -vE '/od_inflate_app\.(c|cpp):' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "target transfer code bypassed od_zlib_pump through the backend seam"
        rc=1
    fi

    count=$(grep -RFl 'core/od_zlib_pump.c' shared/sources.cmake targets/*/CMakeLists.txt \
            targets/*/*/CMakeLists.txt targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake \
            2>/dev/null | wc -l)
    if [ "$count" -ne 1 ] || ! grep -q 'core/od_zlib_pump.c' shared/sources.cmake; then
        echo "od_zlib_pump.c must be registered exactly once, in shared/sources.cmake (found $count)"
        rc=1
    fi
    return $rc
}
check "structure: od_zlib_pump owns inflater progression"  inflate_pump_structure

silabs_c13_config() {
    grep -q '^#define PSA_WANT_ALG_CCM 1$' \
        targets/efr32bg22-slc/config/od_psa_config_autogen.h || {
        echo "Silabs tracked PSA CCM selection missing"; return 1; }
    grep -A2 -q 'id: psa_crypto_ccm' targets/efr32bg22-slc/opendisplay-bg22.slcp || {
        echo "Silabs SLC project no longer selects PSA CCM"; return 1; }
    grep -q '^#define SL_PSA_KEY_USER_SLOT_COUNT[[:space:]]\+1$' \
        targets/efr32bg22-slc/config/psa_crypto_config.h || {
        echo "Silabs application PSA key-slot reserve is not one"; return 1; }
    grep -A1 -q "name: SL_PSA_KEY_USER_SLOT_COUNT[[:space:]]*$" \
        targets/efr32bg22-slc/opendisplay-bg22.slcp || {
        echo "Silabs SLC project lost its PSA key-slot setting"; return 1; }
    grep -A1 "name: SL_PSA_KEY_USER_SLOT_COUNT[[:space:]]*$" \
        targets/efr32bg22-slc/opendisplay-bg22.slcp | grep -q "value: '1'" || {
        echo "Silabs SLC project would regenerate the PSA key-slot reserve incorrectly"; return 1; }
    grep -q '^#define SL_BGAPI_MAX_PAYLOAD_SIZE[[:space:]]\+(263)$' \
        targets/efr32bg22-slc/config/sl_bgapi_config.h || {
        echo "Silabs BGAPI payload no longer supports ATT MTU 256"; return 1; }
    grep -A2 -q 'id: bluetooth_feature_resource_report' \
        targets/efr32bg22-slc/opendisplay-bg22.slcp || {
        echo "Silabs TX completion reporting component missing from tracked SLC input"; return 1; }
    if grep -q '^#define SL_CATALOG_KERNEL_PRESENT' \
        targets/efr32bg22-slc/autogen/sl_component_catalog.h; then
        echo "Silabs kernel component invalidates the BGAPI event-retention design"; return 1
    fi
}
check "silabs: C13 config ratchets" silabs_c13_config

silabs_advertising_ownership() {
    local count close_calls

    if grep -q '\bopendisplay_ble_restart_advertising[[:space:]]*(' \
        targets/efr32bg22-slc/app.c targets/efr32bg22-slc/opendisplay_ble.h; then
        echo "Silabs advertising restart escaped the BLE event-layer owner"
        grep -n '\bopendisplay_ble_restart_advertising[[:space:]]*(' \
            targets/efr32bg22-slc/app.c targets/efr32bg22-slc/opendisplay_ble.h
        return 1
    fi
    count=$(grep -c '\bopendisplay_ble_restart_advertising[[:space:]]*(' \
        targets/efr32bg22-slc/opendisplay_ble.c)
    if [ "$count" -ne 2 ]; then
        echo "Silabs restart must have one private definition and one close-event call; found $count"
        return 1
    fi
    close_calls=$(sed -n '/case sl_bt_evt_connection_closed_id:/,/^[[:space:]]*break;/p' \
        targets/efr32bg22-slc/opendisplay_ble.c \
        | grep -c '\bopendisplay_ble_restart_advertising[[:space:]]*(')
    if [ "$close_calls" -ne 1 ]; then
        echo "Silabs close-event handler must restart advertising exactly once; found $close_calls"
        return 1
    fi
}
check "silabs: advertising lifecycle ownership" silabs_advertising_ownership

# ================================================================================== host suites ==
# The real boundary enforcement: shared/ compiled for the host at -std=c99 -Wall -Wextra -Werror
# under BOTH compilers. A GNU-ism gcc accepts is a failure discovered later on a target instead.
host_suite() {
    local cc="$1" dir="$BUILD_ROOT/host-$cc"
    command -v "$cc" >/dev/null 2>&1 || return 127
    cmake -S tests/host -B "$dir" -DCMAKE_C_COMPILER="$cc" >/dev/null \
        && cmake --build "$dir" -j"$(nproc)" \
        && ctest --test-dir "$dir" --output-on-failure --no-tests=error
}

for cc in gcc clang; do
    if command -v "$cc" >/dev/null 2>&1; then
        check "host suite ($cc)" host_suite "$cc"
    else
        skip "host suite ($cc)" "$cc not installed"
    fi
done

# ASan + UBSan over every host test at once -- including the two PRE-AUTH parsers, od_config_tlv
# and od_session, whose inputs an unauthenticated peer controls. halt_on_error is not decoration:
# UBSan's default is to print a diagnostic and carry on with status 0.
host_sanitizers() {
    local dir="$BUILD_ROOT/host-asan"
    cmake -S tests/host -B "$dir" \
          -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
          -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" >/dev/null \
        && cmake --build "$dir" -j"$(nproc)" \
        && UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ASAN_OPTIONS=detect_leaks=1 \
           ctest --test-dir "$dir" --output-on-failure --no-tests=error
}
check "host suite (ASan + UBSan)" host_sanitizers

# ======================================================================================== fuzz ==
# MIGRATION.md Gate 1 requires fuzz coverage for anything reachable pre-authentication. Clang
# only: -fsanitize=fuzzer is a Clang feature and gcc has no libFuzzer. The corpora under
# tests/fuzz/corpus/ are committed, so each run starts from the coverage the last one found.
#
# A crash writes its input to ./crash-<sha1>. Commit it to the corpus AND pin it as a numbered
# case in tests/host/session_test.c, so the regression fails the ordinary run too.
FUZZ_TARGETS=(session_open_raw session_open_sealed session_auth)

fuzz_all() {
    local dir="$BUILD_ROOT/fuzz" t
    cmake -S tests/host -B "$dir" -DCMAKE_C_COMPILER=clang >/dev/null \
        && cmake --build "$dir" -j"$(nproc)" || return 1
    # If tests/fuzz ever stops being registered -- a renamed guard, a moved directory -- the loop
    # below would pass with nothing to run. Fail loudly instead.
    for t in "${FUZZ_TARGETS[@]}"; do
        if [ ! -x "$dir/fuzz/od_fuzz_$t" ]; then
            echo "od_fuzz_$t was not built -- the pre-auth surface is UNFUZZED"
            return 1
        fi
    done
    for t in "${FUZZ_TARGETS[@]}"; do
        echo "--- $t (${FUZZ_TIME}s)"
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            "$dir/fuzz/od_fuzz_$t" -max_total_time="$FUZZ_TIME" -runs=2000000 \
            -print_final_stats=1 "tests/fuzz/corpus/$t" || return 1
    done
}

if command -v clang >/dev/null 2>&1; then
    check "fuzz: pre-auth surface (${FUZZ_TIME}s x ${#FUZZ_TARGETS[@]})" fuzz_all
else
    skip "fuzz: pre-auth surface" "clang not installed; libFuzzer is Clang-only"
fi

# ============================================================================== wire corpus ==
# The cross-repo wire contract: the same vectors decoded by this repo's C core and by the
# py-opendisplay release Home_Assistant_Integration pins. py-opendisplay 7.14.0 needs Python
# >= 3.13, which the system Python may not be -- uv fetches one rather than failing on install.
replay_pinned() {
    if command -v uv >/dev/null 2>&1; then
        uv run --python 3.13 --with 'py-opendisplay==7.14.0' \
            python tests/host/replay_vectors.py tests/vectors
    else
        python3 -c 'import opendisplay' 2>/dev/null || return 127
        python3 tests/host/replay_vectors.py tests/vectors
    fi
}
if command -v uv >/dev/null 2>&1 || python3 -c 'import opendisplay' 2>/dev/null; then
    # Guarded above rather than relying on the 127 return, which check() would report as a FAIL:
    # a missing toolchain is a skip everywhere else in this file and must be here too.
    check "wire corpus vs py-opendisplay 7.14.0 (pinned)" replay_pinned
else
    skip "wire corpus vs py-opendisplay 7.14.0 (pinned)" "needs uv, or py-opendisplay importable"
fi

# The nightly advisory check that used to catch upstream drift within 24 h. WITHOUT CI THIS ONLY
# RUNS WHEN ASKED, so a py-opendisplay release that breaks the wire contract is now found
# whenever someone next passes --latest, not the next morning. That is the real cost of having
# no scheduled runner; run it before any release.
replay_latest() {
    command -v uv >/dev/null 2>&1 || return 127
    uv run --python 3.13 --with py-opendisplay python tests/host/replay_vectors.py tests/vectors
}
if [ "$DO_LATEST" = 1 ]; then
    if command -v uv >/dev/null 2>&1; then
        check "wire corpus vs py-opendisplay latest (advisory)" replay_latest
    else
        skip "wire corpus vs py-opendisplay latest (advisory)" "needs uv"
    fi
fi

# ======================================================================================= esp32 ==
# THE SHIM IS GONE, and this is what keeps it gone. It replaces compat/ratchet.sh, whose metric
# was "how many files INCLUDE a shim header" -- a count that reached 0 while three call sites
# were still reaching shim primitives, because `delay(long)` and `millis()` are declared by an
# OD-PATCH in third_party/bb_epaper/src/bb_epaper.h, not by any header the ratchet grepped for.
# So this checks CALLS, which is the property that actually matters.
#
# panel/ and vendor/ are exempt BY DESIGN, not by oversight: od_bbep_idf_io.inl and
# fastepd_adapter.cpp exist precisely to supply these primitives to the two vendored libraries
# that demand them, and third_party/ is exempt from the one rule (CLAUDE.md, decision 13).
arduino_free() {
    local rc=0

    # SPI.h is deliberately NOT in this list: the only one in the tree is the PERMANENT FastEPD
    # adapter, vendor/fastepd/SPI.h, and counting it would make this check fail forever on a
    # file that is meant to include it. The guard against a shim SPI.h reappearing is the next
    # block, which pins where SPI.h may live -- compat/ used to hold that risk and is now gone.
    local hdrs='(arduino_compat|Arduino|Wire|WiFi|ledc_compat|esp32-hal-gpio|HardwareSerial)\.h'
    local hits
    hits=$(grep -rlE "#[[:space:]]*include[[:space:]]*[<\"][[:space:]]*$hdrs" \
             targets/esp32-idf --include='*.c' --include='*.cpp' --include='*.h' \
             --include='*.hpp' --include='*.inl' 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "::error::Arduino header included under targets/esp32-idf/:"
        echo "$hits" | sed 's/^/    /'
        rc=1
    fi

    hits=$(find targets/esp32-idf -name 'SPI.h' -not -path '*/build/*' \
           | grep -v '^targets/esp32-idf/vendor/fastepd/SPI\.h$' || true)
    if [ -n "$hits" ]; then
        echo "::error::an SPI.h outside the FastEPD vendor adapter -- the header pattern above"
        echo "         does not count <SPI.h>, so files including this one would be invisible:"
        echo "$hits" | sed 's/^/    /'
        rc=1
    fi

    # Calls to Arduino primitives in APP code. panel/ and vendor/ are exempt BY DESIGN:
    # od_bbep_idf_io.inl and fastepd_adapter.cpp exist precisely to supply these to the two
    # vendored libraries that demand them, and third_party/ is exempt from the one rule
    # (CLAUDE.md, decision 13).
    #
    # The awk pass strips comments before matching. Without it the check fires on prose --
    # "millis()-poll from loop()" in a header comment is not a call, and a gate that cannot
    # tell those apart gets disabled rather than obeyed.
    #
    # `[(]` rather than `\(`: the pattern crosses shell -> awk -v -> dynamic regex, and a
    # backslash does not survive that trip. It arrived as a bare `(`, which made the regex
    # invalid, and awk died on the first file -- while the check still reported OK, because the
    # `|| true` that was here swallowed the status. A gate that cannot fail is worse than none,
    # so the scan's own failure is now a failure.
    local prims='(pinMode|digitalWrite|digitalRead|analogRead|analogWrite|analogReadResolution|analogSetPinAttenuation|delay|delayMicroseconds|millis|micros|temperatureRead|yield)'
    hits=$(find targets/esp32-idf/src targets/esp32-idf/hal targets/esp32-idf/ble \
                targets/esp32-idf/main -type f \
                \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.inl' \) 2>/dev/null \
           | xargs awk -v pat="(^|[^_[:alnum:]>.])$prims[[:space:]]*[(]" '
               FNR == 1 { inblock = 0 }
               { line = $0
                 if (inblock) { if (sub(/.*\*\//, "", line)) inblock = 0; else next }
                 gsub(/\/\*[^*]*\*\//, "", line)
                 if (sub(/\/\*.*/, "", line)) inblock = 1
                 sub(/\/\/.*/, "", line)
                 if (line ~ pat) printf "%s:%d:%s\n", FILENAME, FNR, $0
               }') || { echo "::error::the Arduino-primitive scan failed to run"; return 1; }
    if [ -n "$hits" ]; then
        echo "::error::Arduino primitive called from app code (use od_hal_*):"
        echo "$hits" | sed 's/^/    /'
        rc=1
    fi

    [ $rc -eq 0 ] && echo "OK: no Arduino header or primitive in targets/esp32-idf/ app code."
    return $rc
}
check "esp32: arduino-free app code" arduino_free

# ALL 11 FRAGMENTS, not a sampled subset. The c3/c6/classic parts differ in PSRAM,
# LAN and panel backends, which is precisely where a shared-core change breaks one target and not
# the others. `build.sh` with no arguments builds every fragment; `sdkconfig_baseline.sh` with no
# arguments then checks every board that has a build, so the two stay in step automatically
# instead of through a hand-maintained list that silently narrows coverage.
#
# Opt-in only because it needs ESP-IDF (never on PATH; build.sh sources it) and several minutes.
esp32_all_and_baseline() {
    ( cd targets/esp32-idf && ./build.sh ) || return 1
    targets/esp32-idf/tools/sdkconfig_baseline.sh
}
if [ "$DO_ESP32" = 1 ]; then
    check "esp32: all fragments + sdkconfig baseline" esp32_all_and_baseline
else
    skip "esp32: all fragments + sdkconfig baseline" "needs --esp32/--targets (builds 10 fragments)"
fi

# ====================================================================================== nordic ==
# Nordic has no cheap check at all -- no ratchet, no baseline -- so without this the crypto HAL and
# the session adapter on this target are covered by nothing but somebody remembering to build. It
# is opt-in only because it needs the nRF toolchain and several minutes, NOT because it is
# optional before merge.
nordic_all() {
    ( cd targets/nordic-zephyr && ./build.sh --all )
}
if [ "$DO_NORDIC" = 1 ]; then
    check "nordic: all three boards" nordic_all
else
    skip "nordic: all three boards" "needs --nordic/--targets (nRF toolchain, several minutes)"
fi

# ===================================================================================== silabs ==
silabs_bg22() {
    targets/efr32bg22-slc/build-and-flash.sh --clean --no-flash
}
if [ "$DO_SILABS" = 1 ]; then
    check "silabs: BG22 headless build" silabs_bg22
else
    skip "silabs: BG22 headless build" "needs --silabs/--targets (Simplicity SDK 2025.12.2)"
fi

# ===================================================================================== summary ==
printf '\n\033[1m======== summary ========\033[0m\n'
printf '%d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -gt 0 ]; then
    printf '\n\033[31mfailed:\033[0m\n'
    printf '  %s\n' "${FAILED_NAMES[@]}"
fi
if [ "$SKIP" -gt 0 ]; then
    # Reprinted so a skipped check cannot be mistaken for a passing one when the summary is the
    # only thing read.
    printf '\n\033[33mnot checked:\033[0m\n'
    printf '  %s\n' "${SKIPPED_NAMES[@]}"
fi
[ "$FAIL" -gt 0 ] && exit 1
[ "$SKIP" -gt 0 ] && exit 2
exit 0
