# Native Windows Plugin Companion-DLL Compatibility Design

Date: 2026-10-06  
Status: Design approved in chat; awaiting written-spec review  
Primary repository: `valleybranch/prometheos-webclap-universalloader`  
Consumer repository: `dahlgrenmartin/prometheos-apps`

## Purpose

PrometheOS can already wrap and run unmodified 32-bit Windows VST2, VST3, and
native Jeskola Buzz machine DLLs through one BoxedWine/Wine runtime. Some
historical Buzz machines do not load because they import companion Windows DLLs
that are not present in the runtime, for example `DSPLIB.DLL` or
`MFC42.DLL`. The current bridge uploads only the selected plugin DLL to
`C:\\winvst\\<sha>.dll`, so Wine returns error 126 without giving the wrapper
a useful dependency-resolution workflow.

The same local testing also exposed two BoxedWine integration defects: SDL's
Emscripten message-box fallback invokes JavaScript `alert()` from pthread
workers where `alert` does not exist, and BoxedWine probes
`vstpoc-jit-modules.zip` although the PrometheOS runtime does not publish a
server JIT cache.

This design adds isolated companion-DLL packaging and explicit dependency
diagnostics while fixing those two emulator integration problems.

## Success criteria

A user selecting a supported x86 Windows plugin in PrometheOS Desktop can:

1. detect direct DLL imports that Wine cannot resolve;
2. satisfy missing imports with DLL files already present in the PrometheOS VFS;
3. describe and wrap the plugin only after those companion DLLs are available;
4. install the resulting WebCLAP in Buzz;
5. run the plugin later without requiring the original companion files to
   remain in the VFS; and
6. receive a specific missing-dependency message instead of a bare
   `LoadLibrary failed, error 126` whenever the host can identify the missing
   direct import.

Existing VST2, VST3, and self-contained Buzz bundles must keep working without
requiring companion metadata.

The runtime must no longer crash because an emulator pthread calls
`alert()`, and a normal runtime boot must not request a JIT-cache ZIP when
server JIT caching is disabled.

## Non-goals

This change does not:

- add 64-bit Windows plugin support;
- run installers, COM registration, services, or out-of-process helpers;
- redistribute Microsoft runtime DLLs such as `MFC42.DLL`;
- download third-party dependency DLLs automatically;
- claim redistribution rights for historical Buzz `dsplib.dll`;
- make arbitrary Windows applications work under the WebCLAP runtime; or
- replace Wine's loader. The final `LoadLibraryEx` result remains
  authoritative.

## Dependency ownership and licensing

PrometheOS will not silently add historical or proprietary runtime DLLs to the
shared Wine image.

A dependency included in a wrapped WebCLAP comes from bytes the user selected
or that the wrapper found beside the selected plugin in the user's VFS.
PrometheOS records and packages those bytes but does not assert that the user
may redistribute the resulting bundle. The wrapper UI should state this
clearly when companion DLLs are included.

A globally bundled compatibility DLL may be added in a later change only when
its source and redistribution license have been established independently.
Until that happens, `DSPLIB.DLL` and `MFC42.DLL` follow the same
user-supplied companion path.

## Guest filesystem layout

Each plugin gets an isolated guest directory keyed by the plugin SHA-256:

```
C:\winvst\<plugin-sha>\plugin.dll
C:\winvst\<plugin-sha>\DSPLIB.DLL
C:\winvst\<plugin-sha>\MFC42.DLL
...
```

The runtime must never place user-supplied dependencies into Wine's global
`system32`, the executable directory, or a directory shared by unrelated
plugins.

The Wine host loads the primary module with
`LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)`. This makes the
primary module's directory the first dependency-search location without
changing process-global DLL search state. The same rule applies to Buzz and
VST2 DLLs. VST3 continues to use its plugin file as the primary module.

Within one BoxedWine session, uploaded files are cached by guest path and
content SHA-256. Repeated instances of the same wrapped plugin therefore reuse
the same guest files.

## Dependency preflight

### Windows-host operation

The bridge gains a control operation named `VSTB_OP_PROBE_IMPORTS`. Its
request payload is the Windows path of the primary module. Its response is
UTF-8 JSON with this shape:

```json
{
  "imports": [
    { "name": "KERNEL32.DLL", "resolved": true, "path": "..." },
    { "name": "DSPLIB.DLL", "resolved": false, "path": "" }
  ],
  "missing": ["DSPLIB.DLL"]
}
```

The Windows host parses the PE import table of the primary x86 image without
executing the plugin. For each direct import it checks resolution against:

1. the primary module's isolated directory; then
2. Wine's normal searchable Windows directories.

The probe is advisory. After dependencies have been uploaded, the host still
performs the real `LoadLibraryExA`. If loading fails with error 126 while the
direct-import probe reports no missing DLL, the error returned to the browser
must say that a transitive or loader-specific dependency is unresolved and
include the direct import names for diagnostics.

