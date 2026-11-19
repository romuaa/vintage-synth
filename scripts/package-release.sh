#!/usr/bin/env bash
# Packs, verifies and checksums the release zips of Exitium Vintage Synth (docs/architecture.md
# section 10). One script for CI and for the local gate. Bash 3.2 compatible (macOS system bash).
#
#   package-release.sh pack --platform macos|windows|linux --bundle <path to .vst3> --version <v>
#                           --commit <40-hex sha> --out <dir> [--repository-url <url>]
#       checks the bundle, writes <dir>/Exitium-Vintage-Synth-<v>-<macOS|Windows|Linux>.zip and
#       prints its path
#   package-release.sh verify [--license <LICENSE file>] <zip>
#       unpacks into a temporary folder and checks everything from the zip alone; exit 1 with one
#       "verify: FAIL:" line per problem
#   package-release.sh sums <dir> [--check]
#       writes <dir>/SHA256SUMS.txt (one "<sha256>  <zip>" line per zip), or with --check verifies it
#   package-release.sh changelog <version> [CHANGELOG.md]
#       prints the text of the section "## [<version>]" (the release notes); exit 1 when the file has
#       no such section or it is empty
#   package-release.sh tag-check <version> <sha> | repo-public
#       gh checks of the release workflow (tag v<version> absent or at <sha>; repository public)
#   package-release.sh selftest <good zip>
#       proves that `verify` rejects a missing file, a wrong version, a missing slice and a
#       B-Side string (plus a metadata entry, an unreplaced placeholder and an escaping symlink)
#
# Tools: unzip, shasum or sha256sum, od, grep, sed, awk. macOS zips need ditto, lipo, codesign
# (so the macOS zip is packed on macOS); the Windows and Linux zips need zip (Info-ZIP) and can be
# packed on any host. No signing identity is used: the macOS bundle keeps the ad-hoc signature
# the build gave it (it is copied with ditto and packed with ditto, never re-signed).
set -euo pipefail

here=$(cd -P "$(dirname "$0")" && pwd)
repo=$(cd -P "$here/.." && pwd)
tpl=$repo/resources/release

PLUGIN="Exitium Vintage Synth"
BUNDLE="$PLUGIN.vst3"
DEFAULT_REPO_URL="https://github.com/romuaa/vintage-synth"
# What must never be in a Vintage release: the B-Side plugin code and bundle identifier (and name).
BSIDE_STRINGS=("Bsy1" "com.exitium.bside" "Exitium B-Side")
MAC_MIN_OS="11.0"

die() { echo "package-release: $*" >&2; exit 2; }
need() { command -v "$1" >/dev/null 2>&1 || die "'$1' not found: $2"; }

platform_label() {
  case $1 in macos) echo macOS ;; windows) echo Windows ;; linux) echo Linux ;; *) die "unknown platform '$1'" ;; esac
}

# ---- bundle checks (shared by pack and verify) ---------------------------------------------------
# Every problem is reported through `problem`; the caller decides what to do with the count.
problems=0
problem() { echo "verify: FAIL: $*" >&2; problems=$((problems + 1)); }

# The distinct "Version" values of moduleinfo.json (JUCE writes the project version for the package and
# for every class); a release needs exactly one, equal to the release version.
moduleinfo_version() {
  sed -n 's/^[[:space:]]*"Version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | LC_ALL=C sort -u | tr '\n' ' ' | sed 's/ $//'
}

# Machine type of a PE file (Windows): x64 or arm64. Prints "" when unknown.
pe_machine() {
  local f=$1 off sig mach
  off=$(od -An -tu4 -j60 -N4 "$f" 2>/dev/null | tr -d ' ')
  [[ -n $off ]] || return 0
  sig=$(od -An -c -j"$off" -N4 "$f" 2>/dev/null | tr -d ' ')
  [[ $sig == 'PE\0\0' ]] || return 0
  mach=$(od -An -tx1 -j$((off + 4)) -N2 "$f" | tr -d ' \n')
  case $mach in 6486) echo x64 ;; 64aa) echo arm64 ;; *) echo "0x$mach" ;; esac
}
# Machine type of an ELF file (Linux): x86_64 or aarch64.
elf_machine() {
  local f=$1 magic mach
  magic=$(od -An -tx1 -N4 "$f" 2>/dev/null | tr -d ' \n')
  [[ $magic == 7f454c46 ]] || return 0
  mach=$(od -An -tx1 -j18 -N2 "$f" | tr -d ' \n')
  case $mach in 3e00) echo x86_64 ;; b700) echo aarch64 ;; *) echo "0x$mach" ;; esac
}

# The files a bundle holds, relative to the .vst3 folder (one per line); `optional_files` may be there
# too; `binary_files` are the executables (mode 755 in the zip). Anything else is an error.
expected_files() {
  case $1 in
    macos) printf '%s\n' "Contents/Info.plist" "Contents/PkgInfo" "Contents/MacOS/$PLUGIN" \
             "Contents/Resources/moduleinfo.json" "Contents/_CodeSignature/CodeResources" ;;
    windows) printf '%s\n' "Contents/Resources/moduleinfo.json" "Contents/x86_64-win/$BUNDLE" "Contents/arm64-win/$BUNDLE" ;;
    linux) printf '%s\n' "Contents/Resources/moduleinfo.json" "Contents/x86_64-linux/$PLUGIN.so" "Contents/aarch64-linux/$PLUGIN.so" ;;
  esac
}
# JUCE writes a folder icon next to the Windows bundle's Contents (extras/Build/CMake/JUCEUtils.cmake).
optional_files() { case $1 in windows) printf '%s\n' "desktop.ini" "Plugin.ico" ;; esac; }
binary_files() {
  case $1 in
    macos) printf '%s\n' "Contents/MacOS/$PLUGIN" ;;
    windows) printf '%s\n' "Contents/x86_64-win/$BUNDLE" "Contents/arm64-win/$BUNDLE" ;;
    linux) printf '%s\n' "Contents/x86_64-linux/$PLUGIN.so" "Contents/aarch64-linux/$PLUGIN.so" ;;
  esac
}

