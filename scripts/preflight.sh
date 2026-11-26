#!/usr/bin/env bash
# The local gate: every check this Mac can do, in one command. CI is a deliberate confirmation step
# (see the header of .github/workflows/build.yml); this script is what you run before asking for it.
#
#   scripts/preflight.sh                 every local step (--full is the same), the full test tier
#   scripts/preflight.sh --quick         the everyday loop: docs + mac + linux, the smoke test tier
#   scripts/preflight.sh --tests full|smoke
#                                        the test tier of the steps that run tests: smoke = every test not
#                                        tagged [slow] (ctest -LE slow), full = every test (ctest). Default:
#                                        smoke with --quick, full otherwise; "smoke" is refused for the
#                                        default and --full runs (only --quick or --only may narrow the gate)
#   scripts/preflight.sh --only mac,docs run only these steps (repeatable)
#   scripts/preflight.sh --skip scan     skip a step (repeatable)
#   scripts/preflight.sh --keep-going    run every step even after a failure (default: stop at the first)
#   scripts/preflight.sh --quiet         log to files only; show the tail of a failing step
#   scripts/preflight.sh --list          list the steps and what each one does
#   scripts/preflight.sh --clean         remove everything this script created outside the repo's
#                                        git-ignored build/ (caches, Docker volumes and images), then exit
#
# Steps, in run order (cheap and fast-failing first):
#   docs           brand-name deny-list, no Claude-Session trailers, workflow YAML + trigger policy + JSON + CI plan table,
#                  shell syntax, pluginval-wrapper self-test          (scripts/preflight/docs-check.sh)
#   scan           Trivy (vuln, secret, misconfig; HIGH/CRITICAL fail) from its pinned container image,
#                  scanning a read-only snapshot of the tracked files (same settings as security.yml)
#   mac            macOS universal Release build of both plugins, warnings as errors, lipo check of
#                  both slices of each, ctest (tier above, parallel), pluginval strictness 10 on each bundle (with its
#                  editor tests), then the release packer on the Vintage bundle it just validated:
#                  pack the macOS zip, verify it from the zip alone, self-test the verifier, check the
#                  checksums, and load the bundle unpacked from the zip in host_smoke
#                  (scripts/package-release.sh, docs/architecture.md 10.4)
#   linux          scripts/linux-check.sh: GCC, Linux ARM64 container, tests (tier above, parallel) under xvfb, bundle check
#   validator      Steinberg's VST3 validator, built once from the SDK commit pinned in build.yml (cached),
#                  run on each bundle the `mac` step built; needs "N tests passed, 0 tests failed"
#   mac-sanitize   arm64 RelWithDebInfo ASan+UBSan: unit tests and render regression (tier above, parallel)
#   linux-sanitize scripts/linux-check.sh --sanitize (clang ASan+UBSan, Ubuntu 24.04, ARM64)
#
# Tests run in parallel (docs/architecture.md 7.1.2): as many at a time as the machine (or, in the Linux
# steps, the Docker VM) has logical cores, from scripts/test-jobs.sh; VS_TEST_JOBS=<n> overrides it
# (1 = serial). Every step that runs tests prints "test run: tier=<t> jobs=<n>" and the summary repeats it.
# A test tagged [serial] always runs alone. The steps themselves still run one after another.
#
# Two plugins (Exitium Vintage Synth, Exitium B-Side): every step covers both. While iterating,
#   VS_PREFLIGHT_PLUGINS=vintage|bside|all   (default all) limits the pluginval and validator runs of
# the `mac` and `validator` steps to one bundle (the build, ctest and the other steps always cover both).
# The hand-over gate always runs `all`.
#
# Steps run one after another, not concurrently: the macOS builds and the Docker VM already use all
# cores, and concurrent runs would only make timings and memory-pressure failures (the container's
# OOM killer) flaky (the tests inside a step do run in parallel, see above). Nothing is written into the repository except build/preflight/ (git-ignored).
#
# Where things go:
#   build/preflight/mac, build/preflight/mac-san   CMake build trees (git-ignored via /build/)
#   build/preflight/logs/<step>.log                full output of every step
#   ~/Library/Caches/vs-preflight/                 pluginval downloads (checksum verified) and the VST3 SDK
#                                                  clone + built validator (vst3sdk-<sha>/); override the
#                                                  directory with VS_PREFLIGHT_CACHE. PLUGINVAL / VST3_VALIDATOR
#                                                  env vars point at your own copies instead.
#   Docker: image vs-linux-check:*, volume vs-linux-check-build, vs-preflight-trivy-cache,
#           the pinned Trivy image
#
# Exit code: 0 = every selected step passed, 1 = a step failed, 2 = usage or missing tool.
set -uo pipefail