Malformed PE import data must return a controlled probe error rather than
crashing the persistent host.

### Browser runtime API

The runtime exposes these operations to the wrapper:

```js
probeBinary(pluginBytes, dependencies)
describeBinary(pluginBytes, dependencies)
```

`dependencies` is a collection of
`{ name, sha256, bytes }`. Names are normalized to their basename, compared
case-insensitively, and must end in `.dll`. Directory traversal, absolute
paths, duplicate case-insensitive names, and a companion named `plugin.dll`
are rejected.

Both methods create the same isolated guest layout. `probeBinary` uploads the
primary module and supplied companions, then invokes
`VSTB_OP_PROBE_IMPORTS`. `describeBinary` performs the probe first and
refuses to invoke `VSTB_OP_LOAD` while direct imports remain missing.

The existing one-argument `describeBinary(pluginBytes)` remains valid and
therefore preserves current wrappers and tests.

## WebCLAP bundle format

A wrapped bundle keeps the existing files:

```
module.wasm
resources/plugin.dll
resources/vstloader.txt
```

and may add:

```
resources/deps/<DLL basename>
```

The descriptor gains one repeated line per companion:

```
dependency=DSPLIB.DLL<TAB><sha256><TAB>resources/deps/DSPLIB.DLL
```

The bundle builder enforces:

- x86 PE input for every dependency;
- basename-only DLL names;
- case-insensitive uniqueness;
- deterministic dependency ordering by normalized name;
- a SHA-256 recorded for every companion; and
- no duplicate resource path.

A plugin with zero dependencies produces the same logical descriptor and
resources as today, apart from unrelated future version metadata.

## WebCLAP shim and runtime handoff

The shim parses repeated `dependency=` descriptor lines at entry
initialization. Before its first plugin `HELLO`, it reads each dependency
resource and sends a new runtime frame `VL_DEPENDENCY` containing:

```
u16 nameBytes
u16 reserved
u32 dataBytes
u8  sha256[32]
u8  name[nameBytes]
u8  data[dataBytes]
```

Dependency frames are sent on the main/control path before `VL_HELLO`; they
are never sent from the audio processing path. The shim copies each expected
SHA-256 from the frozen descriptor into the frame. The runtime stores pending
dependencies on the instance. When `HELLO` arrives, it computes each payload
hash and compares it with the expected frame hash, uploads all verified files
into the isolated guest directory, and then loads the primary module.

For wrapping, the browser wrapper passes the same dependency collection
directly to `probeBinary` / `describeBinary`; it does not need a temporary
WebCLAP.

If the runtime receives a dependency whose bytes do not match the descriptor
SHA-256, loading fails before Wine executes the plugin.

## PrometheOS Desktop wrapper flow

`apps/vstwrap-remote` remains responsible for VFS selection and saving the
final archive.

After the user chooses a plugin:

1. read the plugin bytes;
2. call the universal runtime preflight with no companions;
3. if all direct imports resolve, continue as today;
4. for each missing basename, look for an exact case-insensitive filename in
   the selected plugin's VFS directory;
5. automatically stage only exact matches, not every sibling DLL;
6. probe again;
7. if imports are still missing, show their exact names and offer
   **Add dependency DLL…**;
8. reject a selected dependency whose basename does not match one of the
   unresolved imports unless the user explicitly adds it as an additional
   companion for a transitive dependency;
9. once the probe is clear, describe and wrap using the complete dependency
   collection; and
10. show the included companion names before save/install.

This keeps the normal VST and self-contained Buzz flow one-click while making
legacy dependencies explicit.

For error 126 with no missing direct import, the UI explains that a transitive
dependency may be missing and offers **Add dependency DLL…** without pretending
that preflight identified a specific file.

The existing **Install in Buzz** handoff remains unchanged. Buzz's package
manager treats companion resources as opaque WebCLAP contents.

## BoxedWine worker message-box fix

The pinned BoxedWine source is patched at the SDL Emscripten message-box
fallback. Instead of calling `alert(...)` unconditionally, it does:

```js
const message = ...;
if (typeof alert === "function") alert(message);
else if (typeof console !== "undefined") console.error(message);
```

This must be patched in the source that is compiled into workers, not only by
assigning `window.alert` in `boxedwine-shell.js`. Window globals are not
available inside pthread workers.

This change is diagnostic only; it does not suppress the underlying Wine or
plugin error.

## BoxedWine JIT-cache probe

The BoxedWine patch set gains an explicit URL/config switch that disables
server JIT-cache discovery. The universal runtime passes that switch when
booting BoxedWine.

