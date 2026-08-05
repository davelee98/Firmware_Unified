/* od_adv_control.c -- see od_adv_control.h for the design and why it is not a lock.
 *
 * THE FIRST SOURCE IN shared/. It is here rather than od_config.c by a deliberate, recorded
 * decision (docs/NEXT_STEPS_2026-08-05.md D1): the advertising controller has no wire surface
 * and no vendor coupling, so it establishes the shared-source build and test pattern without
 * moving protocol state. od_config.c remains the first PROTOCOL subsystem promoted.
 *
 * Everything below is loop-context only, plain C99, no allocation and no blocking.
 */
#include "od_adv_control.h"
#include "od_hal_adv.h"

#include <string.h>

void od_adv_control_init(struct od_adv_control *s)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof *s);
}

/* --------------------------------------------------------------------- application intent --- */

void od_adv_set_payload(struct od_adv_control *s, const uint8_t msd[OD_ADV_MSD_LEN])
{
    if (!s || !msd) {
        return;
    }
    /* An unchanged payload is a no-op. A caller that rebuilds the MSD from sensor state every
     * pass would otherwise force stop/program/start on every pass -- pure radio churn on a
     * battery device that shares its antenna with WiFi. */
    if (s->payload_valid && memcmp(s->msd, msd, OD_ADV_MSD_LEN) == 0) {
        return;
    }
    memcpy(s->msd, msd, OD_ADV_MSD_LEN);
    s->payload_valid = true;
    s->payload_dirty = true;
    /* Bumped once per genuine change. Compared only for equality, so wrap is a non-event. */
    s->desired_revision++;
}

void od_adv_request_start(struct od_adv_control *s)
{
    if (!s) {
        return;
    }
    s->desired = true;
}

void od_adv_request_stop(struct od_adv_control *s)
{
    if (!s) {
        return;
    }
    /* Intent only. The stack call happens in process(), on the loop, which is what stops a
     * host-task event from racing the teardown. */
    s->desired = false;
}

/* ------------------------------------------------------------------- facts from the stack --- */

void od_adv_stack_ready(struct od_adv_control *s)
{
    if (!s) {
        return;
    }
    s->stack_ready = true;
}

void od_adv_stack_reset(struct od_adv_control *s)
{
    if (!s) {
        return;
    }
    /* Applied stack state is gone; intent is NOT. A reset is something that happened to the
     * radio, not a decision by the application, so it must not silently cancel a start the
     * application still wants. */
    s->stack_ready = false;
    s->active      = false;
    s->faulted     = false;   /* a new stack generation is the natural recovery point */
    if (s->payload_valid) {
        s->payload_dirty = true;
    }
    s->applied_revision = s->desired_revision - 1u;   /* force a reprogram; equality-only */
}

void od_adv_set_connection_count(struct od_adv_control *s, uint8_t count)
{
    if (!s) {
        return;
    }
    s->connection_count = count;
}

void od_adv_observe_ended(struct od_adv_control *s)
{
    if (!s) {
        return;
    }
    /* Idempotent by construction: repeated or coalesced notifications assert the same fact.
     * A stale one arriving after a fresh start is corrected on the next pass, because a start
     * on something already advertising returns ALREADY, which is success. */
    s->active = false;
}

/* ------------------------------------------------------------------------ reconciliation --- */

/* Fold a HAL result into "did this advance, hold, or fail". Keeping the mapping in one place
 * is what makes the idempotent-success rule impossible to apply inconsistently. */
static enum od_adv_process_result classify(enum od_hal_adv_result r)
{
    switch (r) {
    case OD_HAL_ADV_OK:
    case OD_HAL_ADV_ALREADY:
    case OD_HAL_ADV_NOT_ACTIVE:
        return OD_ADV_ACTED;
    case OD_HAL_ADV_RETRY:
        return OD_ADV_BUSY;
    case OD_HAL_ADV_ERROR:
    default:
        return OD_ADV_FAULTED;
    }
}

enum od_adv_process_result od_adv_process(struct od_adv_control *s, bool start_allowed)
{
    if (!s) {
        return OD_ADV_RECONCILED;
    }
    /* 1. Nothing is possible before the host has an identity address. */
    if (!s->stack_ready) {
        return OD_ADV_RECONCILED;
    }

    /* 2. Stop first, and for three distinct reasons: intent withdrawn, a connection exists, or
     *    a newer payload must be installed and programming requires an inactive advertiser.
     *    Stopping is never gated on start_allowed -- that flag gates NEW starts only. */
    if (s->active && (!s->desired || s->connection_count > 0u || s->payload_dirty)) {
        const enum od_adv_process_result r = classify(od_hal_adv_stop());
        if (r == OD_ADV_ACTED) {
            s->active = false;
        } else if (r == OD_ADV_FAULTED) {
            s->faulted = true;
        }
        return r;
    }

    /* A latched fault suppresses further stack-mutating work until a reset or recovery clears
     * it. Checked AFTER the stop branch so a teardown can still stop a faulted advertiser. */
    if (s->faulted) {
        return OD_ADV_FAULTED;
    }

    /* 3. Install the latest snapshot while inactive. */
    if (!s->active && s->payload_dirty && s->payload_valid) {
        const enum od_adv_process_result r = classify(od_hal_adv_program(s->msd));
        if (r == OD_ADV_ACTED) {
            s->applied_revision = s->desired_revision;
            s->payload_dirty    = false;
        } else if (r == OD_ADV_FAULTED) {
            s->faulted = true;
        }
        return r;
    }

    /* 4. Start, once every gate agrees. payload_valid && !payload_dirty is the "the stack
     *    holds the payload we want" test -- never advertise bytes nobody supplied. */
    if (!s->active && s->desired && s->connection_count == 0u &&
        s->payload_valid && !s->payload_dirty && start_allowed) {
        const enum od_adv_process_result r = classify(od_hal_adv_start());
        if (r == OD_ADV_ACTED) {
            s->active = true;
        } else if (r == OD_ADV_FAULTED) {
            s->faulted = true;
        }
        return r;
    }

    /* 5. Desired state reached, or blocked on something only a new fact can change. */
    return OD_ADV_RECONCILED;
}

bool od_adv_is_quiescent(const struct od_adv_control *s)
{
    if (!s) {
        return true;
    }
    return !s->active && !s->desired;
}
