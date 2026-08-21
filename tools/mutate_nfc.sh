#!/usr/bin/env bash
# Apply each 0x0083 mutation to shared/core/od_nfc.c, rebuild, run the suite, restore.
#
# THE POINT IS THAT EACH ONE TURNS THE SUITE RED. A green suite says the code passes its tests; it
# says nothing about whether the tests would notice the code being wrong. Two claims on this
# subsystem were already wrong in ways only a mutation exposed -- an ownership check that was never
# exercised because the mutated build failed and a stale binary ran, and a wrapping-length oracle
# that od_span_split() had silently made redundant.
#
# The capability-off mutation is checked against the STRUCTURAL proof rather than the suite, because
# what OD_CAP_NFC=0 claims is an absence: nfc_off_test would stay green with an assembler sitting
# unused beside it. That row runs the same nm assertions as check.sh's ratchet.
#
# A FAILED BUILD IS A FAILED MUTATION, not a skip. That is why every row reports its build.
set -uo pipefail
cd "$(dirname "$0")/.."

SRC=shared/core/od_nfc.c
BUILD=${OD_MUTATE_BUILD:-build/mutate}
BAK=$(mktemp)
# Restore the source AND rebuild, so the tree is not left holding the last mutant's binaries.
# Without the rebuild, build/mutate/od_nfc_test keeps reporting failures and the off fixture keeps
# an assembler in it -- a mutant left lying around where a passing artifact is expected.
cleanup () {
    cp "$BAK" "$SRC"
    rm -f "$BAK"
    cmake --build "$BUILD" --target od_nfc_test od_nfc_off_test -j"$(nproc)" >/dev/null 2>&1 || true
}
trap cleanup EXIT
cp "$SRC" "$BAK"

# The nm half of the capability-off claim, run the same way check.sh runs it.
off_proof_holds () { ! nm -a "$BUILD"/od_nfc_off_test 2>/dev/null | grep -qE "\bs_nfc\b"; }

cmake -S tests/host -B "$BUILD" >/dev/null || exit 1
cmake --build "$BUILD" --target od_nfc_test od_nfc_off_test -j"$(nproc)" >/dev/null 2>&1 || {
    echo "baseline build failed"; exit 1; }

# EVERY PROOF A MUTATION IS JUDGED BY MUST BE GREEN FIRST. A structural check that was already
# failing would make "the mutation was detected" true for the wrong reason -- the mutation would be
# credited with a symbol that was there before it.
"$BUILD"/od_nfc_test >/dev/null 2>&1 || { echo "baseline suite is not green"; exit 1; }
"$BUILD"/od_nfc_off_test >/dev/null 2>&1 || { echo "baseline capability-off suite is not green"; exit 1; }
off_proof_holds || { echo "baseline nm proof already fails: s_nfc present before any mutation"; exit 1; }

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

# The capability-off arm, judged by nm rather than by the suite.
mutate_off () {
    local name="capability-off allocates" obj hits
    cp "$BAK" "$SRC"
    python3 - <<'PY' || { printf '%-34s ANCHOR FAIL\n' "$name"; fail=1; return; }
p = "shared/core/od_nfc.c"
s = open(p).read()
old = '''od_cmd_result_t od_nfc_frame(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx;
    (void)body;
    return OD_CMD_UNKNOWN;
}'''
new = '''static struct { uint8_t data[OD_NFC_ASSEMBLY_MAX]; } s_nfc;

od_cmd_result_t od_nfc_frame(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx;
    (void)body;
    s_nfc.data[0] = 0u;
    return OD_CMD_UNKNOWN;
}'''
if s.count(old) != 1:
    raise SystemExit(1)
open(p, "w").write(s.replace(old, new))
PY
    if ! cmake --build "$BUILD" --target od_nfc_off_test -j"$(nproc)" >/dev/null 2>&1; then
        printf '%-34s BUILD FAILED -- not exercised\n' "$name"; fail=1; return
    fi
    # A FLIP, not a state: the baseline above established the proof holds, so finding the symbol
    # now is attributable to this mutation rather than to something that was already true.
    if off_proof_holds; then
        printf '%-34s SURVIVED -- the nm proof cannot see this\n' "$name"; fail=1
    else
        hits=$(nm -a "$BUILD"/od_nfc_off_test | grep -E "\bs_nfc\b" | head -1 | tr -s ' ')
        printf '%-34s nm proof flips to fail (%s)\n' "$name" "$hits"
    fi
}
mutate_off

cp "$BAK" "$SRC"
[ "$fail" -eq 0 ] && echo "all mutations detected" || echo "SOME MUTATIONS WERE NOT DETECTED"
exit "$fail"