here=$(cd -P "$(dirname "$0")" && pwd)
repo=$(cd -P "$here/.." && pwd)
cd "$repo" || exit 2

CACHE=${VS_PREFLIGHT_CACHE:-$HOME/Library/Caches/vs-preflight}
OUT=$repo/build/preflight
LOGS=$OUT/logs
# Trivy: the version is read from security.yml (one source of truth); the digest pins the image
# (multi-arch manifest list) for that version. Bump both together, deliberately.
TRIVY_IMAGE_DIGEST=sha256:8f6bb947dbc528902e11f2f5be52f7b519f6a39d5d148f41166f2040499768fb  # v0.76.2
TRIVY_IMAGE_VERSION=v0.76.2
TRIVY_CACHE_VOLUME=vs-preflight-trivy-cache

ALL_STEPS=(docs scan mac validator linux mac-sanitize linux-sanitize)
PLUGINS=${VS_PREFLIGHT_PLUGINS:-all}
case $PLUGINS in vintage|bside|all) ;; *) echo "preflight: VS_PREFLIGHT_PLUGINS must be vintage, bside or all (got '$PLUGINS')" >&2; exit 2 ;; esac
DEFAULT_STEPS=("${ALL_STEPS[@]}")
QUICK_STEPS=(docs mac linux)

die() { echo "preflight: $*" >&2; exit 2; }
note() { echo "preflight: $*"; }
need() { command -v "$1" >/dev/null 2>&1 || die "'$1' not found: $2"; }
need_docker() {
  need docker "install Docker Desktop (or another engine) and retry"
  docker info >/dev/null 2>&1 || die "the Docker daemon is not running: start Docker Desktop and retry"
}

# ---- options ----------------------------------------------------------------------------------
selected=("${DEFAULT_STEPS[@]}")
only=() skip=() keep_going=0 quiet=0 mode_set="" tests_opt=""
split_into() { local -n arr=$1; local IFS=,; local x; for x in $2; do arr+=("$x"); done; }
while [[ $# -gt 0 ]]; do
  case $1 in
    --quick) selected=("${QUICK_STEPS[@]}"); mode_set=quick ;;
    --full) selected=("${ALL_STEPS[@]}"); mode_set=full ;;
    --tests) [[ $# -ge 2 ]] || die "--tests needs smoke or full"
             case $2 in smoke|full) tests_opt=$2 ;; *) die "--tests must be smoke or full (got '$2')" ;; esac; shift ;;
    --only) [[ $# -ge 2 ]] || die "--only needs a step name"; split_into only "$2"; shift ;;
    --skip) [[ $# -ge 2 ]] || die "--skip needs a step name"; split_into skip "$2"; shift ;;
    --keep-going) keep_going=1 ;;
    --quiet) quiet=1 ;;
    --list) sed -n '/^# Steps, in run order/,/^#$/p' "$0" | sed '1d;$d;s/^# \{0,1\}//'; exit 0 ;;
    --clean) mode_set=clean ;;
    -h|--help) sed -n '2,/^# Exit code/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option '$1' (see --help)" ;;
  esac
  shift
done

