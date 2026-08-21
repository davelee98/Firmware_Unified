/* od_nfc_app.h -- target tag seam for the shared CMD_NFC_ENDPOINT (0x0083) machine.
 *
 * Shared code owns the sub-command parsing, record-type validation, length bounds, chunk assembly,
 * ownership, reply construction and every error code. Targets own NDEF encode/decode and the tag
 * transport beneath it -- the on-SoC NFCT peripheral on Nordic, an I2C tag controller on BG22.
 *
 * NOTHING ELSE CROSSES. An implementation that starts parsing a sub-command, choosing an error
 * code, or reading a length field out of the wire is on the wrong side of the line, and the seam
 * is wrong rather than the caller.
 */
#ifndef OD_NFC_APP_H
#define OD_NFC_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the tag's current record.
 *
 * `cap` is the SOLE bound, and `*len_io` is OUTPUT-ONLY: an implementation must not read it on
 * entry. The two shipping adapters happen to be called with it pre-set to the same value as `cap`,
 * and neither reads it back -- so the in-value is dead today and is pinned dead here, because a
 * signature this close to the one it replaces is an invitation for a later adapter to start
 * depending on whatever the caller left there.
 *
 * ABOVE `cap` BOTH OUTCOMES ARE DEPLOYED, and the shared machine normalises neither. Refusal and
 * truncation are properties of the record and the adapter together, not of the adapter alone:
 * BG22 refuses everything over its staging buffer, which is smaller than the cap a caller
 * requests, while Nordic truncates a verbatim or well-known record and refuses a MIME one that
 * would not fit whole (`opendisplay_nfc.c`, the `out_pack > out_max` arm). Report what came back
 * -- false, or a short length -- and let the caller turn that into a wire answer. Do not assume
 * either outcome from the target alone.
 *
 * On true, `*type` is one of OD_NFC_REC_* and `*len_io` is how many bytes were written to `data`,
 * never more than `cap`. On false nothing in `data`, `*type` or `*len_io` is meaningful. */
bool od_nfc_app_read(uint8_t *type, uint8_t *data, uint16_t *len_io, uint16_t cap);

/* Commit one record to the tag. `data` is `len` bytes of host payload, already assembled and
 * length-checked; encoding it as NDEF is the implementation's job. Returns false if the tag was
 * not written, which the caller reports and does not retry. */
bool od_nfc_app_write(uint8_t type, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* OD_NFC_APP_H */
