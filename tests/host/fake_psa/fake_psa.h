/* fake_psa.h -- the scriptable state behind tests/host/fake_psa/psa/crypto.h.
 *
 * Injection is per-ENTRY-POINT and per-CALL, not one global flag, because the defects under test
 * are about what happens when ONE step of a multi-step sequence fails while the rest succeed --
 * a destroy that fails inside a key replacement, an init that fails before a random draw. A single
 * flag makes every call fail and never reaches those branches.
 */

#ifndef OD_TEST_FAKE_PSA_H
#define OD_TEST_FAKE_PSA_H

#include "psa/crypto.h"

#include <stdbool.h>

/* Call before every case. Clears the key table, the counters and every injection. */
void fake_psa_reset(void);

/* --- what happened ------------------------------------------------------------------------- */

extern unsigned fake_psa_init_calls;
extern unsigned fake_psa_import_calls;
extern unsigned fake_psa_destroy_calls;
extern unsigned fake_psa_random_calls;

/* The id passed to the most recent psa_destroy_key(), and every id ever destroyed. A test asserts
 * on these to catch a destroy aimed at a REUSED or FRESH id -- the failure mode a retry-the-old-id
 * repair introduces, where PSA has already reissued the number to somebody else's key. */
extern psa_key_id_t fake_psa_last_destroyed;
#define FAKE_PSA_DESTROY_LOG_MAX 16u
extern psa_key_id_t fake_psa_destroy_log[FAKE_PSA_DESTROY_LOG_MAX];
extern unsigned     fake_psa_destroy_log_n;

/* True while `id` is a key this fake believes exists. */
bool fake_psa_key_live(psa_key_id_t id);
/* How many imported keys are still live -- a leaked slot is a live key nobody can name. */
unsigned fake_psa_live_keys(void);
/* The attributes the most recent successful import was given. */
extern psa_key_attributes_t fake_psa_last_attr;

/* --- what to break ------------------------------------------------------------------------- */

/* Status returned by the Nth call to each entry point, counting from 1. Set the index to 0 to
 * inject nothing (the reset default). */
extern unsigned     fake_psa_fail_init_on;
extern psa_status_t fake_psa_fail_init_status;
extern unsigned     fake_psa_fail_import_on;
extern psa_status_t fake_psa_fail_import_status;
extern unsigned     fake_psa_fail_destroy_on;
extern psa_status_t fake_psa_fail_destroy_status;
extern unsigned     fake_psa_fail_random_on;
extern psa_status_t fake_psa_fail_random_status;

/* Make psa_generate_random() report success but write fewer bytes than asked. The one failure a
 * status check alone cannot see. */
extern bool fake_psa_random_short;

#endif /* OD_TEST_FAKE_PSA_H */
