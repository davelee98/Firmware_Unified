/* od_config_asm.c -- see od_config_asm.h for the defect this exists to fix (F3). */
#include "od_config_asm.h"

#include <string.h>

void od_config_asm_reset(struct od_config_asm *s)
{
    if (!s) {
        return;
    }
    s->active     = false;
    s->total_size = 0;
    s->received   = 0;
    s->chunks     = 0;
    /* buffer deliberately not cleared -- see the header. */
}

enum od_config_asm_result od_config_asm_start(struct od_config_asm *s,
                                              const uint8_t *payload, uint16_t len)
{
    if (!s || !payload) {
        return OD_CONFIG_ASM_REJECTED;
    }
    /* A start frame ALWAYS abandons whatever came before. A client that restarts mid-transfer
     * is not resuming, and silently keeping the old partial record is how two sequences merge
     * into one blob nobody sent. */
    od_config_asm_reset(s);

    if (len == 0u) {
        return OD_CONFIG_ASM_REJECTED;
    }

    /* Single-frame configs, including the header's short-first-chunk fallback at 201: too long
     * to be a chunk payload, too short to be a chunked start. The caller stores `payload`
     * directly and no transfer is left in progress. */
    if (len < CONFIG_CHUNK_SIZE_WITH_PREFIX) {
        return OD_CONFIG_ASM_SINGLE;
    }
    /* Neither legal shape. The old code took these as chunked starts and silently discarded
     * everything past the first 200 data bytes. */
    if (len > CONFIG_CHUNK_SIZE_WITH_PREFIX) {
        return OD_CONFIG_ASM_REJECTED;
    }

    const uint32_t total = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8);

    /* Bound the DECLARATION here, at the start, rather than letting an impossible total fail
     * later for an indirect reason:
     *   - <= CONFIG_CHUNK_SIZE would have been a single frame, so it is nonsense chunked;
     *   - > the transferable ceiling cannot be delivered within MAX_CONFIG_CHUNKS, and a
     *     transfer that cannot succeed should be refused before any bytes are stored. */
    if (total <= CONFIG_CHUNK_SIZE || total > OD_CONFIG_ASM_MAX_TRANSFERABLE) {
        return OD_CONFIG_ASM_REJECTED;
    }

    memcpy(s->buffer, payload + 2, CONFIG_CHUNK_SIZE);
    s->active     = true;
    s->total_size = total;
    s->received   = CONFIG_CHUNK_SIZE;
    s->chunks     = 1u;
    return OD_CONFIG_ASM_ACCEPTED;
}

enum od_config_asm_result od_config_asm_chunk(struct od_config_asm *s,
                                              const uint8_t *payload, uint16_t len)
{
    if (!s || !payload) {
        return OD_CONFIG_ASM_REJECTED;
    }
    if (!s->active) {
        return OD_CONFIG_ASM_REJECTED;
    }
    if (len == 0u || len > CONFIG_CHUNK_SIZE) {
        od_config_asm_reset(s);
        return OD_CONFIG_ASM_REJECTED;
    }
    /* Bounded against the DECLARED TOTAL, not merely against the buffer. Bounding on
     * MAX_CONFIG_SIZE alone is what let 400 bytes be committed against a declared 201. */
    if ((uint32_t)len > s->total_size - s->received) {
        od_config_asm_reset(s);
        return OD_CONFIG_ASM_REJECTED;
    }
    /* A non-final chunk must be full. A short one that does not complete the total means the
     * two sides disagree about framing, and continuing would silently accept a record whose
     * length the client never intended. */
    if ((uint32_t)len < CONFIG_CHUNK_SIZE && ((uint32_t)len != s->total_size - s->received)) {
        od_config_asm_reset(s);
        return OD_CONFIG_ASM_REJECTED;
    }
    if (s->chunks >= MAX_CONFIG_CHUNKS) {
        od_config_asm_reset(s);
        return OD_CONFIG_ASM_REJECTED;
    }

    memcpy(s->buffer + s->received, payload, len);
    s->received += len;
    s->chunks++;

    /* COMMIT ON THE BYTE COUNT, never on the chunk count. This one line is the F3 fix; the
     * checks above exist so that reaching it means the record is exactly what was declared. */
    if (s->received == s->total_size) {
        s->active = false;
        return OD_CONFIG_ASM_COMPLETE;
    }
    return OD_CONFIG_ASM_ACCEPTED;
}
