/* od_adv_app -- "something a host should see just happened; advertise sooner."
 *
 * ONE SEAM FOR EVERY EVENT SOURCE. Buttons, touch, NFC and connection-lifecycle edges all want
 * the same thing and used to say it in per-target words -- Nordic had a private BLE-module name
 * called from four places, ESP32 an empty BleTransport method called from one, and shared code
 * could not say it at all. Both names are retired; git holds them.
 *
 * WHY IT IS NOT IN od_hal_adv.h. That header's contract is "called from the application loop,
 * never from a stack callback" -- od_adv_control is single-owner by design and its HAL ops
 * inherit that. This one is the opposite: it is reached from ISR and callback context (Nordic's
 * NFC callback, GPIO edges) and MUST stay safe there. Putting the two contracts in one header is
 * how someone ends up calling od_hal_adv_start() from an ISR.
 *
 * THE IMPLEMENTATION CONTRACT IS THEREFORE NARROW: set a plain flag or a timestamp and return.
 * Do not call into od_adv_control, do not touch the stack, do not take a lock, do not log. The
 * loop notices on its next pass. Nordic's implementation is the reference for how little this is
 * allowed to do.
 *
 * A NO-OP IS A CORRECT IMPLEMENTATION, not a stub. A target already advertising fast enough for
 * an event to be seen has nothing to boost -- see the ESP32 implementation, which explains why
 * its stack default makes the boost unnecessary rather than merely unimplemented. Every target
 * that links a caller must define this, so the choice is stated per target instead of a shared
 * caller silently doing nothing.
 */
#ifndef OD_ADV_APP_H
#define OD_ADV_APP_H

/* See od_hal_adv.h: shared/ is plain C, but ESP32 implements its BLE seams from C++ translation
 * units. Without the guard those definitions get C++ linkage and never match the C caller. */
#ifdef __cplusplus
extern "C" {
#endif

/* Request a temporary fast-advertising window, if this target has one.
 *
 * ISR-SAFE. Idempotent and cheap: callers invoke it on every state change worth publishing, and
 * a repeat inside an open window simply restarts it.
 */
void od_adv_app_boost(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_ADV_APP_H */
