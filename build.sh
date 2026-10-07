#!/usr/bin/env bash
# Builds the proof of concept:
#   1. the Windows side with MinGW (i686): vsthost.exe and the two test plugins;
#   2. vstpoc.zip, the Boxedwine "app" zip mounted at C:\files;
#   3. dist/, a static site: Boxedwine's Emscripten build, the Wine filesystem
#      zip, vstpoc.zip and the demo page.
#
# Environment:
#   BOXEDWINE_BUILD  Boxedwine Emscripten output dir (default ../boxedwine/project/emscripten/Build/Jit;
#                    the real-time page needs Build/MultiThreadedJit with patches 0003/0004)
#   DIST             output directory for the static site (default dist)
#   WINE_FS_ZIP      Boxedwine Wine filesystem zip (default: downloaded to .cache/)
#   WITH_DEXED=1     also fetch Dexed 0.9.3's 32-bit VST2 (GPL-3, third-party binary)
#   BUILD_SITE=0     stop after the Windows binaries and the zips (no Boxedwine needed)
#   WASI_SDK         wasi-sdk directory for the vstloader WebCLAP (default /opt/wasi-sdk)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

CC=${CC:-i686-w64-mingw32-gcc}
CXX=${CXX:-i686-w64-mingw32-g++}
BOXEDWINE_BUILD=${BOXEDWINE_BUILD:-$here/../boxedwine/project/emscripten/Build/Jit}
WINE_FS_URL=${WINE_FS_URL:-https://boxedwine.org/v2/10/TinyCore15Wine11.0.zip}
WINE_FS_FALLBACK_URL=${WINE_FS_FALLBACK_URL:-}
WINE_FS_SHA256=e38234f93e85b1714c54f87ec3246a8275683b091219a8a4651ea7e3acd16b79
DEXED_URL=https://github.com/asb2m10/dexed/releases/download/v0.9.3/dexed-0.9.3-win.zip

mkdir -p build .cache app
echo "== Windows binaries (MinGW i686)"
$CC -O2 -std=c11 -Wall -shared -Iinclude plugins/vst2/poc_synth_vst2.c plugins/vst2/poc_synth_vst2.def \
  -o build/PoCSynth.dll -static-libgcc -lm
$CC -O2 -std=c11 -Wall -shared -Iinclude plugins/vst2/poc_invert_vst2.c plugins/vst2/poc_invert_vst2.def \
  -o build/PoCInvert.dll -static-libgcc
$CXX -O2 -std=c++17 -Wall -shared -Iinclude -Ivendor plugins/vst3/poc_synth_vst3.cpp plugins/vst3/poc_synth_vst3.def \
  -o build/PoCSynth.vst3 -static -Wl,--kill-at -Wl,--enable-stdcall-fixup
$CC -O2 -std=c11 -Wall -shared tests/fixtures/companion_dep.c -Wl,--out-implib,build/libcompanion_dep.a -o build/companion_dep.dll
cp build/companion_dep.dll build/companion_dep.fixture
$CC -O2 -std=c11 -Wall -shared -Iinclude tests/fixtures/companion_plugin.c build/libcompanion_dep.a -o build/companion_plugin.dll
$CXX -O2 -std=c++17 -Wall -Iinclude -Ivendor host/vsthost.cpp host/bridge.cpp host/pe_imports.cpp host/plugin_instance.cpp host/vst2_instance.cpp host/vst3_instance.cpp host/buzz_instance.cpp \
  -o build/vsthost.exe -static -lwinmm
$CXX -O2 -std=c++17 -Wall -Iinclude -Ihost tests/pe_imports.cpp host/pe_imports.cpp -o build/pe_imports_test.exe -static
$CC -O2 tests/wintest.c -o build/wintest.exe
$CC -O2 -Wall tests/devtest.c -o build/devtest.exe

plugins_json='[
  {"file": "PoCSynth.dll", "label": "PoC Synth", "format": "vst2", "note": "test plugin built here"},
  {"file": "PoCSynth.vst3", "label": "PoC Synth", "format": "vst3", "note": "test plugin built here"},
  {"file": "PoCInvert.dll", "label": "PoC Invert", "format": "vst2", "note": "test effect built here: output = -input"}'
rm -rf app && mkdir app
cp build/vsthost.exe build/PoCSynth.dll build/PoCInvert.dll build/PoCSynth.vst3 build/devtest.exe app/
if [ "${WITH_DEXED:-0}" = 1 ]; then
  echo "== Dexed 0.9.3 (third-party 32-bit Windows VST2)"
  [ -f .cache/dexed-0.9.3-win.zip ] || curl -fsSL -o .cache/dexed-0.9.3-win.zip "$DEXED_URL"
  unzip -o -q -j .cache/dexed-0.9.3-win.zip 'dexed-0.9.3/win32/Dexed.dll' -d app/
  plugins_json="$plugins_json"',
  {"file": "Dexed.dll", "label": "Dexed 0.9.3", "format": "vst2", "note": "third-party JUCE plugin (GPL-3)"}'
fi
plugins_json="$plugins_json
]"
(cd app && rm -f ../build/vstpoc.zip && zip -q -X -r ../build/vstpoc.zip .)
# A zip layered *before* the Wine filesystem (Boxedwine takes a file from the
# first zip that has it): "disable" in .update-timestamp stops Wine re-running
# wineboot's prefix update on every fresh in-memory prefix, which otherwise
# blocks start-up in the browser until wineboot's 5-minute timeout.
rm -rf build/overlay && mkdir -p build/overlay/home/username/.wine
printf 'disable\n' > build/overlay/home/username/.wine/.update-timestamp
(cd build/overlay && rm -f ../vstpoc-prefix.zip && zip -q -X -r ../vstpoc-prefix.zip home)

