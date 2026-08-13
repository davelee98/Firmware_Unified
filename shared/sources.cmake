# shared/sources.cmake — the single source list for shared/.
#
# Four consumers include this file:
#
#   targets/esp32-idf/       idf_component_register(SRCS ${OD_SHARED_SOURCES} ...)
#   targets/nordic-zephyr/   target_sources(app PRIVATE ${OD_SHARED_SOURCES})
#   targets/efr32bg22-slc/   cmake_gcc/opendisplay-bg22.cmake's source set (NOT the .slcp: shared/
#                            sits outside the SLC project dir, same as third_party/, so it is
#                            hand-maintained there like every other out-of-tree source)
#   tests/host/              the host unit-test build
#
# One list, four consumers, so a file added to shared/ that is not tested is a build
# error in three places rather than an omission nobody notices. That is the whole point
# of the file existing instead of each build globbing — do not replace this with a GLOB.
#
# Paths are relative to this file's directory. Consumers prefix with ${OD_SHARED_DIR}.
#
# The list is no longer empty. core/od_adv_control.c is the first entry, landed with its host
# tests (tests/host/adv_control_test.c), which were written against the header before the
# implementation existed.
#
# WHY THE ADVERTISING CONTROLLER AND NOT THE CONFIG PARSER. Decided 2026-08-05
# (docs/NEXT_STEPS_2026-08-05.md D1, docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md): it has no wire
# surface and no vendor coupling, so it establishes the shared-source build and test pattern
# without moving protocol state. It is a narrow infrastructure exception, NOT a reordering of
# the subsystem sequence below — od_config.c is still the first subsystem that parses, stores,
# or alters wire behaviour, and its pre-auth-attack-surface rationale is untouched.
#
# Order of arrival, per docs/MIGRATION.md § "Per-target procedure" step 4:
#   core/od_adv_control.c advertising/lifecycle policy          (LANDED — no wire surface)
#   core/od_config_asm.c  chunked CONFIG_WRITE reassembly        (LANDED — closes F3)
#   core/od_config_tlv.c  config blob walk + CRC                 (LANDED — pre-auth surface)
#   core/od_watchdog.c    strike/safe-mode/breadcrumb policy     (LANDED — SCAFFOLD: no target
#                         implements hal/od_hal_wdt.h yet, so nothing arms a watchdog by linking
#                         it. Policy first because it must behave identically on four chips and
#                         is cheap to get wrong in a way only a field boot-loop reveals.
#                         Like od_adv_control.c this has no wire surface, so it is not a
#                         reordering of the protocol subsystem sequence below.)
#   core/od_dispatch.c    opcode dispatch, encryption gate
#   core/od_xfer_direct.c 0x70/0x71/0x72
#   core/od_xfer_partial.c 0x76
#   core/od_session.c     auth, KDF, nonce/replay
#   core/od_advert.c      16-byte MSD build
#   core/od_pipe.c        0x80-0x82          (compile-gated OD_PIPE_ENABLE)
#   compress/od_zlib_stream.c

get_filename_component(OD_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

# ----------------------------------------------------------------------------- source tiers ---
#
# The tiers answer ONE question: what must a consumer already have in order to link this source?
# They are NOT a second dimension of policy and NOT a way to opt out of shared code.
#
#   PURE     needs the C standard library and shared/protocol only. Any consumer can take it.
#   HAL_ADV  needs shared/hal/od_hal_adv.h implemented (three link-time C functions).
#   HAL_WDT  needs shared/hal/od_hal_wdt.h implemented (reset reason, retained byte, arm, feed).
#
# WHY THE SPLIT EXISTS. A target part-way through migration can consume the sources whose HAL it
# has and add the rest as each HAL lands, instead of waiting to take all of shared/ at once. That
# is a real state — targets/nordic-zephyr implements NEITHER HAL today — and the alternative was
# a target that consumes nothing from shared/ for as long as one unimplemented HAL exists.
#
# THIS DOES NOT WEAKEN THE ONE-LIST RULE. OD_SHARED_SOURCES is COMPOSED from the tiers below, not
# hand-maintained beside them, so a file added to any tier still reaches the host tests and the
# ESP32 build automatically. Adding a source to a tier and forgetting the aggregate is not a
# possible mistake. Do not turn these into GLOBs and do not add a source to OD_SHARED_SOURCES
# directly — every source belongs to exactly one tier.
#
# A tier is a statement about LINKAGE, not about quality or readiness: od_config_tlv.c is PURE and
# also the most wire-sensitive file here.

set(OD_SHARED_SOURCES_PURE
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config_asm.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config_tlv.c"
)

set(OD_SHARED_SOURCES_HAL_ADV
    "${CMAKE_CURRENT_LIST_DIR}/core/od_adv_control.c"
)

set(OD_SHARED_SOURCES_HAL_WDT
    "${CMAKE_CURRENT_LIST_DIR}/core/od_watchdog.c"
)

# The aggregate: every tier, for consumers that have every HAL (today: the host tests, which fake
# them, and targets/esp32-idf).
set(OD_SHARED_SOURCES
    ${OD_SHARED_SOURCES_PURE}
    ${OD_SHARED_SOURCES_HAL_ADV}
    ${OD_SHARED_SOURCES_HAL_WDT}
)

# Public headers live alongside their sources; shared/protocol is the wire contract and is
# a byte-for-byte synced copy (never hand-edited — see CLAUDE.md § "Protocol header").
set(OD_SHARED_INCLUDE_DIRS
    "${OD_SHARED_DIR}"
    "${OD_SHARED_DIR}/protocol"
    "${OD_SHARED_DIR}/core"
    "${OD_SHARED_DIR}/hal"
)
# core/ and hal/ were missing until the first source landed and exposed it: with an empty
# source list nothing ever included a shared header, so the gap could not present. Left as a
# note rather than silently fixed, because it is the first evidence that this file's "green
# against an empty list" property also means "unexercised against an empty list".
#
# ORDER IS LOAD-BEARING WHEN A TARGET CARRIES ITS OWN PROTOCOL HEADERS. Two shared headers pull
# in the wire contract by its bare name — od_config_asm.h includes "opendisplay_protocol.h",
# od_config_tlv.h includes "opendisplay_structs.h" — and both names also exist, as hand-written
# SUBSETS, in targets/nordic-zephyr/src/ (77 and 319 lines, against 991 and 1242 here). Neither
# quoted include resolves relative to shared/core, so whichever -I comes first wins.
#
# Getting that backwards is not a build error in the general case: od_config_tlv.c derives packet
# body sizes with sizeof on the canonical structs, so a subset that merely differs in layout
# yields a silently WRONG size table — a wire divergence introduced by an include path. A
# consumer whose own include dirs contain either name MUST put OD_SHARED_INCLUDE_DIRS ahead of
# them for the shared sources, per-source if the target's own translation units still need their
# copies. targets/nordic-zephyr/zephyr/CMakeLists.txt does exactly that and says so.
