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
# CXX is here for ONE reason: two Nordic target sources are .cpp, and the suites that cover them
# compile the production file rather than a transcription of it. shared/ remains plain C and is
# still built by the C compiler -- the boundary checks in tools/check.sh enforce that separately.
project(od_host_tests C CXX)

enable_testing()
find_package(Threads REQUIRED)

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

# txq: the response queue, its capacity reservation and the drain, against a scripted radio.
#
# The cases that carry the weight are the ordering ones: RETRY must STOP a pass rather than skip
# past the entry (skipping reorders the wire), GONE must drop a whole tag while preserving the
# order of the survivors, and ERROR must retire exactly one entry -- retrying a permanent refusal
# is how a drain becomes a spin. Plus the reservation property the token exists for: one dispatch
# cannot spend another dispatch's capacity.
add_executable(od_txq_test txq_test.c)
target_link_libraries(od_txq_test od_shared)
add_test(NAME txq COMMAND od_txq_test)

add_executable(od_rxq_test rxq_test.c)
target_link_libraries(od_rxq_test od_shared Threads::Threads)
add_test(NAME rxq COMMAND od_rxq_test)

# reply: the seal-or-plain decision, against a fake radio AND a fake od_session_app seam.
#
# The property is not "does it encrypt" but "is confidentiality chosen by the CALL". Both targets
# used to infer it from the response bytes, and Nordic sealed its own rejection frames -- which the
# host then decrypted and validated as an ACK for a command the device refused. The load-bearing
# cases are the ones where the bytes would mislead: a {0x00,cmd,0xFF} rejection, a TLS-LAN frame
# that must not be double-wrapped, and a live session with no security configured.
add_executable(od_reply_test reply_test.c)
target_link_libraries(od_reply_test od_session_fake)
add_test(NAME reply COMMAND od_reply_test)

# gate: one case per od_session result, checking the WIRE BYTES and the outcome.
#
# The compiler already proves the switches are exhaustive (od_gate.h explains the -Wswitch ratchet,
# verified by deleting a case and watching the build fail). What it cannot prove is that each
# member maps to the right action, which is where the consequences are: 0xFE advances the link's
# auth-abuse run and 0xFF does not, and a replayed PIPE frame must draw SILENCE because the 0x81
# NACK is fatal to the client's upload loop.
add_executable(od_gate_test gate_test.c)
target_link_libraries(od_gate_test od_session_fake)
add_test(NAME gate COMMAND od_gate_test)

# config_read: CONFIG_READ as a resumable producer, driven against a queue that keeps filling.
#
# The property is NO TRUNCATION AND NO DUPLICATION UNDER BACKPRESSURE. The shipped handler emitted
# every chunk in one call and drained the ring between them, which only worked because it could
# block the loop task it ran on. The reassembly check is what makes a dropped or repeated chunk
# visible -- a count alone would not.
add_executable(od_config_read_test config_read_test.c)
target_link_libraries(od_config_read_test od_session_fake)
add_test(NAME config_read COMMAND od_config_read_test)

# dispatch: the ordering in od_dispatch.h, which IS the specification.
#
# Every bug these guard against is an ordering bug with a specific consequence: reserve after the
# gate loses the gate's own FE/FF answer; reserve after the handler lets a handler mutate state it
# cannot then report; defer after decrypt advances the replay window so the re-dispatched frame is
# refused the second time; and a late producer-conflict check splices two configs into one
# CRC-valid read-back.
add_executable(od_dispatch_test dispatch_test.c)
target_link_libraries(od_dispatch_test od_session_fake)
add_test(NAME dispatch COMMAND od_dispatch_test)

# dispatch_route: the opcode map, one case per canonical opcode.
#
# Separate from od_dispatch_test because it asks a different question. That suite drives ONE opcode
# through the ordering; this one drives EVERY opcode through the map and records which hook ran.
# The map used to be a switch inside each target, where an opcode could be routed on one target and
# unknown on the other with nothing to say so -- and "unknown" is wire-visible, since it carries no
# activity stamp. A new opcode must fail here until its route and capability behaviour are chosen.
add_executable(od_dispatch_route_test dispatch_route_test.c)
target_link_libraries(od_dispatch_route_test od_session_fake)
add_test(NAME dispatch_route COMMAND od_dispatch_route_test)

# core_reset: the shared half of a teardown -- producer, egress and session, in that order.
#
# The cases that carry weight are the negative ones: RX must NOT be in it (its producer differs per
# target, so a shared reset would race one and reorder the other), and the session must go through
# od_session_clear() rather than a memset, or its prepared HAL key slot is stranded in a finite
# pool with nothing left naming it.
add_executable(od_core_reset_test core_reset_test.c)
target_link_libraries(od_core_reset_test od_session_fake)
add_test(NAME core_reset COMMAND od_core_reset_test)

