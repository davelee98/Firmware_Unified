/* corpus_runner.h -- the shape of the generated vector table, and the seam a proof profile fills.
 *
 * ONE RUNNER, THREE EXECUTABLES. `od_cmd_app_*` is static link-time composition (C11), so multiple sets of
 * hook definitions cannot coexist in one binary -- and the fix for that is emphatically NOT a
 * runtime registry, which C11's plan rejected for the same reason it rejected a vtable. Each
 * executable links the same runner and generated table against ONE profile, and resolves the seam
 * once.
 *
 * WHAT A PROFILE MAY SEE. Knobs, never answers. The generated table is included by exactly one
 * translation unit -- the runner -- and a profile gets this header instead. A profile that could
 * read `expect.reply` would let the corpus become its own oracle, which is the failure mode this
 * whole exercise exists to avoid: every vector green, nothing proven.
 */

#ifndef OD_TEST_CORPUS_RUNNER_H
#define OD_TEST_CORPUS_RUNNER_H

#include "od_hal_radio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Capability predicates, as a bitmask so `requires`/`forbids` are one test each. The generator
 * emits these names from the corpus's closed capability set. */
#define OD_VEC_CAP_PARTIAL      0x01u
#define OD_VEC_CAP_BUZZER       0x02u
#define OD_VEC_CAP_POWER_LATCH  0x04u
#define OD_VEC_CAP_PIPE         0x08u
#define OD_VEC_CAP_NFC          0x10u
#define OD_VEC_CAP_CONFIG_4K    0x20u
#define OD_VEC_CAP_RXQ          0x40u

/* What a passing vector is entitled to CLAIM. Reported separately so a fixture-produced legacy
 * shape can never be totalled as current target coverage. */
typedef enum {
    OD_PROOF_SHARED,             /* the reply came from shared dispatch; no target hook ran */
    OD_PROOF_TARGET_PRODUCTION,  /* the reply came from a target's production command code */
    OD_PROOF_HISTORICAL_FIXTURE  /* a shape current production does not emit */
} od_proof_t;

typedef struct { const uint8_t *bytes; size_t len; } od_vec_reply_t;

typedef struct {
    const uint8_t         *frame;
    size_t                 frame_len;
    const od_vec_reply_t  *replies;
    unsigned               reply_n;
    unsigned               silent;    /* expect.reply was null: the device emits NOTHING */
    od_origin_t            origin;
    unsigned               d2h;       /* an observation, not an input -- never dispatched */
} od_vec_step_t;

typedef struct {
    const char          *id;
    const od_vec_step_t *steps;
    unsigned             step_n;
    od_proof_t           proof;
    unsigned             requires_caps;
    unsigned             forbids_caps;
    unsigned             sec_enabled;
    unsigned             session_live;
    unsigned             xfer_active;
    unsigned             storage_ok;
    unsigned             fw_patch_byte;
    const char          *fw_sha;      /* the build SHA this device reports; "" = unspecified */
} od_vec_t;

/* ------------------------------------------------------------------------ the profile seam --- */

/* Which capabilities this profile presents. A vector whose `requires` are not all present, or any
 * of whose `forbids` are, is excluded and counted as such -- never silently skipped. */
unsigned od_corpus_profile_caps(void);

/* A human name for the report line, e.g. "portable" or "nordic-production". */
const char *od_corpus_profile_name(void);

/* True when every od_cmd_app_* hook in this executable is production target code.
 *
 * IT DECIDES WHAT A HISTORICAL VECTOR MEANS HERE. A `historical-fixture` expectation is a shape
 * current production does not emit -- a pre-patch firmware-version reply, a compiled-out
 * subsystem's NACK. A production profile CANNOT satisfy one without lying about what the firmware
 * does, so it excludes them and says so; only the portable profile, whose fixtures can be placed
 * in those states legitimately, executes them. Forcing one through here would mean editing either
 * the vector or the firmware to agree, and both are the failure this classification exists to
 * prevent. */
bool od_corpus_profile_is_production(void);

/* Put the profile's command layer in the state this vector declares, and clear everything left
 * over from the previous one. Called before every vector, never between steps. */
void od_corpus_profile_reset(const od_vec_t *vec);

/* SEMANTIC KNOBS the profile reads while serving a vector. They are inputs a device would really
 * have -- a version, a driver's return code -- not wire bytes. */
struct od_corpus_knobs {
    unsigned xfer_active;      /* a direct-write transfer is already open */
    unsigned storage_ok;       /* config storage init/save succeeds */
    unsigned fw_patch_byte;    /* this firmware appends the trailing 0x43 patch byte */
    const char *fw_sha;        /* the build SHA it reports -- a device property, not a wire byte */
    unsigned caps;             /* od_corpus_profile_caps(), for the hooks that answer on absence */
};
extern struct od_corpus_knobs od_corpus_knobs;

#endif /* OD_TEST_CORPUS_RUNNER_H */
