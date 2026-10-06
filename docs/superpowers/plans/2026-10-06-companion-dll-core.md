# Companion DLL Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add deterministic companion-DLL probing, packaging, integrity checking, and isolated Wine loading to the universal Windows-plugin loader.

**Architecture:** The Windows host parses PE import tables without executing the plugin and exposes a new additive bridge operation for direct-import diagnostics. The browser runtime stages the primary DLL and companions into one SHA-keyed guest directory, the WebCLAP bundle persists companions under `resources/deps/`, and the shim sends verified dependency frames before `HELLO`.

**Tech Stack:** C++17/Win32/MinGW i686, JavaScript ES modules, C/WASI WebCLAP shim, shared-memory `vstbridge`, Node contract tests, Playwright/browser integration tests.

**Spec:** `docs/superpowers/specs/2026-10-06-native-windows-companion-dlls-design.md`

## Global Constraints

- Support only 32-bit x86 PE plugins and x86 companion DLLs.
- Do not redistribute `MFC42.DLL`, historical `dsplib.dll`, or any dependency whose redistribution rights are not established.
- Guest layout is `C:\\winvst\\<plugin-sha>\\plugin.dll` plus companion basenames in the same directory.
- User-supplied companions never go into global Wine directories.
- Primary DLL loading uses `LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)`.
- Existing `describeBinary(pluginBytes)` and dependency-free bundles remain valid.
- New bridge and shim operations are additive; existing numeric op values do not change.
- Companion names are basename-only, case-insensitively unique, end in `.dll`, and cannot be `plugin.dll`.
- Bundle/runtime integrity is SHA-256 checked before Wine executes the plugin.
- All protocol twins and the BoxedWine ABI copy must remain synchronized with their canonical headers.

## Review Focus

- PE files with a truncated/corrupt import directory must return a probe error without crashing the persistent host; Task 1 adds the malformed-fixture test.
- Two companions named `DSP.DLL` and `dsp.dll` must be rejected before upload; Task 3 adds the case-insensitive duplicate test.
- A direct import that resolves but has a missing transitive dependency must become a loader-dependency error, not “all dependencies satisfied”; Task 2 adds the error-126 classification test.
- A bundle whose dependency resource is modified after descriptor creation must fail integrity verification before `VSTB_OP_LOAD`; Task 4 adds the corruption test.
- A dependency-free VST2/VST3/Buzz bundle must preserve the existing one-argument runtime path and descriptor shape; Tasks 3 and 4 add compatibility assertions.

---

### Task 1: PE import probe in the Windows host

**Files:**
- Create: `host/pe_imports.h`
- Create: `host/pe_imports.cpp`
- Create: `tests/fixtures/companion_dep.c`
- Create: `tests/fixtures/companion_plugin.c`
- Create: `tests/pe_imports.cpp`
- Modify: `build.sh`

**Interfaces:**
- Produces: `struct ImportResolution { std::string name; bool resolved; std::string path; };`
- Produces: `struct ImportProbe { std::vector<ImportResolution> imports; std::vector<std::string> missing; };`
- Produces: `bool probePeImports(const std::string &modulePath, ImportProbe &out, std::string &error);`
- Produces: `std::string importProbeJson(const ImportProbe &probe);`
- Consumes: Win32 file APIs only; it must not call `LoadLibrary` on the primary module or its companions.

- [ ] **Step 1: Write failing PE-probe tests**

Add tests named `reports_exact_missing_companion`, `resolves_companion_beside_primary`, `resolves_normal_windows_imports`, and `rejects_truncated_import_table`. Build `companion_dep.dll` and `companion_plugin.dll` with MinGW; the plugin must import one exported function from `COMPANION_DEP.DLL`.

- [ ] **Step 2: Run the probe test and verify RED**

Run: `BUILD_SITE=0 ./build.sh && ./build/pe_imports_test.exe` under 32-bit Wine in CI/local Wine.  
Expected: compile/link failure because `probePeImports` does not exist.

- [ ] **Step 3: Implement `probePeImports`**

Parse DOS header, PE32 headers, section table, and `IMAGE_IMPORT_DESCRIPTOR` entries from the file bytes. Resolve each basename first as `<primary-dir>\\<name>`; if absent, call `SearchPathA(NULL, name, NULL, ...)`. Sort/deduplicate imported names case-insensitively while preserving canonical import spelling in JSON.

- [ ] **Step 4: Implement deterministic JSON**

`importProbeJson` must emit `{"imports":[{"name":"...","resolved":true|false,"path":"..."}],"missing":["..."]}` with stable import ordering and JSON escaping.

- [ ] **Step 5: Run tests and verify GREEN**

Run: `BUILD_SITE=0 ./build.sh && WINEARCH=win32 wine build/pe_imports_test.exe`  
Expected: all four probe tests PASS.

- [ ] **Step 6: Commit**

```bash
git add host/pe_imports.* tests/fixtures tests/pe_imports.cpp build.sh
git commit -m "feat: probe Windows plugin imports"
```

