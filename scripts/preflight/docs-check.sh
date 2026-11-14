#!/usr/bin/env bash
# Repository hygiene checks that need no build (the `docs` step of scripts/preflight.sh; also
# runnable alone). Reads the repository, writes nothing into it. Exit 0 = all pass.
#
#   1. No third-party brand names (project rule in CLAUDE.md), using the deny-list file
#      scripts/preflight/names.sha256, a list of hashes: the names are not stored in the repository.
#   2. No "Claude-Session:" trailer in the commit messages of the current branch (relative to main).
#   3. Every workflow YAML parses; the JSON row strings inside build.yml are valid JSON; and the
#      "plan" step of build.yml, run for every `targets` value, selects exactly the documented
#      rows and jobs (the table below), so the `if:` conditions and the docs cannot drift apart.
#   4. Source layout: source/common includes nothing from an instrument directory; the JUCE-free
#      directories include no JUCE header; no two files differ only by case.
#   5. Shell scripts pass `bash -n` (and shellcheck at error severity, when it is installed).
set -uo pipefail

here=$(cd -P "$(dirname "$0")" && pwd)
repo=$(cd -P "$here/../.." && pwd)
cd "$repo" || exit 2
fails=0
fail() { echo "docs-check: FAIL: $*" >&2; fails=$((fails + 1)); }
ok() { echo "docs-check: ok: $*"; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/vs-docs-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

# Files under version control (tracked + new, not ignored), minus submodules and deleted files.
git ls-files -z --cached --others --exclude-standard \
  | while IFS= read -r -d '' f; do
      case $f in external/*) continue ;; esac
      [[ -f $f ]] && printf '%s\0' "$f"
    done > "$tmp/files"

# 1. Brand names -------------------------------------------------------------------------------
if python3 scripts/preflight/name-check.py --check < "$tmp/files"; then
  ok "no third-party brand or model names (hashed list: scripts/preflight/names.sha256)"
else
  fail "third-party brand or model names found (project rule; see the lines above)"
fi

# 2. Commit trailers ---------------------------------------------------------------------------
base=""
for c in main origin/main; do git rev-parse --verify -q "$c" >/dev/null && { base=$c; break; }; done
if [[ -z $base ]]; then
  echo "docs-check: skip: no 'main' branch to compare the commit messages against"
else
  bad_c=$(git log "$base"..HEAD --format='%h' --grep='^Claude-Session:' || true)
  if [[ -n $bad_c ]]; then
    fail "commits with a Claude-Session: line since $base: $(echo "$bad_c" | tr '\n' ' ')"
  else
    ok "no Claude-Session: trailers in $(git rev-list --count "$base"..HEAD) commit(s) since $base"
  fi
fi

# 3. Workflows ---------------------------------------------------------------------------------
if ! command -v ruby >/dev/null 2>&1; then
  echo "docs-check: skip: ruby not found (needed to parse the workflow YAML)"
else
  for wf in .github/workflows/*.yml; do
    if ruby -ryaml -e 'YAML.load_file(ARGV[0])' "$wf" 2>"$tmp/err"; then
      ok "$wf parses as YAML"
    else
      fail "$wf does not parse: $(head -n 1 "$tmp/err")"
    fi
  done
  # JSON row strings: every '{...}' single-quoted shell literal that starts with {"name" in build.yml.
  n=0
  while IFS= read -r row; do
    n=$((n + 1))
    printf '%s' "$row" | ruby -rjson -e 'JSON.parse(STDIN.read)' 2>/dev/null || fail "invalid JSON row in build.yml: $row"
  done < <(grep -o "'{\"name\":[^']*}'" .github/workflows/build.yml | sed "s/^'//; s/'\$//")
  [[ $n -ge 5 ]] && ok "$n JSON matrix rows in build.yml are valid JSON" || fail "expected at least 5 JSON rows in build.yml, found $n"

  # Run the plan step for every target and compare with the documented table.
  ruby -ryaml -e '
    y = YAML.load_file(".github/workflows/build.yml")
    step = y["jobs"]["plan"]["steps"].find { |s| s["id"] == "plan" }
    File.write(ARGV[0], step["run"])' "$tmp/plan.sh" 2>/dev/null
  if [[ ! -s $tmp/plan.sh ]]; then
    fail "build.yml has no job 'plan' with a step id 'plan'"
  elif ! command -v jq >/dev/null 2>&1; then
    echo "docs-check: skip: jq not found (needed to run the CI plan step)"
  else
    # target | rows | distros sanitizers windows_bundle linux_bundle   (the documented table)
    while IFS='|' read -r target rows flags; do
      : > "$tmp/out"; : > "$tmp/summary"
      if ! TARGETS=$target GITHUB_OUTPUT=$tmp/out GITHUB_STEP_SUMMARY=$tmp/summary GITHUB_SHA=x GITHUB_REF_NAME=y \
           bash "$tmp/plan.sh" >/dev/null 2>"$tmp/err"; then
        fail "plan step failed for targets=$target: $(head -n 1 "$tmp/err")"; continue
      fi
      got_rows=$(sed -n 's/^matrix=//p' "$tmp/out" | jq -r 'map(.name) | join(",")' 2>/dev/null)
      got_flags=$(grep -v '^matrix=' "$tmp/out" | sed 's/^[a-z_]*=//' | tr '\n' ' ' | sed 's/ $//')
      if [[ $got_rows == "$rows" && $got_flags == "$flags" ]]; then
        ok "plan targets=$target -> $rows | $flags"
      else
        fail "plan targets=$target: got '$got_rows' | '$got_flags', documented '$rows' | '$flags'"
      fi
    done <<'TABLE'
windows|windows-x64,windows-arm64|false false true false
linux-x64|linux-x64|true false false false
linux|linux-x64,linux-arm64|true true false true
no-macos|windows-x64,windows-arm64,linux-x64,linux-arm64|true true true true
all|macos-universal,windows-x64,windows-arm64,linux-x64,linux-arm64|true true true true
TABLE
    # The flag order above is: distros sanitizers windows_bundle linux_bundle (order of the echo lines).
    # The workflow must not trigger on anything but a manual dispatch (policy: nothing automatic).
    trig=$(ruby -ryaml -e 'y = YAML.load_file(".github/workflows/build.yml"); puts((y["on"] || y[true]).keys.join(","))')
    [[ $trig == workflow_dispatch ]] && ok "build.yml triggers: workflow_dispatch only" \
      || fail "build.yml triggers are '$trig' (policy: workflow_dispatch only)"
  fi
fi

# 4. Source layout -----------------------------------------------------------------------------
# source/common/ is shared by every instrument and must not depend on one: no include of
# "vintage/...", "bside/..." (or any other instrument directory) from it. The pathspec covers every
# subdirectory, the shared processor skeleton source/common/shell/ included. The directories that are JUCE-free
# today stay so (common/engine too, except Oversampler.h, which wraps juce::dsp). No two tracked
# files may differ only by case (macOS is case-insensitive, Linux is not).
bad=$(git grep -n --untracked -E '#[[:space:]]*include[[:space:]]*[<"](vintage|bside|chip)/' -- 'source/common/*' || true)
if [[ -n $bad ]]; then
  fail "source/common includes an instrument directory:"; echo "$bad" >&2
else
  ok "source/common (incl. shell/) does not include anything from an instrument directory"
fi
bad=$(git grep -n --untracked -iE '#[[:space:]]*include[[:space:]]*[<"]juce' -- \
        'source/common/dsp/*' 'source/common/fx/*' 'source/common/seq/*' 'source/vintage/dsp/*' \
        'source/common/engine/*' ':!source/common/engine/Oversampler.h' || true)
if [[ -n $bad ]]; then
  fail "JUCE include in a directory that is JUCE-free by design:"; echo "$bad" >&2
else
  ok "the JUCE-free directories (common dsp/fx/seq/engine, vintage dsp) include no JUCE header"
fi
dups=$(tr '\0' '\n' < "$tmp/files" | tr 'A-Z' 'a-z' | sort | uniq -d)
if [[ -n $dups ]]; then
  fail "file names that differ only by case: $(echo "$dups" | tr '\n' ' ')"
else
  ok "no two files differ only by case"
fi

# 5. Shell scripts -----------------------------------------------------------------------------
sh_n=0
while IFS= read -r -d '' f; do
  case $f in *.sh) ;; *) continue ;; esac
  sh_n=$((sh_n + 1))
  head -n 1 "$f" | grep -q 'bash' && sh_cmd=bash || sh_cmd=sh
  "$sh_cmd" -n "$f" 2>"$tmp/err" || fail "$sh_cmd -n $f: $(head -n 1 "$tmp/err")"
  if command -v shellcheck >/dev/null 2>&1; then
    shellcheck -S error "$f" >"$tmp/err" 2>&1 || fail "shellcheck $f: $(head -n 3 "$tmp/err" | tr '\n' ' ')"
  fi
done < "$tmp/files"
ok "$sh_n shell scripts pass the syntax check"

if [[ $fails -gt 0 ]]; then
  echo "docs-check: $fails check(s) FAILED" >&2
  exit 1
fi
echo "docs-check: all checks passed"
