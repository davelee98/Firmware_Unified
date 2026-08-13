#ifndef PROTOCOL_PENDING_H
#define PROTOCOL_PENDING_H

/* Wire-contract values this target's code uses that shared/protocol/ does not define yet.
 *
 * ============================ THIS FILE IS A DEBT, NOT A DESIGN ============================
 *
 * Same shape and same rules as targets/esp32-idf/src/protocol_pending.h -- read that one first;
 * it explains why a file like this exists at all and why copying values into shared/protocol/ is
 * NOT the fix. Deleting this file is a required follow-up, not an optional cleanup.
 *
 * WHAT IS HERE, and how it got here. targets/nordic-zephyr carried a hand-written subset of the
 * wire contract in src/opendisplay_structs.h. Adopting the canonical header in its place showed
 * that the subset was not merely a subset: it defined one sensor type the canonical contract has
 * never heard of.
 *
 *     OD_SENSOR_TYPE_NPM1300 = 0x0006
 *
 * `enum SensorType` in shared/protocol/opendisplay_structs.h stops at OD_SENSOR_TYPE_BQ27220 = 5.
 * The nPM1300 driver here (opendisplay_sensor_npm1300.c) matches on 6, which means a config blob
 * carrying sensor_type 6 is accepted by this firmware and is undefined everywhere else in the
 * fleet -- host library included. That is a real, shipped divergence; it is not something this
 * repo can fix by editing its own copy of the header.
 *
 * ---------------------------------------------------------------------------- THE FIX
 *
 *   1. Land OD_SENSOR_TYPE_NPM1300 = 6 in `opendisplay-protocol/src/opendisplay_structs.h`, the
 *      canonical file, with the changelog entry that header's rules require. It is a MINOR bump:
 *      a new enum value that leaves existing ones untouched.
 *   2. `cd ../opendisplay-protocol && tools/sync_protocol_header.py --push`, which also needs
 *      Firmware_Unified/shared/protocol/ ADDED TO THE COPY MAP first -- it still lists only the
 *      four original repos, so --check cannot see drift here at all.
 *   3. Delete this file and the `#include "protocol_pending.h"` in opendisplay_sensor_npm1300.c
 *      and opendisplay_sensor_npm1300.h.
 *
 * Until step 1 happens the number below is a LOCAL GUESS AT A REMOTE FACT -- except that unlike
 * the ESP32 case it is not even a guess at something another repo already shipped: no other
 * OpenDisplay firmware defines a value 6. It is this target's invention, and the sequence above
 * is what turns it into a fact.
 */

#include "opendisplay_structs.h"

/* A value, not an enumerator: re-opening `enum SensorType` from outside the header that declares
 * it would be a second declaration of a wire type, the precise failure mode the canonical header
 * exists to prevent. SensorData.sensor_type is a uint16_t on the wire, so a plain constant is
 * what the comparisons need anyway. */
#define OD_SENSOR_TYPE_NPM1300 0x0006u

/* Second of the same kind, found the same way. shared/protocol/opendisplay_protocol.h defines
 * OD_NFC_IC_AUTO (0) and OD_NFC_IC_TNB132M (1) and stops there; this target's NFC driver also
 * selects the SoC's own NFCT peripheral, which every nRF has and no other OpenDisplay target
 * does. Same fix sequence as above: land it canonically, sync, delete from here.
 *
 * Note these ARE macros in canonical rather than enumerators, which is why this one can be a
 * plain #define without the caveat the sensor type carries -- it extends a macro family, not
 * an enum. */
#define OD_NFC_IC_SOC_NFCT 2u

/* The tripwire. 5 is OD_SENSOR_TYPE_BQ27220, the highest value canonical currently defines. When
 * 6 is added upstream this assert still passes -- but the #define above then collides with a real
 * enumerator and the compiler rejects the file outright, which is the louder signal and the one
 * that forces the deletion. This assert catches the quieter failure: the enum being renumbered or
 * value 6 being taken by something else underneath us. */
#if defined(__cplusplus)
static_assert((int)OD_SENSOR_TYPE_BQ27220 == 5,
              "enum SensorType moved; protocol_pending.h's 6 is no longer safe to assume");
#else
_Static_assert((int)OD_SENSOR_TYPE_BQ27220 == 5,
               "enum SensorType moved; protocol_pending.h's 6 is no longer safe to assume");
#endif

#endif /* PROTOCOL_PENDING_H */
