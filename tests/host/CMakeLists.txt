# Host build of shared/ + the test harness.
#
# This is the real enforcement of the shared/ boundary. .github/workflows/shared-boundary.yml
# is a grep: it sees #include lines and literal tokens, and structurally cannot see an
# extern-declared vendor symbol, a macro leaked through a target config header, or a libc
# call that does not exist on a freestanding target. Compiling shared/ for the host with a
# strict, warning-clean C99 toolchain does. See docs/DESIGN_REVIEW_2026-07-25.md § F4/F8.
#
#   cmake -S tests/host -B build && cmake --build build && ctest --test-dir build
#
# Green against an empty shared/ by design (docs/TEST_OWNERSHIP.md § "First step").

cmake_minimum_required(VERSION 3.16)
project(od_host_tests C)

enable_testing()

# -----------------------------------------------------------------------------------
# The contract: C99, no compiler extensions, warnings are errors.
# -----------------------------------------------------------------------------------
# No GNU extensions: shared/ must build under three unrelated vendor toolchains, and a
# GNU-ism that gcc accepts here is a failure discovered later on a target instead.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror)

# Both gcc and clang are supported and CI runs both; neither is required locally. Select
# with -DCMAKE_C_COMPILER=clang. Anything else builds too, it is just not what CI gates on.
message(STATUS "od: C compiler is ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")

# -----------------------------------------------------------------------------------
# shared/ — one source list, shared with the three target builds.
# -----------------------------------------------------------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/../../shared/sources.cmake)

