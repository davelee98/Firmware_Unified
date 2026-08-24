/* Framing, CRC and bounds for the stored config record. See od_config_store.h. */

#include "od_config_store.h"

#include <string.h>

#include "od_hal_nvs.h"

/* The header is written and read byte-wise rather than as a struct. The bytes are the same on
 * every target this repo builds -- all little-endian -- but the record is a persistence format
 * with deployed devices behind it, and a format is not something to leave to a struct layout
 * that a padding rule or a compiler flag can move. */
static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

uint32_t od_config_store_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static enum od_config_store_result from_hal(int rc)
{
    switch (rc) {
    case OD_HAL_NVS_OK:     return OD_CONFIG_STORE_OK;
    case OD_HAL_NVS_ENOENT: return OD_CONFIG_STORE_EMPTY;
    case OD_HAL_NVS_E2BIG:  return OD_CONFIG_STORE_TOO_BIG;
    default:                return OD_CONFIG_STORE_IO;
    }
}

enum od_config_store_result od_config_store_init(void)
{
    return from_hal(od_hal_nvs_init());
}

enum od_config_store_result od_config_store_save(void *workspace, uint32_t workspace_cap,
                                                 const uint8_t *payload, uint32_t len)
{
    uint8_t *ws = (uint8_t *)workspace;
    uint8_t *slot;
    uint32_t total;
    uint32_t crc;

    if (ws == NULL || payload == NULL) {
        return OD_CONFIG_STORE_IO;
    }
    if (len > OD_CONFIG_MAX_SIZE) {
        return OD_CONFIG_STORE_TOO_BIG;
    }
    total = OD_CONFIG_STORE_HEADER_SIZE + len;
    if (workspace_cap < total) {
        return OD_CONFIG_STORE_TOO_BIG;
    }

    /* Before the header is written: on a target whose workspace overlays the config assembler,
     * the four words this is about to overwrite are that assembler's live state, and the
     * payload may be its buffer. */
    crc = od_config_store_crc32(payload, len);

    slot = od_config_store_payload(ws);
    if (payload != slot && len > 0u) {
        /* memmove, not memcpy: a caller may legitimately stage the payload somewhere that
         * overlaps the workspace. */
        memmove(slot, payload, len);
    }

    put_u32le(ws + 0,  OD_CONFIG_STORE_MAGIC);
    put_u32le(ws + 4,  OD_CONFIG_STORE_VERSION);
    put_u32le(ws + 8,  crc);
    put_u32le(ws + 12, len);

    return from_hal(od_hal_nvs_write(ws, total));
}

enum od_config_store_result od_config_store_load(uint8_t *payload, uint32_t *len)
{
    uint8_t header[OD_CONFIG_STORE_HEADER_SIZE];
    uint32_t cap;
    uint32_t stored = 0;
    uint32_t data_len;
    int rc;

    if (len == NULL) {
        return OD_CONFIG_STORE_IO;
    }
    cap = *len;
    *len = 0;
    if (payload == NULL) {
        return OD_CONFIG_STORE_IO;
    }

    rc = od_hal_nvs_size(&stored);
    if (rc != OD_HAL_NVS_OK) {
        return from_hal(rc);
    }
    if (stored < OD_CONFIG_STORE_HEADER_SIZE) {
        /* Too short to carry a header at all: not a record this firmware wrote. */
        return OD_CONFIG_STORE_CORRUPT;
    }
    if (stored > OD_CONFIG_STORE_MAX_RECORD) {
        /* Longer than any record this build can produce, so refuse before trusting a header
         * inside it. The medium can hold one -- BG22's NVM3 objects go to 2112 bytes against a
         * 2064-byte record cap -- and a device provisioned by a larger-cap build is exactly
         * how one arrives. Bounding `stored` here is what keeps this decision the core's
         * rather than each medium's: two of the three HALs happen to refuse it first, and
         * BG22's does not. */
        return OD_CONFIG_STORE_TOO_BIG;
    }

    rc = od_hal_nvs_read(0, header, OD_CONFIG_STORE_HEADER_SIZE);
    if (rc != OD_HAL_NVS_OK) {
        return from_hal(rc);
    }
    if (get_u32le(header + 0) != OD_CONFIG_STORE_MAGIC) {
        return OD_CONFIG_STORE_CORRUPT;
    }
    /* header + 4 is `version`, carried and deliberately not checked. */
    data_len = get_u32le(header + 12);

    if (data_len > OD_CONFIG_MAX_SIZE || data_len > cap) {
        return OD_CONFIG_STORE_TOO_BIG;
    }
    if (stored - OD_CONFIG_STORE_HEADER_SIZE < data_len) {
        /* The header declares more payload than the medium is holding. */
        return OD_CONFIG_STORE_CORRUPT;
    }

    if (data_len > 0u) {
        rc = od_hal_nvs_read(OD_CONFIG_STORE_HEADER_SIZE, payload, data_len);
        if (rc != OD_HAL_NVS_OK) {
            return from_hal(rc);
        }
    }
    if (od_config_store_crc32(payload, data_len) != get_u32le(header + 8)) {
        return OD_CONFIG_STORE_CORRUPT;
    }

    *len = data_len;
    return OD_CONFIG_STORE_OK;
}

enum od_config_store_result od_config_store_clear(void)
{
    return from_hal(od_hal_nvs_erase());
}
