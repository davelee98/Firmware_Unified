/* od_hal_i2c for the EFR32BG22: the TNB132M transport, extracted from opendisplay_ble.c.
 *
 * EXTRACTED NEARLY VERBATIM, INCLUDING THE EDGE PACING. The sl_udelay_wait() counts below are
 * what the deployed TNB132M sequence was tuned against, and no board in this fleet carries that
 * part, so there is nothing here that could prove a replacement works. The Silabs I2C peripheral
 * driver is deliberately NOT substituted for the same reason.
 *
 * WHAT THIS ENGINE CANNOT DO, stated because the shared header's status set depends on it: it
 * drives SCL push-pull and only ever READS SDA. It never checks that SCL was released, so it
 * cannot detect clock stretching or a stuck bus, and a held-low SDA reads as ACK and as data 0.
 * That is why od_hal_i2c has no stuck-bus code -- reporting a distinction this hardware layer
 * cannot make would be a lie on one of the three targets. Neither the host trace nor the hardware
 * gate may assert stuck-bus behaviour here.
 *
 * Everything ABOVE the transaction stays in the NFC adapter: powering the controller, the
 * critical section, the prime commands, and parking SCL and SDA afterwards. That session is the
 * adapter's, not the transport's.
 */

#include "od_hal_i2c.h"

#include "od_config.h"
#include "opendisplay_ble.h"

#include "em_gpio.h"
#include "sl_udelay.h"

#define GPIO_PIN_UNUSED 0xFFu

struct od_bg22_bus {
    GPIO_Port_TypeDef scl_port;
    uint8_t           scl_pin;
    GPIO_Port_TypeDef sda_port;
    uint8_t           sda_pin;
};

static bool pin_decode(uint8_t v, GPIO_Port_TypeDef *port_out, uint8_t *pin_out)
{
    if (v == GPIO_PIN_UNUSED) {
        return false;
    }
    unsigned pr = (unsigned)(v >> 4) & 0x0Fu;
    unsigned pn = (unsigned)(v & 0x0Fu);
    if (pr > (unsigned)GPIO_PORT_MAX || pn > 15u) {
        return false;
    }
    *port_out = (GPIO_Port_TypeDef)(gpioPortA + pr);
    *pin_out = (uint8_t)pn;
    return true;
}

/* bus_id is taken literally, including 0xFF: refusing the unassigned sentinel is the consumer's
 * rule. NfcConfig.bus_instance was already literal and stays so. */
static bool select_bus(uint8_t bus_id, struct od_bg22_bus *out)
{
    const struct od_config *cfg = opendisplay_get_global_config();
    const struct DataBus *bus;

    if (cfg == NULL) {
        return false;
    }
    bus = od_config_data_bus(cfg, bus_id);
    if (bus == NULL || bus->bus_type != 0x01u) {
        return false;
    }
    return pin_decode(bus->pin_1, &out->scl_port, &out->scl_pin) &&
           pin_decode(bus->pin_2, &out->sda_port, &out->sda_pin);
}

static void bb_start(const struct od_bg22_bus *b)
{
    GPIO_PinOutSet(b->sda_port, b->sda_pin);
    GPIO_PinOutSet(b->scl_port, b->scl_pin);
    sl_udelay_wait(2);
    GPIO_PinOutClear(b->sda_port, b->sda_pin);
    sl_udelay_wait(2);
    GPIO_PinOutClear(b->scl_port, b->scl_pin);
}

static void bb_stop(const struct od_bg22_bus *b)
{
    GPIO_PinOutClear(b->sda_port, b->sda_pin);
    sl_udelay_wait(2);
    GPIO_PinOutSet(b->scl_port, b->scl_pin);
    sl_udelay_wait(2);
    GPIO_PinOutSet(b->sda_port, b->sda_pin);
    sl_udelay_wait(2);
}

static bool bb_write_byte(const struct od_bg22_bus *b, uint8_t byte)
{
    for (uint8_t i = 0; i < 8u; i++) {
        if ((byte & 0x80u) != 0u) {
            GPIO_PinOutSet(b->sda_port, b->sda_pin);
        } else {
            GPIO_PinOutClear(b->sda_port, b->sda_pin);
        }
        byte <<= 1;
        sl_udelay_wait(1);
        GPIO_PinOutSet(b->scl_port, b->scl_pin);
        sl_udelay_wait(2);
        GPIO_PinOutClear(b->scl_port, b->scl_pin);
    }

    GPIO_PinModeSet(b->sda_port, b->sda_pin, gpioModeInputPull, 1);
    sl_udelay_wait(1);
    GPIO_PinOutSet(b->scl_port, b->scl_pin);
    sl_udelay_wait(2);
    bool ack = (GPIO_PinInGet(b->sda_port, b->sda_pin) == 0);
    GPIO_PinOutClear(b->scl_port, b->scl_pin);
    GPIO_PinModeSet(b->sda_port, b->sda_pin, gpioModeWiredAndFilter, 0);
    return ack;
}

