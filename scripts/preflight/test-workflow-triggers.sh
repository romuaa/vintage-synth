#!/usr/bin/env bash
# Workflow trigger policy (owner decision: GitHub Actions never runs costly work by itself).
#
#   test-workflow-triggers.sh check DIR   apply the policy to every *.yml / *.yaml in DIR; prints one
#                                         "ok <file>: ..." or "fail <file>: ..." line per workflow;
#                                         exit 1 if any failed (used by docs-check.sh)
#   test-workflow-triggers.sh             self-test: runs the policy on fixture workflows in a
#                                         temporary directory and asserts pass/fail for each
#
# Policy: every workflow may trigger on workflow_dispatch only. Sole exception, the allow-list
# below: security.yml may have push (branches exactly [main]), pull_request, schedule (one cron,
# at most weekly) and workflow_dispatch. pull_request_target and workflow_run always fail: they run
# with write permission and secrets on input from forks.
set -uo pipefail

read -r -d '' POLICY_RB <<'RUBY'
require "yaml"
dir = ARGV[0]
ALLOWED_EXTRA = { "security.yml" => %w[push pull_request schedule workflow_dispatch] }
bad = 0
files = Dir.glob(File.join(dir, "*.{yml,yaml}")).sort
puts "ok (none): no workflow files in #{dir}" if files.empty?
files.each do |f|
  name = File.basename(f)
  begin
    y = YAML.load_file(f)
    raise "top level is not a mapping" unless y.is_a?(Hash)
    raise "no 'on:' trigger section" unless y.key?("on") || y.key?(true)
    on = y.key?("on") ? y["on"] : y[true]   # YAML 1.1 loads the key `on` as boolean true
    cfg = case on
          when String then { on => nil }
          when Array then on.each_with_object({}) { |t, h| raise "non-string trigger in list" unless t.is_a?(String); h[t] = nil }
          when Hash then on.transform_keys(&:to_s)
          else raise "'on:' is empty or has an unsupported form"
          end
    raise "'on:' lists no trigger" if cfg.empty?
  rescue StandardError, Psych::Exception => e
    puts "fail #{name}: cannot check triggers: #{e.message.lines.first.to_s.strip}"
    bad += 1
    next
  end
  trig = cfg.keys
  errs = []
  (trig & %w[pull_request_target workflow_run]).each do |t|
    errs << "trigger #{t} (never allowed: it runs with write permission and secrets on input from forks)"
  end
  allowed = ALLOWED_EXTRA[name] || %w[workflow_dispatch]
  (trig - allowed - %w[pull_request_target workflow_run]).each do |t|
    errs << "trigger #{t} (policy: #{allowed == %w[workflow_dispatch] ? 'workflow_dispatch only' : "only #{allowed.join(', ')} in #{name}"})"
  end
  if name == "security.yml" && cfg.key?("push")
    p = cfg["push"]
    br = p.is_a?(Hash) ? p["branches"] : nil
    br = [br] if br.is_a?(String)
    other = p.is_a?(Hash) ? p.keys - ["branches"] : []
    errs << "push must have branches exactly [main] and nothing else (no tags, paths, other branches)" unless br == ["main"] && other.empty?
  end
  if name == "security.yml" && cfg.key?("schedule")
    s = cfg["schedule"]
    crons = s.is_a?(Array) ? s.map { |e| e.is_a?(Hash) ? e["cron"] : nil } : []
    if crons.size != 1 || !crons[0].is_a?(String)
      errs << "schedule must have exactly one cron entry"
    else
      m, h, dom, mon, dow = crons[0].split
      int = ->(v) { v =~ /\A\d+\z/ }
      weekly = int.(m) && int.(h) && dom == "*" && mon == "*" && int.(dow)
      monthly = int.(m) && int.(h) && int.(dom) && dow == "*" && (mon == "*" || int.(mon))
      errs << "schedule cron '#{crons[0]}' is more often than weekly" unless weekly || monthly
    end
  end
  if errs.empty?
    puts "ok #{name}: triggers #{trig.join(', ')}#{allowed == %w[workflow_dispatch] ? ' (workflow_dispatch only)' : ' (allow-listed)'}"
  else
    puts "fail #{name}: #{errs.join('; ')}"
    bad += 1
  end
