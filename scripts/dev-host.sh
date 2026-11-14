#!/usr/bin/env bash
# Build one of our plugins + JUCE's AudioPluginHost (from the JUCE submodule) for this Mac's native
# architecture and launch the host with a graph: MIDI in -> the plugin -> audio out.
#
#   scripts/dev-host.sh              Exitium Vintage Synth: build (incremental), launch the host detached and return
#   scripts/dev-host.sh --plugin bside   the same for Exitium B-Side (--plugin vintage is the default)
#   scripts/dev-host.sh --wait       launch the host and stay attached until it is closed
#   scripts/dev-host.sh --build-only build, don't launch
#   scripts/dev-host.sh --no-build   launch what is already built
#   scripts/dev-host.sh --debug      Debug build in build/dev-host-debug
#   scripts/dev-host.sh --install    also copy the .vst3 to ~/Library/Audio/Plug-Ins/VST3/
#
# Both plugins share the build tree build/dev-host (build/dev-host-debug), so switching between them
# only builds what is missing. Nothing outside the repo is touched unless --install is given.
set -euo pipefail

die() { echo "dev-host: error: $*" >&2; exit 1; }
info() { echo "dev-host: $*"; }

DO_BUILD=1; DO_LAUNCH=1; DO_INSTALL=0; DO_WAIT=0; CONFIG=Release; BDIR_NAME=dev-host; WHICH=vintage
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-only) DO_LAUNCH=0 ;;
    --no-build)   DO_BUILD=0 ;;
    --debug)      CONFIG=Debug; BDIR_NAME=dev-host-debug ;;
    --install)    DO_INSTALL=1 ;;
    --wait)       DO_WAIT=1 ;;
    --plugin)     [[ $# -ge 2 ]] || die "--plugin needs vintage or bside"; WHICH=$2; shift ;;
    -h|--help)    sed -n '2,14p' "$0"; exit 0 ;;
    *)            die "unknown option '$1' (see --help)" ;;
  esac
  shift
done
case "$WHICH" in
  vintage) TARGET=VintageSynth_VST3; BUNDLE_NAME="Exitium Vintage Synth.vst3"; GRAPH_NAME=vintage-synth-dev.filtergraph ;;
  bside)   TARGET=BSide_VST3;        BUNDLE_NAME="Exitium B-Side.vst3";        GRAPH_NAME=bside-dev.filtergraph ;;
  *)       die "unknown plugin '$WHICH' (vintage or bside)" ;;
esac
[[ $DO_BUILD -eq 0 && $DO_LAUNCH -eq 0 ]] && die "--build-only and --no-build together do nothing"

[[ "$(uname -s)" == "Darwin" ]] || die "macOS only"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/$BDIR_NAME"
ARCH="$(uname -m)"   # arm64 on Apple silicon, x86_64 on Intel; native only, never universal

if [[ $DO_BUILD -eq 1 ]]; then
  command -v cmake >/dev/null || die "cmake not found (brew install cmake ninja)"
  command -v ninja >/dev/null || die "ninja not found (brew install cmake ninja)"
  [[ -f "$ROOT/external/JUCE/CMakeLists.txt" ]] || die "JUCE submodule is empty: run 'git submodule update --init --recursive'"
  info "configuring ($CONFIG, $ARCH) in build/$BDIR_NAME"
  cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DVS_BUILD_TESTS=OFF \
    -DVS_BUILD_DEV_HOST=ON >/dev/null || die "configure failed"
  info "building $WHICH plugin + host (first build takes several minutes)"
  cmake --build "$BUILD" --parallel --target "$TARGET" AudioPluginHost || die "build failed"
fi

[[ -d "$BUILD" ]] || die "no build at build/$BDIR_NAME; run without --no-build first"
PLUGIN="$(find "$BUILD" -maxdepth 4 -type d -name "$BUNDLE_NAME" | head -n1)"
HOST_APP="$(find "$BUILD" -maxdepth 4 -type d -name 'AudioPluginHost.app' | head -n1)"
[[ -n "$PLUGIN" ]] || die "plugin bundle not found under build/$BDIR_NAME (build it first)"
[[ -n "$HOST_APP" ]] || die "AudioPluginHost.app not found under build/$BDIR_NAME (build it first)"
HOST_BIN="$HOST_APP/Contents/MacOS/AudioPluginHost"
[[ -x "$HOST_BIN" ]] || die "host executable missing: $HOST_BIN"

if [[ $DO_INSTALL -eq 1 ]]; then
  DEST="$HOME/Library/Audio/Plug-Ins/VST3"
  mkdir -p "$DEST"
  rm -rf "${DEST:?}/$BUNDLE_NAME"
  # The Vintage bundle was called "Vintage Synth.vst3" before the rename. It has the same class ID, so a
  # host would see two copies of one plugin: remove the old one.
  if [[ $WHICH == vintage && -d "$DEST/Vintage Synth.vst3" ]]; then
    rm -rf "$DEST/Vintage Synth.vst3"
    info "removed the old bundle $DEST/Vintage Synth.vst3 (renamed to Exitium Vintage Synth, same plugin ID)"
  fi
  cp -R "$PLUGIN" "$DEST/"
  info "installed to $DEST/$BUNDLE_NAME"