### Task 2: Bridge operation, isolated guest paths, and loader error classification

**Files:**
- Modify: `include/vstbridge_abi.h`
- Modify: `include/vstbridge_abi.json`
- Modify: `web/vstbridge-abi.js`
- Modify: `patches/boxedwine/0003-vstbridge-device.patch`
- Modify: `host/bridge.cpp`
- Modify: `host/plugin_instance.cpp`
- Modify: `host/vst2_instance.cpp`
- Modify: `host/vst3_instance.cpp`
- Modify: `host/buzz_instance.cpp`
- Test: `tests/abi_layout.mjs`
- Create: `tests/import_probe_bridge.mjs`

**Interfaces:**
- Consumes: Task 1 `probePeImports(...)` and `importProbeJson(...)`.
- Produces: `VSTB_OP_PROBE_IMPORTS = 8`; request payload is UTF-8 Windows primary-module path, response is Task 1 JSON.
- Produces: loader errors prefixed by stable categories `missing-direct-dependency:`, `loader-dependency-failure:`, or `plugin-initialization-failure:`.

- [ ] **Step 1: Write failing ABI/bridge tests**

Extend ABI goldens to expect `PROBE_IMPORTS: 8`. Add a bridge contract test that sends op 8 for the synthetic primary module and asserts `missing == ["COMPANION_DEP.DLL"]`.

- [ ] **Step 2: Run tests and verify RED**

Run: `node tests/abi_layout.mjs && node tests/import_probe_bridge.mjs`  
Expected: ABI/op test fails because op 8 is absent.

- [ ] **Step 3: Add `VSTB_OP_PROBE_IMPORTS = 8` and regenerate twins**

Update the canonical header first, regenerate/update JSON and `web/vstbridge-abi.js`, then update patch 0003's embedded header copy.

- [ ] **Step 4: Handle op 8 in `host/bridge.cpp`**

Decode the path, call Task 1, return JSON on success and `VSTB_STATUS_ERROR` with the probe error text on malformed input.

- [ ] **Step 5: Switch primary-module loading to isolated-search semantics**

Use `LoadLibraryExA(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH)` for VST2/Buzz primary DLLs and the VST3 primary module. Preserve existing export detection and format ordering.

- [ ] **Step 6: Classify error 126**

Before returning a `LoadLibraryExA` error 126, run the direct-import probe. If `missing` is non-empty, return `missing-direct-dependency:<comma-separated names>`; otherwise return `loader-dependency-failure:error 126; direct imports=<names>`. Other load/init failures use the existing message plus `plugin-initialization-failure:` only after the module loaded.

- [ ] **Step 7: Run ABI and bridge tests**

Run: `node tests/abi_layout.mjs && node tests/import_probe_bridge.mjs`  
Expected: PASS, including patch 0003 equality.

- [ ] **Step 8: Commit**

```bash
git add include web patches/boxedwine/0003-vstbridge-device.patch host tests
git commit -m "feat: expose plugin import probing over vstbridge"
```

### Task 3: Runtime dependency model and bundle format

**Files:**
- Modify: `runtime/runtime.js`
- Modify: `wrap/bundle.js`
- Modify: `runtime/wrap.js`
- Create: `tests/dependency-bundle-contract.mjs`
- Create: `tests/runtime-dependency-contract.mjs`

**Interfaces:**
- Produces JS type shape: `{ name: string, sha256: string, bytes: Uint8Array }`.
- Produces: `window.vstloaderRuntime.probeBinary(pluginBytes, dependencies = [])`.
- Changes: `describeBinary(pluginBytes, dependencies = [])` while keeping the one-argument call valid.
- Changes: `buildBundle({ ..., dependencies = [] })`.
- Produces descriptor lines: `dependency=<name>\t<sha256>\tresources/deps/<name>`.

- [ ] **Step 1: Write failing normalization/bundle tests**

Assert rejection of traversal/absolute names, non-DLLs, `plugin.dll`, non-x86 companions, and case-insensitive duplicates. Assert deterministic ordering and exact dependency descriptor/resource paths. Assert dependency-free descriptor text remains unchanged.

- [ ] **Step 2: Run contract tests and verify RED**

Run: `node tests/dependency-bundle-contract.mjs`  
Expected: FAIL because `buildBundle` ignores dependencies.

- [ ] **Step 3: Implement dependency normalization in `wrap/bundle.js`**

Export `normalizeDependencies(dependencies)`. Reuse `checkBinary` for x86 PE validation, lower-case only for comparison/sort keys, preserve the user's basename for the resource name, and require the supplied SHA-256 to be 64 lowercase hex characters.

- [ ] **Step 4: Extend descriptor/tar construction**

Emit sorted repeated `dependency=` lines and matching `resources/deps/<basename>` tar entries. Do not emit dependency lines for an empty list.

- [ ] **Step 5: Write failing runtime probe tests**

Stub `ControlClient` and assert `probeBinary` uploads primary + companions into `C:\\winvst\\<plugin-sha>\\`, then calls op 8. Assert the same guest path+SHA upload is cached within one runtime session.

