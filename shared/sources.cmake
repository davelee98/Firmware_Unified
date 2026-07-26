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
# The list is EMPTY and that is correct: no target code is imported yet, and shared/core,
# shared/hal and shared/compress hold placeholder READMEs on purpose (see CLAUDE.md).
# The host build is green against an empty list by design — that is what makes every later
# import a test-first change rather than a retrofit.
#
# Order of arrival, per docs/MIGRATION.md § "Per-target procedure" step 4:
#   core/od_config.c      config TLV parse + chunked assembly   (first — pre-auth surface)
#   core/od_dispatch.c    opcode dispatch, encryption gate
#   core/od_xfer_direct.c 0x70/0x71/0x72
#   core/od_xfer_partial.c 0x76
#   core/od_session.c     auth, KDF, nonce/replay
#   core/od_advert.c      16-byte MSD build
#   core/od_pipe.c        0x80-0x82          (compile-gated OD_PIPE_ENABLE)
#   compress/od_zlib_stream.c

get_filename_component(OD_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

set(OD_SHARED_SOURCES
    # (empty — first entry lands with the first shared/core promotion)
)

# Public headers live alongside their sources; shared/protocol is the wire contract and is
# a byte-for-byte synced copy (never hand-edited — see CLAUDE.md § "Protocol header").
set(OD_SHARED_INCLUDE_DIRS
    "${OD_SHARED_DIR}"
    "${OD_SHARED_DIR}/protocol"
)