end
exit(bad.zero? ? 0 : 1)
RUBY

check_dir() { ruby -e "$POLICY_RB" "$1"; }

if [[ ${1:-} == check ]]; then
  [[ -d ${2:-} ]] || { echo "usage: $0 check DIR" >&2; exit 2; }
  check_dir "$2"
  exit $?
fi

# ---- self-test ----
t=$(mktemp -d "${TMPDIR:-/tmp}/vs-trig-test.XXXXXX")
trap 'rm -rf "$t"' EXIT
fails=0
# expect NAME pass|fail FILENAME  (content on stdin)
expect() {
  local label=$1 want=$2 file=$3 d
  d=$(mktemp -d "$t/case.XXXXXX")
  cat > "$d/$file"
  local out rc
  out=$(check_dir "$d" 2>&1); rc=$?
  if [[ $want == pass && $rc -eq 0 ]] || [[ $want == fail && $rc -ne 0 ]]; then
    echo "workflow-triggers self-test: ok: $label ($want)"
  else
    echo "workflow-triggers self-test: FAIL: $label expected $want, got rc=$rc: $out" >&2
    fails=$((fails + 1))
  fi
}
# An empty directory is fine.
d=$(mktemp -d "$t/empty.XXXXXX"); check_dir "$d" >/dev/null && echo "workflow-triggers self-test: ok: empty directory (pass)" \
  || { echo "workflow-triggers self-test: FAIL: empty directory" >&2; fails=$((fails + 1)); }

expect "dispatch-only build" pass build.yml <<'Y'
name: b
on:
  workflow_dispatch:
    inputs: {x: {type: string}}
jobs: {}
Y
expect "allowed security shape" pass security.yml <<'Y'
name: s
on:
  push:
    branches: [main]
  pull_request:
  schedule:
    - cron: "17 4 * * 1"
  workflow_dispatch:
jobs: {}
Y
expect "push on a build workflow" fail build.yml <<'Y'
on:
  push:
    branches: [main]
  workflow_dispatch:
Y
expect "pull_request_target" fail build.yml <<'Y'
on:
  pull_request_target:
Y
expect "pull_request_target in security" fail security.yml <<'Y'
on:
  push:
    branches: [main]
  pull_request_target:
Y
expect "string form 'on: push'" fail ci.yml <<'Y'
on: push
jobs: {}
Y
expect "list form [push, workflow_dispatch]" fail ci.yaml <<'Y'
on: [push, workflow_dispatch]
jobs: {}
Y
expect "string form 'on: workflow_dispatch'" pass ok.yml <<'Y'
on: workflow_dispatch
jobs: {}
Y
expect "workflow_run" fail chain.yml <<'Y'
on:
  workflow_run:
    workflows: [x]
Y
expect "security with a tag push" fail security.yml <<'Y'
on:
  push:
    branches: [main]
    tags: ["v*"]
Y
expect "security push to other branches" fail security.yml <<'Y'
on:
  push:
    branches: [main, dev]
Y
expect "security push without branches" fail security.yml <<'Y'
on: [push]
Y
expect "security with a daily cron" fail security.yml <<'Y'
on:
  schedule:
    - cron: "17 4 * * *"
Y
expect "security with two crons" fail security.yml <<'Y'
on:
  schedule:
    - cron: "17 4 * * 1"
    - cron: "17 4 * * 3"
Y
expect "security with an extra trigger" fail security.yml <<'Y'
on:
  issues:
Y
expect "unparsable YAML" fail broken.yml <<'Y'
on: [push
  : : {
Y
expect "no 'on:' section" fail none.yml <<'Y'
name: x
jobs: {}
Y

if [[ $fails -gt 0 ]]; then
  echo "workflow-triggers self-test: $fails case(s) FAILED" >&2
  exit 1
fi
echo "workflow-triggers self-test: all cases passed"
