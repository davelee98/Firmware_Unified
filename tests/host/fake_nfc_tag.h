/* fake_nfc_tag.h -- a programmable tag behind shared/core/od_nfc_app.h.
 *
 * KNOBS, NEVER ANSWERS. Nothing here may be given an expected wire frame: the fake exposes what a
 * real tag controller would have -- a record to hand back, a failure to inject, and a record of
 * what it was asked to write -- and the reply bytes are assembled by the shared machine above it.
 *
 * IT MODELS BOTH ADAPTERS, and that is the point rather than an accident. Asked for more than it
 * can supply, BG22 refuses and Nordic truncates (plan N2b), and the shared machine is required to
 * handle either without normalising them. A fake that could only do one of the two would make
 * half that requirement untestable.
 */
#ifndef OD_TEST_FAKE_NFC_TAG_H
#define OD_TEST_FAKE_NFC_TAG_H

#include <stdbool.h>
#include <stdint.h>

#define FAKE_NFC_TAG_MAX 1024u

/* What a read hands back, when it succeeds at all. */
typedef enum {
    FAKE_NFC_OVER_CAP_TRUNCATE = 0,  /* Nordic: return `cap` bytes */
    FAKE_NFC_OVER_CAP_REFUSE         /* BG22: return false, as its staging buffer forces */
} fake_nfc_over_cap_t;

extern bool                fake_nfc_read_ok;      /* false = the tag read fails outright */
extern uint8_t             fake_nfc_read_type;    /* the OD_NFC_REC_* value a read reports */
extern uint16_t            fake_nfc_read_len;     /* how many bytes the tag holds */
extern uint8_t             fake_nfc_read_fill;    /* every held byte, so content is checkable */
extern fake_nfc_over_cap_t fake_nfc_over_cap;

extern bool     fake_nfc_write_ok;                /* false = the tag write fails */

/* Observations. The call count is what proves a REFUSED frame touched no hardware -- an absence
 * that reply bytes cannot show. The data is kept, not just its length, so a record assembled out
 * of order fails here rather than passing on a correct byte count. */
extern unsigned fake_nfc_read_calls;
extern uint16_t fake_nfc_read_cap_seen;           /* the bound the caller asked under */
extern unsigned fake_nfc_write_calls;
extern uint8_t  fake_nfc_write_type;
extern uint16_t fake_nfc_write_len;
extern uint8_t  fake_nfc_write_data[FAKE_NFC_TAG_MAX];

/* Back to a benign, working tag holding four bytes. Call before each case. */
void fake_nfc_tag_reset(void);

#endif /* OD_TEST_FAKE_NFC_TAG_H */
