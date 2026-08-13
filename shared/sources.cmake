# shared/sources.cmake — the single source list for shared/.
#
# Four consumers include this file:
#
#   targets/esp32-idf/       idf_component_register(SRCS ${OD_SHARED_SOURCES} ...)
#   targets/nordic-zephyr/   target_sources(app PRIVATE ${OD_SHARED_SOURCES})
#   targets/efr32bg22-slc/   the SLC-declared source set
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

set(OD_SHARED_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/core/od_adv_control.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config_asm.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config_tlv.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_watchdog.c"
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
