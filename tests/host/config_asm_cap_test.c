/* A separately compiled 2048-byte profile. This is intentionally not the ordinary config_asm
 * executable: struct od_config_asm changes ABI with OD_CONFIG_MAX_SIZE, so both this translation
 * unit and the shared implementation must be compiled with the same definition. */

#include "od_config_asm.h"

#include <stdio.h>
#include <string.h>

static struct od_config_asm s;
static uint8_t frame[CONFIG_CHUNK_SIZE_WITH_PREFIX];

static enum od_config_asm_result start(uint16_t total)
{
    frame[0] = (uint8_t)(total & 0xffu);
    frame[1] = (uint8_t)(total >> 8);
    memset(frame + 2, 0xa5, CONFIG_CHUNK_SIZE);
    return od_config_asm_start(&s, od_span_make(frame, sizeof frame));
}

int main(void)
{
    if (OD_CONFIG_MAX_SIZE != 2048u || sizeof s.buffer != 2048u) {
        return 1;
    }
    od_config_asm_reset(&s);
    if (start(2049u) != OD_CONFIG_ASM_REJECTED || s.active) {
        fprintf(stderr, "cap+1 declaration was accepted\n");
        return 1;
    }
    od_config_asm_reset(&s);
    if (start(2048u) != OD_CONFIG_ASM_ACCEPTED || !s.active) {
        fprintf(stderr, "cap declaration was rejected\n");
        return 1;
    }
    return 0;
}
