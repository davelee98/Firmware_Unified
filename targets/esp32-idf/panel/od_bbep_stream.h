/* od_bbep_stream.h -- data-stream writes on the bb_epaper SPI device.
 *
 * OUR CODE, not vendored, and not part of the bb_epaper contract: upstream has no notion of a
 * caller streaming payload bytes of its own between bbepStartDataStream() and
 * bbepEndDataStream(). The E1004 dual-controller panel needs exactly that, because it splits
 * one plane across two chip selects and display_service.cpp reorders the bytes as it goes.
 *
 * WHY THIS EXISTS AT ALL. Before this, e1004_write_stream_bytes() sent its payload through the
 * Arduino SPI object in vendor/fastepd/SPI.h, which registers its OWN device on SPI2_HOST --
 * the same host panel/od_bbep_idf_io.inl owns for bb_epaper. Two drivers, two device handles,
 * one bus. It was visible in the log: bbepDeInitIO() warns "another device is still attached"
 * on teardown and then cannot re-attach the pins, which is the failure mode that function's
 * comment describes at length. Routing the payload here leaves ONE owner of the host.
 *
 * It also removes the last non-FastEPD consumer of the FastEPD vendor adapter, so
 * vendor/fastepd stops being reachable from display_service.cpp (main/CMakeLists.txt).
 *
 * CS SEMANTICS -- the reason this is not just bbepWriteData(). od_bbep_spi_write() asserts CS,
 * transmits, and DEASSERTS on every call, which is bb_epaper's normal per-row framing. The
 * E1004 stream instead holds CS asserted across a whole half-plane. These three preserve that
 * exactly: begin asserts, write transmits with CS untouched, end deasserts. The wire behaviour
 * is bit-for-bit what shipped; only the device handle carrying the bytes changes.
 *
 * NOT re-entrant and not thread-safe: one stream at a time, on the loop task, which is the
 * only way display_service.cpp uses it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Assert CS on the pin currently selected in pBBEP->iCSPin and hold it. The caller is
 * responsible for having issued the controller's data-write command first
 * (bbepStartDataStream). Returns false if the SPI device is not up. */
bool od_bbep_stream_begin(int cs_pin);

/* Transmit payload with CS held. Returns false on a driver error; the error is logged once per
 * session by the same rate limit od_bbep_spi_write() uses. */
bool od_bbep_stream_write(const uint8_t *buf, int len);

/* Release CS. Idempotent -- safe to call without a matching begin(). */
void od_bbep_stream_end(void);

#ifdef __cplusplus
}
#endif
