#!/usr/bin/env bash
# Repository hygiene checks that need no build (the `docs` step of scripts/preflight.sh; also
# runnable alone). Reads the repository, writes nothing into it. Exit 0 = all pass.
#
#   1. No third-party brand names (project rule in CLAUDE.md), using the deny-list file
#      scripts/preflight/names.sha256, a list of hashes: the names are not stored in the repository.
#   2. No "Claude-Session:" trailer in the commit messages of the current branch (relative to main).
#   3. Every workflow YAML parses; trigger policy over every workflow (workflow_dispatch only, plus
#      the security.yml allow-list; self-tested by test-workflow-triggers.sh); the JSON row strings inside build.yml are valid JSON; and the
#      "plan" step of build.yml, run for every `targets` value, selects exactly the documented
#      rows and jobs (the table below), so the `if:` conditions and the docs cannot drift apart.
#   4. Source layout: source/common includes nothing from an instrument directory; the JUCE-free
#      directories include no JUCE header; no two files differ only by case; no Python bytecode
#      (`__pycache__/`, `*.pyc`, `*.pyo`) is tracked; no tracked file, text or binary, contains an
#      absolute home path (macOS, Windows or Linux user directory followed by a user name).
#   5. Shell scripts pass `bash -n` (and shellcheck at error severity, when it is installed).
#   6. The self-test of the pluginval teardown-crash wrapper (scripts/linux-check/pluginval-run-selftest.sh).
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
  for wf in .github/workflows/*.yml .github/workflows/*.yaml; do
    [[ -f $wf ]] || continue
    if ruby -ryaml -e 'YAML.load_file(ARGV[0])' "$wf" 2>"$tmp/err"; then
      ok "$wf parses as YAML"
    else
      fail "$wf does not parse: $(head -n 1 "$tmp/err")"
    fi
  done
  # Trigger policy over every workflow: workflow_dispatch only, except the allow-list for
  # security.yml (see test-workflow-triggers.sh). The self-test proves the check can fail.
  while IFS= read -r line; do
    case $line in
      ok\ *) ok "trigger policy: ${line#ok }" ;;
      fail\ *) fail "trigger policy: ${line#fail }" ;;
    esac
  done < <(scripts/preflight/test-workflow-triggers.sh check .github/workflows 2>&1)
  if scripts/preflight/test-workflow-triggers.sh >"$tmp/selftest" 2>&1; then
    ok "trigger policy self-test passes"
  else
    fail "trigger policy self-test: $(grep FAIL "$tmp/selftest" | head -n 2 | tr '\n' ' ')"
  fi
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
  fi
fi

# Permissions of build.yml (contract 10.2): the workflow default is `contents: read` and nothing else; only
# the `package` job may write, and then exactly `contents: write` (no other scope, no write-all).
if command -v ruby >/dev/null 2>&1; then
  perm_report=$(ruby -ryaml -e '
    y = YAML.load_file(".github/workflows/build.yml")
    bad = []
    top = y["permissions"]
    bad << "workflow default is #{top.inspect}, expected {contents: read}" unless top == { "contents" => "read" }
    y["jobs"].each do |name, j|
      p = j["permissions"]
      next if p.nil?
      if name == "package"
        bad << "package has #{p.inspect}, expected exactly {contents: write}" unless p == { "contents" => "write" }
      else
        bad << "job #{name} sets permissions #{p.inspect}" if p.is_a?(String) || p.values.include?("write")
      end
    end
    bad << "package job has no permissions block" unless (y["jobs"]["package"] || {})["permissions"]
    puts bad' 2>&1)
  if [[ -z $perm_report ]]; then
    ok "build.yml permissions: default read; only the package job has contents: write and nothing more"
  else
    fail "build.yml permissions: $perm_report"
  fi
fi

# resources/release/IDENTITY.txt (what the release packer expects of a bundle) must agree with CMakeLists.txt:
# the bundle identifier, and the manufacturer and plugin code that JUCE turns into the class IDs' last 8 bytes.
mcode=$(sed -n 's/^[[:space:]]*PLUGIN_MANUFACTURER_CODE[[:space:]]*\([A-Za-z0-9]*\).*/\1/p' CMakeLists.txt | head -n 1)
pcode=$(sed -n 's/^[[:space:]]*PLUGIN_CODE[[:space:]]*\([A-Za-z0-9]*\).*/\1/p' CMakeLists.txt | head -n 1)
bid=$(sed -n 's/^[[:space:]]*BUNDLE_ID[[:space:]]*"\([^"]*\)".*/\1/p' CMakeLists.txt | head -n 1)
hex=$(printf '%s%s' "$mcode" "$pcode" | od -An -tx1 | tr -d ' \n' | tr 'a-f' 'A-F')
id_ok=1
[[ -n $hex && $(sed -n 's/^bundle-id=//p' resources/release/IDENTITY.txt) == "$bid" ]] || id_ok=0
cid_n=0
while IFS= read -r c; do cid_n=$((cid_n + 1)); [[ ${c: -16} == "$hex" ]] || id_ok=0; done < <(sed -n 's/^cid=//p' resources/release/IDENTITY.txt)
[[ $cid_n -eq 2 ]] || id_ok=0
if [[ $id_ok -eq 1 ]]; then ok "resources/release/IDENTITY.txt agrees with CMakeLists.txt (bundle id $bid, plugin code $mcode$pcode)"
else fail "resources/release/IDENTITY.txt disagrees with CMakeLists.txt (bundle id '$bid', codes '$mcode$pcode' = $hex)"; fi

