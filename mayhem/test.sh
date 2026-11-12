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

BIN=/mayhem/bin
for b in test-basic test-cuckoo kat redisbloom.so; do
  [ -e "$BIN/$b" ] || { echo "missing $BIN/$b: mayhem/build.sh did not build it" >&2; emit_ctrf "redisbloom" 0 1 0; exit 1; }
done

passed=0; failed=0

# (1) Upstream unit tests (tests/unit/test-{basic,cuckoo}.c). test.h prints one "--- TEST" line per
#     test and "Assertion failed" per failed assertion, and does not change the exit status, so both
#     are counted; a run that executes fewer tests than the floor below (what upstream runs today) fails the difference.
for t in test-basic test-cuckoo; do
  case $t in test-basic) expected=11 ;; test-cuckoo) expected=9 ;; esac
  out=$("$BIN/$t" 2>&1); rc=$?
  ran=$(printf '%s\n' "$out" | grep -c '^--- TEST ')
  bad=$(printf '%s\n' "$out" | grep -c '^Assertion failed')
  [ "$rc" -eq 0 ] || bad=$((bad + 1))
  [ "$ran" -le "$expected" ] || ran=$expected
  missing=$((expected - ran))
  ok=$((ran - bad)); [ "$ok" -ge 0 ] || ok=0
  echo "$t: expected=$expected ran=$ran assertion_failures=$bad exit=$rc"
  passed=$((passed + ok)); failed=$((failed + bad + missing))
  [ "$bad" -eq 0 ] && [ "$missing" -eq 0 ] || printf '%s\n' "$out" | head -60
done

# (2) Known-answer probes (mayhem/kat.c): each prints "KAT PASS|FAIL <name>".
expected=$(grep -cE '^\s*KAT(_NEAR)?\("' mayhem/kat.c)
out=$("$BIN/kat" 2>&1); rc=$?
kp=$(printf '%s\n' "$out" | grep -c '^KAT PASS ')
kf=$(printf '%s\n' "$out" | grep -c '^KAT FAIL ')
[ "$rc" -eq 0 ] || [ "$kf" -gt 0 ] || kf=1
missing=$((expected - kp - kf)); [ "$missing" -ge 0 ] || missing=0
echo "kat: expected=$expected pass=$kp fail=$kf missing=$missing exit=$rc"
printf '%s\n' "$out" | grep -v '^KAT PASS ' | head -40
passed=$((passed + kp)); failed=$((failed + kf + missing))

emit_ctrf "redisbloom-unit+kat" "$passed" "$failed" 0
