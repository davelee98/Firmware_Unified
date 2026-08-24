/* od_hal_i2c -- four I2C transactions, keyed by logical bus.
 *
 * The seam is the TRANSACTION, not the register. A register-shaped read/write cannot express
 * what these devices need: an SHT40 measurement is write-command, STOP, wait ~12 ms, then a bare
 * read -- no register to name, and the delay sits between two separate transactions. A BQ27220
 * read is the opposite, a selector write and a read joined by a repeated START into ONE
 * transaction. Collapsing both into "write then read" gets the framing wrong for each in a
 * different direction, and on the BQ that produces plausible garbage rather than an error.
 *
 * THE KEY IS DataBus.instance_number. `bus_id` is what SensorData.bus_id and
 * TouchController.bus_id carry (opendisplay_structs.h:802) -- not a position in `data_buses[]`,
 * which is only ever the same while records arrive in order with no gaps. Implementations
 * resolve it with od_config_data_bus(), which also refuses a duplicated instance rather than
 * picking one by packet order.
 *
 * THE BUS ARGUMENT IS TAKEN LITERALLY. `0xFF` is the contract's absent sentinel, but deciding
 * what an unassigned device should do belongs to the consumer that knows -- shared sensor and
 * touch policy REFUSE it. A transport that invents a default for its caller's sentinel cannot
 * know which rule applies, and that is exactly how the bus-0 substitution defect arose
 * (DIVERGENCE_MATRIX 13).
 *
 * NO init, deinit, set_clock OR STOP FLAG. Setup, caching, locking, reconfiguration and
 * invalidation are implementation details below these four calls. `write_read` versus `write`
 * then `read` expresses the two legal framings more precisely than a flag would.
 */

#ifndef OD_HAL_I2C_H
#define OD_HAL_I2C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Four outcomes, and no more.
 *
 * THERE IS DELIBERATELY NO STUCK-BUS CODE. Two of the three engines cannot produce one
 * honestly: the BG22 NFC bit-banger reads SDA only -- for ACK and for data -- and never checks
 * that SCL was released, so it cannot distinguish clock stretching from a held-low line, and a
 * held-low SDA reads as ACK and as data 0. Reporting a distinction the hardware layer cannot
 * make is worse than aggregating. A stretch timeout is EIO. */
#define OD_HAL_I2C_OK       0
#define OD_HAL_I2C_EIO     (-1)   /* bus fault, stretch timeout, or a peer that NACKed data */
#define OD_HAL_I2C_ENODEV  (-2)   /* nobody acknowledged the address */
#define OD_HAL_I2C_EINVAL  (-3)   /* bad argument, or the bus could not be resolved/brought up */

/* WHAT COUNTS AS A BAD ARGUMENT, stated because leaving it open lets two adapters disagree under
 * one header. All of these are OD_HAL_I2C_EINVAL and MUST NOT reach the bus:
 *
 *   - addr7 > 0x7F. This seam is 7-bit addressing; a caller passing a shifted address is a bug,
 *     not a device at some other address.
 *   - a NULL data pointer, on any operation that has one.
 *   - a zero length, on ANY of write, read or write_read's two phases. A zero-length write is
 *     not a portable presence check -- ESP-IDF rejects it outright, so nothing reaches the bus
 *     and every address reads as absent. od_hal_i2c_probe() is the operation for that, and it
 *     is why probe exists separately at all.
 *   - a bus_id that resolves to no record, to more than one, or to a record that is not a usable
 *     I2C bus. */

/* Address-only presence check: START, address, STOP, no data phase.
 *
 * Its own operation rather than "a write of length zero", because a zero-length write does not
 * reach the bus on every backend -- ESP-IDF rejects it outright, so a scan built on one reports
 * "absent" for every address whether or not hardware is there. */
int od_hal_i2c_probe(uint8_t bus_id, uint8_t addr7);

/* Write len bytes and complete with STOP. */
int od_hal_i2c_write(uint8_t bus_id, uint8_t addr7, const uint8_t *data, uint16_t len);

/* A complete START / address+R / read / STOP operation. */
int od_hal_i2c_read(uint8_t bus_id, uint8_t addr7, uint8_t *data, uint16_t len);

/* ONE transaction: write, REPEATED START, read. No STOP between the phases.
 *
 * Not the same as write() followed by read(). A device that requires the repeated START returns
 * whatever an unaddressed read yields when given STOP-then-START -- plausible values, not an
 * error -- so the two must stay separately expressible. */
int od_hal_i2c_write_read(uint8_t bus_id, uint8_t addr7,
                          const uint8_t *tx, uint16_t tx_len,
                          uint8_t *rx, uint16_t rx_len);

/* Every call resolves and selects `bus_id` before touching hardware, and A COMPLETED CALL
 * RETAINS NO BUS OWNERSHIP. An implementation may keep one bus live and switch it on demand --
 * that is a cache, not ownership, and the next call for a different bus must get that bus. */

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_I2C_H */
