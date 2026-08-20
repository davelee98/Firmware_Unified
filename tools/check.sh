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

# od_hal_time is the two-function ambient/busy-wait seam. Millisecond sleep stays private until
# ESP32's unsigned round-up contract is reconciled with Nordic's signed k_msleep contract, and the
# BG22 wrapper must convert the SDK's extended tick count before narrowing to uint32_t milliseconds.
time_hal_structure() {
    local rc=0 hits

    hits=$(find targets -name od_hal_time.h \
           ! -path '*/build/*' ! -path '*/build-*/*' -print 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "target-local od_hal_time.h shadows shared/hal/od_hal_time.h"
        rc=1
    fi

    hits=$(grep -rInE '\b(od_uptime_get_32|od_busy_wait)[[:space:]]*\(' \
           --exclude-dir='build*' targets/nordic-zephyr 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "retired Nordic time wrapper returned"
        rc=1
    fi

    if grep -qE '^[[:space:]]*void[[:space:]]+od_hal_delay_ms[[:space:]]*\(' \
            shared/hal/od_hal_time.h; then
        echo "bounded millisecond sleep entered the shared time HAL without its contract decision"
        rc=1
    fi
    if ! grep -qE '^uint32_t od_hal_uptime_ms\(void\);' shared/hal/od_hal_time.h ||
       ! grep -qE '^void od_hal_delay_us\(uint32_t us\);' shared/hal/od_hal_time.h; then
        echo "shared time HAL declaration drifted"
        rc=1
    fi
    if ! grep -qE '\bod_hal_delay_ms[[:space:]]*\(' targets/esp32-idf/hal/od_hal_sleep.h; then
        echo "ESP32 private millisecond-sleep declaration is missing"
        rc=1
    fi

    if ! grep -q 'sl_sleeptimer_get_tick_count64()' targets/efr32bg22-slc/od_hal_time.c ||
       ! grep -q 'sl_sleeptimer_tick64_to_ms' targets/efr32bg22-slc/od_hal_time.c; then
        echo "BG22 uptime must convert the 64-bit tick count before narrowing"
        rc=1
    fi
    if grep -qE 'sl_sleeptimer_tick_to_ms|sl_sleeptimer_get_tick_count\(' \
            targets/efr32bg22-slc/od_hal_time.c; then
        echo "BG22 time HAL reintroduced the 32-bit tick-domain clock"
        rc=1
    fi

    return "$rc"
}
check "structure: shared time HAL" time_hal_structure

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

xfer_direct_dispatch() {
    local rc=0 hits
    local rows=shared/core/od_dispatch_ops.h

    for row in \
        'X(CMD_DIRECT_WRITE_START,  od_xfer_direct_start,       1u)' \
        'X(CMD_DIRECT_WRITE_DATA,   od_xfer_data,               2u)' \
        'X(CMD_DIRECT_WRITE_END,    od_xfer_end,                2u)' \
        'X(CMD_PARTIAL_WRITE_START, od_xfer_partial_start,      1u)'; do
        if ! grep -Fq "$row" "$rows"; then
            echo "shared transfer dispatch row or reservation budget drifted: $row"
            rc=1
        fi
    done

    hits=$(grep -RInE '\bod_cmd_app_(direct_start|direct_data|direct_end|partial_start)\b' \
        shared/core targets --include='*.c' --include='*.cpp' --include='*.h' \
        --exclude-dir=build 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "legacy direct/partial target hook returned; dispatch must route straight to od_xfer"
        rc=1
    fi
    if [ -e targets/nordic-zephyr/src/od_cmd_direct.c ]; then
        echo "Nordic's retired direct/partial bridge translation unit returned"
        rc=1
    fi
    return $rc
}
check "structure: direct transfer dispatch ownership" xfer_direct_dispatch

esp32_xfer_cutover() {
    local rc=0 hits

    hits=$(grep -rInE '\b(handleDirectWriteStart|handleDirectWriteData|handleDirectWriteEnd|handlePartialWriteStart)[[:space:]]*\(' \
           targets/esp32-idf/src 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "ESP32 legacy direct/partial command policy returned; shared od_xfer owns it"
        rc=1
    fi

    if ! grep -q '\bod_xfer_reset[[:space:]]*(' targets/esp32-idf/src/session_guard.cpp; then
        echo "ESP32 teardown must reset the shared legacy transfer before od_core_reset"
        rc=1
    fi
    return $rc
}
check "esp32: shared legacy transfer cutover" esp32_xfer_cutover

# TRANSITIONAL THROUGH PHASE 3 STEP 6. Target PIPE and shared od_xfer both drive the singleton
# pump during the staged migration, so each START must displace the other owner first. The target
# PIPE cutover must remove these two arms and replace this check with the permanent production
# invariant: no target calls od_zlib_pump_reset/push; shared/core/od_xfer.c is the only caller.
esp32_xfer_interim_pipe_arbitration() {
    local rc=0 pipe_start legacy_start

    pipe_start=$(sed -n '/^od_cmd_result_t handlePipeWriteStart(/,/^}/p' \
                 targets/esp32-idf/src/display_service.cpp)
    if ! grep -q '\bod_xfer_active[[:space:]]*(' <<<"$pipe_start" ||
       ! grep -q '\bod_xfer_reset[[:space:]]*(' <<<"$pipe_start"; then
        echo "ESP32 PIPE START must displace a live shared legacy transfer before pump use"
        rc=1
    fi

    legacy_start=$(sed -n '/^extern "C" void od_xfer_app_prepare_start(/,/^}/p' \
                   targets/esp32-idf/src/display_service.cpp)
    if ! grep -q '\bresetPipeWriteState[[:space:]]*(' <<<"$legacy_start"; then
        echo "ESP32 legacy START must displace target PIPE through od_xfer_app_prepare_start"
        rc=1
    fi
    return $rc
}
check "esp32: interim PIPE/od_xfer arbitration" esp32_xfer_interim_pipe_arbitration

# Phase 2 step 10b's executable boundary. These names are not all permanent: the delete inventory
# is intentionally required while target PIPE owns it, and Phase 3 step 6 must remove this check in
# the same commit that removes the inventory. The retain list prevents a bulk PIPE deletion from
# taking adapter hardware primitives with it.
esp32_xfer_10b_inventory() {
    local rc=0 symbol pump_hits pump_count
    local files=(
        targets/esp32-idf/src/display_service.cpp
        targets/esp32-idf/src/display_service.h
        targets/esp32-idf/src/main.h
        targets/esp32-idf/src/structs.h
    )
    local delete_symbols=(
        PipeWriteState PipeReorderSlot pipeState pipeReorder
        handlePipeWriteStart handlePipeWriteData handlePipeWriteEnd
        resetPipeWriteState pipeWriteActive pipeSlot pipeChunkReceived pipeBuildAckPayload
        sendPipeAck pipeAbortNoReply sendPipeNack sendPipeStartNack pipeUpdateHighestSeen
        pipeConsumePayload directWriteComputeGeometry directWriteActivatePanel
        directWriteFinishAndRefresh directWriteSinkBytes streamControllerPlaneBytes
        direct_zlib_sink partial_consume_bytes partial_prepare_panel_ram partial_write_to_panel
        partial_write_stream_bytes partial_zlib_sink zlib_stream_to_direct_write
        zlib_stream_to_partial_write mono_plane_bytes parse_be_u32 PartialStreamContext partialCtx
        sessionOrigin directWriteActive directWriteCompressed directWriteBitplanes
        directWriteBytesWritten directWriteDecompressedTotal directWriteWidth directWriteHeight
        directWriteTotalBytes directWriteCompressedReceived directWriteStartTime
        directWritePlaneBytes directWriteInitialPlane
    )
    local retain_symbols=(
        directWriteResolveGeometry xferAppClear xferAppWriteFull xferAppWritePartial
        od_xfer_app_prepare_start od_xfer_app_panel_info od_xfer_app_begin_full
        od_xfer_app_begin_partial od_xfer_app_write od_xfer_app_inflate_scratch
        od_xfer_app_abort od_xfer_app_before_refresh od_xfer_app_barrier_abort
        od_xfer_app_refresh od_xfer_app_displayed_etag od_xfer_app_set_displayed_etag
        od_xfer_app_now_ms partial_set_addr_window partial_prepare_panel_ram_for
        partial_refresh_for directWriteTouchSuspended cleanupDirectWriteState
        cleanup_partial_write_state
    )

    for symbol in "${delete_symbols[@]}"; do
        if ! grep -wq "$symbol" "${files[@]}"; then
            echo "ESP32 10b delete inventory drifted before Phase 3: $symbol"
            rc=1
        fi
    done
    for symbol in "${retain_symbols[@]}"; do
        if ! grep -wq "$symbol" targets/esp32-idf/src/display_service.cpp; then
            echo "ESP32 adapter primitive missing from the 10b retain inventory: $symbol"
            rc=1
        fi
    done

    pump_hits=$(grep -RInE '\bod_zlib_pump_(reset|push)[[:space:]]*\(' \
        targets/esp32-idf/src --include='*.c' --include='*.cpp' --include='*.h' 2>/dev/null || true)
    pump_count=$(grep -c . <<<"$pump_hits")
    if [ "$pump_count" -ne 4 ] || grep -v '^targets/esp32-idf/src/display_service\.cpp:' <<<"$pump_hits" | grep -q .; then
        echo "$pump_hits"
        echo "ESP32 interim pump ownership drifted: expected four target-PIPE calls in display_service.cpp"
        rc=1
    fi
    return $rc
}
check "esp32: Phase 2 step 10b inventory" esp32_xfer_10b_inventory

nordic_xfer_cutover() {
    local rc=0 teardown reset_line core_line
    if ! grep -q '\${OD_SHARED_SOURCES_APP_XFER}' \
            targets/nordic-zephyr/zephyr/CMakeLists.txt \
       || grep -q 'od_xfer_compile' targets/nordic-zephyr/zephyr/CMakeLists.txt; then
        echo "Nordic production image must link APP_XFER directly, without a compile-only target"
        rc=1
    fi

    teardown=$(sed -n '/if (atomic_cas(&s_close_pending/,/\/\* BOUNDED/p' \
               targets/nordic-zephyr/src/opendisplay_pipe.c)
    reset_line=$(grep -n '\bod_xfer_reset[[:space:]]*(' <<<"$teardown" | head -n 1 | cut -d: -f1)
    core_line=$(grep -n '\bod_core_reset[[:space:]]*(' <<<"$teardown" | head -n 1 | cut -d: -f1)
    if [ -z "$reset_line" ] || [ -z "$core_line" ] || [ "$reset_line" -ge "$core_line" ]; then
        echo "Nordic disconnect must reset od_xfer before od_core_reset"
        rc=1
    fi
    return $rc
}
check "nordic: shared legacy transfer cutover" nordic_xfer_cutover

# TRANSITIONAL THROUGH NORDIC PHASE 3 STEP 6. Retire this with the target PIPE machine and replace
# it with the permanent production invariant that only shared/core/od_xfer.c resets or pushes the
# zlib pump.
nordic_xfer_interim_pipe_arbitration() {
    local rc=0 pipe_start legacy_start

    pipe_start=$(sed -n '/^extern "C" od_cmd_result_t opendisplay_pipe_write_start(/,/^}/p' \
                 targets/nordic-zephyr/src/opendisplay_pipe_write.cpp)
    if ! grep -q '\bod_xfer_active[[:space:]]*(' <<<"$pipe_start" ||
       ! grep -q '\bod_xfer_reset[[:space:]]*(' <<<"$pipe_start"; then
        echo "Nordic PIPE START must displace a live shared legacy transfer before pump use"
        rc=1
    fi

    legacy_start=$(sed -n '/^extern "C" void od_xfer_app_prepare_start(/,/^}/p' \
                   targets/nordic-zephyr/src/opendisplay_display.cpp)
    if ! grep -q '\bopendisplay_pipe_write_reset[[:space:]]*(' <<<"$legacy_start"; then
        echo "Nordic legacy START must displace target PIPE through od_xfer_app_prepare_start"
        rc=1
    fi
    return $rc
}
check "nordic: interim PIPE/od_xfer arbitration" nordic_xfer_interim_pipe_arbitration

# Phase 2 step 10b's Nordic executable boundary. Phase 3 step 6 removes this check with the delete
# inventory, preserves the adapter primitives, and installs the single-pump-owner ratchet above.
nordic_xfer_10b_inventory() {
    local rc=0 symbol pump_hits pump_count
    local files=(
        targets/nordic-zephyr/src/opendisplay_display.cpp
        targets/nordic-zephyr/src/opendisplay_display.h
        targets/nordic-zephyr/src/opendisplay_pipe_write.cpp
        targets/nordic-zephyr/src/opendisplay_pipe_write.h
    )
    local delete_symbols=(
        PipeWriteState PipeReorderSlot s_pipe s_reorder pipe_slot pipe_chunk_received
        pipe_build_ack_payload pipe_abort_no_reply send_pipe_ack sack_or_abort send_pipe_nack
        send_pipe_start_nack pipe_update_highest_seen pipe_consume_payload finish_and_refresh
        opendisplay_pipe_write_start opendisplay_pipe_write_data opendisplay_pipe_write_end
        opendisplay_pipe_write_reset opendisplay_pipe_write_active od_cmd_app_pipe_start
        od_cmd_app_pipe_data od_cmd_app_pipe_end s_active s_total_bytes s_written_bytes
        s_dw_chunk_n s_dw_log_pct s_dw_trailing_ignores s_dw_init_t0 s_color_scheme s_plane_size
        s_plane2_started s_dw_compressed s_dw_decompressed_total PartialStreamContext s_partial
        s_partial_panel_up parse_be_u32 mono_plane_bytes partial_write_stream_bytes
        partial_zlib_sink zlib_stream_to_partial_write partial_consume_bytes
        partial_prepare_panel_ram partial_write_to_panel opendisplay_display_partial_active
        opendisplay_display_dw_active opendisplay_display_bytes_written
        opendisplay_display_total_bytes opendisplay_display_expected_dw_bytes
        opendisplay_display_displayed_etag opendisplay_display_clear_etag
        opendisplay_display_set_partial_new_etag opendisplay_display_partial_bytes_written
        opendisplay_display_partial_expected opendisplay_display_partial_compressed
        opendisplay_display_calc_plane_bytes opendisplay_display_partial_write_start
        dw_init_mark dw_log_progress dw_stream_raw_bytes direct_zlib_sink
        zlib_stream_to_direct_write opendisplay_display_direct_write_start
        opendisplay_display_direct_write_data opendisplay_display_direct_write_end_prepare
        opendisplay_display_direct_write_end_refresh opendisplay_display_pipe_full_start
        opendisplay_display_pipe_partial_arm opendisplay_display_pipe_partial_prepare
    )
    local retain_symbols=(
        XferAppMode XferAppHardwareState s_xfer_app xfer_app_clear xfer_app_write_full
        xfer_app_write_partial od_xfer_app_prepare_start od_xfer_app_panel_info
        od_xfer_app_begin_full od_xfer_app_begin_partial od_xfer_app_write
        od_xfer_app_inflate_scratch od_xfer_app_abort od_xfer_app_before_refresh
        od_xfer_app_barrier_abort od_xfer_app_refresh od_xfer_app_displayed_etag
        od_xfer_app_set_displayed_etag od_xfer_app_now_ms s_epd s_decompression_chunk
        s_displayed_etag display_cfg display_power_set wait_for_refresh
        panel_skips_bbep_set_addr_window panel_uses_pixel_ram_x
        panel_uses_ep397_y_decrement panel_uses_ep426_x_decrement
        panel_skips_reinit_on_partial_refresh partial_set_ep397_ram_y partial_set_ep426_ram_y
        partial_set_pixel_ram_x partial_set_addr_window partial_trigger_refresh
        partial_prepare_panel_ram_hardware partial_cleanup opendisplay_display_abort
    )

    for symbol in "${delete_symbols[@]}"; do
        if ! grep -wq "$symbol" "${files[@]}"; then
            echo "Nordic 10b delete inventory drifted before Phase 3: $symbol"
            rc=1
        fi
    done
    for symbol in "${retain_symbols[@]}"; do
        if ! grep -wq "$symbol" targets/nordic-zephyr/src/opendisplay_display.cpp; then
            echo "Nordic adapter primitive missing from the 10b retain inventory: $symbol"
            rc=1
        fi
    done

    pump_hits=$(grep -RInE '\bod_zlib_pump_(reset|push)[[:space:]]*\(' \
        targets/nordic-zephyr/src --include='*.c' --include='*.cpp' --include='*.h' \
        2>/dev/null || true)
    pump_count=$(grep -c . <<<"$pump_hits")
    if [ "$pump_count" -ne 5 ] ||
       grep -v '^targets/nordic-zephyr/src/opendisplay_display\.cpp:' <<<"$pump_hits" | grep -q .; then
        echo "$pump_hits"
        echo "Nordic interim pump ownership drifted: expected five target-PIPE calls in opendisplay_display.cpp"
        rc=1
    fi
    return $rc
}
check "nordic: Phase 2 step 10b inventory" nordic_xfer_10b_inventory

silabs_xfer_cutover() {
    local rc=0 hits teardown reset_line core_line
    local commands=targets/efr32bg22-slc/od_cmd_silabs.c
    local display=targets/efr32bg22-slc/opendisplay_display.cpp
    local cmake=targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake

    hits=$(grep -InE '\bopendisplay_display_direct_write_(start|data|end|end_prepare|end_refresh)[[:space:]]*\(' \
           "$commands" "$display" targets/efr32bg22-slc/opendisplay_display.h 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "Silabs legacy direct command policy returned; the target must expose hardware only"
        rc=1
    fi
    if ! grep -q '\${OD_SHARED_SOURCES_APP_XFER}' "$cmake" || grep -q 'od_xfer_compile' "$cmake"; then
        echo "Silabs production image must link APP_XFER directly, without a compile-only target"
        rc=1
    fi

    teardown=$(sed -n '/^void opendisplay_pipe_reset_transport(/,/^}/p' \
               targets/efr32bg22-slc/opendisplay_pipe.c)
    reset_line=$(grep -n '\bod_xfer_reset[[:space:]]*(' <<<"$teardown" | head -n 1 | cut -d: -f1)
    core_line=$(grep -n '\breset_transport_state[[:space:]]*(' <<<"$teardown" | head -n 1 | cut -d: -f1)
    if [ -z "$reset_line" ] || [ -z "$core_line" ] || [ "$reset_line" -ge "$core_line" ]; then
        echo "Silabs teardown must reset od_xfer before common transport/session state"
        rc=1
    fi

    hits=$(grep -RInE '\bod_zlib_pump_(reset|push)[[:space:]]*\(' \
        targets/efr32bg22-slc --include='*.c' --include='*.cpp' --include='*.h' \
        --exclude-dir=build 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "Silabs target code bypassed shared od_xfer pump ownership"
        rc=1
    fi
    return $rc
}
check "silabs: shared legacy transfer cutover" silabs_xfer_cutover

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

log_hal_structure() {
    local rc=0 hits count

    hits=$(find targets \( -name od_log.c -o -name od_log.cpp -o -name od_log.h \
             -o -name od_hal_log.h \) \
           ! -path '*/build/*' ! -path '*/build-*/*' -print 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "target-local logger API or implementation shadows shared ownership"
        rc=1
    fi

    count=$(grep -c '^void _od_log(' shared/core/od_log.c || true)
    if [ "$count" -ne 1 ]; then
        echo "shared od_log.c must contain exactly one _od_log definition (found $count)"
        rc=1
    fi
    count=$(grep -c '^void od_log_hex_line(' shared/core/od_log.c || true)
    if [ "$count" -ne 1 ]; then
        echo "shared od_log.c must contain exactly one od_log_hex_line definition (found $count)"
        rc=1
    fi

    hits=$(grep -rInE '\b(od_hal_log_room|od_log_set_ready_hook|od_log_set_loop_task)[[:space:]]*\(|\[DROP:' \
           shared targets/esp32-idf/src targets/esp32-idf/hal targets/esp32-idf/panel \
           targets/nordic-zephyr/src targets/nordic-zephyr/panel \
           targets/efr32bg22-slc --include='*.c' --include='*.cpp' --include='*.h' \
           --include='*.inl' --exclude-dir='build*' 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "retired logging backpressure or loop-task machinery returned"
        rc=1
    fi

    if ! grep -q '"OD_CAP_LOG=0"' targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake; then
        echo "BG22 must state logging capability-off explicitly"
        rc=1
    fi
    hits=$(grep -rInE '^([A-Za-z_][A-Za-z0-9_]*[[:space:]]+)+od_hal_log_[A-Za-z0-9_]+[[:space:]]*\(' \
           targets/efr32bg22-slc --include='*.c' --include='*.cpp' --exclude-dir='build*' \
           2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "BG22 capability-off target must not implement the log HAL"
        rc=1
    fi

    hits=$(grep -rInE '\bod_hal_uptime_ms[[:space:]]*\(' shared/core --include='*.c' \
           | grep -v '/od_log.c:' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "shared policy replaced an explicit clock with the ambient time HAL"
        rc=1
    fi

    if grep -q '%\.\*s' targets/nordic-zephyr/src/od_hal_log.c ||
       grep -qE '\(const[[:space:]]+char[[:space:]]*\*\)' targets/nordic-zephyr/src/od_hal_log.c; then
        echo "Nordic logger must submit the mutable transient record as LOG_RAW(\"%s\", record)"
        rc=1
    fi

    hits=$(grep -rInE '\bod_log_(error|warn|info|debug|raw)[[:space:]]*\([^;]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' \
           shared/core --include='*.c' --exclude='od_log.c' 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "shared log argument contains a nested function call; capability-off must not hide side effects"
        rc=1
    fi

    return $rc
}
check "structure: shared logging ownership" log_hal_structure

nordic_epd_spi_ownership() {
    local rc=0 hits count
    local adapter=targets/nordic-zephyr/panel/od_bbep_zephyr_io.inl
    local backend=targets/nordic-zephyr/src/od_epd_spi_nrfx.c
    local fallback=targets/nordic-zephyr/src/od_epd_spi_bitbang.c

    hits=$(grep -nE '\bbb_spi_bitbang\b|od_gpio_write[[:space:]]*\([[:space:]]*pBBEP->i(CLK|MOSI)' \
           "$adapter" || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "bb_epaper adapter regained a per-edge generic GPIO loop"
        rc=1
    fi

    hits=$(grep -nE '\bod_(pin_decode|gpio_write)[[:space:]]*\(|\bgpio_pin_(set|configure)[[:space:]]*\(' \
           "$fallback" || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "EPD fallback inner path must use pre-decoded nRF GPIO registers"
        rc=1
    fi

    for selection in 'NRF_SPIM2' 'NRF_SPIM00' 'NRF_SPIM23'; do
        count=$(grep -c "$selection" "$backend" || true)
        if [ "$count" -lt 1 ]; then
            echo "missing Nordic EPD SPIM selection: $selection"
            rc=1
        fi
    done

    count=$(grep -c '^CONFIG_NRFX_SPIM=y$' targets/nordic-zephyr/zephyr/prj.conf || true)
    if [ "$count" -ne 1 ]; then
        echo "common Nordic config must enable the nrfx SPIM family exactly once (found $count)"
        rc=1
    fi

    if ! sed -n '/^&spi2 {/,/^};/p' \
          targets/nordic-zephyr/zephyr/boards/xiao_ble_nrf52840.overlay \
          | grep -q 'status = "disabled";'; then
        echo "xiao_nrf52840: application-owned SPIM2 must remain disabled in devicetree"
        rc=1
    fi
    if ! sed -n '/^&spi00 {/,/^};/p' \
          targets/nordic-zephyr/zephyr/boards/xiao_nrf54l15_nrf54l15_cpuapp.overlay \
          | grep -q 'status = "disabled";'; then
        echo "xiao_nrf54l15: application-owned SPIM00 must remain disabled in devicetree"
        rc=1
    fi
    for node in spi23 spi00; do
        if ! sed -n "/^&${node} {/,/^};/p" \
              targets/nordic-zephyr/zephyr/boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay \
              | grep -q 'status = "disabled";'; then
            echo "xiao_nrf54lm20a: ${node} must remain disabled in devicetree"
            rc=1
        fi
    done

    if ! grep -q 'if(NOT OD_EPD_SPI_REQUIRE_SPIM)' targets/nordic-zephyr/zephyr/CMakeLists.txt ||
       ! grep -q -- '-DOD_EPD_SPI_REQUIRE_SPIM=ON' targets/nordic-zephyr/build.sh ||
       ! grep -q 'zephyr_get(OD_EPD_SPI_REQUIRE_SPIM)' targets/nordic-zephyr/zephyr/CMakeLists.txt; then
        echo "OD_EPD_SPI_REQUIRE_SPIM must both omit fallback code and cross the sysbuild boundary"
        rc=1
    fi

    return $rc
}
check "nordic: EPD SPI ownership" nordic_epd_spi_ownership

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

log_off_link_proof() {
    local binary="$BUILD_ROOT/host-gcc/od_log_off_test" hits

    [ -x "$binary" ] || { echo "capability-off logging fixture was not built"; return 1; }
    hits=$(nm "$binary" | grep -E 'od_hal_log_|s_armed|s_level_chars' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "OD_CAP_LOG=0 retained log-HAL linkage or logger state at -O0"
        return 1
    fi
}
check "host: logging capability-off link proof" log_off_link_proof

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
