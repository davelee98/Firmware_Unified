/* od_adv_app -- "something a host should see just happened; advertise sooner."
 *
 * ONE SEAM FOR EVERY EVENT SOURCE. Buttons, touch, NFC and connection-lifecycle edges all want
 * the same thing and used to say it in per-target words -- Nordic had a private BLE-module name
 * called from five places, BG22 a static one called from its button publish, ESP32 an empty
 * BleTransport method called from one, and shared code could not say it at all. All three names
 * are retired; git holds them.
 *
 * BG22's existence is the reason the ratchet matches a SHAPE rather than a list of names: the
 * first version of this seam missed it entirely, because the ESP32 comment it was written against
 * claimed the fast-advertising window was nRF-only. It is not -- BG22 boosts 20-30 ms against a
 * 1000 ms idle, as Nordic does.
 *
 * WHY IT IS NOT IN od_hal_adv.h. That header's contract is "called from the application loop,
 * never from a stack callback" -- od_adv_control is single-owner by design and its HAL ops
 * inherit that. This one is the opposite: it is reached from ISR and callback context (Nordic's
 * NFC callback, GPIO edges) and MUST stay safe there. Putting the two contracts in one header is
 * how someone ends up calling od_hal_adv_start() from an ISR.
 *
 * THE IMPLEMENTATION CONTRACT IS THEREFORE NARROW: publish a request and return. Do not call into
 * od_adv_control, do not touch the stack, do not take a lock, do not log. The loop notices on its
 * next pass.
 *
 * "PUBLISH", NOT "SET A FLAG" -- the distinction is not pedantry. Nordic's implementation writes
 * two plain objects that the loop thread reads and also clears, which is a data race the seam
 * inherited rather than introduced, and it can lose a renewal that arrives as the loop expires the
 * window. It is the existing implementation, not the reference for a new one; docs/FOLLOWUPS.md
 * § 20 carries the fix. A target adding an implementation should publish atomically.
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
