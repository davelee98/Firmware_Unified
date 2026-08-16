/* zephyr/drivers/hwinfo.h -- host stand-in, one function.
 *
 * The silicon id is WIRE-VISIBLE on this target: four bytes of it feed both the KDF and the auth
 * proof, so how they are selected out of the eight is the difference between one device and
 * another to the host. That is the whole reason a test drives this rather than trusting a move. */

#ifndef OD_TEST_FAKE_ZEPHYR_HWINFO_H
#define OD_TEST_FAKE_ZEPHYR_HWINFO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of bytes written, or negative on failure -- the real contract. */
int hwinfo_get_device_id(uint8_t *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
