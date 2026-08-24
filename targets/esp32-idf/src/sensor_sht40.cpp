#include "sensor_sht40.h"
#include "structs.h"
#include "display_service.h"
#include "od_log.h"

#include "od_hal_i2c.h"
#include "od_hal_time.h"
#include "od_hal_sleep.h"

extern struct od_config globalConfig;
extern uint8_t dynamicreturndata[11];

static_assert(sizeof(SensorData) == 30, "SensorData must remain 30 bytes");

static const uint8_t SHT40_CMD_MEASURE_HIGH = 0xFD;
static const uint8_t SHT40_CMD_SOFT_RESET = 0x94;
static const uint8_t SHT40_MEASURE_DELAY_MS = 12;

static uint8_t sht40_crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; bit--) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static uint8_t sht40_addr_7bit(const SensorData* s) {
    uint8_t a = s->i2c_addr_7bit;
    if (a == 0 || a == 0xFF) {
        return 0x44;
    }
    return a;
}

static uint8_t sht40_msd_start(const SensorData* s) {
    uint8_t st = s->msd_data_start_byte;
    if (st == 0xFF || st == 0) {
        return 7;
    }
    return st;
}

// err_out keeps Arduino's endTransmission() encoding (0 ok, 2 address NACK, 4 other) because
// it is reported straight into a log line and, on the failure path, into the sensor's error
// byte -- a value a host may already be matching on. Translating at this one site rather than
// letting od_hal_i2c's codes leak into the wire-visible byte.
static bool sht40_write_cmd(uint8_t bus_id, uint8_t addr7, uint8_t cmd, uint8_t* err_out) {
    const int rc = od_hal_i2c_write(bus_id, addr7, &cmd, 1);
    if (err_out) {
        *err_out = (rc == OD_HAL_I2C_OK) ? 0u : ((rc == OD_HAL_I2C_EINVAL) ? 4u : 2u);
    }
    return rc == OD_HAL_I2C_OK;
}

static bool sht40_read_measurement(uint8_t bus_id, uint8_t addr7, int16_t* temp_centi, uint16_t* rh_centi, uint8_t* err_out) {
    uint8_t err = 0;
    if (!sht40_write_cmd(bus_id, addr7, SHT40_CMD_MEASURE_HIGH, &err)) {
        if (err_out) {
            *err_out = err;
        }
        return false;
    }
    // The command is a SEPARATE transaction from the read, with a STOP and this delay between:
    // the SHT40 needs the conversion time before it will answer. Not a repeated-START read --
    // that is the BQ27220's idiom, and it is why od_hal_i2c exposes both rather than one
    // register-shaped call (see od_hal_i2c.h).
    od_hal_delay_ms(SHT40_MEASURE_DELAY_MS);
    uint8_t b[6];
    if (od_hal_i2c_read(bus_id, addr7, b, sizeof(b)) != OD_HAL_I2C_OK) {
        if (err_out) {
            // 0xFE was the "requestFrom returned negative" code; a short read cannot happen
            // through the HAL, which is all-or-nothing, so the partial-count values that used
            // to be reported here no longer arise.
            *err_out = 0xFEu;
        }
        return false;
    }
    if (sht40_crc8(b, 2) != b[2]) {
        if (err_out) {
            *err_out = 0xFD;
        }
        return false;
    }
    if (sht40_crc8(b + 3, 2) != b[5]) {
        if (err_out) {
            *err_out = 0xFC;
        }
        return false;
    }
    uint16_t rawT = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
    uint16_t rawRh = (uint16_t)(((uint16_t)b[3] << 8) | b[4]);
    float tc = -45.0f + 175.0f * ((float)rawT / 65535.0f);
    float rh = -6.0f + 125.0f * ((float)rawRh / 65535.0f);
    if (rh < 0.0f) {
        rh = 0.0f;
    }
    if (rh > 100.0f) {
        rh = 100.0f;
    }
    *temp_centi = (int16_t)(tc * 100.0f);
    *rh_centi = (uint16_t)(rh * 100.0f);
    return true;
}

