#!/bin/sh
# od_select_log_profile() contract, one cmake script-mode run per case.
#
# The accept cases prove the definition; the refuse cases prove the refusal, which is the half
# that matters -- a selector that quietly returns nothing for an unknown profile hands the build
# od_log.h's implicit default and looks like it worked.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case_file="$here/log_profile_case.cmake"
failures=0
checks=0

# A refuse case must fail because the selector refused, not because the fixture ran past the
# call and said so itself -- both are FATAL_ERROR and both exit non-zero. The sentinel is the
# only thing that separates them, so its presence is a failure regardless of exit status.
run_case() {
    name=$1
    want=$2  # accept | refuse
    checks=$((checks + 1))
    if output=$(cmake -DCASE="$name" -P "$case_file" 2>&1); then
        status=accept
    else
        status=refuse
    fi
    if [ "$status" != "$want" ]; then
        failures=$((failures + 1))
        echo "FAIL: case '$name' expected $want, got $status"
        printf '%s\n' "$output" | sed 's/^/    /'
        return
    fi
    if printf '%s' "$output" | grep -q 'SELECTOR-RETURNED'; then
        failures=$((failures + 1))
        echo "FAIL: case '$name' was refused by the fixture, not by the selector"
        printf '%s\n' "$output" | sed 's/^/    /'
    fi
}

run_case info      accept
run_case debug     accept
run_case appends   accept
run_case empty     refuse
run_case unknown   refuse
run_case wrong-case refuse
run_case duplicate refuse
run_case duplicate-bare  refuse
run_case duplicate-dashd refuse
run_case duplicate-conditional-genex refuse
run_case duplicate-if-genex refuse
run_case similar-name accept

echo "log profile selector: $checks cases, $failures failures"
[ "$failures" -eq 0 ]