fi

# ---- graph file ------------------------------------------------------------------------------
# The host identifies a VST3 by name + a hash of its class ID, both read from the bundle's
# moduleinfo.json (written by the JUCE build). Format derived from PluginGraph::createXml and
# PluginDescription::createXml in the JUCE submodule.
GRAPH="$BUILD/$GRAPH_NAME"
MODINFO="$PLUGIN/Contents/Resources/moduleinfo.json"
make_graph() {
  [[ -f "$MODINFO" ]] || return 1
  local cid name
  cid="$(plutil -extract Classes.0.CID raw -o - "$MODINFO" 2>/dev/null)" || return 1
  name="$(plutil -extract Classes.0.Name raw -o - "$MODINFO" 2>/dev/null)" || return 1
  [[ ${#cid} -eq 32 ]] || return 1
  local u=0 d=0 i b long
  # deprecated uid: hash over the 16 bytes as signed chars; uniqueId: hash over 4 big-endian words.
  for ((i = 0; i < 16; i++)); do
    b=$((16#${cid:i*2:2}))
    (( b > 127 )) && b=$((b - 256))
    d=$(( (d * 31 + b) & 0xFFFFFFFF ))
  done
  for ((i = 0; i < 4; i++)); do
    long=$((16#${cid:i*8:8}))
    u=$(( (u * 31 + long) & 0xFFFFFFFF ))
  done
  local uh dh
  uh="$(printf '%x' "$u")"; dh="$(printf '%x' "$d")"
  local nm=${name//&/&amp;}
  local io='format="Internal" category="Effect" manufacturer="JUCE" version="9.0.5"'
  cat > "$GRAPH" <<EOF
<?xml version="1.0" encoding="UTF-8"?>

<FILTERGRAPH>
  <FILTER uid="1" x="0.25" y="0.1" useARA="0">
    <PLUGIN name="Audio Input" $io file="Audio Input" uniqueId="0" isInstrument="0" numInputs="0" numOutputs="2" isShell="0" hasARAExtension="0" uid="0"/>
  </FILTER>
  <FILTER uid="2" x="0.5" y="0.1" useARA="0">
    <PLUGIN name="Midi Input" $io file="Midi Input" uniqueId="0" isInstrument="0" numInputs="0" numOutputs="0" isShell="0" hasARAExtension="0" uid="0"/>
  </FILTER>
  <FILTER uid="3" x="0.5" y="0.9" useARA="0">
    <PLUGIN name="Audio Output" $io file="Audio Output" uniqueId="0" isInstrument="0" numInputs="2" numOutputs="0" isShell="0" hasARAExtension="0" uid="0"/>
  </FILTER>
  <FILTER uid="5" x="0.5" y="0.5" useARA="0">
    <PLUGIN name="$nm" format="VST3" category="Instrument|Synth" manufacturer="Exitium" version="0.1.0" file="$PLUGIN" uniqueId="$uh" isInstrument="1" numInputs="0" numOutputs="2" isShell="0" hasARAExtension="0" uid="$dh"/>
  </FILTER>
  <CONNECTION srcFilter="2" srcChannel="4096" dstFilter="5" dstChannel="4096"/>
  <CONNECTION srcFilter="5" srcChannel="0" dstFilter="3" dstChannel="0"/>
  <CONNECTION srcFilter="5" srcChannel="1" dstFilter="3" dstChannel="1"/>
</FILTERGRAPH>
EOF
}

GRAPH_OK=1
make_graph || GRAPH_OK=0

echo
info "plugin : $PLUGIN"
info "host   : $HOST_APP"
[[ $GRAPH_OK -eq 1 ]] && info "graph  : $GRAPH"

# Detached by default: the host keeps running after this script returns (output in host.log).
# --wait keeps it attached to the terminal until the host is closed.
launch() {
  if [[ $DO_WAIT -eq 1 ]]; then
    "$HOST_BIN" "$@"
  else
    nohup "$HOST_BIN" "$@" >"$BUILD/host.log" 2>&1 </dev/null &
    disown $! 2>/dev/null || true
    info "host started (pid $!), log: $BUILD/host.log"
  fi
}

if [[ $DO_LAUNCH -eq 1 ]]; then
  if [[ $GRAPH_OK -eq 1 ]]; then
    info "launching host with the graph (open the plugin window by double-clicking its node)"
    launch "$GRAPH"
  else
    info "could not generate the graph (moduleinfo.json unreadable). Launching the bare host."
    info "In the host: Options > Edit the list of available plug-ins > Scan for new or updated VST3 plug-ins,"
    info "or drag '$PLUGIN' onto the host window; then wire Midi Input -> plugin -> Audio Output."
    launch
  fi
fi
