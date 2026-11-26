#!/usr/bin/env bash
# The marks of the two test tiers must arrive in ctest (docs/architecture.md 7.1.5 item 2). For every test
# executable that ctest knows in a build tree:
#   the number of tests Catch2 itself lists for [slow]   (hidden tests excluded) = the tests ctest labels slow
#   the number of tests Catch2 itself lists for [serial] (hidden tests excluded) = the tests with RUN_SERIAL
# It fails when a label or property is lost or split (tests/CMakeLists.txt, vs_discover_tests). Comparing
# `ctest -L slow` with `ctest -LE slow` would prove nothing: the two are complements by definition.
#
#   scripts/check-tiers.sh <build dir>    run it on a build tree after the build, before ctest
#
# Which executables count: those whose ctest tests came from catch_discover_tests, recognised by the
# `--order decl` argument it always appends to the command. host_smoke and host_smoke_bside (JUCE host) and
# vst3_validator* (Steinberg's tool) are not Catch2 programs and have no tags: they are skipped by that rule,
# never by name, and are never slow. Only executables the build tree has built are checked (the sanitizer trees
# build some of them). Every output that is parsed goes through tr -d '\r' (Windows) and only stdout of the test
# executables is read (sanitizers write to stderr). Needs only bash, awk, grep, tr and ctest.
set -u
b=${1:?usage: check-tiers.sh <build dir>}
tmp=$(mktemp "${TMPDIR:-/tmp}/vs-check-tiers.XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

# One line per Catch2 executable: path, tests labelled slow, tests with RUN_SERIAL (from ctest's JSON).
ctest --test-dir "$b" --show-only=json-v1 | tr -d '\r' | awk '
  function flush() {
    if (have && catch) { n[exe]++; s[exe] += slow; r[exe] += serial }
    have = 0
  }
  /"command" : *$/ { flush(); getline; getline; exe = $0; gsub(/^[ \t]*"|",?[ \t]*$/, "", exe); gsub(/\\\\/, "\\", exe)
                     have = 1; slow = 0; serial = 0; catch = 0; inlabels = 0; next }
  /"--order decl"/ { catch = 1 }
  /"name" : "LABELS"/ { inlabels = 1; next }
  inlabels && /^ *\]/ { inlabels = 0 }
  inlabels && /"slow"/ { slow = 1 }
  /"name" : "RUN_SERIAL"/ { getline; if ($0 ~ /true|ON|1/) serial = 1 }
  END { flush(); for (e in n) printf "%s\t%d\t%d\n", e, s[e], r[e] }
' > "$tmp" || exit 1

bad=0 checked=0 slow_total=0
while IFS=$'\t' read -r exe ct_slow ct_serial; do
  [ -n "$exe" ] || continue
  if [ ! -x "$exe" ] && [ ! -x "$exe.exe" ]; then
    echo "check-tiers: ctest knows $exe but it is not built: skipped"; continue
  fi
  cat_slow=$("$exe" "[slow]~[.]" --list-tests --verbosity quiet 2>/dev/null | tr -d '\r' | grep -c .)
  cat_serial=$("$exe" "[serial]~[.]" --list-tests --verbosity quiet 2>/dev/null | tr -d '\r' | grep -c .)
  name=${exe##*/}
  echo "tiers: $name: slow catch2=$cat_slow ctest=$ct_slow, serial catch2=$cat_serial ctest=$ct_serial"
  checked=$((checked + 1)); slow_total=$((slow_total + ct_slow))
  if [ "$cat_slow" -ne "$ct_slow" ] || [ "$cat_serial" -ne "$ct_serial" ]; then
    echo "check-tiers: $name: the tags and ctest disagree (a label or RUN_SERIAL was lost or split)" >&2; bad=1
  fi
done < "$tmp"
if [ "$checked" -eq 0 ]; then echo "check-tiers: no Catch2 test executable found in $b" >&2; exit 1; fi
if [ "$slow_total" -eq 0 ]; then echo "check-tiers: no test is labelled slow in $b" >&2; bad=1; fi
exit $bad