# check_bundle <bundle> <platform> <version>
check_bundle() {
  local b=$1 plat=$2 ver=$3 info v f n bin archs a mach d cw rc
  [[ -d $b/Contents ]] || { problem "$BUNDLE: no Contents folder"; return 0; }
  cw=$(mktemp -d "${TMPDIR:-/tmp}/vs-bundle.XXXXXX")

  info=$b/Contents/Resources/moduleinfo.json
  if [[ ! -f $info ]]; then
    problem "$BUNDLE: Contents/Resources/moduleinfo.json is missing"
  else
    v=$(moduleinfo_version "$info")
    [[ $v == "$ver" ]] || problem "version mismatch: moduleinfo.json says '${v:-none}', the release is '$ver'"
  fi

  # Release bundles hold no symlinks, and exactly the files of the platform (no stray DLL).
  n=$(find "$b" -type l | sed -e "s|^$b/||" | head -n 3 | tr '\n' ' ' || true)
  [[ -z $n ]] || problem "symlink in the bundle: $n"
  (cd "$b" && find . \( -type f -o -type l \) | sed 's|^\./||' | LC_ALL=C sort) > "$cw/actual" || true
  { expected_files "$plat"; optional_files "$plat"; } | LC_ALL=C sort > "$cw/allowed"
  expected_files "$plat" | LC_ALL=C sort > "$cw/expected"
  n=$(LC_ALL=C comm -13 "$cw/allowed" "$cw/actual" | head -n 5 | tr '\n' ' ')
  [[ -z $n ]] || problem "unexpected file(s) in the bundle: $n"
  n=$(LC_ALL=C comm -23 "$cw/expected" "$cw/actual" | head -n 5 | tr '\n' ' ')
  [[ -z $n ]] || problem "file(s) missing from the bundle: $n"

  case $plat in
    macos)
      bin=$b/Contents/MacOS/$PLUGIN
      if [[ ! -f $bin ]]; then
        problem "missing binary: Contents/MacOS/$PLUGIN"
      elif ! command -v lipo >/dev/null 2>&1; then
        problem "lipo not found: cannot check the slices of the macOS bundle"
      else
        archs=$(lipo -archs "$bin" 2>/dev/null || true)
        for a in arm64 x86_64; do
          case " $archs " in *" $a "*) ;; *) problem "missing slice: $a (the binary has: ${archs:-none})" ;; esac
        done
        for a in $archs; do
          case $a in arm64|x86_64) ;; *) problem "unexpected slice: $a (only arm64 and x86_64 are released)" ;; esac
        done
        if command -v otool >/dev/null 2>&1; then
          # Every slice must be built for macOS 11 (the minimum in the documentation).
          for d in $(otool -l "$bin" | awk '/LC_BUILD_VERSION/ {f=1} f && $1=="minos" {print $2; f=0}' | sort -u); do
            [[ $d == "$MAC_MIN_OS" ]] || problem "minimum macOS of a slice is $d, expected $MAC_MIN_OS"
          done
        fi
      fi
      if command -v codesign >/dev/null 2>&1; then
        n=$(codesign --verify --deep --strict "$b" 2>&1) || problem "codesign --verify --deep --strict failed: $n"
      else
        problem "codesign not found: cannot verify the signature of the macOS bundle"
      fi
      ;;
    windows)
      for a in x86_64-win:x64 arm64-win:arm64; do
        d=${a%%:*}; mach=${a#*:}
        f=$b/Contents/$d/$BUNDLE
        n=$(find "$b/Contents/$d" -maxdepth 1 -type f -name '*.vst3' 2>/dev/null | wc -l | tr -d ' ' || true)
        if [[ $n -ne 1 ]]; then
          problem "missing slice: Contents/$d must hold exactly one .vst3 binary (found $n)"
        elif [[ -f $f ]]; then
          v=$(pe_machine "$f")
          [[ $v == "$mach" ]] || problem "Contents/$d: the binary is '${v:-not a PE file}', expected $mach"
        fi
      done
      ;;
    linux)
      for a in x86_64-linux:x86_64 aarch64-linux:aarch64; do
        d=${a%%:*}; mach=${a#*:}
        f=$b/Contents/$d/$PLUGIN.so
        n=$(find "$b/Contents/$d" -maxdepth 1 -type f -name '*.so' 2>/dev/null | wc -l | tr -d ' ' || true)
        if [[ $n -ne 1 ]]; then
          problem "missing slice: Contents/$d must hold exactly one .so binary (found $n)"
        elif [[ -f $f ]]; then
          v=$(elf_machine "$f")
          [[ $v == "$mach" ]] || problem "Contents/$d: the binary is '${v:-not an ELF file}', expected $mach"
        fi
      done
      ;;
  esac

  # B-Side must not be in a Vintage release: no file name and no byte of any file may carry its
  # strings, in any case. NUL bytes are removed first, so the UTF-16 forms (every other byte NUL)
  # are found too. A file that cannot be read or searched is a failure, never a pass.
  for a in "${BSIDE_STRINGS[@]}"; do
    n=$(find "$b" -iname "*$a*" | head -n 3 | sed -e "s|^$b/||" | tr '\n' ' ' || true)
    [[ -z $n ]] || problem "B-Side name '$a' in a file name: $n"
  done
  while IFS= read -r -d '' f; do
    if [[ ! -r $f ]]; then problem "cannot read ${f#"$b/"} (B-Side scan)"; continue; fi
    if ! LC_ALL=C tr -d '\0' < "$f" > "$cw/stripped" 2>/dev/null; then problem "cannot read ${f#"$b/"} (B-Side scan)"; continue; fi
    for a in "${BSIDE_STRINGS[@]}"; do
      rc=0; LC_ALL=C grep -aiqF -e "$a" "$cw/stripped" 2>/dev/null || rc=$?
      case $rc in
        0) problem "B-Side string '$a' found in: ${f#"$b/"}" ;;
        1) ;;
        *) problem "the B-Side scan of ${f#"$b/"} failed (grep exit $rc)" ;;
      esac
    done
  done < <(find "$b" -type f -print0)
  rm -rf "$cw"
}

# ---- text files ----------------------------------------------------------------------------------
# convert_eol <in> <out> <platform>: CRLF for Windows, LF otherwise (whatever the input had).
convert_eol() {
  if [[ $3 == windows ]]; then
    awk '{ sub(/\r$/, ""); printf "%s\r\n", $0 }' "$1" > "$2"
  else
    awk '{ sub(/\r$/, ""); print }' "$1" > "$2"
  fi
}
# render_text <template> <out> <platform> <version> <commit> <tag> <url>: placeholders replaced; the
# Windows files get CRLF line endings, the others LF.
render_text() {
  local src=$1 out=$2 plat=$3 ver=$4 commit=$5 tag=$6 url=$7 left
  [[ -f $src ]] || die "template missing: $src"
  # '|' is the sed delimiter: none of the values may contain it or a backslash/ampersand.
  # shellcheck disable=SC1003  # the backslash is meant literally
  case "$ver$commit$tag$url" in *'|'*|*'\'*|*'&'*) die "a placeholder value contains | \\ or &" ;; esac
  # A line holding only @SUBMODULES@ becomes the contents of $SUBMODULES_FILE (set by pack).
  awk -v f="${SUBMODULES_FILE:-/dev/null}" '$0 == "@SUBMODULES@" { while ((getline l < f) > 0) print l; close(f); next } { print }' "$src" > "$out.sub"
  sed -e "s|@VERSION@|$ver|g" -e "s|@COMMIT@|$commit|g" -e "s|@TAG@|$tag|g" -e "s|@REPOSITORY_URL@|$url|g" "$out.sub" > "$out.tmp"
  rm -f "$out.sub"
  left=$(grep -o '@[A-Z_][A-Z_]*@' "$out.tmp" | sort -u | tr '\n' ' ' || true)
  [[ -z $left ]] || die "unreplaced placeholder(s) in $src: $left"
  convert_eol "$out.tmp" "$out" "$plat"
  rm -f "$out.tmp"
}

