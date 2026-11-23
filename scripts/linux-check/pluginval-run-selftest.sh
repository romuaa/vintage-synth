#!/bin/sh
# Self-test of the decision logic of pluginval-run.sh (--classify mode) on canned outputs in
# pluginval-samples/. Runs no pluginval and no gdb. Part of `scripts/preflight.sh --only docs`.
# Delete together with pluginval-run.sh.
here=$(cd "$(dirname "$0")" && pwd)
run="$here/pluginval-run.sh"
samples="$here/pluginval-samples"
fails=0

# expect <sample> <0|1> <text the verdict line must contain>
expect() {
  out=$(sh "$run" --classify "$samples/$1" 2>&1); rc=$?
  if [ "$rc" = "$2" ] && printf '%s\n' "$out" | grep -q "$3"; then
    echo "ok   $1 -> exit $rc"
  else
    echo "FAIL $1: expected exit $2 with '$3', got exit $rc:"; echo "$out"; fails=$((fails + 1))
  fi
}

expect clean-pass.txt                  0 'PASS'
expect known-crash-before-success.txt  0 'ACCEPTED KNOWN CRASH'
expect known-crash-after-success.txt   0 'ACCEPTED KNOWN CRASH'
expect exit-nonzero.txt                1 'exited with code 1'
expect test-failure.txt                1 'reported a failure'
expect known-crash-after-failure.txt   1 'reported a failure'
expect crash-in-test.txt               1 'while a test was running'
expect crash-other-backtrace.txt       1 'another frame #0'
expect crash-exitium-frame.txt         1 'mentions Exitium'
expect other-signal.txt                1 'unexpected signal'

# GitHub Actions annotation only for an accepted crash, only inside Actions.
if GITHUB_ACTIONS=true sh "$run" --classify "$samples/known-crash-after-success.txt" | grep -q '^::warning::known pluginval 1.0.4 teardown crash'; then
  echo "ok   warning annotation"
else
  echo "FAIL warning annotation missing"; fails=$((fails + 1))
fi

# A missing file, an empty file and a gdb-less output must not pass.
: >"${TMPDIR:-/tmp}/pv-empty.$$"
_=$(sh "$run" --classify "${TMPDIR:-/tmp}/pv-empty.$$" 2>&1); rc=$?
rm -f "${TMPDIR:-/tmp}/pv-empty.$$"
if [ "$rc" = 1 ]; then echo "ok   empty output -> exit 1"; else echo "FAIL empty output passed"; fails=$((fails + 1)); fi

if [ "$fails" -eq 0 ]; then echo "pluginval-run self-test: all passed"; exit 0; fi
echo "pluginval-run self-test: $fails failed"; exit 1
