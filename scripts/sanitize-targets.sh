#!/usr/bin/env bash
# Sourced (never run) by the three places that build and run the sanitizer tests: scripts/preflight.sh
# (mac-sanitize), scripts/linux-check/inner.sh (linux-sanitize) and the linux-sanitizers job of
# .github/workflows/build.yml. One list, one selection rule, one report, so a test executable cannot
# be left out of one of them without anybody noticing.
#
#   san_select <build dir>   after configuring: fills SAN_TARGETS with the test targets to build.
#                            Required targets that the build system does not define are an error
#                            (return 1); future ones are added when they exist.
#   san_report <build dir>   after building: prints the number of tests ctest knows for every built
#                            target and returns 1 if one has none. Run it before ctest.
#
# No pipeline reads the target list: `ninja -t targets | grep -q` dies of SIGPIPE under pipefail
# once grep has matched, which made every target look missing. The list is captured once and
# searched with a shell pattern.

# Test executables that exist today (all required) and the ones a later step adds (optional until then).
SAN_REQUIRED_TARGETS="VintageSynthTests render_regression bside_unit bside_independent BSideTests bside_trace"
SAN_FUTURE_TARGETS="bside_render"

san_select() {
  local b=$1 defined t missing=0
  defined=$(ninja -C "$b" -t targets all) || { echo "sanitizer targets: cannot list the targets of $b"; return 1; }
  [[ -n $defined ]] || { echo "sanitizer targets: ninja listed no targets in $b"; return 1; }
  defined=$'\n'$defined
  SAN_TARGETS=()
  for t in $SAN_REQUIRED_TARGETS; do
    case $defined in
      *$'\n'"$t:"*) SAN_TARGETS+=("$t") ;;
      *) echo "sanitizer targets: ERROR: the required test target '$t' is not defined by the build system"; missing=1 ;;
    esac
  done
  for t in $SAN_FUTURE_TARGETS; do
    case $defined in *$'\n'"$t:"*) SAN_TARGETS+=("$t") ;; esac
  done
  # A test-looking executable target (*Tests, *_unit, *_render, *_trace, render_regression) that the
  # build system defines but this list does not name is an error too: add it to the lists above.
  local line name known
  while IFS= read -r line; do
    name=${line%%:*}
    case $name in */*|*.*|*" "*|cmake_*) continue ;; esac # files, and ninja's own cmake_object_order_depends_target_* phonies
    case $name in
      *Tests|*_unit|*_render|*_trace|render_regression)
        known=0
        for t in "${SAN_TARGETS[@]}"; do [[ $t == "$name" ]] && known=1; done
        if [[ $known -eq 0 ]]; then
          echo "sanitizer targets: ERROR: '$name' looks like a test target and is defined, but is in neither list of scripts/sanitize-targets.sh"
          missing=1
        fi ;;
    esac
  done <<<"$defined"
  [[ $missing -eq 0 ]] || return 1
  echo "sanitizer targets to build: ${SAN_TARGETS[*]}"
}

san_report() {
  local b=$1 listing t n bad=0
  # The executable of every registered test: the first element of each "command" array of ctest's JSON
  # (printed as `"command" :`, then `[`, then the executable's path, each on its own line).
  listing=$(ctest --test-dir "$b" --show-only=json-v1 | awk '/"command" : *$/ { getline; getline; gsub(/^[ \t]*"|",?[ \t]*$/, ""); n = split($0, p, "/"); print p[n] }') \
    || { echo "sanitizer tests: ctest could not list the tests of $b"; return 1; }
  echo "tests registered per built test target (ctest runs all of them below):"
  for t in "${SAN_TARGETS[@]}"; do
    n=$(grep -c -x -- "$t" <<<"$listing") || true
    printf '  %-20s %6d tests\n' "$t" "$n"
    if [[ $n -eq 0 ]]; then echo "sanitizer tests: ERROR: '$t' was built but ctest knows no test of it"; bad=1; fi
  done
  return $bad
}