sha256_of() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
  elif command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else die "neither shasum nor sha256sum found"; fi
}

# ---- zip helpers -----------------------------------------------------------------------------------
# zip_folder <platform> <parent dir> <folder name> <zip>: the folder becomes the zip's one top-level
# entry. macOS: ditto, which keeps symlinks and the signature; --norsrc and --noextattr stop it
# from adding "__MACOSX/._*" resource-fork entries (the default of `ditto -c -k` on a Mac), and
# --noqtn keeps a quarantine mark of the source out of the archive.
zip_folder() {
  local plat=$1 parent=$2 folder=$3 zipf=$4
  rm -f "$zipf"
  if [[ $plat == macos ]]; then
    need ditto "the macOS zip is packed with ditto (run this on macOS)"
    ditto -c -k --norsrc --noextattr --noqtn --keepParent "$parent/$folder" "$zipf"
  else
    need zip "install Info-ZIP zip"
    (cd "$parent" && zip -q -r -y -X "$zipf" "$folder")
  fi
}
# unzip_to <platform> <zip> <dir>
unzip_to() {
  local plat=$1 zipf=$2 dir=$3
  if [[ $plat == macos ]] && command -v ditto >/dev/null 2>&1; then
    ditto -x -k "$zipf" "$dir"
  else
    need unzip "install unzip"
    unzip -q -o "$zipf" -d "$dir"
  fi
}

