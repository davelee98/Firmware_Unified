/* od_config.c -- see od_config.h. Plain C99, no HAL, no allocation, no logging. */

#include "od_config.h"

#include <stddef.h>
#include <string.h>

/* One repeatable type's storage, so the cap-and-append rule is written ONCE. The ESP32 spelled
 * it out eight times and each copy was an independent chance to compare against the wrong
 * bound or to increment the wrong counter. */
struct od_config_slot {
    uint8_t *base;        /* first element */
    uint8_t *count;       /* the instance counter beside it */
    uint16_t elem_size;
    uint8_t  max;
};

static enum od_config_apply store_repeatable(const struct od_config_slot *slot,
                                             const uint8_t *body, uint16_t body_len)
{
    if (body_len < slot->elem_size) {
        return OD_CONFIG_APPLY_SHORT_BODY;
    }
    if (*slot->count >= slot->max) {
        /* SKIP, NOT OVERWRITE: the instances already stored are the ones the host sent first,
         * and all three targets agreed on keeping them. */
        return OD_CONFIG_APPLY_FULL;
    }
    memcpy(slot->base + ((size_t)(*slot->count) * slot->elem_size), body, slot->elem_size);
    (*slot->count)++;
    return OD_CONFIG_APPLY_STORED;
}

static enum od_config_apply store_single(void *dst, uint16_t dst_size,
                                         const uint8_t *body, uint16_t body_len, bool *loaded)
{
    if (body_len < dst_size) {
        return OD_CONFIG_APPLY_SHORT_BODY;
    }
    memcpy(dst, body, dst_size);
    if (loaded != NULL) {
        *loaded = true;
    }
    return OD_CONFIG_APPLY_STORED;
}

