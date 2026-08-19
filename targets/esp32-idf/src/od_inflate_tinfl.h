/*
 * od_inflate_tinfl — ESP32 inflate adapter backed by the ROM miniz `tinfl`.
 *
 * Alternative backend for the shared od_zlib_pump. It exposes the same streaming contract and
 * status type as od_zlib_stream_*; od_inflate_app.cpp selects this backend at compile time.
 *
 * SCOPE — this serves EVERY compressed path, not just WiFi/LAN. The selection in
 * od_inflate_app.cpp is unconditional within a build, so this engine handles direct-write,
 * partial-region and PIPE_WRITE regardless of transport. PIPE_WRITE is BLE-only (see "NO PIPE ON
 * LAN" in opendisplay_protocol.h), so the busiest BLE transfer path decodes through tinfl.
 * The gate below keys off OPENDISPLAY_ENABLE_WIFI only as a proxy for "a build that
 * cares about inflate throughput and can spare the RAM" — it does NOT mean the
 * engine is limited to WiFi traffic. Do not read the gate as a transport filter.
 * The "can spare the RAM" half is now load-bearing rather than incidental: that flag
 * is set only on envs with -DBOARD_HAS_PSRAM (see src/wifi_service.h), so the parts
 * without PSRAM — esp32-c3-N4, esp32-c6-N4, esp32-c3-N16, plus the classic esp32-N4
 * and esp32-wrover-e-N4R8 — fall back to uzlib and reclaim the ~15 KB of tables
 * below. They have no PSRAM to relocate anything into, and internal DRAM there is
 * the scarcest resource on the part.
 *
 * WHY: the uzlib engine is a bit-serial, byte-at-a-time resumable state machine —
 * tolerable for BLE (wire << inflate), but the LAN wire is ~10-100x faster so
 * software inflate becomes the bottleneck and compression turns into a net loss.
 * `tinfl` is word-at-a-time + table-driven and lives in mask ROM on S3/C3/C6 (fixed
 * addresses in <chip>.rom.ld), so it is faster at zero flash cost. BLE transfers get
 * the same speedup for free; the LAN wire is just what made it necessary.
 *
 * COST: ~11 KB of internal DRAM for the Huffman tables (tinfl_decompressor), plus a
 * caller-supplied LZ dictionary/output ring (see OD_TINFL_DICT_SIZE in
 * od_inflate_tinfl.cpp — 4 KB at the default 9-bit window, sized for decode-path
 * headroom rather than the 32 KB the TINFL_LZ_DICT_SIZE constant suggests; a window
 * wider than 9 bits sizes it to the window instead). vs ~2.5 KB for uzlib.
 *
 * The status enum (od_zlib_status_t, OD_ZLIB_STATUS_*) is defined by od_zlib_inflate.h; we
 * only include it, never modify it.
 */

#ifndef OD_INFLATE_TINFL_H
#define OD_INFLATE_TINFL_H

#include "od_zlib_inflate.h" /* od_zlib_status_t + OD_ZLIB_STATUS_* */

/* Gate: which BUILDS use this engine — NOT which transports it serves (see SCOPE
 * above; once enabled it handles BLE too). Mirrors the condition that defines
 * OPENDISPLAY_HAS_WIFI (src/wifi_service.h) because a LAN-capable build is the one
 * that needs the throughput. ROM tinfl itself exists on S3/C3/C6 regardless.
 * Overridable via a build flag if a target ever needs to force one engine or the
 * other, e.g. -DOPENDISPLAY_USE_TINFL=0 to keep uzlib's smaller footprint. */
#if !defined(OPENDISPLAY_USE_TINFL)
#  if defined(OPENDISPLAY_ENABLE_WIFI)
#    define OPENDISPLAY_USE_TINFL 1
#  else
#    define OPENDISPLAY_USE_TINFL 0
#  endif
#endif

#if OPENDISPLAY_USE_TINFL

#ifdef __cplusplus
extern "C" {
#endif

/* Same semantics/signatures as od_zlib_stream_* in od_zlib_inflate.h. */
void od_inflate_tinfl_reset(uint32_t expected_output_size);
od_zlib_status_t od_inflate_tinfl_push(const uint8_t *input, size_t len, bool final);
od_zlib_status_t od_inflate_tinfl_poll(uint8_t *output, size_t capacity, size_t *produced);
const char *od_inflate_tinfl_error(void);
uint32_t od_inflate_tinfl_output_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDISPLAY_USE_TINFL */

#endif /* OD_INFLATE_TINFL_H */
