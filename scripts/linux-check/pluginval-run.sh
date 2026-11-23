#!/bin/sh
# Runs pluginval (Linux x86_64) under gdb and accepts ONE known crash. Temporary workaround.
#
#   pluginval-run.sh <pluginval> <pluginval arguments...> <bundle>   run it; exit 0 = validation passed
#   pluginval-run.sh --classify <captured-output-file>                decide on a captured output only
#
# Why: pluginval 1.0.4 (the newest Linux build that runs on our glibc 2.35 floor) is built with JUCE
# 8.0.3, whose VST3 host code has a use-after-free during shutdown: the main thread's dispatch loop
# calls an fd callback that still points into an event-handler map of an already unloaded plugin
# (fixed upstream in JUCE commit 04e167d646, "VST3 Host: Fix an occasional crash when removing
# callbacks from the message loop during shutdown"). About 6 to 7 % of runs end with SIGSEGV (exit 139)
# after every test has completed, sometimes before and sometimes after the final "SUCCESS" line.
# With two plugins and eight environments per CI run, most runs would fail on this by chance.
#
# What it accepts: a SIGSEGV is accepted ONLY on the exact signature (see classify below):
# no failure marker in the output, every started test completed and the last one is the final test of
# the run, backtrace frame #0 is the IEventHandler map operator[] called from frame #1
# RunLoop::Impl::registerEventHandler, and no frame names our plugin. Everything else fails: a test
# failure, any other signal, a crash during a test, another backtrace, gdb missing (this script never
# runs pluginval without gdb).
#
# Exit status under gdb: in batch mode gdb itself exits 0 or 1 and loses the program's exit code. So
# after `run` we make gdb print `PV_EXITCODE=<n>` with `printf "%d", $_exitcode` ($_exitcode is set
# only when the program exited; after a signal it is void, the printf errors and prints nothing). The
# verdict reads that line, and for a normal exit also requires the "SUCCESS" line of pluginval.
# A signal is read from gdb's "received signal SIG..." stop message and the `bt 12` that follows.
#
# DELETE this script, its self-test (pluginval-run-selftest.sh), the samples and the wrapper calls in
# build.yml and distro-check.sh when a pluginval Linux build with JUCE 9.0.2 or newer exists that runs
# on glibc 2.35. Record: docs/engineering-notes.md "pluginval 1.0.4 crashes at teardown".
#
# POSIX sh: it also runs inside minimal distribution containers (distro-check.sh).

# The last test pluginval runs at strictness 10. Hard-coded on purpose (checked together with "every
# started test completed"); if a pluginval upgrade adds a later test, the crash is rejected, not accepted.
FINAL_TEST="Fuzz parameters"

warn_gha() { [ -n "${GITHUB_ACTIONS:-}" ] && echo "::warning::$1"; return 0; }

reject() { echo "PLUGINVAL VERDICT: FAIL - $1"; return 1; }

