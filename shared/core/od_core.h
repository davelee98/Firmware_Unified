/* od_core.h -- the shared half of a teardown.
 *
 * WHAT IT DOES NOT DO IS THE POINT. Three shared objects outlive a single dispatch -- the config
 * read producer, the response queue and the session -- and every target had its own hand-written
 * list of them, which is exactly the kind of list that loses an entry when a fourth is added. This
 * is that list, once.
 *
 * IT DOES NOT TOUCH RX, deliberately. Nordic's producer is the BT thread and can push into the
 * ring concurrently, and ESP32's connection policy has stricter ordering around its own; a reset
 * here would race the first and reorder the second. RX teardown stays with the target that knows
 * its producer.
 *
 * TARGET TRANSFER, DISPLAY, CONFIG AND NFC STATE IS ALSO THE TARGET'S. Those are not shared
 * objects and this cannot see them -- the call sites run their own resets alongside this one.
 *
 * CONSUMER CONTEXT ONLY. Never from a stack callback: it cancels a producer and drops queued
 * frames, both of which the consumer may be inside.
 */

#ifndef OD_CORE_H
#define OD_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Cancel the config-read producer, drop every queued response, and clear the app session.
 *
 * The session goes through od_session_clear(), never a memset: clear also releases the HAL key
 * slot and preserves the slot index, and a memset would strand a prepared key in a finite pool. */
void od_core_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_CORE_H */
