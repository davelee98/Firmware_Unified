/* od_check — the assertion boilerplate every host suite was re-declaring.
 *
 * Thirty-eight files had grown their own copy of the same counters, the same CHECK macro and the
 * same closing printf. The copies had already drifted in what they print, which makes a failure
 * read differently depending on which suite found it.
 *
 * Header-only and single-TU by design: the counters are file-scope statics, so a suite spread over
 * several translation units keeps a count per unit. Every suite here is one file.
 *
 * Usage:
 *     #include "od_check.h"
 *     CASE("what this group is about");
 *     CHECK(cond);
 *     int main(void) { ...; return OD_CHECK_REPORT("suite_name"); }
 */

#ifndef OD_CHECK_H
#define OD_CHECK_H

#include <stdio.h>

static unsigned od_check_count;
static unsigned od_check_failures;
static const char *od_check_case = "(none)";

/* Label the group a failure belongs to. A bare condition rarely says what was being asserted. */
#define CASE(name) (od_check_case = (name))

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++od_check_count;                                                 \
        if (!(cond)) {                                                    \
            ++od_check_failures;                                          \
            printf("FAIL %s:%d [%s] %s\n",                                \
                   __FILE__, __LINE__, od_check_case, #cond);             \
        }                                                                 \
    } while (0)

/* Print the tally and yield main()'s exit status: 0 clean, 1 otherwise. */
#define OD_CHECK_REPORT(suite)                                            \
    (printf("%s: %u checks, %u failures\n",                               \
            (suite), od_check_count, od_check_failures),                  \
     od_check_failures == 0u ? 0 : 1)

/* A suite that runs no assertions has not passed -- it has not run. Call this instead of
 * OD_CHECK_REPORT where the case count is expected to be non-zero and could silently become zero
 * (a table that stopped being populated, a predicate that excluded everything). */
#define OD_CHECK_REPORT_NONEMPTY(suite, minimum)                          \
    ((od_check_count < (unsigned)(minimum)                                \
      ? (printf("FAIL %s: only %u checks ran, expected at least %u\n",    \
                (suite), od_check_count, (unsigned)(minimum)),            \
         ++od_check_failures)                                             \
      : 0u),                                                              \
     OD_CHECK_REPORT(suite))

#endif /* OD_CHECK_H */
