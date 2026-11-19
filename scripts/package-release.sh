#!/usr/bin/env bash
# Packs, verifies and checksums the release zips of Exitium Vintage Synth (docs/architecture.md
# section 10). One script for CI and for the local gate. Bash 3.2 compatible (macOS system bash).
#
#   package-release.sh pack --platform macos|windows|linux --bundle <path to .vst3> --version <v>
#                           --commit <40-hex sha> --out <dir> [--repository-url <url>]
#       checks the bundle, writes <dir>/Exitium-Vintage-Synth-<v>-<macOS|Windows|Linux>.zip and
#       prints its path
#   package-release.sh verify <zip>
#       unpacks into a temporary folder and checks everything from the zip alone; exit 1 with one
#       "verify: FAIL:" line per problem
#   package-release.sh sums <dir> [--check]
#       writes <dir>/SHA256SUMS.txt (one "<sha256>  <zip>" line per zip), or with --check verifies it
#   package-release.sh changelog <version> [CHANGELOG.md]
#       prints the text of the section "## [<version>]" (the release notes); exit 1 when the file has
#       no such section or it is empty
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

# The "Version" of the plugin in moduleinfo.json (JUCE writes the project version there).
moduleinfo_version() {
  # The first "Version" at the top level: the file lists "Version" for the package and again for
  # each class; the package one comes first.
  sed -n 's/^[[:space:]]*"Version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -n 1
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

# check_bundle <bundle> <platform> <version>
check_bundle() {
  local b=$1 plat=$2 ver=$3 info v f n bin archs a mach d
  [[ -d $b/Contents ]] || { problem "$BUNDLE: no Contents folder"; return 0; }

  info=$b/Contents/Resources/moduleinfo.json
  if [[ ! -f $info ]]; then
    problem "$BUNDLE: Contents/Resources/moduleinfo.json is missing"
  else
    v=$(moduleinfo_version "$info")
    [[ $v == "$ver" ]] || problem "version mismatch: moduleinfo.json says '${v:-none}', the release is '$ver'"
  fi

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
        n=$(find "$b/Contents/$d" -maxdepth 1 -type f -name '*.vst3' 2>/dev/null | wc -l | tr -d ' ' || true)
        if [[ $n -ne 1 ]]; then
          problem "missing slice: Contents/$d must hold exactly one .vst3 binary (found $n)"
        else
          f=$(find "$b/Contents/$d" -maxdepth 1 -type f -name '*.vst3')
          v=$(pe_machine "$f")
          [[ $v == "$mach" ]] || problem "Contents/$d: the binary is '${v:-not a PE file}', expected $mach"
        fi
      done
      ;;
    linux)
      for a in x86_64-linux:x86_64 aarch64-linux:aarch64; do
        d=${a%%:*}; mach=${a#*:}
        n=$(find "$b/Contents/$d" -maxdepth 1 -type f -name '*.so' 2>/dev/null | wc -l | tr -d ' ' || true)
        if [[ $n -ne 1 ]]; then
          problem "missing slice: Contents/$d must hold exactly one .so binary (found $n)"
        else
          f=$(find "$b/Contents/$d" -maxdepth 1 -type f -name '*.so')
          v=$(elf_machine "$f")
          [[ $v == "$mach" ]] || problem "Contents/$d: the binary is '${v:-not an ELF file}', expected $mach"
        fi
      done
      ;;
  esac

  # B-Side must not be in a Vintage release: no byte of any file may contain its strings, and no
  # file or folder name may carry them.
  for a in "${BSIDE_STRINGS[@]}"; do
    n=$(grep -rlaF -e "$a" "$b" 2>/dev/null | head -n 3 | sed -e "s|^$b/||" | tr '\n' ' ' || true)
    [[ -z $n ]] || problem "B-Side string '$a' found in: $n"
    n=$(find "$b" -name "*$a*" 2>/dev/null | head -n 3 | sed -e "s|^$b/||" | tr '\n' ' ' || true)
    [[ -z $n ]] || problem "B-Side name '$a' in a file name: $n"
  done
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
  case "$ver$commit$tag$url" in *'|'*|*'\'*|*'&'*) die "a placeholder value contains | \\ or &" ;; esac
  sed -e "s|@VERSION@|$ver|g" -e "s|@COMMIT@|$commit|g" -e "s|@TAG@|$tag|g" -e "s|@REPOSITORY_URL@|$url|g" "$src" > "$out.tmp"
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
    unzip -q "$zipf" -d "$dir"
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

  zipf=$out/Exitium-Vintage-Synth-$ver-$label.zip
  zip_folder "$plat" "$tmp" "$folder" "$zipf"
  [[ -s $zipf ]] || die "the zip was not written"
  echo "$zipf"
}

# lexical_depth <relative path below the top folder>: "outside" when the path climbs above the folder
# (no file system access), else "inside".
lexical_depth() {
  local p=$1 depth=0 part old_ifs=$IFS
  IFS=/
  # shellcheck disable=SC2086
  set -f; set -- $p; set +f
  IFS=$old_ifs
  for part in "$@"; do
    case $part in
      ''|.) ;;
      ..) depth=$((depth - 1)); [[ $depth -ge 0 ]] || { echo outside; return 0; } ;;
      *) depth=$((depth + 1)) ;;
    esac
  done
  echo inside
}