if(OD_SHARED_SOURCES)
    add_library(od_shared STATIC ${OD_SHARED_SOURCES})
    target_include_directories(od_shared PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    message(STATUS "od: shared/ has ${CMAKE_CURRENT_LIST_DIR} sources — building od_shared")
else()
    # No target code is imported yet. An INTERFACE library keeps every consumer below
    # writing `target_link_libraries(... od_shared)` unchanged, so the first real source
    # to land flips the branch above and nothing else in this file moves.
    add_library(od_shared INTERFACE)
    target_include_directories(od_shared INTERFACE ${OD_SHARED_INCLUDE_DIRS})
    message(STATUS "od: shared/ source list is empty (expected — scaffold; see shared/sources.cmake)")
endif()

# -----------------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------------
# canary: compiles the synced protocol headers under the flags above and checks a few
# wire invariants at compile time. Its job is to make this build non-vacuous while
# shared/ is empty — delete it once there are real sources. See canary.c.
add_executable(od_canary canary.c)
target_link_libraries(od_canary od_shared)
add_test(NAME canary COMMAND od_canary)

# span: od_span_t itself. Header-only, so nothing else in this file would compile it on its own
# terms -- and it is now the single place the config walk's "does this packet fit" arithmetic is
# written, after fifteen copies of it on the pre-auth surface were collapsed into one. A defect
# in od_span_split() is a defect in every caller simultaneously, which is precisely the shape of
# bug that a suite testing only the callers can miss.
add_executable(od_span_test span_test.c)
target_link_libraries(od_span_test od_shared)
add_test(NAME span COMMAND od_span_test)

# adv_control: the loop-owned advertising policy machine (correctness review F4) driven against
# a scripted fake HAL. The fake supplies od_hal_adv_{program,start,stop} at LINK time, exactly
# as a target does — shared/ binds its HAL at link time, so there is no injection seam to fake
# around and the test exercises the real linkage.
#
# What this cannot prove, stated so a green run is not over-quoted: AD-record bytes, that
# NimBLE accepts the call sequence, and that a target's callback-to-loop bridge publishes
# coherently. Those need the Milestone 0 byte fixture and a board.
add_executable(od_adv_control_test adv_control_test.c)
target_link_libraries(od_adv_control_test od_shared)
add_test(NAME adv_control COMMAND od_adv_control_test)

# config_asm: chunked CONFIG_WRITE reassembly (correctness review F3). Pure state machine over
# a byte stream, so the host test IS the coverage -- there is no target-specific behaviour to
# leave unverified here, unlike the advertising controller.
#
# Every rejection case asserts the result is not COMPLETE, which is the same assertion as
# "storage did not change": COMPLETE is the only result that lets a caller write NVS.
add_executable(od_config_asm_test config_asm_test.c)
target_link_libraries(od_config_asm_test od_shared)
add_test(NAME config_asm COMMAND od_config_asm_test)

# config_tlv: the config-blob walk and its CRC. The walk is the PRE-AUTH attack surface -- this
# blob is parsed at boot from storage a client can write -- so the cases that matter are the
# hostile ones, and the CRC is checked DIFFERENTIALLY against an independent transcription of
# the shipped algorithm rather than against constants derived from the code under test.
add_executable(od_config_tlv_test config_tlv_test.c)
target_link_libraries(od_config_tlv_test od_shared)
add_test(NAME config_tlv COMMAND od_config_tlv_test)

# config: the parsed aggregate -- instance caps, the Firmware zero-key rule, string termination,
# and what a truncated blob leaves behind. Downstream of the pre-auth surface od_config_tlv
# guards, so the cases are the ones where a hostile blob still reaches storage.
#
# The zero-key case is the one to keep: Nordic and Silabs do not normalise it today, so this is
# the assertion that says the promotion settled the divergence rather than averaging it.
add_executable(od_config_test config_test.c)
target_link_libraries(od_config_test od_shared)
add_test(NAME config COMMAND od_config_test)

# aes128: FIPS-197 known-answer vectors for the host AES core (tests/host/aes128.c).
#
# TEST SUPPORT, not a shared/ module -- but it is registered, and deliberately, because it is the
# foundation every session expectation is computed on: the KDF, the session id, the auth proof and
# every CCM tag come out of od_test_aes128_encrypt(). If it were wrong the session suite would
# still agree with itself and prove nothing, so its vectors are the standard's own rather than
# values this implementation produced.
#
# Never linked into firmware: the targets use mbedTLS and PSA. It is not constant time.
add_executable(od_aes128_test aes128_test.c aes128.c)
add_test(NAME aes128 COMMAND od_aes128_test)

# advert: the 16-byte MSD encoder. DIFFERENTIAL -- the test carries an independent
# transcription of the shipped Zephyr/Silabs byte assembly and sweeps both encoders over the
# input space, because this is a promotion of bytes three targets already broadcast. A vector
# table derived from the new code would prove only self-consistency.
#
# What a green run does NOT prove: that any target calls it yet. All three still build their
# own copy; this pins the bytes so those swaps are mechanical.
add_executable(od_advert_test advert_test.c)
target_link_libraries(od_advert_test od_shared)
add_test(NAME advert COMMAND od_advert_test)

# watchdog: strike counting, safe mode, the retained-byte layout and breadcrumb caching, driven
# against a fake retained store and a fake WDT. SCAFFOLD -- no target implements od_hal_wdt.h
# yet, so a green run here says the policy is right, NOT that any device is protected.
#
# The boot-loop cases carry the weight. A watchdog that cannot escape a boot loop is worse than
# no watchdog: the device never advertises, is unreachable over BLE/DFU, and flattens its
# battery faster than if it had simply hung.
add_executable(od_watchdog_test watchdog_test.c)
target_link_libraries(od_watchdog_test od_shared)
add_test(NAME watchdog COMMAND od_watchdog_test)

# Nordic config pin bytes are a wire contract and deliberately differ by
# platform: nRF52840 uses absolute port*32+pin values, while nRF54 uses the
# compact packed form. Keep the pure decoder checked without a Zephyr build.
add_executable(od_nordic_pin_codec_test
    nordic_pin_codec_test.c
    ../../targets/nordic-zephyr/src/od_pin_codec.c)
target_include_directories(od_nordic_pin_codec_test PRIVATE
    ../../targets/nordic-zephyr/src)
add_test(NAME nordic_pin_codec COMMAND od_nordic_pin_codec_test)

# replay_vectors: the corpus replayed through py-opendisplay's public API. This is the
# dual-stack half of the contract test — the same vectors will later run against the C
# core, and a vector the two sides disagree on is exactly the host/firmware divergence
# this repo exists to eliminate.
#
# Optional by design: it needs Python plus a pinned py-opendisplay from PyPI, which is a
# heavier dependency than a C build should mandate. CI always runs it; a developer with
# neither still gets the compile gate. Absence is reported, never silently skipped —
# a test that vanishes quietly is how coverage rots.
find_package(Python3 COMPONENTS Interpreter)

set(OD_REPLAY_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/replay_vectors.py)
set(OD_VECTORS_DIR   ${CMAKE_CURRENT_LIST_DIR}/../vectors)

if(Python3_FOUND AND EXISTS ${OD_REPLAY_SCRIPT})
    # py-opendisplay must be importable by *this* interpreter. It declares
    # requires-python >=3.13, so a distro python of 3.12 or older cannot host it at all —
    # which is the common local case and must not present as a test failure.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import opendisplay"
        RESULT_VARIABLE OD_PYOD_IMPORT
        OUTPUT_QUIET ERROR_QUIET)

    if(OD_PYOD_IMPORT EQUAL 0)
        add_test(NAME replay_vectors
                 COMMAND ${Python3_EXECUTABLE} ${OD_REPLAY_SCRIPT} ${OD_VECTORS_DIR})
        # The runner exits non-zero on failure and prints SKIP lines for vectors it cannot
        # express against the host API; those are not failures. It also exits non-zero on an
        # empty corpus — a runner that is green because it found nothing is the exact
        # failure mode TEST_OWNERSHIP.md indicts in py-opendisplay's own fixture fallbacks.
        message(STATUS "od: replay_vectors test registered (${Python3_EXECUTABLE})")
    else()
        message(WARNING
            "od: py-opendisplay is not importable by ${Python3_EXECUTABLE} "
            "(v${Python3_VERSION}; py-opendisplay 7.14.0 needs >=3.13) — replay_vectors "
            "NOT registered. The corpus is UNCHECKED in this build. CI always runs it; "
            "to run it here: uv run --python 3.13 --with py-opendisplay==7.14.0 "
            "python tests/host/replay_vectors.py tests/vectors")
    endif()
elseif(NOT Python3_FOUND)
    message(WARNING "od: Python3 not found — replay_vectors NOT registered. "
                    "The corpus is unchecked in this build.")
else()
    message(WARNING "od: ${OD_REPLAY_SCRIPT} missing — replay_vectors NOT registered.")
endif()
