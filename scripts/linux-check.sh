#!/usr/bin/env bash
# Linux pre-flight check, run on a developer machine with Docker, before pushing.
#
#   scripts/linux-check.sh              GCC Release build of everything, both plugins (warnings are
#                                       errors, all errors shown) + ctest under xvfb (incl. both
#                                       host_smoke tests)
#   scripts/linux-check.sh --sanitize   Clang ASan+UBSan build + tests (mirrors CI "linux-sanitizers",
#                                       Ubuntu 24.04 image like CI)
#   --tests smoke|full                  with either of the two: the test tier (default full; smoke = every
#                                       test not tagged [slow], ctest -LE slow)
#   scripts/linux-check.sh --shell      interactive shell in the container (source snapshot in /work/src)
#   scripts/linux-check.sh --clean      remove the cached volume and images, then exit
#
# On Apple silicon (or any ARM host) the container is native Linux ARM64, with no emulation: this
# IS the Linux ARM64 target (a shipped architecture), built with GCC 11 on Ubuntu 22.04 like CI's
# linux-arm64 row, plus the same bundle/dependency check (architecture folder, ldd, glibc).
# The x86_64 binary is only proven in CI (linux-x64 row and the linux-distros job). On an x86_64
# host the same script runs x86_64 Linux instead. pluginval has no ARM64 Linux release, so it is
# not part of this check.
# The container VM needs ~1 GB of memory per parallel compile; the script limits the job count to
# the memory Docker reports (the default 8 GB Docker Desktop VM works). The tests run in parallel too,
# as many at a time as the container has CPUs (scripts/test-jobs.sh; VS_TEST_JOBS=<n> overrides it).
#
# How it works: the repository and both submodules are mounted read-only; a snapshot of the tracked
# and untracked-not-ignored files is rsynced into the named volume "vs-linux-check-build" together
# with the build trees and a ccache, so a second run only recompiles what changed. Nothing is
# written into the repository tree. The image (Dockerfile in scripts/linux-check/, base images
# pinned by digest below) is built once and cached; it is rebuilt when the Dockerfile or
# packages.txt change. The apt list is scripts/linux-check/packages.txt (mirrors the CI build step).
#
# Exit code: 0 = all passed, otherwise the failing step's status.
set -euo pipefail

# Pinned base images (linux/arm64 and linux/amd64 are both covered by the manifest-list digests).
# Bump deliberately, like the runner images in CI.
UBUNTU_2204=ubuntu:22.04@sha256:74b12b11ffa550fba1929ad4d8d16533c34f6612728469671afba1b03b66881c
UBUNTU_2404=ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5
VOLUME=vs-linux-check-build   # owned by the image's unprivileged build user

die() { echo "linux-check: $*" >&2; exit 1; }

mode=gcc
tier=full
while [[ $# -gt 0 ]]; do
  case $1 in
    --gcc) mode=gcc ;;
    --sanitize) mode=sanitize ;;
    --shell) mode=shell ;;
    --clean) mode=clean ;;
    --tests) [[ $# -ge 2 ]] || die "--tests needs smoke or full"
             case $2 in smoke|full) tier=$2 ;; *) die "--tests must be smoke or full (got '$2')" ;; esac; shift ;;
    -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option '$1' (use --sanitize, --tests smoke|full, --shell, --clean)" ;;
  esac
  shift
done

here=$(cd -P "$(dirname "$0")" && pwd)
repo=$(cd -P "$here/.." && pwd)
ctx=$here/linux-check

command -v docker >/dev/null 2>&1 || die "docker is not installed or not on PATH."
docker info >/dev/null 2>&1 || die "the Docker daemon is not running (start Docker Desktop or the engine and retry)."

if [[ $mode == clean ]]; then
  docker volume rm -f "$VOLUME" vs-linux-check-work >/dev/null   # (second name: older versions of this script)
  ids=$(docker images -q --filter label=vs.linux-check=1 | sort -u)
  [[ -z $ids ]] || docker rmi -f $ids >/dev/null
  echo "linux-check: removed volume $VOLUME and the cached images."
  exit 0
