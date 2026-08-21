/* See fake_nfc_tag.h. */

#include "fake_nfc_tag.h"

#include "od_nfc_app.h"

#include <string.h>

bool                fake_nfc_read_ok;
uint8_t             fake_nfc_read_type;
uint16_t            fake_nfc_read_len;
uint8_t             fake_nfc_read_fill;
fake_nfc_over_cap_t fake_nfc_over_cap;

bool     fake_nfc_write_ok;

unsigned fake_nfc_read_calls;
uint16_t fake_nfc_read_cap_seen;
unsigned fake_nfc_write_calls;
uint8_t  fake_nfc_write_type;
uint16_t fake_nfc_write_len;
uint8_t  fake_nfc_write_data[FAKE_NFC_TAG_MAX];

void fake_nfc_tag_reset(void)
{
    fake_nfc_read_ok = true;
    fake_nfc_read_type = 1u;              /* OD_NFC_REC_URI: not 0, so a dropped type shows */
    fake_nfc_read_len = 4u;
    fake_nfc_read_fill = 0x5Au;
    fake_nfc_over_cap = FAKE_NFC_OVER_CAP_TRUNCATE;
    fake_nfc_write_ok = true;
    fake_nfc_read_calls = 0u;
    fake_nfc_read_cap_seen = 0u;
    fake_nfc_write_calls = 0u;
    fake_nfc_write_type = 0u;
    fake_nfc_write_len = 0u;
    memset(fake_nfc_write_data, 0, sizeof fake_nfc_write_data);
}

bool od_nfc_app_read(uint8_t *type, uint8_t *data, uint16_t *len_io, uint16_t cap)
{
    uint16_t n = fake_nfc_read_len;

    ++fake_nfc_read_calls;
    fake_nfc_read_cap_seen = cap;
    /* *len_io is deliberately NOT read: the seam says it is output-only, and a fake that consulted
     * it would let a caller depending on the in-value pass here and fail on a real adapter. */
    if (!fake_nfc_read_ok || type == NULL || data == NULL || len_io == NULL) {
        return false;
    }
    if (n > cap) {
        if (fake_nfc_over_cap == FAKE_NFC_OVER_CAP_REFUSE) {
            return false;
        }
        n = cap;
    }
    memset(data, fake_nfc_read_fill, n);
    *type = fake_nfc_read_type;
    *len_io = n;
    return true;
}

bool od_nfc_app_write(uint8_t type, const uint8_t *data, uint16_t len)
{
    ++fake_nfc_write_calls;
    fake_nfc_write_type = type;
    fake_nfc_write_len = len;
    if (data != NULL && len <= FAKE_NFC_TAG_MAX) {
        memcpy(fake_nfc_write_data, data, len);
    }
    return fake_nfc_write_ok;
}