# ---- verify ------------------------------------------------------------------------------------------
cmd_verify() {
  [[ $# -eq 1 ]] || die "usage: verify <zip>"
  local zipf=$1 name base plat label ver folder tmp list top n f
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

  # Entry names, from the zip's directory.
  unzip -Z1 "$zipf" > "$tmp/list" 2>"$tmp/err" || die "cannot read the zip: $(head -n 1 "$tmp/err")"
  unzip -tq "$zipf" >/dev/null 2>"$tmp/err" || problem "the zip is damaged: $(head -n 1 "$tmp/err")"
  if [[ ! -s $tmp/list ]]; then problem "the zip is empty"; fi
  n=$(grep -c -E '(^|/)(__MACOSX|\.DS_Store|\._[^/]*)(/|$)' "$tmp/list" || true)
  [[ $n -eq 0 ]] || problem "the zip holds $n macOS metadata entries (__MACOSX, .DS_Store, ._*): $(grep -E '(^|/)(__MACOSX|\.DS_Store|\._[^/]*)(/|$)' "$tmp/list" | head -n 3 | tr '\n' ' ')"
  n=$(grep -c -E '(^/|(^|/)\.\.(/|$))' "$tmp/list" || true)
  [[ $n -eq 0 ]] || problem "the zip holds $n entries with an absolute or '..' path"
  top=$(sed 's|/.*||' "$tmp/list" | sort -u)
  [[ $top == "$folder" ]] || problem "the zip must have the one top-level folder '$folder' (found: $(echo "$top" | tr '\n' '|'))"

  # Symlinks, from the zip's directory: the target is the entry's content. None may be absolute or leave
  # the folder (checked before unpacking: unzip itself refuses some of them).
  unzip -Z "$zipf" 2>/dev/null | awk '$1 ~ /^l/ { sub(/^([^ ]+ +){8}/, ""); print }' > "$tmp/links" || true
  while IFS= read -r f; do
    [[ -n $f ]] || continue
    n=$(unzip -p "$zipf" "$f" 2>/dev/null || true)
    case $n in
      /*) problem "symlink with an absolute target: $f -> $n" ;;
      *) if [[ $(lexical_depth "$(dirname "${f#*/}")/$n") == outside ]]; then problem "symlink pointing outside the folder: $f -> $n"; fi ;;
    esac
  done < "$tmp/links"

  unzip_to "$plat" "$zipf" "$tmp/x" 2>"$tmp/err" || { problem "cannot unpack the zip: $(head -n 1 "$tmp/err")"; return_summary; }
  local root=$tmp/x/$folder
  [[ -d $root ]] || { problem "no folder '$folder' after unpacking"; return_summary; }

  # Symlinks: none may leave the folder (or dangle).
  local rootp; rootp=$(cd -P "$root" && pwd)
  while IFS= read -r f; do
    [[ -n $f ]] || continue
    local tdir; tdir=$(cd -P "$(dirname "$f")" 2>/dev/null && cd -P "$(dirname "$(readlink "$f")")" 2>/dev/null && pwd || true)
    case $(readlink "$f") in
      /*) problem "symlink with an absolute target: ${f#"$tmp/x/"} -> $(readlink "$f")" ;;
      *) case "$tdir" in
           "$rootp"|"$rootp"/*) [[ -e $f ]] || problem "dangling symlink: ${f#"$tmp/x/"} -> $(readlink "$f")" ;;
           *) problem "symlink pointing outside the folder: ${f#"$tmp/x/"} -> $(readlink "$f")" ;;
         esac ;;
    esac
  done < <(find "$tmp/x" -type l)

  # The text files.
  local t
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
  if [[ -f $root/SOURCE.txt ]]; then
    grep -q "v$ver" "$root/SOURCE.txt" || problem "SOURCE.txt does not name the tag v$ver"
    grep -Eq '[^0-9a-f]([0-9a-f]{40})([^0-9a-f]|$)' "$root/SOURCE.txt" || problem "SOURCE.txt holds no full commit hash"
    grep -Eq 'https?://' "$root/SOURCE.txt" || problem "SOURCE.txt holds no repository URL"
  fi
  [[ ! -f $root/INSTALL.txt ]] || grep -q "$ver" "$root/INSTALL.txt" || problem "INSTALL.txt does not name the version $ver"
  [[ ! -f $root/LICENSE.txt ]] || grep -q 'GNU AFFERO GENERAL PUBLIC LICENSE' "$root/LICENSE.txt" || problem "LICENSE.txt is not the AGPLv3 text"
  [[ ! -f $root/THIRD-PARTY.txt ]] || grep -q 'SIL OPEN FONT LICENSE Version 1.1' "$root/THIRD-PARTY.txt" \
    || problem "THIRD-PARTY.txt does not contain the full SIL Open Font License text"
  # Nothing else at the top level.
  n=$(find "$root" -mindepth 1 -maxdepth 1 ! -name "$BUNDLE" ! -name INSTALL.txt ! -name LICENSE.txt ! -name SOURCE.txt ! -name THIRD-PARTY.txt | wc -l | tr -d ' ')
  [[ $n -eq 0 ]] || problem "unexpected top-level entries in the folder: $(find "$root" -mindepth 1 -maxdepth 1 ! -name "$BUNDLE" ! -name INSTALL.txt ! -name LICENSE.txt ! -name SOURCE.txt ! -name THIRD-PARTY.txt -exec basename {} \; | tr '\n' ' ')"

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
      [[ $(sha256_of "$dir/$n") == "$h" ]] && echo "$n: OK" || { echo "sums: $n: checksum mismatch" >&2; bad=1; }
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

  local fails=0 rc=0
  # The CHANGELOG check of the release workflow, on fixtures.
  printf '# Changelog\n\n## [Unreleased]\n- later\n\n## [1.2.3] - 2030-01-01\n### Added\n- a thing\n\n## [1.2.30]\n- other\n\n## [1.2.4]\n\n## [1.2.5]\n   \n' > "$tmp/CHANGELOG.md"
  local want
  for want in "1.2.3:0" "1.2.30:0" "1.2.4:1" "1.2.5:1" "1.2.:1" "9.9.9:1" "Unreleased:0"; do
    if "$0" changelog "${want%%:*}" "$tmp/CHANGELOG.md" >"$tmp/cl.out" 2>/dev/null; then rc=0; else rc=1; fi
    if [[ $rc -eq ${want#*:} ]]; then echo "selftest: ok: changelog section '${want%%:*}' -> exit $rc"
    else echo "selftest: FAIL: changelog section '${want%%:*}' gave exit $rc"; fails=$((fails + 1)); fi
  done
  "$0" changelog 1.2.3 "$tmp/CHANGELOG.md" 2>/dev/null | grep -q -- '- a thing' && ! "$0" changelog 1.2.3 "$tmp/CHANGELOG.md" 2>/dev/null | grep -q -- '- other' \
    || { echo "selftest: FAIL: the notes of 1.2.3 are not exactly its section"; fails=$((fails + 1)); }
  # run_verify <zip>: output in $tmp/out, status in $rc
  run_verify() { rc=0; "$0" verify "$1" >"$tmp/out" 2>&1 || rc=$?; }
  expect_reject() { # <case name> <zip> <message pattern>
    run_verify "$2"
    if [[ $rc -ne 0 ]] && grep -q -- "$3" "$tmp/out"; then
      echo "selftest: ok: $1 rejected ($(grep -m1 -- "$3" "$tmp/out" | sed 's/^verify: FAIL: //' | cut -c1-110))"
    else
      echo "selftest: FAIL: $1 was not rejected as expected (exit $rc, wanted '$3')"; sed 's/^/    /' "$tmp/out"; fails=$((fails + 1))
    fi
  }
  # mutate <case>: unpack the good zip into $tmp/<case>/ and echo that tree's folder
  mutate_tree() { rm -rf "$tmp/$1"; mkdir -p "$tmp/$1"; unzip_to "$plat" "$good" "$tmp/$1"; echo "$tmp/$1/$folder"; }
  rezip() { zip_folder "$plat" "$tmp/$1" "$folder" "$tmp/$1/$name"; echo "$tmp/$1/$name"; }

  run_verify "$good"
  if [[ $rc -eq 0 ]]; then echo "selftest: ok: the good zip is accepted"; else echo "selftest: FAIL: the good zip is rejected"; cat "$tmp/out"; fails=$((fails + 1)); fi

  local d b
  # 1. missing file
  d=$(mutate_tree missing-file); rm -f "$d/SOURCE.txt"
  expect_reject "missing file (SOURCE.txt)" "$(rezip missing-file)" 'missing file: SOURCE.txt'

  # 2. wrong version in moduleinfo.json
  d=$(mutate_tree wrong-version)
  sed -i.bak "s/\"Version\"[[:space:]]*:[[:space:]]*\"$ver\"/\"Version\": \"9.9.9\"/" "$d/$BUNDLE/Contents/Resources/moduleinfo.json"
  rm -f "$d/$BUNDLE/Contents/Resources/moduleinfo.json.bak"
  expect_reject "wrong version (moduleinfo.json 9.9.9)" "$(rezip wrong-version)" 'version mismatch'

  # 3. missing slice
  d=$(mutate_tree missing-slice)
  case $plat in
    macos)
      lipo -remove x86_64 "$d/$BUNDLE/Contents/MacOS/$PLUGIN" -output "$d/$BUNDLE/Contents/MacOS/$PLUGIN.thin"
      mv "$d/$BUNDLE/Contents/MacOS/$PLUGIN.thin" "$d/$BUNDLE/Contents/MacOS/$PLUGIN" ;;
    windows) rm -rf "$d/$BUNDLE/Contents/arm64-win" ;;
    linux) rm -rf "$d/$BUNDLE/Contents/aarch64-linux" ;;
  esac
  expect_reject "missing slice" "$(rezip missing-slice)" 'missing slice'

  # 4. B-Side string
  d=$(mutate_tree bside)
  printf 'plugin code Bsy1\n' > "$d/$BUNDLE/Contents/Resources/extra.txt"
  expect_reject "B-Side string" "$(rezip bside)" "B-Side string 'Bsy1'"

  # 5. macOS metadata entry
  d=$(mutate_tree metadata); mkdir -p "$d/../__MACOSX"; echo x > "$d/../__MACOSX/._junk"
  rm -f "$tmp/metadata/$name"; (cd "$tmp/metadata" && zip -q -r -y -X "$tmp/metadata/$name" "$folder" __MACOSX)
  expect_reject "macOS metadata entries" "$tmp/metadata/$name" 'metadata entries'

  # 6. unreplaced placeholder
  d=$(mutate_tree placeholder); printf '\nsee @REPOSITORY_URL@\n' >> "$d/INSTALL.txt"
  expect_reject "unreplaced placeholder" "$(rezip placeholder)" 'unreplaced placeholder'

  # 7. symlink pointing outside the folder
  d=$(mutate_tree symlink); ln -s ../../outside "$d/escape"
  expect_reject "symlink outside the folder" "$(rezip symlink)" 'symlink pointing outside'

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
  selftest) cmd_selftest "$@" ;;
  -h|--help|help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//' ;;
  *) die "unknown command '$cmd' (pack, verify, sums, changelog, selftest)" ;;
esac
