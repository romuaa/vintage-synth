#!/usr/bin/env bash
# Runs INSIDE the container (started by scripts/linux-check.sh). Do not run on the host.
#   /repo   the repository, read-only          /deps/JUCE, /deps/Catch2   submodules, read-only
#   /work   named volume: source snapshot, build trees, compiler cache
#   /list   file list (NUL separated) of the files to snapshot
set -euo pipefail
mode=${1:?mode}
tier=${2:-full}   # smoke = every test not tagged [slow] (ctest -LE slow); full = every test

export CCACHE_DIR=/work/ccache CCACHE_MAXSIZE=3G
src=/work/src
mkdir -p "$src" /work/ccache

# 1. Snapshot the tracked + untracked-not-ignored files into the volume (never writes to /repo).
#    Compare by content (--checksum) and do NOT keep the source's timestamps: a file whose content
#    changed gets a fresh mtime in the volume, so ninja rebuilds it even when the checkout it came
#    from is older than the last build here (switching branches or worktrees). Unchanged files are
#    not touched, so the build stays incremental.
rsync -a --no-times --checksum --from0 --files-from=/list /repo/ "$src/"
mkdir -p "$src/external"
ln -sfn /deps/JUCE "$src/external/JUCE"
ln -sfn /deps/Catch2 "$src/external/Catch2"
# Remove files deleted since the last run.
export LC_ALL=C
(cd "$src" && find . -type f -not -path './external/*' -print0 | sort -z) > /work/have.lst
sort -z /list | sed -z 's|^|./|' > /work/want.lst
# (the listed paths are relative to the snapshot, so run in it)
(cd "$src" && comm -z -23 /work/have.lst /work/want.lst | xargs -0 -r rm -f --)

# Parallel jobs: one per CPU, but at most one per GB of the Docker VM's memory (GCC needs ~1 GB per
# JUCE translation unit; more jobs get cc1plus killed by the OOM killer).
mem_gb=$(awk '/MemTotal/ { print int($2 / 1048576) }' /proc/meminfo)
jobs=$(nproc)
if [ "$mem_gb" -ge 1 ] && [ "$jobs" -gt "$mem_gb" ]; then jobs=$mem_gb; fi
# Tests: one job per CPU the container sees (scripts/test-jobs.sh; VS_TEST_JOBS overrides), no memory cap.
test_jobs=$(bash /repo/scripts/test-jobs.sh)
tier_args=()
[ "$tier" = smoke ] && tier_args=(-LE slow)
run_ctest() { # run_ctest <build dir> [more ctest arguments]
  echo "test run: tier=$tier jobs=$test_jobs (container sees $(nproc) CPUs, $mem_gb GB)"
  xvfb-run -a ctest --test-dir "$1" --output-on-failure --parallel "$test_jobs" "${tier_args[@]+"${tier_args[@]}"}" "${@:2}"
}
extra=()   # (the GCC/ARM64 warning in JUCE's bundled codec is handled in CMakeLists.txt)
t0=$(date +%s)
step() { echo; echo "=== $* ($(( $(date +%s) - t0 ))s elapsed)"; }

case "$mode" in
gcc)
  b=/work/build-gcc
  step "configure (GCC $(gcc -dumpfullversion), Release, ccache)"
  cmake -S "$src" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "${extra[@]}"
  step "build everything, all errors shown (-k 0)"
  cmake --build "$b" --parallel "$jobs" -- -k 0
  ccache --show-stats || true
  step "bundle and dependency check of both plugins (as in CI)"
  for name in "Exitium Vintage Synth.vst3" "Exitium B-Side.vst3"; do
    bundle=$(find "$b" -type d -name "$name" | head -n 1)
    [ -n "$bundle" ] || { echo "no $name bundle found"; exit 1; }
    echo "bundle: $bundle"; ls "$bundle/Contents"
    so=$(ls "$bundle"/Contents/*-linux/*.so | head -n 1)
    echo "architecture folder: $(basename "$(dirname "$so")")"
    # Captured and tested with an explicit exit: `! cmd | grep -q` can never fail a script (the `!` exempts the
    # pipeline from `set -e`), so this check used to pass whatever the plugin linked.
    ldd_out=$(ldd "$so")
    echo "$ldd_out"
    case $ldd_out in *"not found"*) echo "ERROR: ldd reports a missing library for $so"; exit 1 ;; esac
    case $ldd_out in *"libstdc++"*|*"libgcc_s"*) echo "ERROR: $so depends on a shared libstdc++ or libgcc (they must be linked statically)"; exit 1 ;; esac
    echo "Highest glibc symbol version: $(objdump -T "$so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1)"
  done
  step "ctest under xvfb (includes host_smoke and host_smoke_bside)"
  run_ctest "$b"
  ;;
sanitize)
  # Mirrors the linux-sanitizers job in .github/workflows/build.yml.
  b=/work/build-san
  export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  export ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1
  # LeakSanitizer needs ptrace, which Docker's default profile forbids; the script adds SYS_PTRACE.
  step "configure (clang $(clang --version | head -n1), RelWithDebInfo, ASan+UBSan)"
  cmake -S "$src" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
  step "build test binaries"
  # The test targets and the report of what runs come from the one list shared with preflight.sh
  # (mac-sanitize) and the CI job: every required target must exist, every built one must have tests.
  . /repo/scripts/sanitize-targets.sh
  san_select "$b"
  cmake --build "$b" --parallel "$jobs" --target "${SAN_TARGETS[@]}" -- -k 0
  san_report "$b"
  ccache --show-stats || true
  step "ctest (ASan + UBSan), host_smoke and host_smoke_bside excluded as in CI"
  run_ctest "$b" -E '^(host_smoke|host_smoke_bside)$'
  ;;
shell)
  cd "$src"
  exec bash
  ;;
*) echo "unknown mode $mode" >&2; exit 2 ;;
esac
step "done"