# 4. Source layout -----------------------------------------------------------------------------
# source/common/ is shared by every instrument and must not depend on one: no include of
# "vintage/...", "bside/..." (or any other instrument directory) from it. The pathspec covers every
# subdirectory, the shared processor skeleton source/common/shell/ included. The two instruments
# never include each other either (source/vintage <-> source/bside). The directories that are JUCE-free
# today stay so (common/engine too, except Oversampler.h, which wraps juce::dsp). No two tracked
# files may differ only by case (macOS is case-insensitive, Linux is not).
bad=$(git grep -n --untracked -E '#[[:space:]]*include[[:space:]]*[<"](vintage|bside|chip)/' -- 'source/common/*' || true)
if [[ -n $bad ]]; then
  fail "source/common includes an instrument directory:"; echo "$bad" >&2
else
  ok "source/common (incl. shell/) does not include anything from an instrument directory"
fi
bad=$(git grep -n --untracked -E '#[[:space:]]*include[[:space:]]*[<"]bside/' -- 'source/vintage/*' 'tests/dsp_unit/*' 'tests/integration/*' 'tests/preset_unit/*' || true)
bad2=$(git grep -n --untracked -E '#[[:space:]]*include[[:space:]]*[<"]vintage/' -- 'source/bside/*' 'tests/bside/*' || true)
if [[ -n $bad || -n $bad2 ]]; then
  fail "the two instruments include each other (Vintage <-> B-Side):"; echo "$bad$bad2" >&2
else
  ok "source/vintage and source/bside (and their tests) include nothing from each other"
fi
bad=$(git grep -n --untracked -iE '#[[:space:]]*include[[:space:]]*[<"]juce' -- \
        'source/common/dsp/*' 'source/common/fx/*' 'source/common/seq/*' 'source/vintage/dsp/*' \
        'source/common/engine/*' ':!source/common/engine/Oversampler.h' \
        'source/common/ui/models/*' 'source/common/shell/EditorState.h' \
        'source/bside/chip/*' 'source/bside/text/*' 'source/bside/program/*' 'source/bside/trace/*' || true)
if [[ -n $bad ]]; then
  fail "JUCE include in a directory that is JUCE-free by design:"; echo "$bad" >&2
else
  ok "the JUCE-free directories (common dsp/fx/seq/engine incl. UiFeed.h, ui/models, shell/EditorState.h, vintage dsp, bside chip/text/program/trace) include no JUCE header"
fi
dups=$(tr '\0' '\n' < "$tmp/files" | tr 'A-Z' 'a-z' | sort | uniq -d)
if [[ -n $dups ]]; then
  fail "file names that differ only by case: $(echo "$dups" | tr '\n' ' ')"
else
  ok "no two files differ only by case"
fi
# No Python bytecode under version control (it embeds the developer's absolute paths). The owner
# keeps `__pycache__/` in the git-ignore file; this is the gate behind it.
bad=$(git ls-files | grep -E '(^|/)__pycache__/|\.py[co]$' || true)
if [[ -n $bad ]]; then
  fail "Python bytecode is tracked:"; echo "$bad" >&2
else
  ok "no tracked Python bytecode (__pycache__, .pyc, .pyo)"
fi
# No absolute home path in any tracked file. One `git grep -a` pass covers text and binary files;
# LC_ALL=C keeps it byte-wise (in a UTF-8 locale git grep skips files with invalid sequences).
# External submodules are not searched (git grep does not descend into them). No exceptions are
# needed: no legitimate documentation placeholder uses such a path. The first letter of each
# directory name is a bracket class so that this script does not match its own pattern.
bad=$(LC_ALL=C git grep -a -l -E '/[U]sers/[^/[:space:]]+/|C:\\[U]sers\\[^\\[:space:]]+\\|/[h]ome/[^/[:space:]]+/' -- . || true)
if [[ -n $bad ]]; then
  fail "absolute home path in tracked files (a developer's machine path):"; echo "$bad" >&2
else
  ok "no absolute home paths (macOS, Windows, Linux user directories) in tracked files"
fi

# 5. Shell scripts -----------------------------------------------------------------------------
sh_n=0
while IFS= read -r -d '' f; do
  case $f in *.sh) ;; *) continue ;; esac
  sh_n=$((sh_n + 1))
  first_line=$(head -n 1 "$f")
  case $first_line in *bash*) sh_cmd=bash ;; *) sh_cmd="sh" ;; esac
  "$sh_cmd" -n "$f" 2>"$tmp/err" || fail "$sh_cmd -n $f: $(head -n 1 "$tmp/err")"
  if command -v shellcheck >/dev/null 2>&1; then
    shellcheck -S error "$f" >"$tmp/err" 2>&1 || fail "shellcheck $f: $(head -n 3 "$tmp/err" | tr '\n' ' ')"
  fi
done < "$tmp/files"
ok "$sh_n shell scripts pass the syntax check"

# 6. pluginval wrapper decision logic ----------------------------------------------------------
if sh scripts/linux-check/pluginval-run-selftest.sh >"$tmp/pv-selftest" 2>&1; then
  ok "pluginval-run.sh self-test ($(grep -c '^ok ' "$tmp/pv-selftest") cases)"
else
  fail "pluginval-run-selftest.sh: $(grep -E '^FAIL' "$tmp/pv-selftest" | head -n 3 | tr '\n' ' ')"
fi

if [[ $fails -gt 0 ]]; then
  echo "docs-check: $fails check(s) FAILED" >&2
  exit 1
fi
echo "docs-check: all checks passed"
