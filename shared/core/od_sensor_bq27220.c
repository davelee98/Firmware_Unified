/* Shared BQ27220 driver. See od_sensor_bq27220.h. */

#include "od_sensor_bq27220.h"

#include "od_hal_i2c.h"
#include "od_log.h"
#include "od_sensor_app.h"

#define BQ27220_CMD_VOLTAGE      0x08u
#define BQ27220_CMD_SOC          0x2Cu
#define BQ27220_MSD_CHARGING_BIT 0x80u
#define BQ27220_MSD_POLL_TTL_MS  30000u
#define BQ27220_DEFAULT_ADDR     0x55u
#define OD_MSD_DYNAMIC_LEN       11u

static float   s_batt_v = -1.0f;
static uint8_t s_soc = 0xFFu;
static bool    s_gauge_ok;

/* First match only -- see the header. */
static const struct SensorData *bq27220_config(const struct od_config *cfg)
{
    if (cfg == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        if (cfg->sensors[i].sensor_type == OD_SENSOR_TYPE_BQ27220) {
            return &cfg->sensors[i];
        }
    }
    return NULL;
}

static uint8_t bq27220_addr_7bit(const struct SensorData *s)
{
    uint8_t a = s->i2c_addr_7bit;

    return (a == 0u || a == 0xFFu) ? BQ27220_DEFAULT_ADDR : a;
}

static bool bq27220_read_block(const struct SensorData *s, uint8_t cmd, uint8_t *buf, uint8_t len)
{
    /* 0xFF means this gauge was never assigned a bus. Refused, not resolved to bus 0 -- a
     * colliding address returns a plausible-but-wrong voltage (DIVERGENCE_MATRIX 13). */
    if (s->bus_id == 0xFFu) {
        return false;
    }
    /* ONE transaction: selector write, repeated START, read, no STOP between. This part does
     * not accept STOP-then-START for a register read -- it answers as if unaddressed, so
     * splitting it yields plausible garbage rather than an error. */
    return od_hal_i2c_write_read(s->bus_id, bq27220_addr_7bit(s), &cmd, 1u, buf, len)
           == OD_HAL_I2C_OK;
}

bool od_sensor_bq27220_is_configured(const struct od_config *cfg)
{
    return bq27220_config(cfg) != NULL;
}

float od_sensor_bq27220_voltage_volts(void)
{
    return s_gauge_ok ? s_batt_v : -1.0f;
}

void od_sensor_bq27220_init(const struct od_config *cfg)
{
    const struct SensorData *s = bq27220_config(cfg);
    uint8_t raw[2];
    uint8_t addr;

    /* The charger GPIO is established whether or not a gauge is configured: the enable pin is a
     * board property, and a board that charges without a gauge still needs its rail driven. */
    (void)od_sensor_app_bq_enable(true);

    if (s == NULL) {
        return;
    }
    if (s->bus_id == 0xFFu) {
        od_log_info("BQ27220: no data_bus assigned, bus_id 0xFF; not probed");
        return;
    }
    /* Hoisted, not inlined into the log call: with logging compiled out the argument would not
     * be evaluated, so a call there can hide a side effect. Ratcheted by tools/check.sh. */
    addr = bq27220_addr_7bit(s);
    if (!bq27220_read_block(s, BQ27220_CMD_VOLTAGE, raw, 2u)) {
        /* info, not warn: a board configured for a gauge it does not have is a
         * normal build, and the Nordic port reported this at info. */
        od_log_info("BQ27220: not found at 0x%02X", addr);
        return;
    }
    od_log_info("BQ27220: fuel gauge at 0x%02X", addr);
}

void od_sensor_bq27220_poll(const struct od_config *cfg, uint32_t now_ms)
{
    static uint32_t last_poll_ms;
    static bool have_polled;
    const struct SensorData *s = bq27220_config(cfg);
    uint8_t raw[2];
    uint8_t soc = 0xFFu;
    uint16_t mv;
    bool charging = false;
    uint8_t msd_idx;

    if (s == NULL) {
        return;
    }
    /* Unsigned subtraction, so the TTL survives the 32-bit millisecond wrap. */
    if (have_polled && (uint32_t)(now_ms - last_poll_ms) < BQ27220_MSD_POLL_TTL_MS) {
        return;
    }
    last_poll_ms = now_ms;
    have_polled = true;

    if (!bq27220_read_block(s, BQ27220_CMD_VOLTAGE, raw, 2u)) {
        /* A failed read INVALIDATES the cache rather than leaving the last good value in place:
         * a stale voltage presented as current is worse than none. */
        s_gauge_ok = false;
        s_batt_v = -1.0f;
        s_soc = 0xFFu;
        return;
    }
    mv = (uint16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    s_batt_v = (float)mv / 1000.0f;
    /* Zero millivolts is not a discharged pack, it is a gauge that is not answering properly. */
    s_gauge_ok = (mv > 0u);

    if (bq27220_read_block(s, BQ27220_CMD_SOC, &soc, 1u)) {
        if (soc > 100u) {
            soc = 100u;
        }
        s_soc = soc;
    } else {
        s_soc = 0xFFu;
    }

    /* Tri-state: no state pin means unknown, and unknown packs as not-charging because the MSD
     * has one bit and no way to say "do not know". The distinction is preserved at the seam so
     * a caller that can act on it still may. */
    if (!od_sensor_app_bq_charging(&charging)) {
        charging = false;
    }

    msd_idx = s->msd_data_start_byte;
    if (msd_idx >= OD_MSD_DYNAMIC_LEN) {
        return;
    }
    if (!s_gauge_ok || s_soc > 100u) {
        od_sensor_app_msd_write(msd_idx, 0xFFu);
    } else {
        uint8_t packed = (uint8_t)(s_soc & 0x7Fu);

        if (charging) {
            packed |= BQ27220_MSD_CHARGING_BIT;
        }
        od_sensor_app_msd_write(msd_idx, packed);
    }
}