fi

# Submodules may be symlinks (git worktrees): resolve them so Docker mounts the real directories.
for d in JUCE Catch2; do
  [[ -d $repo/external/$d && -n $(ls -A "$repo/external/$d" 2>/dev/null) ]] \
    || die "external/$d is empty: run 'git submodule update --init' (or link it in a worktree)."
done
juce=$(cd -P "$repo/external/JUCE" && pwd)
catch=$(cd -P "$repo/external/Catch2" && pwd)

case $mode in
  sanitize) base=$UBUNTU_2404; flavour=2404 ;;
  *)        base=$UBUNTU_2204; flavour=2204 ;;
esac
hash=$(cat "$ctx/Dockerfile" "$ctx/packages.txt" | shasum -a 256 | cut -c1-10)
image=vs-linux-check:$flavour-$hash

t_all=$(date +%s)
if ! docker image inspect "$image" >/dev/null 2>&1; then
  echo "linux-check: building image $image (once; later runs reuse it)"
  t=$(date +%s)
  docker build -q --build-arg "BASE=$base" -t "$image" "$ctx" >/dev/null \
    || die "building the image failed (see the output above; rerun without -q: docker build --build-arg BASE=$base $ctx)"
  echo "linux-check: image built in $(( $(date +%s) - t ))s"
fi

# One run at a time per build volume: several worktrees (or agents) share the volume, and a second
# snapshot landing in the middle of a build or test run mixes two checkouts (seen as bogus failures).
# The lock is a directory in the temp dir holding the owner's pid; a dead owner's lock is taken over.
lock=${TMPDIR:-/tmp}/$VOLUME.lock
waited=0
until mkdir "$lock" 2>/dev/null; do
  owner=$(cat "$lock/pid" 2>/dev/null || true)
  if [[ -n $owner ]] && ! kill -0 "$owner" 2>/dev/null; then rm -rf "$lock"; continue; fi
  [[ $waited -eq 0 ]] && echo "linux-check: another run (pid ${owner:-?}) is using the build volume; waiting for it to finish"
  waited=1; sleep 5
done
echo $$ > "$lock/pid"
release_lock() { rm -rf "$lock"; }

# File list to snapshot: tracked + untracked-not-ignored, minus the submodules and files that vanished.
tmp=$(mktemp -d "${TMPDIR:-/tmp}/vs-linux-check.XXXXXX")
trap 'rm -rf "$tmp"; release_lock' EXIT
(cd "$repo" && git ls-files -z --cached --others --exclude-standard) \
  | while IFS= read -r -d '' f; do
      case $f in external/*) continue ;; esac
      [[ -e $repo/$f ]] && printf '%s\0' "$f"
    done > "$tmp/list" || true
[[ -s $tmp/list ]] || die "could not list the repository files with git."

# SYS_PTRACE: LeakSanitizer needs it. Mounts: repo and submodules read-only, list read-only.
args=(--rm --cap-add=SYS_PTRACE
  -v "$VOLUME":/work
  -v "$repo":/repo:ro -v "$juce":/deps/JUCE:ro -v "$catch":/deps/Catch2:ro
  -v "$tmp/list":/list:ro)
[[ -t 0 && -t 1 ]] && args+=(-it)

echo "linux-check: mode=$mode tier=$tier image=$image"
rc=0
# VS_TEST_JOBS (when set) reaches the container; otherwise it counts its own CPUs.
[[ -n ${VS_TEST_JOBS-} ]] && args+=(-e VS_TEST_JOBS)
docker run "${args[@]}" "$image" bash /repo/scripts/linux-check/inner.sh "$mode" "$tier" || rc=$?

dur=$(( $(date +%s) - t_all ))
if [[ $rc -eq 0 ]]; then
  echo "linux-check: PASSED ($mode) in ${dur}s"
else
  echo "linux-check: FAILED ($mode, exit $rc) after ${dur}s" >&2
fi
exit $rc
