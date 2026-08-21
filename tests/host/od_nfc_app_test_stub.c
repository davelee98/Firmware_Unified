/* od_nfc_app_test_stub.c -- the tag seam for host binaries that link the shared 0x83 machine but
 * do not exercise it.
 *
 * od_core_reset() names od_nfc_reset(), so od_nfc.o is pulled into every executable that resets
 * shared state -- three steps before dispatch routes anything to it. Those binaries need the seam
 * to resolve and nothing more.
 *
 * IT REFUSES RATHER THAN SUCCEEDS, and is listed in an explicit source list rather than provided
 * as a weak default: a suite that starts depending on NFC behaviour should fail visibly here and
 * be given the real fake (tests/host/fake_nfc_tag.c), not quietly pass against a stub.
 */

#include "od_nfc_app.h"

bool od_nfc_app_read(uint8_t *type, uint8_t *data, uint16_t *len_io, uint16_t cap)
{
    (void)type; (void)data; (void)len_io; (void)cap;
    return false;
}

bool od_nfc_app_write(uint8_t type, const uint8_t *data, uint16_t len)
{
    (void)type; (void)data; (void)len;
    return false;
}