static bool read_sht40_sample(const SensorData* sensor, int16_t* temp_centi, uint16_t* rh_centi, uint8_t* last_err) {
    uint8_t preferred_addr = sht40_addr_7bit(sensor);
    const uint8_t bus_id = sensor->bus_id;

    // 0xFF means this sensor was never assigned a bus. Refused, not resolved to bus 0: probing
    // it on another device's pins risks an address collision returning a plausible-but-wrong
    // temperature (DIVERGENCE_MATRIX 13).
    if (bus_id == 0xFF) {
        if (last_err) {
            *last_err = 0xFB;
        }
        return false;
    }

    const uint8_t candidates[] = {preferred_addr, 0x44u, 0x45u};
    for (uint8_t pass = 0; pass < 2; pass++) {
        if (pass > 0) {
            // The next transaction re-selects the bus; nothing needs to re-open it here.
            invalidateOpenDisplayWire();
            od_hal_delay_ms(2);
        }
        for (uint8_t i = 0; i < sizeof(candidates); i++) {
            uint8_t addr = candidates[i];
            bool duplicate = false;
            for (uint8_t j = 0; j < i; j++) {
                if (candidates[j] == addr) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            uint8_t err = 0;
            if (sht40_read_measurement(bus_id, addr, temp_centi, rh_centi, &err)) {
                return true;
            }
            if (last_err) {
                *last_err = err;
            }
        }
    }
    return false;
}

static void sht40_probe_bus_once(uint8_t bus_id) {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    if (!initOrRestoreWireForBus(bus_id)) {
        return;
    }
    const uint8_t addrs[] = {0x44u, 0x45u, 0x51u, 0x55u, 0x6Au};
    for (uint8_t i = 0; i < sizeof(addrs); i++) {
        // An address-only probe. Under Wire this was a zero-length write, which IDF rejects
        // outright -- nothing reached the bus and every address reported absent whether or not
        // hardware was there, which made a connected SHT40 undetectable. od_hal_i2c_probe() is
        // the address-only primitive; see od_hal_i2c.h.
        const int rc = od_hal_i2c_probe(bus_id, addrs[i]);
        od_log_debug("I2C bus %u probe 0x%02X rc=%d%s", bus_id, addrs[i], rc,
                     rc == OD_HAL_I2C_OK ? " (present)" : "");
    }
}

static void write_sht40_invalid(uint8_t start) {
    dynamicreturndata[start] = 0xFF;
    dynamicreturndata[start + 1] = 0xFF;
    dynamicreturndata[start + 2] = 0xFF;
    if ((uint16_t)start + 3u < 11u) {
        dynamicreturndata[start + 3] = 0;
    }
}

static int round_centi_to_deci(int16_t c) {
    if (c >= 0) {
        return (int)((c + 5) / 10);
    }
    return (int)((c - 5) / 10);
}

// MSD (3 bytes LE): v = rh_deci | (tu << 10); rh_deci 0..1000 = 0..100.0% RH (0.1% steps);
// tu = temp(0.1°C) + 400. Decode: t_deci = (v>>10 & 0x7FF) - 400; temp_centi = t_deci*10; rh_centi = (v&0x3FF)*10
static void write_sht40_msd(uint8_t start, int16_t temp_centi, uint16_t rh_centi) {
    int t_deci = round_centi_to_deci(temp_centi);
    if (t_deci < -400) {
        t_deci = -400;
    }
    if (t_deci > 1250) {
        t_deci = 1250;
    }
    uint32_t tu = (uint32_t)(t_deci + 400);
    uint32_t rh_d = ((uint32_t)rh_centi + 5u) / 10u;
    if (rh_d > 1000u) {
        rh_d = 1000u;
    }
    uint32_t v = (rh_d & 0x3FFu) | (tu << 10);
    dynamicreturndata[start] = (uint8_t)(v & 0xFFu);
    dynamicreturndata[start + 1] = (uint8_t)((v >> 8) & 0xFFu);
    dynamicreturndata[start + 2] = (uint8_t)((v >> 16) & 0xFFu);
    if ((uint16_t)start + 3u < 11u) {
        dynamicreturndata[start + 3] = 0;
    }
}

void initSht40Sensors(void) {
    for (uint8_t i = 0; i < globalConfig.sensor_count; i++) {
        const SensorData* s = &globalConfig.sensors[i];
        if (s->sensor_type != OD_SENSOR_TYPE_SHT40) {
            continue;
        }
        if (s->bus_id == 0xFF) {
            continue;   /* unassigned: not probed (DIVERGENCE_MATRIX 13) */
        }
        uint8_t addr = sht40_addr_7bit(s);
        uint8_t err = 0;
        if (!sht40_write_cmd(s->bus_id, addr, SHT40_CMD_SOFT_RESET, &err)) {
            sht40_write_cmd(s->bus_id, 0x44, SHT40_CMD_SOFT_RESET, nullptr);
            sht40_write_cmd(s->bus_id, 0x45, SHT40_CMD_SOFT_RESET, nullptr);
        }
        od_hal_delay_ms(2);
    }
    for (uint8_t i = 0; i < globalConfig.sensor_count; i++) {
        const SensorData* s = &globalConfig.sensors[i];
        if (s->sensor_type == OD_SENSOR_TYPE_SHT40 && s->bus_id != 0xFF) {
            sht40_probe_bus_once(s->bus_id);
            break;
        }
    }
}

static constexpr uint32_t kSht40MsdPollTtlMs = 30000u;

void pollSht40SensorsForMsd(void) {
    static bool logged_fail = false;
    static uint32_t lastPollMs = 0;
    static bool havePolled = false;
    if (havePolled && (uint32_t)(od_hal_uptime_ms() - lastPollMs) < kSht40MsdPollTtlMs) {
        return;
    }
    lastPollMs = od_hal_uptime_ms();
    havePolled = true;
    for (uint8_t i = 0; i < globalConfig.sensor_count; i++) {
        const SensorData* s = &globalConfig.sensors[i];
        if (s->sensor_type != OD_SENSOR_TYPE_SHT40) {
            continue;
        }
        uint8_t start = sht40_msd_start(s);
        if (start > 8) {
            continue;
        }
        int16_t tc = 0;
        uint16_t rhc = 0;
        uint8_t err = 0;
        if (!read_sht40_sample(s, &tc, &rhc, &err)) {
            if (!logged_fail) {
                // Resolved rather than indexed: bus_id is an instance number, so data_buses[bid]
                // can name a different record entirely, and a log line that reports the wrong
                // pins is worse than one that reports none.
                const struct DataBus* bus = od_config_data_bus(&globalConfig, s->bus_id);
                if (bus != nullptr) {
                    od_log_warn("SHT40: read failed err=%u (I2C bus %u SCL=GPIO%u SDA=GPIO%u)",
                                err, s->bus_id, bus->pin_1, bus->pin_2);
                } else {
                    od_log_warn("SHT40: read failed err=%u (I2C bus %u unresolved)", err, s->bus_id);
                }
                logged_fail = true;
            }
            write_sht40_invalid(start);
            continue;
        }
        logged_fail = false;
        write_sht40_msd(start, tc, rhc);
    }
}