if [ "${BUILD_SITE:-1}" = 0 ]; then
  echo "Built build/vstpoc.zip and build/vstpoc-prefix.zip"
  exit 0
fi

echo "== Wine filesystem"
WINE_FS_ZIP=${WINE_FS_ZIP:-$here/.cache/TinyCore15Wine11.0.zip}
if [ ! -f "$WINE_FS_ZIP" ]; then
  tmp="$WINE_FS_ZIP.tmp"
  rm -f "$tmp"
  urls=()
  [ -z "$WINE_FS_FALLBACK_URL" ] || urls+=("$WINE_FS_FALLBACK_URL")
  urls+=("$WINE_FS_URL")
  for url in "${urls[@]}"; do
    echo "Downloading pinned Wine filesystem from $url"
    if curl --fail --location --show-error --silent --retry 2 --retry-all-errors \
      --connect-timeout 15 --max-time 180 -o "$tmp" "$url"; then
      if echo "$WINE_FS_SHA256  $tmp" | sha256sum -c -; then
        mv "$tmp" "$WINE_FS_ZIP"
        break
      fi
      echo "Wine filesystem hash mismatch from $url" >&2
    fi
    rm -f "$tmp"
  done
  [ -f "$WINE_FS_ZIP" ] || { echo "failed to download verified Wine filesystem" >&2; exit 1; }
fi
echo "$WINE_FS_SHA256  $WINE_FS_ZIP" | sha256sum -c -

DIST=${DIST:-dist}
echo "== $DIST/"
[ -f "$BOXEDWINE_BUILD/boxedwine.html" ] || { echo "missing Boxedwine build in $BOXEDWINE_BUILD"; exit 1; }
rm -rf "$DIST" && mkdir -p "$DIST/boxedwine"
cp -r "$BOXEDWINE_BUILD"/. "$DIST/boxedwine/"
rm -rf "$DIST"/boxedwine/home "$DIST"/boxedwine/.*-config.json
ln -f "$WINE_FS_ZIP" "$DIST/boxedwine/TinyCore15Wine11.0.zip" 2>/dev/null || cp "$WINE_FS_ZIP" "$DIST/boxedwine/"
cp build/vstpoc.zip build/vstpoc-prefix.zip "$DIST/boxedwine/"
cp web/index.html web/app.js web/realtime.html web/realtime.js web/realtime-worklet.js web/bench-worker.js \
  web/vstbridge.js web/vstbridge-abi.js "$DIST/"
printf '%s\n' "$plugins_json" > "$DIST/plugins.json"

# The WebCLAP's runtime page (prometheos.runtime/1), the browser wrapper
# (runtime/wrap.html) and, with wasi-sdk, the shim.
mkdir -p "$DIST/runtime"
cp runtime/index.html runtime/runtime.js runtime/relay-worker.js runtime/protocol.js \
  runtime/wrap.html runtime/wrap.js runtime/dependencies.js wrap/bundle.js web/vstbridge.js web/vstbridge-abi.js "$DIST/runtime/"
WASI_SDK=${WASI_SDK:-/opt/wasi-sdk}
if [ -x "$WASI_SDK/bin/clang" ]; then
  WASI_SDK="$WASI_SDK" ./wclap/build.sh
  cp build/vstloader.wasm "$DIST/runtime/"
else
  echo "(no wasi-sdk at $WASI_SDK: the vstloader WebCLAP is not built)"
fi
echo "Serve $DIST/ with COOP/COEP (python3 serve.py 8080 $DIST) and open /index.html (offline) or /realtime.html"
