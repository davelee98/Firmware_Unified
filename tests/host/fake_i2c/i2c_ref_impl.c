/* A reference implementation of shared/hal/od_hal_i2c.h over the fake wire.
 *
 * Its purpose is to make the CONTRACT executable before any target implements it: staging steps
 * 4, 5 and 9 bind their production adapters to the same body in tests/host/i2c_contract.inc, and
 * a contract nothing has ever satisfied is a contract nobody has checked.
 *
 * It is not a target and must never become one -- it resolves the bus and drives framing, and
 * that is all.
 */

#include "od_hal_i2c.h"

#include "i2c_wire.h"
#include "od_config.h"

/* The config the contract test installs. A real adapter reads its target's global. */
const struct od_config *i2c_ref_cfg;

/* The header's argument rules, in one place so every operation applies the same ones. */
static bool args_ok(uint8_t addr7, const void *p, uint16_t len)
{
    return addr7 <= 0x7Fu && p != NULL && len > 0u;
}

static int select_bus(uint8_t bus_id, uint8_t *scl, uint8_t *sda)
{
    const struct DataBus *bus = od_config_data_bus(i2c_ref_cfg, bus_id);

    /* bus_id is taken literally: 0xFF is not special-cased here. Refusing the sentinel is the
     * consumer's rule, not the transport's. */
    if (bus == NULL || bus->bus_type != 0x01u || bus->pin_1 == 0xFFu || bus->pin_2 == 0xFFu) {
        return OD_HAL_I2C_EINVAL;
    }
    *scl = bus->pin_1;
    *sda = bus->pin_2;
    return OD_HAL_I2C_OK;
}

int od_hal_i2c_probe(uint8_t bus_id, uint8_t addr7)
{
    uint8_t scl, sda;
    int rc = select_bus(bus_id, &scl, &sda);

    if (addr7 > 0x7Fu) { return OD_HAL_I2C_EINVAL; }
    if (rc != OD_HAL_I2C_OK) { return rc; }
    if (!i2c_wire_start(scl, sda)) { return OD_HAL_I2C_EINVAL; }
    rc = i2c_wire_addr(addr7, false) ? OD_HAL_I2C_OK : OD_HAL_I2C_ENODEV;
    i2c_wire_stop();
    return rc;
}

int od_hal_i2c_write(uint8_t bus_id, uint8_t addr7, const uint8_t *data, uint16_t len)
{
    uint8_t scl, sda;
    int rc = select_bus(bus_id, &scl, &sda);

    if (!args_ok(addr7, data, len)) { return OD_HAL_I2C_EINVAL; }
    if (rc != OD_HAL_I2C_OK) { return rc; }
    if (!i2c_wire_start(scl, sda)) { return OD_HAL_I2C_EINVAL; }
    if (!i2c_wire_addr(addr7, false)) { i2c_wire_stop(); return OD_HAL_I2C_ENODEV; }
    for (uint16_t i = 0; i < len; ++i) {
        if (!i2c_wire_tx(data[i])) { i2c_wire_stop(); return OD_HAL_I2C_EIO; }
    }
    i2c_wire_stop();                 /* write COMPLETES with STOP */
    return OD_HAL_I2C_OK;
}

int od_hal_i2c_read(uint8_t bus_id, uint8_t addr7, uint8_t *data, uint16_t len)
{
    uint8_t scl, sda;
    int rc = select_bus(bus_id, &scl, &sda);

    if (!args_ok(addr7, data, len)) { return OD_HAL_I2C_EINVAL; }
    if (rc != OD_HAL_I2C_OK) { return rc; }
    if (!i2c_wire_start(scl, sda)) { return OD_HAL_I2C_EINVAL; }
    if (!i2c_wire_addr(addr7, true)) { i2c_wire_stop(); return OD_HAL_I2C_ENODEV; }
    for (uint16_t i = 0; i < len; ++i) {
        if (!i2c_wire_rx(&data[i])) { i2c_wire_stop(); return OD_HAL_I2C_EIO; }
    }
    i2c_wire_stop();
    return OD_HAL_I2C_OK;
}

int od_hal_i2c_write_read(uint8_t bus_id, uint8_t addr7,
                          const uint8_t *tx, uint16_t tx_len,
                          uint8_t *rx, uint16_t rx_len)
{
    uint8_t scl, sda;
    int rc = select_bus(bus_id, &scl, &sda);

    if (!args_ok(addr7, tx, tx_len) || !args_ok(addr7, rx, rx_len)) { return OD_HAL_I2C_EINVAL; }
    if (rc != OD_HAL_I2C_OK) { return rc; }
    if (!i2c_wire_start(scl, sda)) { return OD_HAL_I2C_EINVAL; }
    if (!i2c_wire_addr(addr7, false)) { i2c_wire_stop(); return OD_HAL_I2C_ENODEV; }
    for (uint16_t i = 0; i < tx_len; ++i) {
        if (!i2c_wire_tx(tx[i])) { i2c_wire_stop(); return OD_HAL_I2C_EIO; }
    }
    /* NO STOP HERE. The repeated START is the whole difference from write() then read(). */
    if (!i2c_wire_start(scl, sda)) { i2c_wire_stop(); return OD_HAL_I2C_EINVAL; }
    if (!i2c_wire_addr(addr7, true)) { i2c_wire_stop(); return OD_HAL_I2C_ENODEV; }
    for (uint16_t i = 0; i < rx_len; ++i) {
        if (!i2c_wire_rx(&rx[i])) { i2c_wire_stop(); return OD_HAL_I2C_EIO; }
    }
    i2c_wire_stop();
    return OD_HAL_I2C_OK;
}
