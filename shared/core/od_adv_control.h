/* od_adv_control.h -- loop-owned BLE advertising policy, portable across all four stacks.
 *
 * THE PROBLEM THIS SOLVES (correctness review finding F4). Advertising state on the ESP32
 * target lives in file statics that two FreeRTOS tasks read and write: the application loop
 * requests start/stop and updates the manufacturer data, while the NimBLE host task calls
 * od_ble_advertise() from sync, ADV_COMPLETE, and failed-connect events. Three consequences
 * follow: a flag can be read stale, the MSD array and its length can be observed as different
 * revisions, and a late stack event can re-arm advertising after the application committed to
 * stop for deep sleep.
 *
 * THE FIX IS AN OWNERSHIP CHANGE, NOT A LOCK. The application loop owns advertising policy;
 * stack callbacks publish FACTS ONLY and never call start, stop, or rebuild. This file holds
 * the policy. See docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md for the full design and the
 * alternatives it rejects.
 *
 * WHY NOT JUST MOVE POLICY ONTO THE NimBLE HOST TASK. It would fix the ESP32 symptom and
 * encode one stack's execution model into a design that must also serve Bluefruit callbacks,
 * a Zephyr work queue, and a Silabs superloop with NO KERNEL AT ALL. Atomics plus a mutex was
 * likewise rejected: it makes individual accesses defined while leaving two owners issuing
 * start/stop, and makes the kernel-free target pay for a synchronisation model it cannot use.
 *
 * WHAT THIS IS. A plain-C, run-to-completion, statically allocated state machine. No heap, no
 * blocking, no kernel primitives, no vendor headers. Every field is owned by the application
 * loop and accessed only from it, so nothing here needs atomics; the per-target event bridge
 * that carries facts ACROSS the context boundary is target code and may use target
 * primitives.
 *
 * CONCURRENCY CONTRACT, STATED PLAINLY: every function here is called from ONE context. It is
 * the caller's job to keep it that way. This file is not thread-safe and is not meant to be --
 * being single-owner is the entire design.
 */
#ifndef OD_ADV_CONTROL_H
#define OD_ADV_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

/* See od_hal_adv.h: shared/ is plain C, but the ESP32 target consumes these headers from
 * C++ translation units. Without the guard the C++ side gets C++ linkage and does not match
 * the C definitions in shared/core. */
#ifdef __cplusplus
extern "C" {
#endif

/* The canonical MsdAdvertisement body: the bytes after the company id. Fixed by the wire
 * contract, not by this controller -- see shared/protocol/opendisplay_structs.h. */
#define OD_ADV_MSD_LEN 16u

/* What one od_adv_process() pass did. Reported so a caller can log transitions and so tests
 * can assert the at-most-one-call rule directly. */
enum od_adv_process_result {
    OD_ADV_RECONCILED = 0, /* desired state reached; no HAL call needed */
    OD_ADV_ACTED,          /* exactly one HAL call was made and it advanced state */
    OD_ADV_BUSY,           /* a HAL call returned RETRY; state unchanged, try next pass */
    OD_ADV_FAULTED         /* a HAL call returned ERROR; fault latched */
};

struct od_adv_control {
    /* --- facts published by the stack, via the target's event bridge --- */
    bool    stack_ready;       /* host has synced and has an identity address */
    bool    active;            /* the controller BELIEVES advertising is running */
    uint8_t connection_count;  /* from the transport instance table, not counted callback edges */

    /* --- application intent, owned by the loop --- */
    bool    desired;           /* STANDING intent, not a prediction of stack state */

    /* --- payload --- */
    bool     payload_valid;    /* a payload has been supplied at least once */
    bool     payload_dirty;    /* the desired payload is not the one the stack holds */
    uint8_t  msd[OD_ADV_MSD_LEN];
    uint32_t desired_revision;
    uint32_t applied_revision;

    /* --- error latch --- */
    bool faulted;              /* a HAL ERROR was seen; cleared by stack_reset or recovery */
};

/* Zero the controller into a coherent "nothing wanted, nothing running" state. */
void od_adv_control_init(struct od_adv_control *s);

/* ---------------------------------------------------------------- application intent --- */

/* Publish the complete 16-byte MSD as ONE record.
 *
 * The array and its revision move together, which is what makes a torn advertisement
 * impossible -- the defect F4 names. Repeated calls before the next process() coalesce: only
 * the latest complete snapshot is ever programmed. An identical payload is a no-op, so a
 * caller polling sensor state every pass does not cause stop/program/start churn.
 */
void od_adv_set_payload(struct od_adv_control *s, const uint8_t msd[OD_ADV_MSD_LEN]);

/* Ask to be advertising. REMEMBERED if the stack is not ready yet -- that is the whole reason
 * intent is stored rather than acted on, and it is what makes a start racing host sync work
 * without the caller knowing about sync at all. */
void od_adv_request_start(struct od_adv_control *s);

/* Ask to stop, and MEAN IT. Clears intent, so no later stack event can revive it -- the
 * deep-sleep teardown path depends on that being true. */
void od_adv_request_stop(struct od_adv_control *s);

/* ------------------------------------------------------------------- facts from the stack --- */

/* The host synced and has an identity address. */
void od_adv_stack_ready(struct od_adv_control *s);

/* The stack reset or was torn down. Applied stack state is invalidated and the retained
 * payload is marked dirty for reprogramming; APPLICATION INTENT SURVIVES, so a later ready
 * resumes automatically. */
void od_adv_stack_reset(struct od_adv_control *s);

/* Authoritative connection count from the transport's instance table.
 *
 * Deliberately a level, not an edge: counting connect/disconnect callbacks drifts the moment
 * one is lost, and a drifted count silently suppresses advertising forever. */
void od_adv_set_connection_count(struct od_adv_control *s, uint8_t count);

/* Advertising ended on its own (ADV_COMPLETE, or a connection consumed it). Idempotent:
 * duplicate or coalesced notifications state the same fact. */
void od_adv_observe_ended(struct od_adv_control *s);

/* ------------------------------------------------------------------------ reconciliation --- */

/* Run one reconciliation step. Makes AT MOST ONE stack-mutating HAL call, which keeps every
 * transition observable, bounds the work per loop pass, and makes retry behaviour obvious.
 *
 * start_allowed gates a NEW start only; it never stops a running advertisement. That is what
 * preserves the existing display policy: advertising is not withdrawn merely because an EPD
 * refresh began, but if a connection stopped it during that refresh, the restart waits until
 * the refresh completes.
 */
enum od_adv_process_result od_adv_process(struct od_adv_control *s, bool start_allowed);

/* True when nothing is running and nothing is wanted -- the teardown barrier's exit test.
 * Pump events and process() until this holds, or until a bounded target timeout reports
 * failure, BEFORE releasing the host and controller. */
bool od_adv_is_quiescent(const struct od_adv_control *s);

#ifdef __cplusplus
}
#endif

#endif /* OD_ADV_CONTROL_H */