# session: the handshake, KDF, replay window and CCM envelope against a fake crypto HAL built on
# the host AES core. DIFFERENTIAL where it matters -- the replay window is upstream Firmware's
# own nonce_window.h, ported verbatim, so this suite exercises the shipped state machine rather
# than a re-derivation of it.
#
# What a green run does NOT prove: that mbedTLS or PSA is driven correctly, that a PSA key policy
# is right, or that a real backend's tag matches this one. Those need the targets and C5/C6
# hardware. With the C0 capture skipped, the reference here is transcription plus a ported header,
# not bytes a device emitted.
# session_fake is its own library because tests/fuzz/ links the SAME fake. A second, subtly
# different fake would make a fuzz finding unreproducible in this suite -- which is where a
# finding has to land to stay fixed.
add_library(od_session_fake STATIC session_fake.c aes128.c)
target_link_libraries(od_session_fake PUBLIC od_shared)
target_include_directories(od_session_fake PUBLIC ${CMAKE_CURRENT_LIST_DIR})

add_executable(od_session_test session_test.c)
target_link_libraries(od_session_test od_session_fake)
add_test(NAME session COMMAND od_session_test)

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

# --------------------------------------------------------------------------------------------
# TARGET HAL suites. These compile PRODUCTION target sources against fake vendor APIs, which is
# the point: the defects they cover are lifecycle defects -- which key id is destroyed, whether a
# failure propagates -- and a test that re-implements the logic proves only that the copy agrees
# with itself. fake_psa/ is on the include path of these binaries ONLY; it is not visible to
# shared/ or to any target build.
add_library(od_fake_psa STATIC fake_psa/fake_psa.c)
target_include_directories(od_fake_psa PUBLIC ${CMAKE_CURRENT_LIST_DIR}/fake_psa)

# nordic_crypto_slot: the prepared-key slot lifecycle in the Nordic PSA HAL. The case that matters
# is the injected destroy failure -- not reproducible on a board, and the one that used to latch
# the slot ready-but-unusable until reboot, leaving the device unable to re-authenticate.
add_executable(od_nordic_crypto_slot_test
    nordic_crypto_slot_test.c
    ../../targets/nordic-zephyr/src/od_hal_crypto.c)
target_include_directories(od_nordic_crypto_slot_test PRIVATE
    ../../targets/nordic-zephyr/src)
target_link_libraries(od_nordic_crypto_slot_test od_shared od_fake_psa)
add_test(NAME nordic_crypto_slot COMMAND od_nordic_crypto_slot_test)

# esp_crypto_random: the ESP32 RNG arm, which feeds the AUTHENTICATE challenge. od_session's rule
# is that a device never offers a challenge it cannot honour, and that rule is unenforceable while
# the backend cannot report failure.
add_executable(od_esp_crypto_random_test
    esp_crypto_random_test.c
    ../../targets/esp32-idf/hal/od_hal_crypto_random.c)
target_include_directories(od_esp_crypto_random_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fake_esp)
target_link_libraries(od_esp_crypto_random_test od_shared od_fake_psa)
add_test(NAME esp_crypto_random COMMAND od_esp_crypto_random_test)

# pipe_write: the Nordic PIPE state machine's VERDICTS, against fake display/reply/kernel seams.
#
# A verdict is invisible in a packet capture -- both sides see the same NACK -- but it decides
# whether the frame stamps the session's activity clock, so a wrong OK on a refused frame keeps a
# link alive on traffic the device rejected. Each case asserts the verdict AND the frames.
add_executable(od_pipe_write_test
    pipe_write_test.c
    ../../targets/nordic-zephyr/src/opendisplay_pipe_write.cpp)
target_include_directories(od_pipe_write_test PRIVATE
    ../../targets/nordic-zephyr/src
    ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr)
