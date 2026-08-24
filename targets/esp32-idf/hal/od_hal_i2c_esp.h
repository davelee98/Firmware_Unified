/* ESP32-private I2C surface: everything shared/hal/od_hal_i2c.h deliberately does not have.
 *
 * The shared seam is four transactions and nothing else -- no init, no deinit, no clock. This
 * target needs those anyway, because IDF permits exactly one i2c_new_master_bus() per port, so
 * whoever opens the bus owns it and the panel path tears it down around a refresh. That is a bus
 * MANAGER, and it is target policy, not portable contract.
 */

#ifndef OD_HAL_I2C_ESP_H
#define OD_HAL_I2C_ESP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the logical bus `bus_id` up, switching pins if a different one is live.
 *
 * IMPLEMENTED BY THE TARGET (display_service.cpp), not by the HAL, because resolving a bus_id
 * means reading globalConfig and that lives on the C++ side. Every shared operation calls this
 * before touching hardware, which is what makes "a completed call retains no bus ownership"
 * true: the next call for a different bus gets that bus. */
bool od_hal_i2c_esp_select(uint8_t bus_id);

/* Open the bus on explicit pins, for the board-default path that runs when no DataBus record
 * exists at all. hz == 0 means 100 kHz. */
bool od_hal_i2c_esp_begin(int scl, int sda, uint32_t hz);

/* Tear the bus down, releasing the cached device handle. Safe when already down. The panel
 * refresh path uses this; the next transaction re-selects. */
void od_hal_i2c_esp_deinit(void);

bool od_hal_i2c_esp_is_up(void);

/* Probe on WHATEVER bus is currently up, without naming one.
 *
 * For the diagnostic scan, which runs after initOrRestoreWireForOpenDisplay() and may therefore
 * be on the board-default pins -- a configuration with no DataBus record at all, so there is no
 * instance_number to pass. Deliberately NOT in the shared seam: "operate on whichever bus
 * happens to be live" is the ambiguity od_hal_i2c exists to remove, and it is tolerable only in
 * a diagnostic that prints addresses. */
int od_hal_i2c_esp_probe_current(uint8_t addr);
int od_hal_i2c_esp_write_current(uint8_t addr, const uint8_t *buf, uint16_t len);
int od_hal_i2c_esp_read_current(uint8_t addr, uint8_t *buf, uint16_t len);
int od_hal_i2c_esp_write_read_current(uint8_t addr, const uint8_t *tx, uint16_t tx_len,
                                      uint8_t *rx, uint16_t rx_len);

/* IDF fixes the clock per DEVICE, not per bus, so this only records the value; it takes effect
 * the next time a device handle is attached. */
void od_hal_i2c_esp_set_clock(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_I2C_ESP_H */
