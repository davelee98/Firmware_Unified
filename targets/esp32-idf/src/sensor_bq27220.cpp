#include "sensor_bq27220.h"
#include "structs.h"
#include "display_service.h"
#include "od_log.h"

#include "od_hal_gpio.h"
#include "od_hal_i2c.h"
#include "od_hal_time.h"

extern struct od_config globalConfig;
extern uint8_t dynamicreturndata[11];

static_assert(sizeof(SensorData) == 30, "SensorData must remain 30 bytes");

#ifndef OD_CHARGER_FLAG_ENABLE_ACTIVE_LOW
#define OD_CHARGER_FLAG_ENABLE_ACTIVE_LOW (1u << 0)
#endif
#ifndef OD_CHARGER_FLAG_STATE_ACTIVE_LOW
#define OD_CHARGER_FLAG_STATE_ACTIVE_LOW (1u << 1)
#endif

#define BQ27220_CMD_VOLTAGE 0x08u
#define BQ27220_CMD_SOC 0x2Cu

#define BQ27220_MSD_CHARGING_BIT 0x80u

static float s_batt_v = -1.0f;
static uint8_t s_soc = 0xFF;
static bool s_gauge_ok = false;

static bool validPin(uint8_t pin) { return pin != 0 && pin != 0xFF; }

static uint8_t bq27220_addr_7bit(const SensorData* s) {
    uint8_t a = s->i2c_addr_7bit;
    if (a == 0 || a == 0xFF) {
        return 0x55;
    }
    return a;
}

// 0xFF means this gauge was never assigned a bus. Refused, not resolved to bus 0 -- see
// DIVERGENCE_MATRIX 13; a colliding address returns a plausible-but-wrong voltage.
static bool bq27220_ensure_bus(const SensorData* s) {
    if (s->bus_id == 0xFF) {
        return false;
    }
    return initOrRestoreWireForBus(s->bus_id);
}

static bool bq27220_read_block(const SensorData* s, uint8_t cmd, uint8_t* buf, uint8_t len) {
    uint8_t addr = bq27220_addr_7bit(s);
    if (!bq27220_ensure_bus(s)) {
        return false;
    }
    // ONE transaction: selector write, repeated START, read -- no STOP between. The BQ27220
    // does not accept STOP + fresh START for a register read; it answers as if unaddressed, so
    // splitting this produces plausible garbage rather than an error. Under Wire this was the
    // endTransmission(false) idiom, and the flag being honoured was the whole point.
    return od_hal_i2c_write_read(addr, &cmd, 1, buf, len) == OD_HAL_I2C_OK;
}

static const SensorData* bq27220_config(void) {
    for (uint8_t i = 0; i < globalConfig.sensor_count; i++) {
        if (globalConfig.sensors[i].sensor_type == OD_SENSOR_TYPE_BQ27220) {
            return &globalConfig.sensors[i];
        }
    }
    return nullptr;
}

static uint8_t bq27220_msd_start(const SensorData* s) {
    return s->msd_data_start_byte;
}

static void write_dynamic_byte(uint8_t idx, uint8_t value) {
    if (idx <= 10) {
        dynamicreturndata[idx] = value;
    }
}

bool bq27220IsConfigured(void) {
    return bq27220_config() != nullptr;
}

float bq27220BatteryVoltageVolts(void) {
    return s_gauge_ok ? s_batt_v : -1.0f;
}

void initChargerGpio(void) {
    const uint8_t flags = globalConfig.power_option.charger_flags;
    const uint8_t en = globalConfig.power_option.charge_enable_pin;
    if (validPin(en)) {
        const bool activeLow = (flags & OD_CHARGER_FLAG_ENABLE_ACTIVE_LOW) != 0;
        // pinMode(OUTPUT) then digitalWrite in one call -- gpio_config() then gpio_set_level(),
        // the same two register writes in the same order. See od_hal_gpio.h.
        od_hal_gpio_config_output(en, !activeLow);
    }
    const uint8_t st = globalConfig.power_option.charge_state_pin;
    if (validPin(st)) {
        od_hal_gpio_config_input(st, /*pull_up=*/true, /*pull_down=*/false);
    }
}

static bool charger_gpio_charging(void) {
    const uint8_t st = globalConfig.power_option.charge_state_pin;
    if (!validPin(st)) {
        return false;
    }
    const bool activeLow = (globalConfig.power_option.charger_flags & OD_CHARGER_FLAG_STATE_ACTIVE_LOW) != 0;
    const int level = od_hal_gpio_read(st);
    return activeLow ? (level == 1) : (level == 0);
}

void initBq27220Sensors(void) {
    initChargerGpio();
    const SensorData* s = bq27220_config();
    if (!s) {
        return;
    }
    if (!bq27220_ensure_bus(s)) {
        od_log_warn("BQ27220: bus init failed");
        return;
    }
    const uint8_t addr = bq27220_addr_7bit(s);
    uint8_t raw[2];
    if (!bq27220_read_block(s, BQ27220_CMD_VOLTAGE, raw, 2)) {
        od_log_warn("BQ27220: not found @0x%02X", addr);
        return;
    }
    od_log_info("BQ27220: fuel gauge @0x%02X", addr);
}

static constexpr uint32_t kBq27220MsdPollTtlMs = 30000u;

void pollBq27220ForMsd(void) {
    const SensorData* s = bq27220_config();
    if (!s) {
        return;
    }
    static uint32_t lastPollMs = 0;
    static bool havePolled = false;
    if (havePolled && (uint32_t)(od_hal_uptime_ms() - lastPollMs) < kBq27220MsdPollTtlMs) {
        return;
    }
    lastPollMs = od_hal_uptime_ms();
    havePolled = true;
    uint8_t raw[2];
    if (!bq27220_read_block(s, BQ27220_CMD_VOLTAGE, raw, 2)) {
        s_gauge_ok = false;
        s_batt_v = -1.0f;
        s_soc = 0xFF;
        return;
    }
    uint16_t mv = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
    s_batt_v = mv / 1000.0f;
    s_gauge_ok = mv > 0;

    uint8_t soc = 0xFF;
    if (bq27220_read_block(s, BQ27220_CMD_SOC, &soc, 1)) {
        if (soc > 100) {
            soc = 100;
        }
        s_soc = soc;
    } else {
        s_soc = 0xFF;
    }

    const bool charging = charger_gpio_charging();

    const uint8_t msdIdx = bq27220_msd_start(s);
    if (msdIdx <= 10) {
        if (!s_gauge_ok || s_soc > 100) {
            write_dynamic_byte(msdIdx, 0xFF);
        } else {
            uint8_t packed = (uint8_t)(s_soc & 0x7Fu);
            if (charging) {
                packed |= BQ27220_MSD_CHARGING_BIT;
            }
            write_dynamic_byte(msdIdx, packed);
        }
    }
}