# The hand-over gate covers both plugins. A narrower VS_PREFLIGHT_PLUGINS (it may be left exported in a
# shell) is only accepted for an iteration run (--quick or --only), never for the default or --full run.
if [[ $PLUGINS != all && $mode_set != quick && ${#only[@]} -eq 0 ]]; then
  die "VS_PREFLIGHT_PLUGINS=$PLUGINS limits the gate to one plugin: allowed only with --quick or --only (the default and --full runs always cover both); unset it"
fi

# The test tier: smoke (ctest -LE slow) or full (ctest). The hand-over gate runs full; "smoke" is only
# accepted for an iteration run (--quick or --only), like a narrowed VS_PREFLIGHT_PLUGINS.
if [[ $tests_opt == smoke && $mode_set != quick && ${#only[@]} -eq 0 ]]; then
  die "--tests smoke narrows the gate to the smoke tier: allowed only with --quick or --only (the default and --full runs always run the full tier)"
fi
TIER=${tests_opt:-$([[ $mode_set == quick ]] && echo smoke || echo full)}
TEST_JOBS=$("$here/test-jobs.sh") || exit 2
tier_args=()
[[ $TIER == smoke ]] && tier_args=(-LE slow)

valid_step() { local s; for s in "${ALL_STEPS[@]}"; do [[ $s == "$1" ]] && return 0; done; return 1; }
for s in "${only[@]+"${only[@]}"}" "${skip[@]+"${skip[@]}"}"; do
  valid_step "$s" || die "unknown step '$s' (steps: ${ALL_STEPS[*]})"
done

if [[ $mode_set == clean ]]; then
  rm -rf "$CACHE" "$OUT"
  note "removed $CACHE and $OUT"
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    docker volume rm -f "$TRIVY_CACHE_VOLUME" >/dev/null
    docker rmi "ghcr.io/aquasecurity/trivy@$TRIVY_IMAGE_DIGEST" >/dev/null 2>&1 || true
    "$here/linux-check.sh" --clean
    note "removed the Docker volume $TRIVY_CACHE_VOLUME, the pinned Trivy image and the linux-check images/volumes"
  else
    note "Docker is not running: its images and volumes were left (rerun --clean with Docker up)"
  fi
  exit 0
fi

if [[ ${#only[@]} -gt 0 ]]; then
  selected=("${only[@]}")
fi
# Drop skipped steps, then put the selection in canonical run order (without duplicates).
run_steps=()
for s in "${ALL_STEPS[@]}"; do
  in_sel=0 skipped=0
  for x in "${selected[@]}"; do [[ $x == "$s" ]] && in_sel=1; done
  for x in "${skip[@]+"${skip[@]}"}"; do [[ $x == "$s" ]] && skipped=1; done
  [[ $in_sel -eq 1 && $skipped -eq 0 ]] && run_steps+=("$s")
done
[[ ${#run_steps[@]} -gt 0 ]] || die "no steps selected"

# ---- helpers ----------------------------------------------------------------------------------
# The workflow files are the single source of truth for pinned versions and hashes.
yml_env() { sed -n "s/^[[:space:]]*$1:[[:space:]]*\\([^[:space:]#]*\\).*/\\1/p" "$repo/$2" | head -n 1; }

# fetch_pluginval macos|linux -> prints the path of the executable. Verified by SHA-256 before unpacking.
fetch_pluginval() {
  local os=$1 ver sha asset exe dir
  ver=$(yml_env PLUGINVAL_VERSION .github/workflows/build.yml)
  case $os in
    macos) sha=$(yml_env PLUGINVAL_SHA256_MACOS .github/workflows/build.yml); asset=pluginval_macOS.zip
           exe=pluginval.app/Contents/MacOS/pluginval ;;
    linux) sha=$(yml_env PLUGINVAL_SHA256_LINUX .github/workflows/build.yml); asset=pluginval_Linux.zip
           exe=pluginval ;;
  esac
  [[ -n $ver && ${#sha} -eq 64 ]] || die "could not read the pluginval version/hash from .github/workflows/build.yml"
  dir=$CACHE/pluginval-$ver-$sha/$os
  if [[ ! -x $dir/$exe ]]; then
    need curl "needed to download pluginval"; need unzip "needed to unpack pluginval"
    mkdir -p "$CACHE" || die "cannot create $CACHE"
    local work; work=$(mktemp -d "$CACHE/dl.XXXXXX")
    note "downloading pluginval $ver ($asset) into $work" >&2
    curl -fsSL -o "$work/$asset" "https://github.com/Tracktion/pluginval/releases/download/$ver/$asset" \
      || { rm -rf "$work"; die "download of $asset failed"; }
    local got; got=$(shasum -a 256 "$work/$asset" | cut -d' ' -f1)
    [[ $got == "$sha" ]] || { rm -rf "$work"; die "checksum mismatch for $asset: got $got, build.yml says $sha"; }
    mkdir "$work/unpacked" && unzip -q "$work/$asset" -d "$work/unpacked" || { rm -rf "$work"; die "unzip failed"; }
    [[ -e $work/unpacked/$exe ]] || { rm -rf "$work"; die "$exe not found in $asset"; }
    chmod +x "$work/unpacked/$exe"
    mkdir -p "$(dirname "$dir")" && rm -rf "$dir" && mv "$work/unpacked" "$dir" && rm -rf "$work"
  fi
  printf '%s\n' "$dir/$exe"
}

ensure_out_ignored() {
  git check-ignore -q build/preflight/x 2>/dev/null \
    || die "build/preflight is not git-ignored; refusing to write into the repository tree"
  mkdir -p "$LOGS"
}

jobs_flag=() # ninja decides the parallelism (all cores).

# run_ctest <build dir> [more ctest arguments]: the tests of the selected tier, TEST_JOBS at a time.
run_ctest() {
  local b=$1; shift
  echo "test run: tier=$TIER jobs=$TEST_JOBS"
  ctest --test-dir "$b" --output-on-failure --parallel "$TEST_JOBS" "${tier_args[@]+"${tier_args[@]}"}" "$@"
}

# The bundles this run checks, "Name.vst3" in build-tree order: one per plugin selected by
# VS_PREFLIGHT_PLUGINS. bundle_path <build tree> <Name.vst3> prints the built bundle or nothing.
selected_bundles() {
  case $PLUGINS in
    vintage) echo "Exitium Vintage Synth.vst3" ;;
    bside) echo "Exitium B-Side.vst3" ;;
    *) echo "Exitium Vintage Synth.vst3"; echo "Exitium B-Side.vst3" ;;
  esac
}
bundle_path() { find "$1" -type d -name "$2" -path '*/Release/VST3/*' 2>/dev/null | head -n 1; }

# ---- steps ------------------------------------------------------------------------------------
step_docs() { "$here/preflight/docs-check.sh"; }

step_scan() {
  need_docker; need git "needed to list the files to scan"
  local ver; ver=$(yml_env TRIVY_VERSION .github/workflows/security.yml)
  [[ $ver == "$TRIVY_IMAGE_VERSION" ]] \
    || die "security.yml uses Trivy $ver but scripts/preflight.sh pins the image for $TRIVY_IMAGE_VERSION: update both together"
  local snap; snap=$(mktemp -d "${TMPDIR:-/tmp}/vs-preflight-scan.XXXXXX")
  # Snapshot = what CI checks out: tracked files (+ new files not ignored), no submodules.
  (cd "$repo" && git ls-files -z --cached --others --exclude-standard) \
    | while IFS= read -r -d '' f; do
        case $f in external/*) continue ;; esac
        [[ -f $repo/$f ]] && printf '%s\0' "$f"
      done > "$snap.list"
  rsync -a --from0 --files-from="$snap.list" "$repo/" "$snap/" || { rm -rf "$snap" "$snap.list"; return 1; }
  rm -f "$snap.list"
  note "scanning a snapshot of the tracked files ($(find "$snap" -type f | wc -l | tr -d ' ') files) with Trivy $ver"
  local rc=0
  docker run --rm -v "$snap":/repo:ro -v "$TRIVY_CACHE_VOLUME":/root/.cache/trivy \
    "ghcr.io/aquasecurity/trivy@$TRIVY_IMAGE_DIGEST" fs \
    --scanners vuln,secret,misconfig --severity HIGH,CRITICAL --ignore-unfixed \
    --format table --exit-code 1 --no-progress /repo || rc=$?
  rm -rf "$snap"
  return $rc
}

step_mac() {
  need cmake "brew install cmake ninja"; need ninja "brew install cmake ninja"; need lipo "install Xcode command line tools"
  local b=$OUT/mac
  cmake -S . -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DCMAKE_COMPILE_WARNING_AS_ERROR=ON || return 1
  cmake --build "$b" -- -k 0 || return 1
  # Both plugins are always built and lipo-checked, whatever VS_PREFLIGHT_PLUGINS selects.
  local name bundle bin archs
  for name in "Exitium Vintage Synth.vst3" "Exitium B-Side.vst3"; do
    bundle=$(bundle_path "$b" "$name")
    [[ -n $bundle ]] || { echo "no $name bundle found under $b"; return 1; }
    bin=$(find "$bundle/Contents/MacOS" -type f | head -n 1)
    archs=$(lipo -archs "$bin")
    echo "bundle: $bundle"; echo "lipo: $archs"
    [[ $archs == *arm64* && $archs == *x86_64* ]] || { echo "the plugin binary $bin is not universal"; return 1; }
  done
  run_ctest "$b" || return 1
  local pv=${PLUGINVAL:-}
  if [[ -z $pv ]]; then pv=$(fetch_pluginval macos) || return 1; fi
  [[ -x $pv ]] || { echo "PLUGINVAL='$pv' is not executable"; return 1; }
  echo "pluginval: $pv"
  # One bundle after the other (strictness 10, in process), with pluginval's editor tests (open,
  # close, automation with the editor open; docs/architecture.md 9.6).
  while IFS= read -r name; do
    bundle=$(bundle_path "$b" "$name")
    echo "pluginval on: $bundle"
    "$pv" --strictness-level 10 --validate-in-process "$bundle" || return 1
  done < <(selected_bundles)
  step_mac_package "$b"
}

# The release packer on the Vintage bundle the `mac` step just built and validated (it runs on every
# full gate, so the packer cannot rot). Zips go to build/preflight/package (git-ignored), never into
# a tracked folder. B-Side is never packed: the Vintage bundle must not contain its strings.
step_mac_package() {
  local b=$1 ver zdir zipf bundle smoke
  ver=$(sed -n 's/^project(VintageSynth VERSION \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' CMakeLists.txt)
  [[ -n $ver ]] || { echo "cannot read the version from CMakeLists.txt"; return 1; }
  bundle=$(bundle_path "$b" "Exitium Vintage Synth.vst3")
  [[ -n $bundle ]] || { echo "no Vintage bundle under $b"; return 1; }
  zdir=$OUT/package
  rm -rf "$zdir" && mkdir -p "$zdir" || return 1
  zipf=$("$here/package-release.sh" pack --platform macos --bundle "$bundle" --version "$ver" \
    --commit "$(git rev-parse HEAD)" --out "$zdir") || return 1
  echo "zip: $zipf"; unzip -Z1 "$zipf" || return 1
  "$here/package-release.sh" verify --license "$repo/LICENSE" "$zipf" || return 1
  "$here/package-release.sh" selftest "$zipf" || return 1
  "$here/package-release.sh" sums "$zdir" && "$here/package-release.sh" sums "$zdir" --check || return 1
  # The same check CI runs on every platform: host_smoke against the bundle taken from the zip.
  smoke=$b/tests/host_smoke_artefacts/Release/host_smoke
  [[ -x $smoke ]] || { echo "host_smoke not found at $smoke"; return 1; }
  mkdir -p "$zdir/unpacked" && ditto -x -k "$zipf" "$zdir/unpacked" || return 1
  "$smoke" --profile vintage "$zdir/unpacked/Exitium Vintage Synth $ver/Exitium Vintage Synth.vst3"
}

step_mac_sanitize() {
  need cmake "brew install cmake ninja"; need ninja "brew install cmake ninja"
  local b=$OUT/mac-san san="-fsanitize=address,undefined -fno-omit-frame-pointer"
  # LeakSanitizer is not supported on macOS/arm64, so detect_leaks stays off here (the Linux job has it).
  export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  export ASAN_OPTIONS=strict_string_checks=1:detect_stack_use_after_return=1
  cmake -S . -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_COMPILE_WARNING_AS_ERROR=ON \
    -DCMAKE_C_FLAGS="$san" -DCMAKE_CXX_FLAGS="$san" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" || return 1
  # shellcheck source=sanitize-targets.sh
  . "$here/sanitize-targets.sh"
  san_select "$b" || return 1
  cmake --build "$b" --target "${SAN_TARGETS[@]}" -- -k 0 || return 1
  san_report "$b" || return 1
  # Every test executable (not the plugins): host_smoke and host_smoke_bside need the bundles and are not built here.
  run_ctest "$b" -E '^(host_smoke|host_smoke_bside)$'
}

step_linux() { need_docker; "$here/linux-check.sh" --tests "$TIER"; }
step_linux_sanitize() { need_docker; "$here/linux-check.sh" --sanitize --tests "$TIER"; }

# VST3 validator. Built once from the SDK commit pinned in build.yml (same steps and options as the CI
# job), cached by SHA outside the repository. It is run directly against the bundle the `mac` step
# built (rather than via -DVS_VST3_VALIDATOR in the mac configure) so that step's build tree stays
# the plain, CI-identical configuration; the pass criterion is the same as the ctest tests
# `vst3_validator` and `vst3_validator_bside`: exit 0 and "Result: N tests passed, 0 tests failed"
# with N >= 1, on every selected bundle.
step_validator() {
  need git "needed to fetch the VST3 SDK"; need cmake "brew install cmake ninja"; need ninja "brew install cmake ninja"
  local sha tag exe=${VST3_VALIDATOR:-}
  sha=$(yml_env VST3_SDK_SHA .github/workflows/build.yml); tag=$(yml_env VST3_SDK_TAG .github/workflows/build.yml)
  [[ ${#sha} -eq 40 && -n $tag ]] || die "could not read VST3_SDK_SHA / VST3_SDK_TAG from .github/workflows/build.yml"
  local root=$CACHE/vst3sdk-$sha
  if [[ -z $exe ]]; then
    exe=$root/validator
    if [[ ! -x $exe ]]; then
      note "building the VST3 validator from SDK $tag ($sha) into $root (once; cached afterwards)"
      mkdir -p "$root" || return 1
      rm -rf "$root/src" "$root/build"
      git init -q "$root/src" || return 1
      git -C "$root/src" remote add origin https://github.com/steinbergmedia/vst3sdk.git
      git -C "$root/src" fetch -q --depth 1 origin "$sha" || return 1
      git -C "$root/src" checkout -q --detach FETCH_HEAD || return 1
      [[ $(git -C "$root/src" rev-parse HEAD) == "$sha" ]] || { echo "fetched commit is not $sha"; return 1; }
      git -C "$root/src" submodule update -q --init --depth 1 base cmake pluginterfaces public.sdk || return 1
      cmake -S "$root/src" -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF \
        -DSMTG_ENABLE_VST3_HOSTING_EXAMPLES=OFF -DSMTG_ADD_VST3_UTILITIES=OFF \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 || return 1
      cmake --build "$root/build" --target validator || return 1
      local built; built=$(find "$root/build" -type f -name validator | head -n 1)
      [[ -n $built ]] || { echo "validator binary not found after the build"; return 1; }
      cp "$built" "$exe.tmp" && mv "$exe.tmp" "$exe"
    fi
  fi
  [[ -x $exe ]] || { echo "validator '$exe' is not executable"; return 1; }
  local name bundle log rc line
  while IFS= read -r name; do
    bundle=$(bundle_path "$OUT/mac" "$name")
    [[ -n $bundle ]] || { echo "no $name bundle from the mac step under $OUT/mac: run '--only mac' first (or '--only mac,validator')"; return 1; }
    echo "validator: $exe"; echo "bundle:    $bundle"
    log=$(mktemp "${TMPDIR:-/tmp}/vs-validator.XXXXXX")
    "$exe" "$bundle" 2>&1 | tee "$log"; rc=${PIPESTATUS[0]}
    line=$(grep -E '^Result: [1-9][0-9]* tests passed, 0 tests failed' "$log" | head -n 1); rm -f "$log"
    [[ $rc -eq 0 && -n $line ]] || { echo "validator failed on $name (exit $rc, no passing summary line)"; return 1; }
    echo "validator summary ($name): $line"
  done < <(selected_bundles)
}

# ---- run --------------------------------------------------------------------------------------
ensure_out_ignored
note "steps: ${run_steps[*]}$([[ $keep_going -eq 1 ]] && echo '  (keep going)')"
note "logs: $LOGS/<step>.log   build trees: $OUT   caches: $CACHE"
[[ -n $(git status --porcelain --untracked-files=no 2>/dev/null) ]] && note "note: the working tree has uncommitted changes (they are what gets checked)"
note "tests: tier=$TIER (${tier_args[*]:-every test}), $TEST_JOBS parallel jobs on this machine (the Linux steps use the container's core count; VS_TEST_JOBS overrides)"
note "machine load at start: $(uptime | sed 's/.*load averages*: //')"

names=() results=() durs=()
overall=0
t_all=$SECONDS
for s in "${run_steps[@]}"; do
  fn=step_${s//-/_}
  echo
  echo "=================== preflight: $s ==================="
  t0=$SECONDS
  if [[ $quiet -eq 1 ]]; then
    ( set -uo pipefail; "$fn" ) >"$LOGS/$s.log" 2>&1; rc=$?
    [[ $rc -eq 0 ]] || { echo "--- tail of $LOGS/$s.log"; tail -n 40 "$LOGS/$s.log"; }
  else
    ( set -uo pipefail; "$fn" ) 2>&1 | tee "$LOGS/$s.log"; rc=${PIPESTATUS[0]}
  fi
  d=$(( SECONDS - t0 ))
  names+=("$s"); durs+=("$d")
  if [[ $rc -eq 0 ]]; then results+=(PASS); else
    results+=("FAIL($rc)"); overall=1
    echo "preflight: step '$s' FAILED (exit $rc); log: $LOGS/$s.log" >&2
    if [[ $keep_going -eq 0 ]]; then
      for r in "${run_steps[@]}"; do
        done_it=0; for n in "${names[@]}"; do [[ $n == "$r" ]] && done_it=1; done
        if [[ $done_it -eq 0 ]]; then names+=("$r"); results+=("not run"); durs+=("-"); fi
      done
      break
    fi
  fi
done

fmt_dur() { [[ $1 == - ]] && echo - || printf '%dm%02ds' $(( $1 / 60 )) $(( $1 % 60 )); }
echo
echo "=================== preflight summary ==================="
# The tier and job count a step ran with: the marker line every ctest run prints ("-" if it ran no tests).
tests_of() {
  local m; m=$(sed -n 's/^.*test run: tier=\([a-z]*\) jobs=\([0-9]*\).*$/\1, \2 jobs/p' "$LOGS/$1.log" 2>/dev/null | head -n 1)
  echo "${m:--}"
}
printf '%-16s %-10s %-10s %s\n' STEP RESULT DURATION TESTS
for i in "${!names[@]}"; do printf '%-16s %-10s %-10s %s\n' "${names[$i]}" "${results[$i]}" "$(fmt_dur "${durs[$i]}")" "$([[ ${results[$i]} == "not run" ]] && echo - || tests_of "${names[$i]}")"; done
printf '%-16s %-10s %-10s %s\n' TOTAL "$([[ $overall -eq 0 ]] && echo PASS || echo FAIL)" "$(fmt_dur $(( SECONDS - t_all )))"
echo "test tier: $TIER ($([[ $TIER == smoke ]] && echo 'tests tagged [slow] not run: NOT the hand-over gate' || echo 'every test')); parallel jobs: $TEST_JOBS here, the container's own count in the Linux steps (VS_TEST_JOBS overrides)"
echo "machine load at end: $(uptime | sed 's/.*load averages*: //')"
case $PLUGINS in
  all) echo "plugins covered by pluginval and the validator: both (Exitium Vintage Synth, Exitium B-Side)" ;;
  *) echo "plugins covered by pluginval and the validator: ONLY $PLUGINS (VS_PREFLIGHT_PLUGINS): this is not the hand-over gate" ;;
esac
echo
echo "Confirmed by CI only (when CI is available): Windows x64/ARM64 (MSVC) and Linux x86_64."
echo "Confirm those with one CI dispatch: gh workflow run build.yml --ref <branch> -f targets=no-macos"
exit $overall