# ---- pack ------------------------------------------------------------------------------------------
cmd_pack() {
  local plat="" bundle="" ver="" commit="" out="" url=$DEFAULT_REPO_URL
  while [[ $# -gt 0 ]]; do
    case $1 in
      --platform) [[ $# -ge 2 ]] || die "--platform needs a value"; plat=$2; shift 2 ;;
      --bundle) [[ $# -ge 2 ]] || die "--bundle needs a value"; bundle=$2; shift 2 ;;
      --version) [[ $# -ge 2 ]] || die "--version needs a value"; ver=$2; shift 2 ;;
      --commit) [[ $# -ge 2 ]] || die "--commit needs a value"; commit=$2; shift 2 ;;
      --out) [[ $# -ge 2 ]] || die "--out needs a value"; out=$2; shift 2 ;;
      --repository-url) [[ $# -ge 2 ]] || die "--repository-url needs a value"; url=$2; shift 2 ;;
      *) die "pack: unknown option '$1'" ;;
    esac
  done
  [[ -n $plat && -n $bundle && -n $ver && -n $commit && -n $out ]] \
    || die "pack needs --platform, --bundle, --version, --commit and --out"
  local label; label=$(platform_label "$plat")
  [[ $ver =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+.][0-9A-Za-z.-]+)?$ ]] || die "version '$ver' is not x.y.z"
  [[ $commit =~ ^[0-9a-f]{40}$ ]] || die "--commit must be a full 40-digit lower-case hash"
  [[ $(basename "$bundle") == "$BUNDLE" && -d $bundle ]] || die "--bundle must be the folder '$BUNDLE'"
  mkdir -p "$out" || die "cannot create $out"
  out=$(cd -P "$out" && pwd)

  problems=0
  check_bundle "$bundle" "$plat" "$ver"
  [[ $problems -eq 0 ]] || die "the bundle failed $problems check(s); nothing was packed"

  local tmp folder stage zipf
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/vs-package.XXXXXX")
  # shellcheck disable=SC2064
  trap "rm -rf '$tmp'" EXIT
  folder="$PLUGIN $ver"
  stage=$tmp/$folder
  mkdir -p "$stage"
  if [[ $plat == macos ]]; then
    need ditto "the macOS bundle is copied with ditto to keep its signature"
    ditto --norsrc --noextattr --noqtn "$bundle" "$stage/$BUNDLE"
  else
    cp -Rp "$bundle" "$stage/$BUNDLE"
  fi
  # Never ship editor or OS litter that happened to be in the build tree.
  find "$stage" \( -name '.DS_Store' -o -name '._*' -o -name '__MACOSX' \) -exec rm -rf {} + 2>/dev/null || true

  # The pinned submodules of the commit, for SOURCE.txt: path, upstream URL, commit.
  git -C "$repo" show "$commit:.gitmodules" > "$tmp/gitmodules" 2>/dev/null || die "commit $commit is not in the checkout (needed for the submodule list)"
  : > "$tmp/submodules.txt"
  local key sm_path sm_name sm_url sm_sha
  while read -r key sm_path; do
    sm_name=${key#submodule.}; sm_name=${sm_name%.path}
    sm_url=$(git config -f "$tmp/gitmodules" --get "submodule.$sm_name.url")
    sm_sha=$(git -C "$repo" ls-tree "$commit" "$sm_path" | awk '$2 == "commit" { print $3 }')
    [[ $sm_sha =~ ^[0-9a-f]{40}$ && -n $sm_url ]] || die "cannot read the pinned commit or URL of the submodule $sm_path"
    printf '  %s\n    %s\n    %s\n' "$sm_path" "$sm_url" "$sm_sha" >> "$tmp/submodules.txt"
  done < <(git config -f "$tmp/gitmodules" --get-regexp '^submodule\..*\.path$')
  [[ -s $tmp/submodules.txt ]] || die "no submodules found in .gitmodules at $commit"
  export SUBMODULES_FILE=$tmp/submodules.txt

  local tag=v$ver
  render_text "$tpl/INSTALL-$plat.txt" "$stage/INSTALL.txt" "$plat" "$ver" "$commit" "$tag" "$url"
  render_text "$tpl/SOURCE.txt" "$stage/SOURCE.txt" "$plat" "$ver" "$commit" "$tag" "$url"
  render_text "$tpl/THIRD-PARTY.txt" "$stage/THIRD-PARTY.txt" "$plat" "$ver" "$commit" "$tag" "$url"
  # The SIL Open Font License requires its text to accompany the font: appended in full.
  [[ -f $repo/resources/fonts/inter/OFL.txt ]] || die "resources/fonts/inter/OFL.txt is missing"
  {
    echo
    echo "-------------------------------------------------------------------------------"
    echo "Full text of the licence of the Inter typeface (resources/fonts/inter/OFL.txt)"
    echo "-------------------------------------------------------------------------------"
    echo
    cat "$repo/resources/fonts/inter/OFL.txt"
  } > "$tmp/ofl-part.txt"
  convert_eol "$tmp/ofl-part.txt" "$tmp/ofl-part.out" "$plat"
  cat "$tmp/ofl-part.out" >> "$stage/THIRD-PARTY.txt"
  [[ -f $repo/LICENSE ]] || die "LICENSE is missing"
  convert_eol "$repo/LICENSE" "$stage/LICENSE.txt" "$plat"

  # upload-artifact drops file modes: set them explicitly (755 folders and binaries, 644 files).
  # codesign does not seal modes, so the macOS signature stays valid (verify runs codesign again).
  find "$stage" -type d -exec chmod 755 {} +
  find "$stage" -type f -exec chmod 644 {} +
  local rel
  while IFS= read -r rel; do chmod 755 "$stage/$BUNDLE/$rel"; done < <(binary_files "$plat")

  zipf=$out/Exitium-Vintage-Synth-$ver-$label.zip
  zip_folder "$plat" "$tmp" "$folder" "$zipf"
  [[ -s $zipf ]] || die "the zip was not written"
  echo "$zipf"
}

# ---- verify ------------------------------------------------------------------------------------------
# The most the unpacked zip may hold (bytes). The real zips unpack to under 100 MB.
MAX_UNPACKED=${VS_PACKAGE_MAX_BYTES:-209715200}

cmd_verify() {
  local license=""
  while [[ $# -gt 0 ]]; do
    case $1 in
      --license) [[ $# -ge 2 ]] || die "--license needs a file"; license=$2; shift 2 ;;
      -*) die "verify: unknown option '$1'" ;;
      *) break ;;
    esac
  done
  [[ $# -eq 1 ]] || die "usage: verify [--license <LICENSE file>] <zip>"
  local zipf=$1 name plat label ver folder tmp top n f t unsafe=0
  [[ -f $zipf ]] || die "no such file: $zipf"
  zipf=$(cd -P "$(dirname "$zipf")" && pwd)/$(basename "$zipf")
  name=$(basename "$zipf")
  need unzip "needed to list and unpack the zip"
  problems=0

  # Name: Exitium-Vintage-Synth-<version>-<macOS|Windows|Linux>.zip
  if [[ $name =~ ^Exitium-Vintage-Synth-(.+)-(macOS|Windows|Linux)\.zip$ ]]; then
    ver=${BASH_REMATCH[1]}; label=${BASH_REMATCH[2]}
  else
    die "the file name '$name' is not Exitium-Vintage-Synth-<version>-<macOS|Windows|Linux>.zip"
  fi
  case $label in macOS) plat=macos ;; Windows) plat=windows ;; Linux) plat=linux ;; esac
  folder="$PLUGIN $ver"

  tmp=$(mktemp -d "${TMPDIR:-/tmp}/vs-verify.XXXXXX")
  # shellcheck disable=SC2064
  trap "rm -rf '$tmp'" EXIT

  # ---- the zip's directory (nothing is unpacked before it is safe) ----
  unzip -Z1 "$zipf" > "$tmp/list" 2>"$tmp/err" || die "cannot read the zip: $(head -n 1 "$tmp/err")"
  unzip -tq "$zipf" >/dev/null 2>"$tmp/err" || problem "the zip is damaged: $(head -n 1 "$tmp/err")"
  # perms|os|name of every entry, as recorded by the packer
  unzip -Z "$zipf" 2>/dev/null | awk '$2 ~ /^[0-9]+\.[0-9]+$/ && NF >= 9 { n = $0; sub(/^([^ ]+ +){8}/, "", n); print $1 "\t" $3 "\t" n }' > "$tmp/modes" || true
  if [[ ! -s $tmp/list ]]; then problem "the zip is empty"; unsafe=1; fi

  # Total size before unpacking.
  n=$(unzip -Z -t "$zipf" 2>/dev/null | awk '/bytes uncompressed/ { for (i = 1; i <= NF; i++) if ($i == "bytes") { print $(i - 1); exit } }')
  if [[ -z $n ]]; then problem "cannot read the total size of the zip"; unsafe=1
  elif [[ $n -gt $MAX_UNPACKED ]]; then problem "the zip unpacks to $n bytes, above the limit of $MAX_UNPACKED"; unsafe=1; fi

  n=$(grep -c -E '(^|/)(__MACOSX|\.DS_Store|\._[^/]*)(/|$)' "$tmp/list" || true)
  [[ $n -eq 0 ]] || problem "the zip holds $n macOS metadata entries (__MACOSX, .DS_Store, ._*): $(grep -E '(^|/)(__MACOSX|\.DS_Store|\._[^/]*)(/|$)' "$tmp/list" | head -n 3 | tr '\n' ' ')"
  n=$(grep -c -E '(^/|(^|/)\.\.(/|$))' "$tmp/list" || true)
  if [[ $n -ne 0 ]]; then problem "the zip holds $n entries with an absolute or '..' path"; unsafe=1; fi
  top=$(sed 's|/.*||' "$tmp/list" | sort -u)
  [[ $top == "$folder" ]] || problem "the zip must have the one top-level folder '$folder' (found: $(echo "$top" | tr '\n' '|'))"

  # Duplicate entries, also ones that differ only by case (they collide on macOS and Windows).
  sed 's|/$||' "$tmp/list" | LC_ALL=C sort | uniq -d > "$tmp/dups"
  if [[ -s $tmp/dups ]]; then problem "duplicate entries in the zip: $(head -n 3 "$tmp/dups" | tr '\n' '|')"; unsafe=1; fi
  sed 's|/$||' "$tmp/list" | tr '[:upper:]' '[:lower:]' | LC_ALL=C sort | uniq -d > "$tmp/dups"
  if [[ -s $tmp/dups ]]; then problem "entries that differ only by letter case: $(head -n 3 "$tmp/dups" | tr '\n' '|')"; unsafe=1; fi

  # No symlink at all: a release holds none (the macOS bundle was checked: it has none).
  n=$(awk -F'\t' '$1 ~ /^l/ { print $3 }' "$tmp/modes" | head -n 3 | tr '\n' '|')
  if [[ -n $n ]]; then problem "symlink entries in the zip (none are allowed): $n"; unsafe=1; fi
  n=$(awk -F'\t' '$1 !~ /^[-d][-rwxsStT]+$/ { print $3 }' "$tmp/modes" | head -n 3 | tr '\n' '|')
  if [[ -n $n ]]; then problem "entries that are neither files nor folders: $n"; unsafe=1; fi

  if [[ $unsafe -ne 0 ]]; then return_summary; fi

  # ---- the modes recorded in the zip: 755 for folders and binaries, 644 for the rest ----
  local want bin_list NL=$'\n'
  bin_list=$(binary_files "$plat" | sed "s|^|$folder/$BUNDLE/|")
  while IFS=$'\t' read -r perms os ename; do
    [[ -n $ename ]] || continue
    if [[ $os != unx ]]; then problem "no Unix permissions recorded for $ename (made on '$os')"; continue; fi
    case $perms in
      d*) want=drwxr-xr-x ;;
      *) case "$NL$bin_list$NL" in *"$NL$ename$NL"*) want=-rwxr-xr-x ;; *) want=-rw-r--r-- ;; esac ;;
    esac
    [[ $perms == "$want" ]] || problem "mode of $ename is $perms, expected $want"
  done < "$tmp/modes"

  unzip_to "$plat" "$zipf" "$tmp/x" </dev/null 2>"$tmp/err" || { problem "cannot unpack the zip: $(head -n 1 "$tmp/err")"; return_summary; }
  local root=$tmp/x/$folder
  [[ -d $root ]] || { problem "no folder '$folder' after unpacking"; return_summary; }
  # Whatever the recorded modes were, make every file readable so that nothing escapes the scan.
  chmod -R u+rwX "$tmp/x"
  n=$(find "$tmp/x" -type l | head -n 1)
  [[ -z $n ]] || problem "a symlink was unpacked: ${n#"$tmp/x/"}"

  # ---- the text files ----
  for t in INSTALL.txt LICENSE.txt SOURCE.txt THIRD-PARTY.txt; do
    [[ -s $root/$t ]] || problem "missing file: $t (or it is empty)"
  done
  for t in INSTALL.txt SOURCE.txt THIRD-PARTY.txt; do
    [[ -f $root/$t ]] || continue
    n=$(grep -o '@[A-Z_][A-Z_]*@' "$root/$t" | sort -u | tr '\n' ' ' || true)
    [[ -z $n ]] || problem "unreplaced placeholder(s) in $t: $n"
  done
  for t in INSTALL.txt LICENSE.txt SOURCE.txt THIRD-PARTY.txt; do
    [[ -f $root/$t ]] || continue
    if [[ $plat == windows ]]; then
      n=$(grep -c -v $'\r$' "$root/$t" || true)
      [[ $n -eq 0 ]] || problem "$t: $n line(s) without CRLF (Windows text files use CRLF)"
    else
      n=$(grep -c $'\r' "$root/$t" || true)
      [[ $n -eq 0 ]] || problem "$t: contains CR characters (LF line endings expected on this platform)"
    fi
  done
  # Content: each text must be the right text, not just any text.
  text_needs() { # <file> <min lines> <fixed strings...>
    local file=$1 min=$2 lines s; shift 2
    [[ -f $root/$file ]] || return 0
    lines=$(grep -c '' "$root/$file" || true)
    [[ $lines -ge $min ]] || problem "$file has $lines lines, at least $min expected"
    for s in "$@"; do grep -qF -- "$s" "$root/$file" || problem "$file does not contain '$s'"; done
  }
  if [[ -f $root/INSTALL.txt ]]; then
    f=$(head -n 1 "$root/INSTALL.txt" | tr -d '\r')
    [[ $f == "Exitium Vintage Synth $ver for $label" ]] || problem "INSTALL.txt is not the one for $label (first line: '$f')"
    # shellcheck disable=SC2088  # the tilde is literal text of the install notes
    case $plat in
      macos) text_needs INSTALL.txt 25 "com.apple.quarantine" '~/Library/Audio/Plug-Ins/VST3' ;;
      windows) text_needs INSTALL.txt 25 "Unblock-File" 'Common Files\VST3' "experimental" ;;
      linux) text_needs INSTALL.txt 25 '~/.vst3' "glibc 2.35" "experimental" ;;
    esac
  fi
  text_needs SOURCE.txt 15 "Release tag: v$ver" "git submodule update --init --recursive" "external/JUCE" "external/Catch2"
  if [[ -f $root/SOURCE.txt ]]; then
    grep -Eq '[^0-9a-f]([0-9a-f]{40})([^0-9a-f]|$)' "$root/SOURCE.txt" || problem "SOURCE.txt holds no full commit hash"
    grep -Eq 'https?://' "$root/SOURCE.txt" || problem "SOURCE.txt holds no repository URL"
  fi
  text_needs THIRD-PARTY.txt 500 "JUCE 9" "Inter" "zlib" "libpng" "HarfBuzz" "VST3 SDK" "SIL OPEN FONT LICENSE Version 1.1"
  text_needs LICENSE.txt 600 "GNU AFFERO GENERAL PUBLIC LICENSE"
  if [[ -n $license && -f $root/LICENSE.txt ]]; then
    [[ -f $license ]] || die "--license: no such file: $license"
    tr -d '\r' < "$license" > "$tmp/lic.expected"; tr -d '\r' < "$root/LICENSE.txt" > "$tmp/lic.actual"
    cmp -s "$tmp/lic.expected" "$tmp/lic.actual" || problem "LICENSE.txt differs from $license"
  fi
  # Nothing else at the top level.
  n=$(find "$root" -mindepth 1 -maxdepth 1 ! -name "$BUNDLE" ! -name INSTALL.txt ! -name LICENSE.txt ! -name SOURCE.txt ! -name THIRD-PARTY.txt -exec basename {} \; | tr '\n' ' ')
  [[ -z $n ]] || problem "unexpected top-level entries in the folder: $n"

  if [[ -d $root/$BUNDLE ]]; then
    check_bundle "$root/$BUNDLE" "$plat" "$ver"
  else
    problem "missing file: $BUNDLE"
  fi
  return_summary
}
return_summary() {
  if [[ $problems -gt 0 ]]; then
    echo "verify: $problems problem(s): the zip is NOT acceptable" >&2
    exit 1
  fi
  echo "verify: ok"
  exit 0
}

