/* fastepd_adapter.cpp -- storage for the FastEPD vendor adapter's one global.
 *
 * The `SPI` object FastEPD's IT8951 transport writes against. It lived in
 * compat/arduino_compat.cpp until 2026-08-04, which meant the shim's translation unit had to
 * include <SPI.h> -- so the adapter could not be taken off the component's global include path
 * while its definition sat in a file that is not part of the adapter.
 *
 * Moving it here is what makes the containment real rather than cosmetic: vendor/fastepd/ is
 * now self-contained, and main/CMakeLists.txt grants its include directory to a named list of
 * translation units instead of to everything.
 *
 * This file is NOT part of compat/ and does not die with it. See SPI.h for why the adapter is
 * permanent, and main/CMakeLists.txt for the consumer list.
 */
#include "SPI.h"

#include <stdint.h>

#include "od_hal_time.h"

SPIClass SPI;

/* millis(), for FastEPD's 19 call sites (third_party/FastEPD/src/arduino_io.inl declares it and
 * deliberately does not define it). The return type must stay uint32_t to match that
 * declaration. Truncating to 32 bits is what Arduino does and what the callers assume: they
 * compare by subtraction, which is wrap-safe.
 *
 * It lives here rather than in a shared primitives file because FastEPD is the only thing that
 * wants it, and this adapter is the permanent home for what FastEPD wants. */
uint32_t millis(void)
{
    return od_hal_uptime_ms();
}
