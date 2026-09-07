/* S130 2.0.1 occupies 0x00000..0x1AFFF. Keep the final two flash pages untouched. */
SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x1B000, LENGTH = 0x4800
    RAM   (rwx) : ORIGIN = 0x20001F00, LENGTH = 0x2100
}

INCLUDE "nrf5x_common.ld"

ASSERT(__preinit_array_start == __preinit_array_end,
       "nRF51 firmware must not contain preinitializers")
ASSERT(__init_array_start == __init_array_end,
       "nRF51 firmware must not contain constructors")
ASSERT(__HeapBase == __HeapLimit, "nRF51 firmware must not reserve a heap")
