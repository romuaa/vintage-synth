#!/usr/bin/env bash
# The number of tests ctest runs side by side (docs/architecture.md 7.1.2): the one helper behind
# every ctest call of the gates (scripts/preflight.sh, scripts/linux-check/inner.sh, build.yml).
#
#   scripts/test-jobs.sh      prints the job count on stdout, nothing else
#
# The count is the number of logical cores of the machine or container this runs on (macOS,
# Linux including the Docker VM's view, Git Bash on a Windows runner). VS_TEST_JOBS=<n> overrides
# it everywhere; 1 gives the old serial run. A value that is not a positive integer is an error
# (exit 2). Callers print the count before the tests start.
set -u

case ${VS_TEST_JOBS-} in
  "") ;;
  *[!0-9]*|0|0[0-9]*) echo "test-jobs: VS_TEST_JOBS must be a positive integer (got '$VS_TEST_JOBS')" >&2; exit 2 ;;
  *) echo "$VS_TEST_JOBS"; exit 0 ;;
esac

n=""
case $(uname -s 2>/dev/null) in
  Darwin) n=$(sysctl -n hw.logicalcpu 2>/dev/null) ;;
  MINGW*|MSYS*|CYGWIN*) n=${NUMBER_OF_PROCESSORS-} ;;
esac
[ -n "$n" ] || n=$(nproc 2>/dev/null) || n=""
[ -n "$n" ] || n=$(getconf _NPROCESSORS_ONLN 2>/dev/null) || n=""
case $n in ""|*[!0-9]*|0) n=1 ;; esac
echo "$n"
