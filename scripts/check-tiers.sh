#!/usr/bin/env bash
# The two test tiers must partition the tests (docs/architecture.md 7.1.1): the tests labelled slow plus
# the tests not labelled slow are all the tests, and there is at least one slow test. Run it on a build
# tree after the build, before ctest; it catches a mangled label (tests/CMakeLists.txt, vs_discover_tests).
#
#   scripts/check-tiers.sh <build dir>      prints "tiers: N tests = S slow + R smoke"; exit 1 if S + R != N or S = 0
set -u
b=${1:?usage: check-tiers.sh <build dir>}
count() { ctest --test-dir "$b" -N "$@" | sed -n 's/^Total Tests: *//p'; }
all=$(count) || exit 1
slow=$(count -L slow) || exit 1
smoke=$(count -LE slow) || exit 1
echo "tiers: ${all:-?} tests = ${slow:-?} slow + ${smoke:-?} smoke"
if [ -z "$all" ] || [ -z "$slow" ] || [ -z "$smoke" ] || [ $((slow + smoke)) -ne "$all" ] || [ "$slow" -eq 0 ]; then
  echo "check-tiers: the slow and smoke tiers do not partition the tests of $b (a label was lost or split)" >&2
  exit 1
fi