# classify <file>: prints the verdict; 0 = validation passed (possibly with the accepted crash).
classify() {
  f=$1
  [ -r "$f" ] || { echo "PLUGINVAL VERDICT: FAIL - cannot read $f"; return 1; }
  # Strip carriage returns once, work on the copy.
  txt=$(mktemp "${TMPDIR:-/tmp}/pvc.XXXXXX") || return 1
  tr -d '\r' <"$f" >"$txt"

  if grep -Eq '!!! Test|FAILED|FAILURE|\*\*\* FAILED' "$txt"; then
    rm -f "$txt"; reject "pluginval reported a failure"; return 1
  fi

  sig=$(grep -E 'received signal|terminated with signal' "$txt" | head -n 1)
  code=$(sed -n 's/^PV_EXITCODE=\([0-9][0-9]*\)$/\1/p' "$txt" | tail -n 1)

  if [ -z "$sig" ]; then
    if [ -z "$code" ]; then
      rm -f "$txt"; reject "no exit status and no signal in the output (did gdb run pluginval?)"; return 1
    fi
    if [ "$code" != 0 ]; then
      rm -f "$txt"; reject "pluginval exited with code $code"; return 1
    fi
    if ! grep -q '^SUCCESS$' "$txt"; then
      rm -f "$txt"; reject "pluginval exited 0 but printed no SUCCESS line"; return 1
    fi
    rm -f "$txt"; echo "PLUGINVAL VERDICT: PASS - pluginval exited 0 with SUCCESS"; return 0
  fi

  # A signal. Only SIGSEGV after the last test is acceptable.
  case $sig in
    *SIGSEGV*) ;;
    *) rm -f "$txt"; reject "unexpected signal: $sig"; return 1 ;;
  esac

  starts=$(sed -n 's/^Starting tests in: \(.*\)\.\.\.$/\1/p' "$txt" | sort)
  dones=$(sed -n 's/^Completed tests in \(.*\)$/\1/p' "$txt" | sort)
  if [ -z "$starts" ] || [ "$starts" != "$dones" ]; then
    rm -f "$txt"; reject "SIGSEGV while a test was running (started and completed tests differ)"; return 1
  fi
  last=$(sed -n 's/^Completed tests in \(.*\)$/\1/p' "$txt" | tail -n 1)
  case $last in
    *"$FINAL_TEST") ;;
    *) rm -f "$txt"; reject "SIGSEGV before the final test '$FINAL_TEST' completed (last completed: $last)"; return 1 ;;
  esac

  f0=$(grep '^#0 ' "$txt" | head -n 1)
  f1=$(grep '^#1 ' "$txt" | head -n 1)
  case $f0 in
    *IEventHandler*operator\[\]*) ;;
    *) rm -f "$txt"; reject "SIGSEGV with another frame #0: ${f0:-none}"; return 1 ;;
  esac
  case $f1 in
    *RunLoop::Impl::registerEventHandler*) ;;
    *) rm -f "$txt"; reject "SIGSEGV with another frame #1: ${f1:-none}"; return 1 ;;
  esac
  if grep -E '^#[0-9]+ ' "$txt" | grep -qi 'Exitium'; then
    rm -f "$txt"; reject "a frame of the backtrace mentions Exitium (the plugin is involved)"; return 1
  fi

  rm -f "$txt"
  echo "PLUGINVAL VERDICT: ACCEPTED KNOWN CRASH - pluginval 1.0.4 teardown use-after-free (JUCE fix 04e167d646), all tests had completed"
  warn_gha "known pluginval 1.0.4 teardown crash (JUCE fix 04e167d646), accepted"
  return 0
}

if [ "${1:-}" = "--classify" ]; then
  [ $# -eq 2 ] || { echo "usage: pluginval-run.sh --classify <captured-output-file>"; exit 2; }
  classify "$2"
  exit $?
fi

[ $# -ge 2 ] || { echo "usage: pluginval-run.sh <pluginval> <pluginval arguments...> <bundle>"; exit 2; }
command -v gdb >/dev/null 2>&1 || { echo "PLUGINVAL VERDICT: FAIL - gdb is not installed (it is required, see the header of $0)"; exit 1; }

out=$(mktemp "${TMPDIR:-/tmp}/pv-out.XXXXXX") || exit 1
trap 'rm -f "$out"' EXIT

# shellcheck disable=SC2016  # $_exitcode is a gdb convenience variable, not a shell one
# SIGPIPE is passed silently; randomization stays on (containers refuse to switch it off).
gdb -q -batch \
  -ex 'set pagination off' \
  -ex 'set disable-randomization off' \
  -ex 'handle SIGPIPE nostop noprint pass' \
  -ex 'run' \
  -ex 'printf "PV_EXITCODE=%d\n", $_exitcode' \
  -ex 'bt 12' \
  --args "$@" 2>&1 | tee "$out"

classify "$out"
