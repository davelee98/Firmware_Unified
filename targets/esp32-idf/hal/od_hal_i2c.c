/* od_hal_i2c for ESP-IDF. See od_hal_i2c.h.
 *
 * The bus mechanics are compat/Wire.h's, moved rather than rewritten -- including the two
 * defects that file had already found and fixed on hardware (the zero-length-write probe, and
 * honouring the no-STOP flag for repeated-START reads). compat/Wire.h is now an adapter over
 * this, so there is exactly one owner of the bus handle: IDF permits only one
 * i2c_new_master_bus() per port, and a second owner does not fail cleanly, it fails at the
 * point where two drivers disagree about who configured the pins.
 */

#include "od_hal_i2c.h"

#include "driver/i2c_master.h"

/* Port 0. Every board's config uses one I2C bus at a time -- display_service.c switches pins
 * by tearing the bus down and bringing it back up, which is why init() is idempotent on the
 * same pins and refuses a silent change. */
#define OD_I2C_PORT      0
#define OD_I2C_TIMEOUT_MS 100

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t  s_dev_addr = 0xFF;
static uint32_t s_freq     = 100000;
static int      s_sda      = -1;
static int      s_scl      = -1;

static void release_device(void)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        s_dev_addr = 0xFF;
    }
}

/* One cached device handle. These drivers talk to one peer at a time and switch rarely, so a
 * single-entry cache removes an attach/detach per transaction without needing a table. */
static bool attach(uint8_t addr)
{
    if (!s_bus) {
        return false;
    }
    if (s_dev && addr == s_dev_addr) {
        return true;
    }
    release_device();

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = s_freq,
    };
    if (i2c_master_bus_add_device(s_bus, &dcfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return false;
    }
    s_dev_addr = addr;
    return true;
}

/* ESP_ERR_NOT_FOUND is the only error that genuinely means "nobody answered". Everything else
 * -- a bad argument, a bus fault, a timeout on a stuck bus -- is a different failure and must
 * not be reported as an absent device, which is a distinction a blanket return would destroy. */
static int map_err(esp_err_t err)
{
    switch (err) {
        case ESP_OK:              return OD_HAL_I2C_OK;
        case ESP_ERR_NOT_FOUND:   return OD_HAL_I2C_ENODEV;
        case ESP_ERR_INVALID_ARG: return OD_HAL_I2C_EINVAL;
        default:                  return OD_HAL_I2C_ERR;
    }
}

bool od_hal_i2c_init(uint8_t sda, uint8_t scl, uint32_t hz)
{
    if (s_bus) {
        /* Same pins: already up, nothing to do. Different pins: the caller must deinit first
         * -- see the header for why this does not silently reconfigure. */
        return ((int)sda == s_sda && (int)scl == s_scl);
    }
    s_freq = hz ? hz : 100000;

    i2c_master_bus_config_t cfg = {
        .i2c_port                     = OD_I2C_PORT,
        .sda_io_num                   = (gpio_num_t)sda,
        .scl_io_num                   = (gpio_num_t)scl,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&cfg, &s_bus) != ESP_OK) {
        s_bus = NULL;
        return false;
    }
    s_sda = (int)sda;
    s_scl = (int)scl;
    return true;
}

void od_hal_i2c_deinit(void)
{
    release_device();
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    s_sda = -1;
    s_scl = -1;
}

bool od_hal_i2c_is_up(void)
{
    return s_bus != NULL;
}

void od_hal_i2c_set_clock(uint32_t hz)
{
    if (hz) {
        s_freq = hz;
    }
}

int od_hal_i2c_probe(uint8_t addr)
{
    if (!s_bus) {
        return OD_HAL_I2C_EINVAL;
    }
    return map_err(i2c_master_probe(s_bus, addr, OD_I2C_TIMEOUT_MS));
}

int od_hal_i2c_write(uint8_t addr, const uint8_t *buf, uint16_t len)
{
    if (!s_bus || buf == NULL || len == 0) {
        return OD_HAL_I2C_EINVAL;
    }
    if (!attach(addr)) {
        return OD_HAL_I2C_ERR;
    }
    return map_err(i2c_master_transmit(s_dev, buf, len, OD_I2C_TIMEOUT_MS));
}

int od_hal_i2c_read(uint8_t addr, uint8_t *buf, uint16_t len)
{
    if (!s_bus || buf == NULL || len == 0) {
        return OD_HAL_I2C_EINVAL;
    }
    if (!attach(addr)) {
        return OD_HAL_I2C_ERR;
    }
    return map_err(i2c_master_receive(s_dev, buf, len, OD_I2C_TIMEOUT_MS));
}

int od_hal_i2c_write_read(uint8_t addr, const uint8_t *tx, uint16_t tx_len,
                          uint8_t *rx, uint16_t rx_len)
{
    if (!s_bus || tx == NULL || tx_len == 0 || rx == NULL || rx_len == 0) {
        return OD_HAL_I2C_EINVAL;
    }
    if (!attach(addr)) {
        return OD_HAL_I2C_ERR;
    }
    return map_err(i2c_master_transmit_receive(s_dev, tx, tx_len, rx, rx_len,
                                               OD_I2C_TIMEOUT_MS));
}