# ---- sums ----------------------------------------------------------------------------------------------
cmd_sums() {
  local dir="" check=0
  while [[ $# -gt 0 ]]; do
    case $1 in --check) check=1 ;; *) [[ -z $dir ]] || die "sums: one directory only"; dir=$1 ;; esac
    shift
  done
  [[ -n $dir && -d $dir ]] || die "usage: sums <dir> [--check]"
  dir=$(cd -P "$dir" && pwd)
  local z
  if [[ $check -eq 1 ]]; then
    [[ -f $dir/SHA256SUMS.txt ]] || die "no SHA256SUMS.txt in $dir"
    local bad=0 listed=0 line h n
    while IFS= read -r line; do
      [[ -n $line ]] || continue
      h=${line%% *}; n=${line#*  }
      listed=$((listed + 1))
      [[ -f $dir/$n ]] || { echo "sums: $n is listed but missing" >&2; bad=1; continue; }
      if [[ $(sha256_of "$dir/$n") == "$h" ]]; then echo "$n: OK"; else echo "sums: $n: checksum mismatch" >&2; bad=1; fi
    done < "$dir/SHA256SUMS.txt"
    for z in "$dir"/*.zip; do
      [[ -e $z ]] || continue
      grep -q "  $(basename "$z")\$" "$dir/SHA256SUMS.txt" || { echo "sums: $(basename "$z") is not listed" >&2; bad=1; }
    done
    [[ $listed -gt 0 && $bad -eq 0 ]] || exit 1
    return 0
  fi
  : > "$dir/SHA256SUMS.txt.tmp"
  local count=0
  for z in "$dir"/*.zip; do
    [[ -e $z ]] || continue
    printf '%s  %s\n' "$(sha256_of "$z")" "$(basename "$z")" >> "$dir/SHA256SUMS.txt.tmp"
    count=$((count + 1))
  done
  [[ $count -gt 0 ]] || { rm -f "$dir/SHA256SUMS.txt.tmp"; die "no .zip in $dir"; }
  mv "$dir/SHA256SUMS.txt.tmp" "$dir/SHA256SUMS.txt"
  cat "$dir/SHA256SUMS.txt"
}

# ---- changelog ---------------------------------------------------------------------------------------------
# The release notes of a version: the lines between "## [<version>]..." and the next "## " heading.
cmd_changelog() {
  [[ $# -ge 1 && $# -le 2 ]] || die "usage: changelog <version> [CHANGELOG.md]"
  local ver=$1 file=${2:-$repo/CHANGELOG.md} text
  [[ -f $file ]] || die "no such file: $file"
  text=$(awk -v h="## [$ver]" 'index($0, h) == 1 { f = 1; next } /^## / && f { exit } f { print }' "$file")
  if [[ -z $(printf '%s' "$text" | tr -d '[:space:]') ]]; then
    echo "package-release: $file has no section '## [$ver]' with text" >&2
    exit 1
  fi
  printf '%s\n' "$text"
}

# ---- gh checks (tag and visibility), used by the release workflow --------------------------------------
# tag-check <version> <sha>: the tag v<version> must not exist, or must point at <sha> (also through an
# annotated tag). Needs gh and GITHUB_REPOSITORY. Exit 1 otherwise, also for any API error but 404.
cmd_tag_check() {
  [[ $# -eq 2 ]] || die "usage: tag-check <version> <sha>"
  [[ -n ${GITHUB_REPOSITORY:-} ]] || die "GITHUB_REPOSITORY is not set"
  local ver=$1 sha=$2 out err rc=0 type obj
  err=$(mktemp "${TMPDIR:-/tmp}/vs-tagcheck.XXXXXX")
  out=$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/v$ver" --jq '.object.type + " " + .object.sha' 2>"$err") || rc=$?
  if [[ $rc -ne 0 ]]; then
    if grep -qF 'HTTP 404' "$err"; then rm -f "$err"; echo "tag v$ver does not exist: ok"; return 0; fi
    echo "tag-check: the API call failed: $(head -n 2 "$err")" >&2; rm -f "$err"; exit 1
  fi
  rm -f "$err"
  type=${out%% *}; obj=${out#* }
  if [[ $type == tag ]]; then obj=$(gh api "repos/$GITHUB_REPOSITORY/git/tags/$obj" --jq .object.sha) || exit 1; fi
  [[ $obj == "$sha" ]] || { echo "tag-check: the tag v$ver already exists and points at $obj, not at $sha" >&2; exit 1; }
  echo "tag v$ver exists and points at $sha: ok"
}
# repo-public: the repository must be public (SOURCE.txt promises a reachable source).
cmd_repo_public() {
  [[ -n ${GITHUB_REPOSITORY:-} ]] || die "GITHUB_REPOSITORY is not set"
  local p
  p=$(gh api "repos/$GITHUB_REPOSITORY" --jq .private) || exit 1
  [[ $p == false ]] || { echo "repo-public: $GITHUB_REPOSITORY is not public (private=$p): its source cannot be reached" >&2; exit 1; }
  echo "$GITHUB_REPOSITORY is public: ok"
}

# ---- selftest --------------------------------------------------------------------------------------------
# Rebuilds bad zips from a good one and checks that `verify` rejects each with its own message.
cmd_selftest() {
  [[ $# -eq 1 && -f $1 ]] || die "usage: selftest <good zip>"
  local good name label plat ver folder tmp
  good=$(cd -P "$(dirname "$1")" && pwd)/$(basename "$1")
  name=$(basename "$good")
  if [[ $name =~ ^Exitium-Vintage-Synth-(.+)-(macOS|Windows|Linux)\.zip$ ]]; then ver=${BASH_REMATCH[1]}; label=${BASH_REMATCH[2]}
  else die "the file name '$name' is not a release zip name"; fi
  case $label in macOS) plat=macos ;; Windows) plat=windows ;; Linux) plat=linux ;; esac
  folder="$PLUGIN $ver"
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/vs-selftest.XXXXXX")
  # shellcheck disable=SC2064
  trap "rm -rf '$tmp'" EXIT

  local fails=0 rc=0 out
  ok() { echo "selftest: ok: $*"; }
  bad() { echo "selftest: FAIL: $*"; fails=$((fails + 1)); }

  # The CHANGELOG check of the release workflow, on fixtures.
  printf '# Changelog\n\n## [Unreleased]\n- later\n\n## [1.2.3] - 2030-01-01\n### Added\n- a thing\n\n## [1.2.30]\n- other\n\n## [1.2.4]\n\n## [1.2.5]\n   \n' > "$tmp/CHANGELOG.md"
  local want
  for want in "1.2.3:0" "1.2.30:0" "1.2.4:1" "1.2.5:1" "1.2.:1" "9.9.9:1" "Unreleased:0"; do
    if "$0" changelog "${want%%:*}" "$tmp/CHANGELOG.md" >"$tmp/cl.out" 2>/dev/null; then rc=0; else rc=1; fi
    if [[ $rc -eq ${want#*:} ]]; then ok "changelog section '${want%%:*}' -> exit $rc"; else bad "changelog section '${want%%:*}' gave exit $rc"; fi
  done
  out=$("$0" changelog 1.2.3 "$tmp/CHANGELOG.md" 2>/dev/null || true)
  case $out in
    *'- a thing'*) case $out in *'- other'*) bad "the notes of 1.2.3 hold the next section" ;; *) ok "the notes of 1.2.3 are exactly its section" ;; esac ;;
    *) bad "the notes of 1.2.3 lack their text" ;;
  esac

  # The gh checks, against a stub gh.
  mkdir -p "$tmp/bin"
  cat > "$tmp/bin/gh" <<'STUB'
#!/bin/sh
# stub: GH_STUB_MODE selects the answer
case "$GH_STUB_MODE:$*" in
  404:*git/ref/tags*) echo "gh: Not Found (HTTP 404)" >&2; exit 1 ;;
  boom:*git/ref/tags*) echo "gh: Server Error (HTTP 500)" >&2; exit 1 ;;
  same:*git/ref/tags*) echo "commit aaaa" ;;
  other:*git/ref/tags*) echo "commit bbbb" ;;
  annotated:*git/ref/tags*) echo "tag tttt" ;;
  annotated:*git/tags/tttt*) echo "aaaa" ;;
  public:*) echo "false" ;;
  private:*) echo "true" ;;
esac
STUB
  chmod +x "$tmp/bin/gh"
  local m exp
  for want in "404:0" "same:0" "annotated:0" "other:1" "boom:1"; do
    m=${want%%:*}; exp=${want#*:}; rc=0
    PATH=$tmp/bin:$PATH GH_STUB_MODE=$m GITHUB_REPOSITORY=a/b "$0" tag-check 0.1.0 aaaa >/dev/null 2>&1 || rc=$?
    if [[ $rc -eq $exp ]]; then ok "tag-check ($m) -> exit $rc"; else bad "tag-check ($m) gave exit $rc, expected $exp"; fi
  done
  for want in "public:0" "private:1"; do
    m=${want%%:*}; exp=${want#*:}; rc=0
    PATH=$tmp/bin:$PATH GH_STUB_MODE=$m GITHUB_REPOSITORY=a/b "$0" repo-public >/dev/null 2>&1 || rc=$?
    if [[ $rc -eq $exp ]]; then ok "repo-public ($m) -> exit $rc"; else bad "repo-public ($m) gave exit $rc, expected $exp"; fi
  done

  # run_verify <zip>: output in $tmp/out, status in $rc
  run_verify() { rc=0; "$0" verify --license "$repo/LICENSE" "$1" >"$tmp/out" 2>&1 </dev/null || rc=$?; }
  expect_reject() { # <case name> <zip> <message pattern>
    run_verify "$2"
    if [[ $rc -ne 0 ]] && grep -qF -- "$3" "$tmp/out"; then
      ok "$1 rejected ($(grep -m1 -F -- "$3" "$tmp/out" | sed 's/^verify: FAIL: //' | cut -c1-110))"
    else
      bad "$1 was not rejected as expected (exit $rc, wanted '$3')"; sed 's/^/    /' "$tmp/out"
    fi
  }
  # mutate_tree <case>: unpack the good zip into $tmp/<case>/ and echo that tree's folder
  mutate_tree() { rm -rf "${tmp:?}/${1:?}"; mkdir -p "$tmp/$1"; unzip_to "$plat" "$good" "$tmp/$1"; echo "$tmp/$1/$folder"; }
  rezip() { zip_folder "$plat" "$tmp/$1" "$folder" "$tmp/$1/$name"; echo "$tmp/$1/$name"; }
  # The binary of the platform (first one), relative to the folder.
  local binrel; binrel="$BUNDLE/$(binary_files "$plat" | head -n 1)"

  run_verify "$good"
  if [[ $rc -eq 0 ]]; then ok "the good zip is accepted"; else bad "the good zip is rejected"; cat "$tmp/out"; fi

  local d other z
  d=$(mutate_tree missing-file); rm -f "${d:?}/SOURCE.txt"
  expect_reject "missing file (SOURCE.txt)" "$(rezip missing-file)" 'missing file: SOURCE.txt'

  d=$(mutate_tree wrong-version)
  sed -i.bak "s/\"Version\"[[:space:]]*:[[:space:]]*\"$ver\"/\"Version\": \"9.9.9\"/" "$d/$BUNDLE/Contents/Resources/moduleinfo.json"
  rm -f "${d:?}/$BUNDLE/Contents/Resources/moduleinfo.json.bak"
  expect_reject "wrong version (moduleinfo.json 9.9.9)" "$(rezip wrong-version)" 'version mismatch'

  d=$(mutate_tree missing-slice)
  case $plat in
    macos)
      lipo -remove x86_64 "$d/$BUNDLE/Contents/MacOS/$PLUGIN" -output "$d/$BUNDLE/Contents/MacOS/$PLUGIN.thin"
      mv "$d/$BUNDLE/Contents/MacOS/$PLUGIN.thin" "$d/$BUNDLE/Contents/MacOS/$PLUGIN"; chmod 755 "$d/$BUNDLE/Contents/MacOS/$PLUGIN" ;;
    windows) rm -rf "${d:?}/$BUNDLE/Contents/arm64-win" ;;
    linux) rm -rf "${d:?}/$BUNDLE/Contents/aarch64-linux" ;;
  esac
  expect_reject "missing slice" "$(rezip missing-slice)" 'missing slice'

  d=$(mutate_tree bside)
  printf 'plugin code Bsy1\n' > "$d/$BUNDLE/Contents/Resources/extra.txt"
  expect_reject "B-Side string" "$(rezip bside)" "B-Side string 'Bsy1'"

  # The same identifiers in UTF-16LE and in other letter case, inside a binary-looking file.
  d=$(mutate_tree bside-utf16)
  { head -c 64 /dev/zero; printf 'c\0o\0m\0.\0e\0x\0i\0t\0i\0u\0m\0.\0b\0s\0i\0d\0e\0'; } > "$d/$BUNDLE/Contents/Resources/blob.bin"
  expect_reject "B-Side string in UTF-16LE" "$(rezip bside-utf16)" "B-Side string 'com.exitium.bside'"
  d=$(mutate_tree bside-case)
  printf 'EXITIUM B-SIDE\n' > "$d/$BUNDLE/Contents/Resources/Notes.txt"
  expect_reject "B-Side string in capitals" "$(rezip bside-case)" "B-Side string 'Exitium B-Side'"
  d=$(mutate_tree bside-name)
  printf 'x\n' > "$d/$BUNDLE/Contents/Resources/exitium b-side.txt"
  expect_reject "B-Side name in a file name" "$(rezip bside-name)" "B-Side name"

  d=$(mutate_tree metadata); mkdir -p "$d/../__MACOSX"; echo x > "$d/../__MACOSX/._junk"
  rm -f "${tmp:?}/metadata/$name"; (cd "$tmp/metadata" && zip -q -r -y -X "$tmp/metadata/$name" "$folder" __MACOSX)
  expect_reject "macOS metadata entries" "$tmp/metadata/$name" 'metadata entries'

  d=$(mutate_tree placeholder); printf '\nsee @REPOSITORY_URL@\n' >> "$d/INSTALL.txt"
  expect_reject "unreplaced placeholder" "$(rezip placeholder)" 'unreplaced placeholder'

  # Symlinks: every one is refused, also one that stays inside and one that leaves through "." and "..".
  d=$(mutate_tree symlink); ln -s ../../outside "$d/escape"
  expect_reject "symlink outside the folder" "$(rezip symlink)" 'symlink entries'
  d=$(mutate_tree symlink-in); ln -s INSTALL.txt "$d/inside"
  expect_reject "symlink inside the folder" "$(rezip symlink-in)" 'symlink entries'
  d=$(mutate_tree symlink-dot); ln -s . "$d/$BUNDLE/Contents/Resources/a"; ln -s a/../../../.. "$d/$BUNDLE/Contents/Resources/b"
  expect_reject "symlinks through . and .." "$(rezip symlink-dot)" 'symlink entries'

  # Extra file in the bundle (a stray DLL), wrong text content, wrong platform text, modified licence.
  d=$(mutate_tree stray); printf 'MZ' > "$d/$BUNDLE/Contents/Resources/evil.dll"
  expect_reject "stray file in the bundle" "$(rezip stray)" 'unexpected file(s) in the bundle'
  d=$(mutate_tree short-install); printf '%s\n' "$ver" > "$d/INSTALL.txt"
  expect_reject "INSTALL.txt with almost no content" "$(rezip short-install)" 'INSTALL.txt'
  d=$(mutate_tree other-install)
  case $plat in macos) other=Windows ;; *) other=macOS ;; esac
  { printf 'Exitium Vintage Synth %s for %s\n' "$ver" "$other"; tail -n +2 "$d/INSTALL.txt"; } > "$d/INSTALL.txt.new"; mv "$d/INSTALL.txt.new" "$d/INSTALL.txt"
  expect_reject "INSTALL.txt of another platform" "$(rezip other-install)" 'is not the one for'
  d=$(mutate_tree license); echo "extra line" >> "$d/LICENSE.txt"
  expect_reject "modified LICENSE.txt" "$(rezip license)" 'LICENSE.txt differs'
  d=$(mutate_tree source-short); printf 'Release tag: v%s\n' "$ver" > "$d/SOURCE.txt"
  expect_reject "SOURCE.txt with almost no content" "$(rezip source-short)" 'SOURCE.txt'

  # Modes: a binary without the executable bit (what upload-artifact leaves), and the packer's repair of it.
  d=$(mutate_tree mode644); chmod 644 "$d/$binrel"
  expect_reject "binary with mode 644" "$(rezip mode644)" "expected -rwxr-xr-x"
  d=$(mutate_tree mode-text); chmod 755 "$d/INSTALL.txt"
  expect_reject "text file with mode 755" "$(rezip mode-text)" "expected -rw-r--r--"
  local head_sha; head_sha=$(git -C "$repo" rev-parse HEAD 2>/dev/null || true)
  if [[ -n $head_sha ]]; then
    d=$(mutate_tree repack)
    chmod 644 "$d/$binrel"
    if z=$("$0" pack --platform "$plat" --bundle "$d/$BUNDLE" --version "$ver" --commit "$head_sha" --out "$tmp/repack-out" 2>"$tmp/pack.err"); then
      run_verify "$z"
      if [[ $rc -eq 0 ]]; then ok "pack repairs a 644 binary (the repacked zip verifies, incl. codesign on macOS)"; else bad "the repacked zip is rejected"; cat "$tmp/out"; fi
    else bad "pack failed on the 644 bundle: $(cat "$tmp/pack.err")"; fi
  else
    echo "selftest: skip: pack repair of modes (not inside a git checkout)"
  fi

  # Size limit before unpacking.
  rc=0; VS_PACKAGE_MAX_BYTES=1000 "$0" verify "$good" >"$tmp/out" 2>&1 || rc=$?
  if [[ $rc -ne 0 ]] && grep -qF 'above the limit' "$tmp/out"; then ok "a zip above the size limit is refused before unpacking"; else bad "the size limit did not stop the zip"; fi

  # Crafted zips (need python3): duplicate entries, case-colliding entries, an unreadable (mode 000) file with B-Side inside.
  if command -v python3 >/dev/null 2>&1; then
    for want in dup casedup unreadable; do
      mkdir -p "$tmp/py-$want"
      python3 -I - "$good" "$tmp/py-$want/$name" "$want" <<'PY'
import sys, zipfile, warnings
warnings.simplefilter('ignore')
good, out, case = sys.argv[1:4]
zin = zipfile.ZipFile(good)
zo = zipfile.ZipFile(out, 'w')
top = zin.infolist()[0].filename.split('/')[0]
for i in zin.infolist():
    zo.writestr(i, zin.read(i.filename))
def info(n, mode):
    zi = zipfile.ZipInfo(n, date_time=(2026, 1, 1, 0, 0, 0)); zi.create_system = 3
    zi.external_attr = (mode & 0xFFFF) << 16; return zi
if case == 'dup':
    zo.writestr(info(top + '/INSTALL.txt', 0o100644), b'second copy\n')
elif case == 'casedup':
    zo.writestr(info(top + '/install.txt', 0o100644), b'other case\n')
else:
    zo.writestr(info(top + '/Exitium Vintage Synth.vst3/Contents/Resources/hidden.txt', 0o100000), b'Bsy1')
zo.close()
PY
    done
    expect_reject "duplicate entries" "$tmp/py-dup/$name" 'duplicate entries'
    expect_reject "entries differing only by case" "$tmp/py-casedup/$name" 'differ only by letter case'
    expect_reject "B-Side string in a file of mode 000" "$tmp/py-unreadable/$name" "B-Side string 'Bsy1'"
  else
    echo "selftest: skip: duplicate, case-colliding and mode-000 entries (python3 not found)"
  fi

  # An extra architecture slice (macOS): a third entry in the universal binary's header.
  if [[ $plat == macos ]] && command -v python3 >/dev/null 2>&1; then
    d=$(mutate_tree extra-slice)
    python3 -I - "$d/$binrel" <<'PY'
import sys, struct
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
magic, n = struct.unpack('>II', b[:8])
assert magic == 0xcafebabe and n == 2
al = 14
off = (len(b) + (1 << al) - 1) & ~((1 << al) - 1)
b += b'\0' * (off - len(b))
sl = struct.pack('<IiiIIIII', 0xfeedface, 7, 3, 8, 0, 0, 0, 0)  # an empty i386 Mach-O as a third slice
b += sl
b[48:68] = struct.pack('>5I', 7, 3, off, len(sl), al)
b[4:8] = struct.pack('>I', 3)
open(p, 'wb').write(b)
PY
    expect_reject "extra architecture slice" "$(rezip extra-slice)" 'unexpected slice'
  fi

  if [[ $fails -gt 0 ]]; then echo "selftest: $fails check(s) FAILED" >&2; exit 1; fi
  echo "selftest: all checks passed"
}

# ---- main ----------------------------------------------------------------------------------------------------
[[ $# -ge 1 ]] || { sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2; }
cmd=$1; shift
case $cmd in
  pack) cmd_pack "$@" ;;
  verify) cmd_verify "$@" ;;
  sums) cmd_sums "$@" ;;
  changelog) cmd_changelog "$@" ;;
  tag-check) cmd_tag_check "$@" ;;
  repo-public) cmd_repo_public ;;
  selftest) cmd_selftest "$@" ;;
  -h|--help|help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//' ;;
  *) die "unknown command '$cmd' (pack, verify, sums, changelog, tag-check, repo-public, selftest)" ;;
esac