- [ ] **Step 6: Implement `probeBinary` and dependency-aware `describeBinary`**

Compute the primary SHA, normalize dependencies, upload `plugin.dll` plus companion basenames into the SHA directory via `VSTB_OP_PUT_FILE`, invoke `VSTB_OP_PROBE_IMPORTS`, and block `VSTB_OP_LOAD` while `missing.length > 0`.

- [ ] **Step 7: Run runtime/bundle tests**

Run: `node tests/dependency-bundle-contract.mjs && node tests/runtime-dependency-contract.mjs`  
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add runtime wrap tests
git commit -m "feat: package and stage companion DLLs"
```

### Task 4: WebCLAP dependency frames and integrity verification

**Files:**
- Modify: `wclap/vstloader_protocol.h`
- Modify: `runtime/protocol.js`
- Modify: `wclap/vstloader.c`
- Modify: `runtime/runtime.js`
- Modify: `wclap/build.sh`
- Create: `tests/dependency-frame-contract.mjs`

**Interfaces:**
- Produces: `VL_DEPENDENCY = 5` (plugin -> runtime).
- Produces frame body: `u16 nameBytes, u16 reserved, u32 dataBytes, u8 sha256[32], u8 name[nameBytes], u8 data[dataBytes]`.
- Runtime contract: all `VL_DEPENDENCY` frames for an instance must precede `VL_HELLO`; `HELLO` verifies/stages them before load.

- [ ] **Step 1: Write failing protocol/frame tests**

Assert C and JS op tables both define `DEPENDENCY: 5`; assert a fixture bundle causes dependency frames before HELLO; corrupt one dependency byte and assert the runtime emits `VL_ERROR` with `dependency integrity error` and never sends `VSTB_OP_LOAD`.

- [ ] **Step 2: Run tests and verify RED**

Run: `node tests/dependency-frame-contract.mjs`  
Expected: FAIL because op 5 and descriptor parsing are absent.

- [ ] **Step 3: Parse dependency descriptor entries in the shim**

Extend `vl_descriptor` with a bounded dependency table containing name, expected binary SHA-256, and resource path. Reject malformed lines during `entry_init`.

- [ ] **Step 4: Send dependency frames before HELLO**

On the main/control path, read each dependency resource, decode descriptor SHA-256 hex into 32 bytes, send `VL_DEPENDENCY`, then send the existing HELLO. Do not allocate/read dependency files in `process()`.

- [ ] **Step 5: Verify and stage frames in the runtime**

Store pending dependency frames per instance, compute SHA-256 with `crypto.subtle.digest`, reject mismatches before PUT_FILE/LOAD, then upload companions into the same SHA guest directory used by Task 3.

- [ ] **Step 6: Run protocol/frame tests and WASI build**

Run: `node tests/dependency-frame-contract.mjs && WASI_SDK=/opt/wasi-sdk ./wclap/build.sh`  
Expected: PASS/build succeeds.

- [ ] **Step 7: Commit**

```bash
git add wclap runtime tests
git commit -m "feat: restore companion DLLs from WebCLAP bundles"
```

### Task 5: Synthetic companion-DLL end-to-end gate

**Files:**
- Modify: `build.sh`
- Modify: `.github/workflows/release.yml`
- Modify: `tests/browser_render.mjs`
- Create: `tests/fixtures/companion_buzz.cpp` only if the simpler VST2 fixture cannot exercise the installed WebCLAP path
- Test: existing `tests/buzz-loader-contract.mjs`, Robotplanet gate

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: reproducible CI proof that a missing direct dependency is named exactly, then the same plugin wraps/loads when the companion is supplied.

- [ ] **Step 1: Add the red end-to-end scenario**

Build the synthetic primary+companion fixtures in `build.sh`. In browser CI, first call `probeBinary(primary, [])` and assert `missing == ["COMPANION_DEP.DLL"]`; then call with the companion and assert no missing imports.

- [ ] **Step 2: Run the browser scenario and verify RED before Tasks 1–4 are merged into the branch**

Run the existing browser test command from `.github/workflows/release.yml`.  
Expected before implementation: missing API/op assertions fail.

- [ ] **Step 3: Wrap and run the synthetic bundle**

Create `build/wraps/companion-test.wclap.tar.gz` with the dependency. Extract the tar in CI and assert the descriptor line and resource exist. Run the browser/installed WebCLAP path and assert the plugin loads/describes successfully.

- [ ] **Step 4: Run compatibility regression gates**

Run:
```bash
node tests/abi_layout.mjs
node tests/buzz-loader-contract.mjs
node tests/dependency-bundle-contract.mjs
node tests/runtime-dependency-contract.mjs
node tests/dependency-frame-contract.mjs
```
Then run the existing release workflow including the Robotplanet native Buzz audio gate.  
Expected: all PASS; Robotplanet render remains finite/non-silent.

- [ ] **Step 5: Commit**

```bash
git add build.sh .github/workflows/release.yml tests
git commit -m "test: gate companion DLL WebCLAPs end to end"
```
