# prometheos-webclap-universalloader

Runs **unmodified 32-bit Windows VST2 and VST3 plugin binaries inside a browser
tab**, in real time.

This repository was split out of
[prometheos-apps](https://github.com/dahlgrenmartin/prometheos-apps)
(`experiments/boxedwine-vst`, with its history) because Boxedwine is GPL: the
emulator, its patches and everything built around them live here, and
buzz-remote only loads the result as a WebCLAP plugin: a thin real-time shim
that runs in the host's AudioWorklet and streams through shared memory to the
emulator, which runs in a separate runtime page (see "The WebCLAP" below and
the [design spec](docs/design/2026-10-04-realtime-windows-plugins-design.md)).

The rest of this README describes the proof of concept and the real-time bridge.
A Windows host program loads the plugin under Wine,
inside [Boxedwine](https://github.com/danoon2/Boxedwine), an x86 emulator that
compiles to WebAssembly with Emscripten. The audio the plugin renders comes back
to the web page, which decodes, draws and plays it.

The design borrows from [yabridge](https://github.com/robbert-vdh/yabridge):
there, a native Linux plugin talks to a long-lived Wine-side host process that
loads the Windows plugin. Here the "native" side is the web page, the Wine side
is `vsthost.exe` inside Boxedwine, and the transport is the emulator's
filesystem instead of sockets and shared memory.

```text
browser page (web/)                        Boxedwine (WebAssembly)
  ├─ boots the emulator once (~20 s) ───▶   Wine 11 (TinyCore filesystem zip)
  │                                           └─ vsthost.exe --serve C:\vstpoc-jobs
  ├─ writes a job into C:\vstpoc-jobs\inbox.txt ──▶ ├─ LoadLibrary(plugin.dll | plugin.vst3)
  │    (through Emscripten's FS)                    ├─ VST2: VSTPluginMain → AEffect
  │                                                 ├─ VST3: GetPluginFactory → IComponent/IAudioProcessor
  │                                                 └─ renders a MIDI chord → WAV + JSON report
  └─ reads C:\vstpoc-out\<n>.json/.wav  ◀──────── C:\vstpoc-out
       → WebAudio decode, waveform, playback
```

![Dexed 0.9.3, a 32-bit Windows VST2 binary, rendered in the browser](docs/dexed-in-browser.png)

## What is in here

| Path | What |
|---|---|
| `wclap/` | The vstloader WebCLAP (`vstloader.c`, built by `wclap/build.sh` with wasi-sdk) and its frame protocol with the runtime. |
| `runtime/` | The runtime page a host loads for the WebCLAP: boots Boxedwine, loads instances, relays blocks (`relay-worker.js`); and `wrap.html`, the browser wrapper. |
| `wrap/` | Wraps a 32-bit `.dll` (VST2) or `.vst3` (VST3) into a `.wclap.tar.gz` bundle: `bundle.js` (shared with `wrap.html`) and `wrap.mjs` (Node). |
| `.github/workflows/release.yml` | Builds Boxedwine, the runtime site and the wrapped test plugins; publishes them as a release on a `v*` tag. |
| `include/prometheos_runtime.h` | The `prometheos.runtime/1` CLAP extension (plugin and host sides). |
| `tests/buzz-remote/` | The wrapped plugins inside buzz-remote's real engine, in headless Chromium. |
| `host/bridge.cpp`, `host/plugin_instance.cpp` | `vsthost --bridge`: real-time hosting through `/dev/vstbridge` (one thread per plugin instance), and `vsthost --replay`, the offline reference that renders a captured request stream through the same code. A plugin is a `PluginInstance`: `vst2_instance.cpp`, `vst3_instance.cpp`, or `buzz_instance.cpp`, selected by the binary exports. The Buzz path loads `GetInfo`/`CreateMachine`, preserves native `Init`/`Save` state, raw Buzz parameter metadata, `Tick`/`Work`, track limits and the +/-32768 sample convention while reusing the same bridge/runtime. |
| `include/vstbridge_abi.h` | The shared-memory layout of `/dev/vstbridge` (the single source of truth; `vstbridge_abi.json` is its golden layout, checked against the JS and TypeScript twins and the patch's copy). |
| `web/realtime.html` | Streams a plugin live into an AudioWorklet (on-screen keyboard, computer keys, Web MIDI), with underrun and block-time readouts; `tests/realtime.mjs` and `tests/identity.mjs` drive it headlessly. |
| `host/vsthost.cpp` | The Windows-side host (MinGW, i686). VST2 through a clean-room ABI header, VST3 through Steinberg's MIT-licensed `pluginterfaces` only. One-shot mode, or persistent `--serve <dir>` mode that takes jobs from a mailbox file. Writes a WAV, a JSON report (plugin info, parameters with display text, peak/RMS, non-finite sample count, load/render time) and a stage trace. `--play` also sends the render to the Windows audio device (`waveOut`), which Boxedwine plays through browser audio. |
| `include/vst2_abi.h` | The VST 2.4 binary interface written from the published ABI (as LMMS's VeSTige does); no Steinberg VST2 SDK code. |
| `plugins/vst2`, `plugins/vst3` | "PoC Synth": the same 8-voice saw synth as a VST2 `.dll` and a VST3 `.vst3`, built here so the pipeline can be tested without third-party binaries. |
| `vendor/pluginterfaces` | Steinberg VST3 `pluginterfaces` (MIT), pinned to `4f547e8` (VST3 SDK 3.8.1). |
| `patches/boxedwine/` | Fixes to Boxedwine needed for this (see below). |
| `web/` | The demo page. |
| `build.sh` | Builds the Windows binaries, `vstpoc.zip` and the static site in `dist/`. |
| `serve.py` | Serves `dist/` with COOP/COEP headers. |
| `tests/` | `browser_render.mjs` (headless Chromium end-to-end), `fputest.c` and `wintest.c` (emulator diagnostics). |
| `docs/results.md` | Measurements, and the problems found and fixed on the way. |
| `docs/performance.md` | Why emulation is slow next to yabridge, and the options for real-time use. |
| [real-time design spec](docs/design/2026-10-04-realtime-windows-plugins-design.md) | Design spec and [Phase 0 plan](docs/design/2026-10-04-realtime-windows-plugins-phase0.md) for real-time Windows plugins in buzz-remote, browser-only. |

## Results

**Real time:** Dexed streams live into an AudioWorklet from Boxedwine's
multithreaded build through `/dev/vstbridge` (shared WebAssembly memory, Atomics
wake-ups, no main thread on the audio path): 10 minutes of sequenced 8-voice
chords at 48 kHz with **zero underruns at L = 2,048 frames**, and its output,
shifted by L, is bit-identical to the offline render of the same requests. See
[results.md](docs/results.md#real-time-phase-0-dexed-streamed-live-into-an-audioworklet).

![Dexed streamed live for 10 minutes](docs/realtime-dexed-600s.png)

**Offline:** measured in headless Chromium (details and screenshots in
[docs/results.md](docs/results.md)):

| Plugin | Format | Origin | In the browser (audio processing) | Natively (x64 JIT) |
|---|---|---|---|---|
| PoC Synth | VST2 | built here | renders, peak 0.42, ~20× realtime | renders, 50× realtime |
| PoC Synth | VST3 | built here | renders, peak 0.42, ~12× realtime | renders, 16× realtime |
| **Dexed 0.9.3** | VST2 | third party (JUCE, MSVC, 2017) | **renders, peak 0.35, 155 parameters, 3.8–4.7× realtime** | blocks in `VSTPluginMain` (headless window creation, see below) |

The emulator boots to a serving `vsthost` in about 20 s. Dexed's first
`VSTPluginMain` in a session takes about 9 s (one-off JUCE/Wine start-up). The
audio processing itself is faster than realtime from the first render. `--play`
was verified to play through Boxedwine's browser audio (`waveOut: played 44100
frames`).

Emulation is still about 100–200× slower than native code: the same synth DSP
takes 0.86 ms natively, 0.79 ms as WebAssembly compiled from source, and about
100–170 ms as an x86 DLL under Boxedwine in the browser.
**[docs/performance.md](docs/performance.md)** explains why, and lays out the
options for real-time use: streaming with the plugin kept loaded and
render-ahead, a faster JIT, static recompilation of plugin DLLs to WebAssembly
("plugin recomp"), source ports to WebCLAP, and a local native companion.

## Boxedwine changes

Boxedwine `509f6a7` (2026-10-02) was used with two local patches:

1. `0001-fpu-frndint-honours-chop.patch`: **an emulation bug found by this
   PoC.** In the interpreter's (and the WebAssembly JIT's) shared FPU code,
   `FRNDINT` ignored the x87 "chop" rounding mode, because `FPU::FROUND`
   leaves chopping to its integer-store callers' casts. The standard x87
   `exp`/`pow` sequence (`fldcw` chop → `frndint` → `f2xm1` → `fscale`, used by
   MinGW's libm and common in compiled DSP code) then returned exactly `1.0`,
   so the test synth rendered digital silence in the browser while working
   under the native x64 JIT (which rounds correctly). `tests/fputest.c`
   reproduces it.
2. `0002-emscripten-link-with-em++.patch`: links with `em++`. Current
   Emscripten (6.x) no longer pulls libc++ into an `emcc` link of C++ objects.

## Build and run

Requirements: `i686-w64-mingw32-gcc/g++`, an Emscripten SDK, Python 3, `zip`.

```bash
git submodule update --init vendor/pluginterfaces

# Boxedwine for the browser
git clone https://github.com/danoon2/Boxedwine && cd Boxedwine
git checkout 509f6a7
git apply /path/to/prometheos-webclap-vstloader/patches/boxedwine/*.patch
cd project/emscripten && make jit        # -> Build/Jit/boxedwine.{html,js,wasm}
make multiThreadedJit                     # -> Build/MultiThreadedJit (the real-time page needs it)

# The PoC (downloads the Wine filesystem zip; WITH_DEXED=1 adds Dexed 0.9.3 win32)
cd prometheos-webclap-vstloader
BOXEDWINE_BUILD=/path/to/Boxedwine/project/emscripten/Build/Jit WITH_DEXED=1 ./build.sh
python3 serve.py 8080 dist      # open http://127.0.0.1:8080/

# Real time (multithreaded build): open /realtime.html
BOXEDWINE_BUILD=/path/to/Boxedwine/project/emscripten/Build/MultiThreadedJit DIST=dist-mt WITH_DEXED=1 ./build.sh
python3 serve.py 8080 dist-mt
node tests/realtime.mjs http://127.0.0.1:8080 --plugin Dexed.dll --latency 2048 --seconds 600
```

The Phase 1 measurements in `docs/results.md` were taken with buzz-remote's
in-tree `winvst` machine and its `tests/winvst-browser` harness (prometheos-apps
branch `feat/buzz-winvst-mvp`), which the WebCLAP replaces.

Emscripten fetches its zlib and SDL2 ports from GitHub archive URLs. Behind a
proxy that refuses those, clone `madler/zlib@v1.3.2` and
`libsdl-org/SDL@release-2.32.10` into Emscripten's `cache/ports/{zlib,sdl2}/`
with a matching `.emscripten_url` marker.

## Limitations and next steps

- **32-bit plugins only.** This includes classic Buzz machine DLLs. Boxedwine emulates 32-bit x86, so modern 64-bit-only
  plugins (most current releases) cannot load. Older free plugins often still
  ship 32-bit builds.
- **Real time needs the multithreaded build and L = 2,048 frames** (42.7 ms) for
  Dexed; lighter plugins can run with less. `index.html` still renders offline.
- **Speed.** Emulated plugin code runs about 100–200× slower than native.
  Dexed still processes at about 4× realtime, but heavier plugins will not fit;
  see [docs/performance.md](docs/performance.md).
- **No plugin editors.** Plugin GUIs would need Boxedwine's window output (it
  already draws Wine windows to a canvas) wired to `effEditOpen` / `IPlugView`.
- **JUCE plugins under headless native Boxedwine.** Creating any window (even a
  hidden `STATIC` control, see `tests/wintest.c`) hangs this native Boxedwine
  build under Xvfb, so JUCE plugins like Dexed, which create a message window
  inside `VSTPluginMain`, block there natively. The browser build has a real
  canvas and Dexed works there.
- **Start-up cost.** The Wine filesystem zip is 158 MB, and every page load
  starts from a fresh in-memory prefix. Boxedwine can persist the prefix and its
  JIT cache in IndexedDB, which would make later visits faster.

## The WebCLAP

A wrapped Windows plugin is an ordinary WebCLAP bundle. A host that supports
the `prometheos.runtime/1` extension (`include/prometheos_runtime.h`; buzz-remote
does) installs and plays it like any other WebCLAP; the emulator is a separate
runtime page it loads once for every wrapped plugin.

```text
host AudioWorklet                         runtime page (hidden frame, the host's origin)
  vstloader.wasm (the shim, per bundle)      Boxedwine (MT) + vsthost --bridge
    process(): one request per 256 frames      /dev/vstbridge channel n
    into a vstbridge channel in its own           ▲ requests        │ answered blocks
    shared memory; output read L later            │                 ▼
    clap.latency = L + plugin delay           relay workers (2 per instance, futex waits)
         ▲ shared WebAssembly.Memory ─────────────┘ copy blocks both ways
host main thread: loads the page, hands it the memory, relays control frames
```

- **Shim** (`wclap/vstloader.c`, wasi-sdk, `wasm32-wasip1-threads` with an
  imported shared memory): what buzz-remote's in-tree `WinVstMachine` did, as
  CLAP. Parameters, ports, notes and the plugin's delay come from
  `resources/vstloader.txt`, frozen when the `.dll` was wrapped; `clap.state`
  is the plugin's chunk as the runtime last reported it (refreshed after
  parameter changes). No allocation in `process()`; silence until the runtime
  has loaded the plugin.
- **Runtime** (`runtime/`): boots Boxedwine, uploads each binary once, loads
  every instance into its own bridge channel (up to 7 at once; channel 8
  describes binaries for the wrapper) and runs two relay workers per instance.
  The workers block on futexes on both sides, so audio never touches an event
  loop or the main thread. Frames between shim and runtime:
  `wclap/vstloader_protocol.h` (`runtime/protocol.js`).
- **Wrapper**: a 32-bit `.dll` (VST2) or `.vst3` (VST3) in, a `.wclap.tar.gz`
  out (`module.wasm`, `resources/plugin.dll`, `resources/vstloader.txt`), after
  one DESCRIBE in the runtime. In the browser: `runtime/wrap.html` on any
  deployed site (its bundles name that site's runtime). From Node:
  `wrap/wrap.mjs`. Both build the bundle with `wrap/bundle.js`.

```bash
# the site with the runtime (and the shim, with wasi-sdk)
WASI_SDK=/path/to/wasi-sdk BOXEDWINE_BUILD=/path/to/Build/MultiThreadedJit DIST=dist-mt WITH_DEXED=1 ./build.sh
python3 serve.py 8080 dist-mt &   # http://127.0.0.1:8080/runtime/wrap.html wraps in the browser
node wrap/wrap.mjs app/Dexed.dll --site http://127.0.0.1:8080 --runtime /vstloader/runtime/index.html --out build/wraps/Dexed.wclap.tar.gz
node wrap/wrap.mjs app/PoCSynth.vst3 --site http://127.0.0.1:8080 --runtime /vstloader/runtime/index.html --out build/wraps/PoCSynth-vst3.wclap.tar.gz

# the wrapped plugins inside buzz-remote's real engine (song/identity, null, state round trip);
# --site takes any runtime site, e.g. an unpacked vstloader-runtime.tar.gz
node tests/buzz-remote/build.mjs --buzz /path/to/prometheos-apps/apps/buzz-remote [--site dist-mt] [--plugins build/wraps]
node tests/buzz-remote/run.mjs --scenarios song,null,bzw --plugin Dexed,PoCSynth-vst3 --seconds 600
```

### Releases

`.github/workflows/release.yml` builds everything on CI (Boxedwine at the
pinned commit with `patches/boxedwine/`, Emscripten 6.0.11, the Wine
filesystem, vsthost, the shim) and wraps the test plugins and Dexed. A `v*` tag
publishes:

- `vstloader-runtime.tar.gz`: `vstloader/` with `runtime/` (the runtime page,
  `wrap.html`, the shim) and `boxedwine/`, plus `SOURCES.md` (where every part
  comes from, and its license). A host unpacks it where it serves the
  runtime: for buzz-remote in PrometheOS, next to the app, at
  `<apps root>/vstloader/`.
- `PoCSynth.wclap.tar.gz` (VST2), `PoCSynth-vst3.wclap.tar.gz` (VST3),
  `PoCInvert.wclap.tar.gz` (an effect) and `Dexed.wclap.tar.gz` (GPL-3.0):
  install them from buzz-remote's Plugins dialog.

Bundles name their runtime by URL. A tag uses the repository variable
`VSTLOADER_RUNTIME_URL` or `/prometheos-apps/vstloader/runtime/index.html`;
`workflow_dispatch` takes another. For any other deployment, wrap with that
deployment's `vstloader/runtime/wrap.html`.

What a host needs to provide:

- **Cross-origin isolation** (COOP/COEP), for shared memory.
- **The runtime on its own origin.** The runtime page must share memory with
  the AudioWorklet, and cross-origin isolated pages only share memory with
  same-origin frames, so the host serves this repository's built site (here
  under `/vstloader/`: `runtime/` and `boxedwine/`) and the bundle's
  `runtime=` URL points there. The page then runs with the host's origin,
  which is why hosts only load runtimes they trust.
- **`prometheos.runtime/1`**: load the named page once, hand it each
  instance's shared memory, relay frames (buzz-remote: `PluginRuntimeHost`).

## Licenses

- This repository (`vsthost`, the bridge, the patches, the test plugins and
  pages): GPL-3.0 (`LICENSE`).
- `vendor/pluginterfaces`: MIT (Steinberg Media Technologies).
- Boxedwine: GPL-2.0-or-later. Wine: LGPL-2.1-or-later.
- Dexed (optional download, not committed; wrapped in releases): GPL-3.0,
  source at https://github.com/asb2m10/dexed/tree/v0.9.3.


## Universal-loader fork

This repository intentionally keeps the original VST transport and runtime architecture intact while adding formats behind `PluginInstance`. The VST-only project can therefore remain small and focused; transport, Boxedwine patches and VST fixes can be cherry-picked between the repositories without requiring Buzz code in the VST-only tree.

Buzz DLL detection uses the native exports `GetInfo` and `CreateMachine`. Wrapped Buzz binaries use the same hidden runtime page and therefore share one Boxedwine/Wine boot and the existing per-instance bridge channels with VST instances. Multiple Buzz machines can be active in one runtime; they do not start one emulator per machine.

The current Buzz implementation is the first integration slice. It covers DLL detection and description, global parameter metadata, track metadata, attributes, `Init`/`Save` state, `Tick`, `Work`, `MidiNote`, mono effects/generators and Buzz sample scaling. The compatibility target for the next slices is the host contract already proven by `prometheos-webclap-buzzmachines`: exact `prometheos.buzz-machine/1` scheduling, waves/envelopes, MI66 multi-I/O/stereo effects/latency and the broader callback surface.
