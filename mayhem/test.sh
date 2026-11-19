#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Upstream's C 99 suite (c/test.c), built by build.sh with normal flags. Each test group runs on its own
# (USEARCH_FILTER) so the counts are per group; a group passes only when the runner exits 0 AND prints
# at least one "- PASSED" verdict line and no "Assert:" line (expect() prints and exits); a group that
# only reaches upstream's closing "All tests passed!" (nothing to run here, e.g. no GPU) counts as skipped.
RUNNER="$SRC/build-test/usearch_test_c"
if [ ! -x "$RUNNER" ]; then echo "missing $RUNNER (build.sh should have built it)" >&2; emit_ctrf usearch-test-c 0 1; exit 1; fi
work=$(mktemp -d "${TMPDIR:-/tmp}/usearch-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
groups=$(grep -o 'selected("[a-z_0-9]*")' "$SRC/c/test.c" | sed 's/selected("\(.*\)")/\1/' | sort -u)
passed=0 failed=0 skipped=0
for g in $groups; do
  out=$( cd "$work" && USEARCH_FILTER="$g" "$RUNNER" 2>&1 ); rc=$?
  if [ $rc -eq 0 ] && ! grep -q '^Assert:' <<<"$out" && grep -q -- '- PASSED' <<<"$out"; then
    passed=$((passed+1))
  elif [ $rc -eq 0 ] && ! grep -q '^Assert:' <<<"$out" && { grep -q -- '- SKIPPED' <<<"$out" || grep -q '^All tests passed!' <<<"$out"; }; then
    # SKIPPED verdicts, or a group with nothing to run on this host (the GPU/VRAM groups return silently).
    skipped=$((skipped+1))
  else
    failed=$((failed+1)); echo "FAIL: $g (rc=$rc)"; tail -5 <<<"$out"
  fi
done
echo "usearch_test_c groups: passed=$passed failed=$failed skipped=$skipped"
# A run that verifies nothing is not a pass.
[ "$passed" -gt 0 ] || failed=$((failed+1))
emit_ctrf usearch-test-c "$passed" "$failed" "$skipped"