With the switch disabled, BoxedWine does not request
`vstpoc-jit-modules.zip` and proceeds with ordinary JIT compilation. Recording
or replaying a server JIT cache remains available to other BoxedWine users when
the switch is not disabled.

PrometheOS will not publish an empty placeholder ZIP merely to hide a 404.

## Error model

Errors reaching the wrapper or plugin host use specific categories:

- **unsupported binary** — not x86 PE;
- **missing direct dependency** — exact unresolved DLL basenames from preflight;
- **dependency integrity error** — bundle hash mismatch;
- **loader dependency failure** — Wine error 126 after direct imports resolved,
  likely transitive/loader-specific;
- **plugin initialization failure** — module loaded but VST/Buzz initialization
  failed;
- **emulator failure** — BoxedWine itself failed.

Raw Wine diagnostics remain logged to the developer console, but user-facing
errors must not require reading the console to identify the category.

## Protocol and compatibility constraints

`VSTB_OP_PROBE_IMPORTS` is appended to the control-op enum; existing op values
do not change. The ABI JSON and BoxedWine patch copy must remain generated or
verified against `include/vstbridge_abi.h`.

`VL_DEPENDENCY` is additive. Old bundles send no such frames. The universal
runtime continues to accept the existing `HELLO` layout for dependency-free
bundles.

The old clean VST-only repository is not modified for companion-DLL support
unless a later generic backport is deliberately chosen. The universal loader
owns the feature first.

## Testing strategy

Implementation is test-driven.

### Universal-loader host tests

Create a small x86 fixture dependency and a small x86 test plugin that imports
it. The first test runs the import probe with only the plugin and must fail with
the exact companion basename. The same probe with the companion beside the
plugin must report no missing direct imports.

A host integration test then proves that the plugin fails to load without the
companion and loads/describes successfully after the companion is staged.

Malformed import-table fixtures verify controlled errors.

### Bundle tests

Node tests verify that:

- dependency resources appear under `resources/deps/`;
- descriptor dependency lines contain the expected name/hash/path;
- ordering is deterministic;
- case-insensitive duplicate names are rejected;
- traversal names are rejected; and
- dependency-free output remains compatible.

### Shim/runtime tests

A shim/runtime contract test proves that dependency frames precede `HELLO`
and that the runtime uploads companions into
`C:\\winvst\\<plugin-sha>\\` before issuing `VSTB_OP_LOAD`.

A negative test corrupts a dependency after descriptor generation and expects
an integrity error before plugin load.

### BoxedWine tests

The patched BoxedWine build is checked for the worker-safe message-box branch.

A browser integration test boots the universal runtime with server JIT cache
disabled and fails if any request URL ends in `-jit-modules.zip`.

### PrometheOS app tests

`vstwrap-remote` tests cover:

- exact sibling dependency discovery;
- case-insensitive DLL matching;
- unresolved-dependency presentation;
- manual companion addition;
- transitive/unknown error-126 fallback; and
- passing companions into the universal bundle builder.

Existing install-in-Buzz tests remain green.

### End-to-end regression gates

The existing pinned Robotplanet native Buzz generator continues to wrap,
install, load, and render finite non-silent audio.

CI also runs the synthetic companion-DLL fixture through the browser wrapper
and installed WebCLAP runtime. This provides a legally reproducible dependency
gate without committing `MFC42.DLL` or an ambiguously licensed historical
`dsplib.dll`.

A real historical machine that requires `dsplib.dll` can be added as an
optional/manual compatibility test once a redistributable DSPLib artifact with
clear provenance is established.

## Rollout

1. Land the protocol, host preflight, isolated guest layout, dependency frames,
   bundle support, and BoxedWine fixes in
   `prometheos-webclap-universalloader`.
2. Keep the universal-loader Robotplanet and synthetic dependency gates green.
3. Update the pinned universal-loader revision in
   `dahlgrenmartin/prometheos-apps`.
4. Add the dependency-resolution UX to `vstwrap-remote` and run its focused
   build/tests plus the native-Buzz end-to-end workflow.
5. Publish the newly composed `universal-runtime` release asset from
   `prometheos-apps`.
6. `prometheos-dev` consumes that release through its existing universal
   runtime bootstrap; no new dev-runtime architecture is required.

## Acceptance

The change is complete when all of the following are demonstrated in CI:

- a synthetic x86 plugin reports its missing companion by exact DLL basename;
- the same plugin wraps and runs when that companion is supplied;
- companion bytes survive bundle creation, Buzz installation, and runtime
  upload with hash verification;
- existing VST2/VST3 and dependency-free native Buzz paths remain green;
- the Robotplanet native-Buzz audio gate remains finite and non-silent;
- a worker-side message-box path cannot throw `ReferenceError: alert is not
  defined`; and
- normal PrometheOS runtime boot performs no
  `vstpoc-jit-modules.zip` request.