target_link_libraries(od_pipe_write_test od_shared)
# The production file is C++ and the target builds it with exceptions and RTTI off; match that so
# the host is not compiling a different language from the device.
target_compile_options(od_pipe_write_test PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
add_test(NAME pipe_write COMMAND od_pipe_write_test)

# nordic_session_app: the device-id packing, which is WIRE-VISIBLE -- four bytes of the silicon id
# feed both the KDF and the AUTHENTICATE proof, so a different selection is a different device to
# the host and every provisioned tag in a fleet becomes one no stored key opens. C11 moved that
# arithmetic between files, which is exactly the change that can alter it without altering anything
# a build or a boot would notice. Expectations are hand-computed, not taken from the code.
add_executable(od_nordic_session_app_test
    nordic_session_app_test.c
    ../../targets/nordic-zephyr/src/od_session_app.c)
target_include_directories(od_nordic_session_app_test PRIVATE
    ../../targets/nordic-zephyr/src
    ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr)
# od_session_fake, not od_shared: this pulls in od_session.c, which binds the crypto HAL at
# link time. The fake is the one every other session binary already uses.
target_link_libraries(od_nordic_session_app_test od_session_fake)
add_test(NAME nordic_session_app COMMAND od_nordic_session_app_test)

# nordic_cmd_device: what this target's device commands ANSWER, which dispatch_route_test cannot
# see -- that suite replaces every hook with a recorder, so it proves routing and nothing about
# behaviour. Two answers here were decided deliberately and had no other coverage: 0x52's
# unsupported NACK (a C11 wire change) and 0x53's deliberate silence. 0x43 is included because
# ESP32 answered nothing to it from C8 to C11.2 and no suite noticed.
add_executable(od_nordic_cmd_device_test
    nordic_cmd_device_test.c
    ../../targets/nordic-zephyr/src/od_cmd_device.c)
target_include_directories(od_nordic_cmd_device_test PRIVATE
    ../../targets/nordic-zephyr/src
    ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr)
target_link_libraries(od_nordic_cmd_device_test od_shared)
add_test(NAME nordic_cmd_device COMMAND od_nordic_cmd_device_test)

# --------------------------------------------------------------------------------------------
# dispatch_corpus: tests/vectors/dispatch.json replayed through the production od_dispatch_frame().
#
# This is the OTHER END of a contract that has only ever been checked from one side.
# replay_vectors.py drives the same corpus through py-opendisplay's public API and says outright
# that firmware replies are "never checkable here" (replay_vectors.py:18) -- so every h2d
# expect.reply has been unchecked against firmware since the corpus was authored. Same file, same
# vectors, the device side.
#
# The table is GENERATED into the build directory and never committed: a checked-in copy is the
# second corpus TEST_OWNERSHIP.md forbids, and it permits a stale green. Regenerating depends on the
# JSON, so editing a vector rebuilds the table.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    set(OD_VEC_TOOL ${CMAKE_CURRENT_LIST_DIR}/vectors_tool.py)
    set(OD_VEC_DIR  ${CMAKE_CURRENT_LIST_DIR}/../vectors)
    set(OD_VEC_INC  ${CMAKE_CURRENT_BINARY_DIR}/generated/dispatch_vectors.inc)
    file(GLOB OD_VEC_JSON ${OD_VEC_DIR}/*.json)
    add_custom_command(
        OUTPUT ${OD_VEC_INC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND ${Python3_EXECUTABLE} ${OD_VEC_TOOL} --emit ${OD_VEC_DIR} ${OD_VEC_INC}
        DEPENDS ${OD_VEC_TOOL} ${OD_VEC_JSON}
        COMMENT "od: generating the dispatch vector table from tests/vectors/")
    add_custom_target(od_vectors_generated DEPENDS ${OD_VEC_INC})

    # The schema gate, as its own test: a malformed or under-specified vector must fail loudly
    # rather than be silently dropped by whatever consumes it next.
    add_test(NAME vectors_schema
             COMMAND ${Python3_EXECUTABLE} ${OD_VEC_TOOL} --check ${OD_VEC_DIR})

    # ONE runner source, TWO executables. od_cmd_app_* is static link-time composition, so two
    # profiles cannot share a binary -- and the answer to that is emphatically not a runtime
    # registry. Each executable resolves the seam once.
    #
    # Only the runner sees the generated table. A profile gets corpus_runner.h, which exposes
    # semantic knobs and no expected bytes: a fake that could read expect.reply would make the
    # corpus its own oracle, and every vector would pass while proving nothing.
    add_executable(od_dispatch_corpus_portable_test
        corpus_runner.c
        corpus_profile_portable.c
        ${OD_VEC_INC})
    target_include_directories(od_dispatch_corpus_portable_test
        PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(od_dispatch_corpus_portable_test od_session_fake)
    add_dependencies(od_dispatch_corpus_portable_test od_vectors_generated)
    add_test(NAME dispatch_corpus_portable COMMAND od_dispatch_corpus_portable_test)
else()
    message(WARNING "od: Python3 not found -- the dispatch corpus runner is NOT built and the "
                    "corpus reply half is UNCHECKED in this build.")
endif()

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

# --------------------------------------------------------------------------------------------
# Fuzz targets (tests/fuzz/), Clang only -- -fsanitize=fuzzer is a Clang feature and gcc has no
# libFuzzer. Added here rather than as a separate project so the SAME configure step CI already
# runs builds them, and so they link od_session_fake instead of a second, divergent fake.
if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/../fuzz/CMakeLists.txt)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../fuzz fuzz)
    message(STATUS "od: Clang detected — tests/fuzz targets registered")
else()
    message(STATUS "od: fuzz targets NOT built (needs Clang); the pre-auth surface is "
                   "UNFUZZED in this build")
endif()