static uint8_t bb_read_byte(const struct od_bg22_bus *b, bool ack)
{
    uint8_t value = 0;

    GPIO_PinModeSet(b->sda_port, b->sda_pin, gpioModeInputPull, 1);
    for (uint8_t i = 0; i < 8u; i++) {
        value <<= 1;
        GPIO_PinOutSet(b->scl_port, b->scl_pin);
        sl_udelay_wait(2);
        if (GPIO_PinInGet(b->sda_port, b->sda_pin) != 0) {
            value |= 1u;
        }
        GPIO_PinOutClear(b->scl_port, b->scl_pin);
        sl_udelay_wait(1);
    }

    GPIO_PinModeSet(b->sda_port, b->sda_pin, gpioModeWiredAndFilter, 0);
    if (ack) {
        GPIO_PinOutClear(b->sda_port, b->sda_pin);
    } else {
        GPIO_PinOutSet(b->sda_port, b->sda_pin);
    }
    sl_udelay_wait(1);
    GPIO_PinOutSet(b->scl_port, b->scl_pin);
    sl_udelay_wait(2);
    GPIO_PinOutClear(b->scl_port, b->scl_pin);
    GPIO_PinOutSet(b->sda_port, b->sda_pin);
    return value;
}

int od_hal_i2c_probe(uint8_t bus_id, uint8_t addr7)
{
    struct od_bg22_bus b;
    bool ack;

    if (addr7 > 0x7Fu) {
        return OD_HAL_I2C_EINVAL;
    }
    if (!select_bus(bus_id, &b)) {
        return OD_HAL_I2C_EINVAL;
    }
    bb_start(&b);
    ack = bb_write_byte(&b, (uint8_t)(addr7 << 1));
    bb_stop(&b);
    return ack ? OD_HAL_I2C_OK : OD_HAL_I2C_ENODEV;
}

int od_hal_i2c_write(uint8_t bus_id, uint8_t addr7, const uint8_t *data, uint16_t len)
{
    struct od_bg22_bus b;

    if (addr7 > 0x7Fu || data == NULL || len == 0u) {
        return OD_HAL_I2C_EINVAL;
    }
    if (!select_bus(bus_id, &b)) {
        return OD_HAL_I2C_EINVAL;
    }
    bb_start(&b);
    if (!bb_write_byte(&b, (uint8_t)(addr7 << 1))) {
        bb_stop(&b);
        return OD_HAL_I2C_ENODEV;
    }
    for (uint16_t i = 0; i < len; i++) {
        if (!bb_write_byte(&b, data[i])) {
            bb_stop(&b);
            return OD_HAL_I2C_EIO;
        }
    }
    bb_stop(&b);
    return OD_HAL_I2C_OK;
}

int od_hal_i2c_read(uint8_t bus_id, uint8_t addr7, uint8_t *data, uint16_t len)
{
    struct od_bg22_bus b;

    if (addr7 > 0x7Fu || data == NULL || len == 0u) {
        return OD_HAL_I2C_EINVAL;
    }
    if (!select_bus(bus_id, &b)) {
        return OD_HAL_I2C_EINVAL;
    }
    bb_start(&b);
    if (!bb_write_byte(&b, (uint8_t)((addr7 << 1) | 1u))) {
        bb_stop(&b);
        return OD_HAL_I2C_ENODEV;
    }
    for (uint16_t i = 0; i < len; i++) {
        /* NACK the final byte, which is how the master tells the peer to stop driving. */
        data[i] = bb_read_byte(&b, (uint16_t)(i + 1u) < len);
    }
    bb_stop(&b);
    return OD_HAL_I2C_OK;
}

int od_hal_i2c_write_read(uint8_t bus_id, uint8_t addr7,
                          const uint8_t *tx, uint16_t tx_len,
                          uint8_t *rx, uint16_t rx_len)
{
    struct od_bg22_bus b;

    if (addr7 > 0x7Fu || tx == NULL || tx_len == 0u || rx == NULL || rx_len == 0u) {
        return OD_HAL_I2C_EINVAL;
    }
    if (!select_bus(bus_id, &b)) {
        return OD_HAL_I2C_EINVAL;
    }
    bb_start(&b);
    if (!bb_write_byte(&b, (uint8_t)(addr7 << 1))) {
        bb_stop(&b);
        return OD_HAL_I2C_ENODEV;
    }
    for (uint16_t i = 0; i < tx_len; i++) {
        if (!bb_write_byte(&b, tx[i])) {
            bb_stop(&b);
            return OD_HAL_I2C_EIO;
        }
    }
    /* REPEATED START: no STOP between the phases. This is the TNB132M block read. */
    bb_start(&b);
    if (!bb_write_byte(&b, (uint8_t)((addr7 << 1) | 1u))) {
        bb_stop(&b);
        return OD_HAL_I2C_ENODEV;
    }
    for (uint16_t i = 0; i < rx_len; i++) {
        rx[i] = bb_read_byte(&b, (uint16_t)(i + 1u) < rx_len);
    }
    bb_stop(&b);
    return OD_HAL_I2C_OK;
}