void od_config_reset(struct od_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

bool od_config_security_key_set(const struct SecurityConfig *sec)
{
    unsigned i;

    if (sec == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(sec->encryption_key); ++i) {
        if (sec->encryption_key[i] != 0u) {
            return true;
        }
    }
    return false;
}

#if OD_CONFIG_WITH_DATA_EXTENDED
/* Every string is a fixed 32-byte field that a host may fill completely, leaving no terminator.
 * Firmware terminates all nine at parse time so no consumer has to remember; doing it here is
 * what keeps a printf on a name from running into the next field. */
static void data_extended_terminate(struct DataExtended *de)
{
    de->manufacturer_name[31] = '\0';
    de->model_name[31] = '\0';
    de->serial_number[31] = '\0';
    de->friendly_name[31] = '\0';
    de->device_location[31] = '\0';
    de->device_id[31] = '\0';
    de->custom_string_1[31] = '\0';
    de->custom_string_2[31] = '\0';
    de->custom_string_3[31] = '\0';
}
#endif

enum od_config_apply od_config_apply_packet(struct od_config *cfg, uint8_t packet_id,
                                            const uint8_t *body, uint16_t body_len)
{
    struct od_config_slot slot;

    if (cfg == NULL || body == NULL) {
        return OD_CONFIG_APPLY_SHORT_BODY;
    }

    switch (packet_id) {
    case 0x01:
        return store_single(&cfg->system_config, (uint16_t)sizeof cfg->system_config,
                            body, body_len, NULL);
    case 0x02:
        return store_single(&cfg->manufacturer_data, (uint16_t)sizeof cfg->manufacturer_data,
                            body, body_len, NULL);
    case 0x04:
        return store_single(&cfg->power_option, (uint16_t)sizeof cfg->power_option,
                            body, body_len, NULL);

    case 0x20:
        slot.base = (uint8_t *)cfg->displays;
        slot.count = &cfg->display_count;
        slot.elem_size = (uint16_t)sizeof cfg->displays[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_DISPLAYS;
        return store_repeatable(&slot, body, body_len);
    case 0x21:
        slot.base = (uint8_t *)cfg->leds;
        slot.count = &cfg->led_count;
        slot.elem_size = (uint16_t)sizeof cfg->leds[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_LEDS;
        return store_repeatable(&slot, body, body_len);
    case 0x23:
        slot.base = (uint8_t *)cfg->sensors;
        slot.count = &cfg->sensor_count;
        slot.elem_size = (uint16_t)sizeof cfg->sensors[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_SENSORS;
        return store_repeatable(&slot, body, body_len);
    case 0x24:
        slot.base = (uint8_t *)cfg->data_buses;
        slot.count = &cfg->data_bus_count;
        slot.elem_size = (uint16_t)sizeof cfg->data_buses[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_DATA_BUSES;
        return store_repeatable(&slot, body, body_len);
    case 0x25:
        slot.base = (uint8_t *)cfg->binary_inputs;
        slot.count = &cfg->binary_input_count;
        slot.elem_size = (uint16_t)sizeof cfg->binary_inputs[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_BINARY_INPUTS;
        return store_repeatable(&slot, body, body_len);
    case 0x2B:
        slot.base = (uint8_t *)cfg->flash_configs;
        slot.count = &cfg->flash_config_count;
        slot.elem_size = (uint16_t)sizeof cfg->flash_configs[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_FLASH;
        return store_repeatable(&slot, body, body_len);

    case 0x28:
#if OD_CONFIG_WITH_TOUCH
        slot.base = (uint8_t *)cfg->touch_controllers;
        slot.count = &cfg->touch_controller_count;
        slot.elem_size = (uint16_t)sizeof cfg->touch_controllers[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_TOUCH;
        return store_repeatable(&slot, body, body_len);
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x29:
#if OD_CONFIG_WITH_BUZZER
        slot.base = (uint8_t *)cfg->passive_buzzers;
        slot.count = &cfg->passive_buzzer_count;
        slot.elem_size = (uint16_t)sizeof cfg->passive_buzzers[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_BUZZERS;
        return store_repeatable(&slot, body, body_len);
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x2A:
#if OD_CONFIG_WITH_NFC
        slot.base = (uint8_t *)cfg->nfc_configs;
        slot.count = &cfg->nfc_config_count;
        slot.elem_size = (uint16_t)sizeof cfg->nfc_configs[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_NFC;
        return store_repeatable(&slot, body, body_len);
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x2C:
#if OD_CONFIG_WITH_DATA_EXTENDED
        {
            enum od_config_apply r = store_single(&cfg->data_extended,
                                                  (uint16_t)sizeof cfg->data_extended,
                                                  body, body_len, &cfg->data_extended_loaded);
            if (r == OD_CONFIG_APPLY_STORED) {
                data_extended_terminate(&cfg->data_extended);
            }
            return r;
        }
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x26:
#if OD_CONFIG_WITH_WIFI
        return store_single(&cfg->wifi_config, (uint16_t)sizeof cfg->wifi_config,
                            body, body_len, &cfg->wifi_config_loaded);
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x27:
        {
            enum od_config_apply r = store_single(&cfg->security, (uint16_t)sizeof cfg->security,
                                                  body, body_len, &cfg->security_loaded);
            if (r != OD_CONFIG_APPLY_STORED) {
                return r;
            }
            /* THE ZERO-KEY RULE, from the Firmware repo. A config that asks for encryption but
             * carries an all-zero key does not get it: the alternative is a device demanding
             * authentication against a key any client can guess, which reads as protected and
             * is not. Applied at store time so no consumer can reach the un-normalised value --
             * the divergence on the other two targets is exactly that their gate reads a field
             * nothing had normalised. */
            if (!od_config_security_key_set(&cfg->security)) {
                cfg->security.encryption_enabled = 0u;
            }
            return OD_CONFIG_APPLY_STORED;
        }

    default:
        return OD_CONFIG_APPLY_UNKNOWN_ID;
    }
}

/* --------------------------------------------------------------------------- the orchestration --- */

struct parse_ctx {
    struct od_config *cfg;
    struct od_config_report *report;
};

static bool on_packet(void *vctx, uint8_t packet_id, const uint8_t *body, uint16_t body_len)
{
    struct parse_ctx *ctx = (struct parse_ctx *)vctx;
    enum od_config_apply r = od_config_apply_packet(ctx->cfg, packet_id, body, body_len);

    if (ctx->report != NULL) {
        switch (r) {
        case OD_CONFIG_APPLY_STORED:     ctx->report->stored++;            break;
        case OD_CONFIG_APPLY_FULL:       ctx->report->dropped_full++;      break;
        case OD_CONFIG_APPLY_NOT_BUILT:  ctx->report->dropped_not_built++; break;
        default:                                                           break;
        }
    }

    /* NEVER ABORTS THE WALK. A cap reached, a subsystem this build lacks, and an id the walk
     * knew but this table does not are all capability differences, not parse errors -- the walk
     * header says so, and aborting would discard packets that follow a full instance array.
     * OD_CONFIG_APPLY_SHORT_BODY cannot arrive from the walk, which guarantees the declared
     * size before calling; if it ever does, the packet is skipped rather than trusted. */
    return true;
}

enum od_config_tlv_result od_config_parse(struct od_config *cfg, const uint8_t *blob,
                                          uint32_t len, struct od_config_report *report)
{
    struct parse_ctx ctx;
    enum od_config_tlv_result walk;
    uint8_t version = 0u;
    uint8_t unknown_id = 0u;

    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    if (cfg == NULL) {
        return OD_CFG_TLV_TOO_SHORT;
    }

    od_config_reset(cfg);
    if (blob == NULL) {
        return OD_CFG_TLV_TOO_SHORT;
    }

    ctx.cfg = cfg;
    ctx.report = report;
    walk = od_config_tlv_walk(blob, len, on_packet, &ctx, &version, &unknown_id);

    cfg->version = version;
    /* Not carried by the current format. Firmware sets it to 0 explicitly rather than leaving
     * it undefined, and a consumer reading a stale minor version is worse than reading 0. */
    cfg->minor_version = 0u;

    if (report != NULL) {
        report->unknown_id = unknown_id;
    }

    if (walk != OD_CFG_TLV_OK) {
        /* loaded stays false: a truncated blob leaves a ZEROED config, not a half-filled one,
         * because the reset above ran before the walk. A caller that ignores this return still
         * cannot mistake a partial parse for a real config. */
        return walk;
    }

    /* Advisory CRC, computed last so it is reported even when nothing was stored. The trailing
     * two bytes are the stored value; everything before them is the covered region. */
    if (len >= OD_CFG_TLV_CRC_LEN && report != NULL) {
        report->crc_checked = true;
        report->crc_stored = (uint16_t)((uint16_t)blob[len - 2u] |
                                        ((uint16_t)blob[len - 1u] << 8));
        report->crc_computed = od_config_tlv_crc16(blob, len - OD_CFG_TLV_CRC_LEN);
    }

    cfg->loaded = true;
    return OD_CFG_TLV_OK;
}
