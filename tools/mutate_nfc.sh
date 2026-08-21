#!/usr/bin/env bash
# Apply each 0x0083 mutation to shared/core/od_nfc.c, rebuild, run the suite, restore.
#
# THE POINT IS THAT EACH ONE TURNS THE SUITE RED. A green suite says the code passes its tests; it
# says nothing about whether the tests would notice the code being wrong. Two claims on this
# subsystem were already wrong in ways only a mutation exposed -- an ownership check that was never
# exercised because the mutated build failed and a stale binary ran, and a wrapping-length oracle
# that od_span_split() had silently made redundant.
#
# A FAILED BUILD IS A FAILED MUTATION, not a skip. That is why every row reports its build.
set -uo pipefail
cd "$(dirname "$0")/.."

SRC=shared/core/od_nfc.c
BUILD=${OD_MUTATE_BUILD:-build/mutate}
BAK=$(mktemp)
trap 'cp "$BAK" "$SRC"; rm -f "$BAK"' EXIT
cp "$SRC" "$BAK"

cmake -S tests/host -B "$BUILD" >/dev/null || exit 1
cmake --build "$BUILD" --target od_nfc_test od_nfc_off_test -j"$(nproc)" >/dev/null 2>&1 || {
    echo "baseline build failed"; exit 1; }
"$BUILD"/od_nfc_test >/dev/null 2>&1 || { echo "baseline suite is not green"; exit 1; }

fail=0
mutate () {
    local name=$1 old=$2 new=$3 out
    cp "$BAK" "$SRC"
    python3 - "$old" "$new" <<'PY' || { printf '%-34s ANCHOR FAIL\n' "$name"; fail=1; return; }
import sys
p = "shared/core/od_nfc.c"
s = open(p).read()
if s.count(sys.argv[1]) != 1:
    sys.exit(1)
open(p, "w").write(s.replace(sys.argv[1], sys.argv[2]))
PY
    if ! cmake --build "$BUILD" --target od_nfc_test od_nfc_off_test -j"$(nproc)" >/dev/null 2>&1; then
        printf '%-34s BUILD FAILED -- not exercised\n' "$name"; fail=1; return
    fi
    out=$("$BUILD"/od_nfc_test 2>&1 | sed -n 's/^nfc: [0-9]* checks, \([0-9]*\) failures$/\1/p')
    if [ "${out:-0}" -gt 0 ]; then
        printf '%-34s %s failures\n' "$name" "$out"
    else
        printf '%-34s SURVIVED -- the suite cannot see this\n' "$name"; fail=1
    fi
}

mutate "owner check removed" \
  'return s_nfc.active && od_reply_same(&ctx->rp, &s_nfc.owner);' \
  'return s_nfc.active && ctx != NULL;'
mutate "owner compares tag only" \
  'return s_nfc.active && od_reply_same(&ctx->rp, &s_nfc.owner);' \
  'return s_nfc.active && ctx->rp.tag == s_nfc.owner.tag;'
mutate "owner compares origin only" \
  'return s_nfc.active && od_reply_same(&ctx->rp, &s_nfc.owner);' \
  'return s_nfc.active && ctx->rp.origin == s_nfc.owner.origin;'
mutate "32-bit widening reverted" \
  'if ((uint32_t)declared + 4u > (uint32_t)body.n) {' \
  'if ((uint16_t)(4u + declared) > (uint16_t)body.n) {'
mutate "short END converted to a reset" \
  '    if (s_nfc.received_len != s_nfc.total_len) {
        return nack(ctx, NFC_ERR_END_LEN_MISMATCH);' \
  '    if (s_nfc.received_len != s_nfc.total_len) {
        od_nfc_reset();
        return nack(ctx, NFC_ERR_END_LEN_MISMATCH);'
mutate "reply-failure unwind removed" \
  '    if (od_reply(ctx->r, &ctx->rp, frame, sizeof frame) != OD_TXQ_OK) {
        od_nfc_reset();
        return OD_CMD_NACK;
    }' \
  '    if (od_reply(ctx->r, &ctx->rp, frame, sizeof frame) != OD_TXQ_OK) {
        return OD_CMD_NACK;
    }'

cp "$BAK" "$SRC"
[ "$fail" -eq 0 ] && echo "all mutations detected" || echo "SOME MUTATIONS WERE NOT DETECTED"
exit "$fail"
