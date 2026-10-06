# PrometheOS Companion DLL UX and Rollout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let PrometheOS Desktop users resolve missing Windows companion DLLs while wrapping VST/Buzz plugins, package those companions into WebCLAP, and publish the updated universal runtime safely.

**Architecture:** `vstwrap-remote` will consume the universal loader's new `probeBinary` and dependency-aware `describeBinary/buildBundle` APIs. It will auto-discover exact sibling DLL matches in the VFS, expose unresolved names and manual addition in the UI, then pass the final companion set into the existing wrap/save/install flow. Deployment pins the tested universal-loader revision and republishes the composed `universal-runtime` release; `prometheos-dev` continues consuming that release unchanged.

**Tech Stack:** React 19, TypeScript, PrometheOS shared filesystem/capability APIs, Vitest, GitHub Actions, BoxedWine/Wine universal runtime.

**Spec:** `valleybranch/prometheos-webclap-universalloader/docs/superpowers/specs/2026-10-06-native-windows-companion-dlls-design.md`

## Global Constraints

- Do not automatically download or redistribute `MFC42.DLL`, historical `dsplib.dll`, or other third-party dependency binaries.
- Auto-discovery searches only the selected plugin's VFS directory and only exact case-insensitive basenames requested by runtime preflight.
- Manual dependency selection must still validate x86 PE, basename-only DLL naming, and case-insensitive uniqueness through the universal loader.
- Existing one-click VST2/VST3/self-contained Buzz wrapping stays unchanged when preflight reports no missing imports.
- Existing **Install in Buzz** behavior remains unchanged.
- Included companion names and redistribution responsibility must be visible before save/install.
- The apps repository pins a tested immutable universal-loader commit, not a moving branch.
- The published `universal-runtime` release remains the source consumed by `prometheos-dev`; no new dev-runtime mechanism is introduced.

## Review Focus

- A sibling directory containing `dsplib.dll` and `DSPLIB.DLL` must not lead to ambiguous auto-selection; Task 1 rejects duplicate case-insensitive candidates.
- A missing import named `MFC42.DLL` with no sibling match must remain visible and actionable rather than falling back to error 126; Task 2 tests the unresolved UI state.
- A manually chosen DLL whose basename does not match any unresolved import must require explicit “additional/transitive companion” intent; Task 2 tests both reject and explicit-add paths.
- A plugin whose direct imports resolve but whose transitive dependency still causes error 126 must show the transitive/loader-specific explanation and manual-add action; Task 2 adds this error-state test.
- Rewrapping a dependency-bearing plugin to the same WebCLAP filename must still reinstall through the existing Buzz handoff token; Task 3 reruns the existing install-handoff tests with a dependency-bearing result.

---

### Task 1: Companion discovery and universal-loader client types

**Files:**
- Create: `apps/vstwrap-remote/src/dependencies.ts`
- Create: `apps/vstwrap-remote/src/dependencies.test.ts`
- Modify: `apps/vstwrap-remote/src/vstloader.ts`
- Modify: `apps/vstwrap-remote/src/vstloader.test.ts`

**Interfaces:**
- Produces: `export interface CompanionDll { name: string; sha256: string; bytes: Uint8Array; sourcePath?: string }`.
- Produces: `makeCompanionDll(name: string, bytes: Uint8Array, sourcePath?: string): Promise<CompanionDll>`, which computes lowercase SHA-256 with Web Crypto.
- Produces: `export interface ImportProbeResult { imports: Array<{name:string; resolved:boolean; path:string}>; missing: string[] }`.
- Produces: `findSiblingDependencies(fs, pluginPath, missingNames): Promise<Array<{name:string; path:string}>>`.
- Changes runtime interface to `probeBinary(bytes, dependencies?: CompanionDll[]): Promise<{sha256:string; probe:ImportProbeResult}>`.
- Changes `describeBinary(bytes, dependencies?: CompanionDll[])`.
- Changes bundle tools to accept `dependencies?: CompanionDll[]`.

- [ ] **Step 1: Write failing discovery tests**

Cover exact sibling match, case-insensitive match, no recursive search, ignoring unrelated DLLs, rejecting ambiguous duplicate case-insensitive sibling names, and `makeCompanionDll` producing the expected SHA-256 for fixed bytes.

- [ ] **Step 2: Run tests and verify RED**

Run: `pnpm exec vitest run apps/vstwrap-remote/src/dependencies.test.ts`  
Expected: FAIL because `findSiblingDependencies` does not exist.

- [ ] **Step 3: Implement hashing and sibling discovery**

Implement `makeCompanionDll` with `crypto.subtle.digest("SHA-256", bytes)`. Implement `findSiblingDependencies` with `directoryOf(pluginPath)`, one `readDir` call, and exact normalized basename comparison. Return only file entries matching requested names; throw a descriptive ambiguity error if more than one VFS entry normalizes to the same requested basename.

- [ ] **Step 4: Update runtime/bundle TypeScript contracts**

Add the Task 1 universal-loader API shapes to `VstloaderRuntime` and `BundleTools`. Extend `wrapPlugin` input with `dependencies?: CompanionDll[]` and pass them to `describeBinary` and `buildBundle`.

- [ ] **Step 5: Run focused tests**

Run: `pnpm exec vitest run apps/vstwrap-remote/src/dependencies.test.ts apps/vstwrap-remote/src/vstloader.test.ts`  
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add apps/vstwrap-remote/src/dependencies* apps/vstwrap-remote/src/vstloader*
git commit -m "feat(vstwrap): model companion DLL dependencies"
```

### Task 2: Dependency-resolution workflow in the wrapper UI

**Files:**
- Modify: `apps/vstwrap-remote/src/App.tsx`
- Create: `apps/vstwrap-remote/src/App.dependencies.test.tsx`
- Modify: `apps/vstwrap-remote/README.md`

**Interfaces:**
- Consumes: Task 1 runtime/client types and `findSiblingDependencies`.
- Produces UI state: staged companions, unresolved direct imports, loader-dependency warning, and manual add action.
- Existing `wrapPlugin(...)` returns the same `WrapResult`; dependency details are wrapper-side state and bundle contents.

- [ ] **Step 1: Write failing UI tests for auto-resolution**

Mock runtime preflight to return `["DSPLIB.DLL"]`, mock VFS siblings with `dsplib.dll`, and assert the wrapper automatically reads/stages that file, reprobes, then enables wrap without another user action.

- [ ] **Step 2: Write failing unresolved-import UI test**

Preflight returns `["MFC42.DLL"]` with no sibling match. Assert the UI displays the exact basename, does not call `describeBinary`, and shows **Add dependency DLL…**.

- [ ] **Step 3: Write failing manual-add tests**

Mock `useFilePicker(...).open({ accept: ["dll"], ... })`. When the user chooses `MFC42.DLL`, assert the VFS file is read, passed through `makeCompanionDll`, staged, and reprobed. When the chosen basename is unrelated, assert it is rejected unless the user invokes the explicit **Add additional/transitive DLL…** path.

- [ ] **Step 4: Write failing transitive-error test**

Mock `describeBinary` to throw `loader-dependency-failure:error 126; direct imports=...`. Assert the UI explains that a transitive or loader-specific DLL may be missing and retains the manual-add action without inventing a filename.

- [ ] **Step 5: Implement the dependency state machine in `App.tsx`**

On plugin selection/wrap: preflight with staged companions, auto-stage exact siblings, read/hash them with `makeCompanionDll`, reprobe until no new exact sibling can be added, then either show unresolved state or call `wrapPlugin`. Use the existing `useFilePicker(windowId)` for manual DLL selection with `accept: ["dll"]` and `startIn: directoryOf(pluginPath)`. Keep manual companions across reprobes for the selected primary plugin; clear them when primary plugin changes.

- [ ] **Step 6: Surface licensing/packaging notice**

Before save/install, list included companion basenames and state that the resulting WebCLAP embeds those user-supplied DLLs and the user is responsible for redistribution rights.

- [ ] **Step 7: Run UI and wrapper tests**

Run:
```bash
pnpm exec vitest run   apps/vstwrap-remote/src/App.dependencies.test.tsx   apps/vstwrap-remote/src/dependencies.test.ts   apps/vstwrap-remote/src/vstloader.test.ts   apps/vstwrap-remote/src/buzzInstall.test.ts
```
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add apps/vstwrap-remote/src/App.tsx apps/vstwrap-remote/src/App.dependencies.test.tsx apps/vstwrap-remote/README.md
git commit -m "feat(vstwrap): resolve companion DLLs while wrapping"
```

### Task 3: Preserve Buzz installation behavior for dependency-bearing bundles

**Files:**
- Modify: `apps/vstwrap-remote/src/buzzInstall.test.ts`
- Modify: `apps/buzz-remote/src/components/plugins/WebVstDialog.test.tsx` only if package parsing needs an explicit opaque-resource regression assertion
- Test: existing `apps/buzz-remote/src/components/plugins/WebVstSources.ts`

**Interfaces:**
- Consumes: dependency-bearing `.wclap.tar.gz` output from Tasks 1–2.
- Produces no new runtime API; verifies Buzz treats `resources/deps/*` as opaque package contents and existing VFS install/reinstall flow is unchanged.

- [ ] **Step 1: Add dependency-bearing package fixture**

Create an in-memory archive fixture containing `module.wasm`, `resources/plugin.dll`, `resources/vstloader.txt`, and `resources/deps/DSPLIB.DLL`.

- [ ] **Step 2: Assert install/reinstall compatibility**

Install the fixture through the same VFS package action used by **Install in Buzz**. Assert package metadata is accepted and the second launch token for the same VFS filename triggers reinstall/update rather than being ignored.

- [ ] **Step 3: Run package/handoff tests**

Run: `pnpm exec vitest run apps/vstwrap-remote/src/buzzInstall.test.ts apps/buzz-remote/src/components/plugins/WebVstDialog.test.tsx`  
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add apps/vstwrap-remote/src/buzzInstall.test.ts apps/buzz-remote/src/components/plugins/WebVstDialog.test.tsx
git commit -m "test(buzz): accept WebCLAP companion resources"
```

### Task 4: Pin the implemented universal loader and extend CI

**Files:**
- Modify: `.github/workflows/native-buzz-universal-loader.yml`
- Modify: `.github/workflows/deploy.yml`

**Interfaces:**
- Consumes: merge commit SHA from completed universal-loader Plans 1 and 2.
- Produces: one immutable `UNIVERSAL_REF` used by native Buzz CI and deployment composition.
- Produces: focused wrapper tests/typecheck/build and universal-loader synthetic companion gate in apps CI.

- [ ] **Step 1: Update `UNIVERSAL_REF` and runtime copy lists**

Set it to the immutable merge commit that contains both companion-DLL core and BoxedWine diagnostic fixes. Use the same SHA in `deploy.yml`. Add `runtime/dependencies.js` to both workflows' explicit universal-runtime copy lists beside `runtime.js`, `protocol.js`, and `bundle.js`.

- [ ] **Step 2: Add focused companion UX tests to `desktop-wrapper`**

Run `dependencies.test.ts`, `App.dependencies.test.tsx`, `vstloader.test.ts`, `buzzInstall.test.ts`, and the existing WebVST package tests before typecheck/build.

- [ ] **Step 3: Exercise the universal synthetic fixture from apps CI**

After checking out the pinned universal loader, run its companion-DLL contract/browser test so an apps PR cannot pin a runtime that lacks dependency support.

- [ ] **Step 4: Keep the Robotplanet gate unchanged**

Run the existing `ld clap.dll` wrap/install/render scenario and require finite, non-silent audio with zero runtime errors.

- [ ] **Step 5: Verify deploy builds the updated runtime release**

Run the deploy workflow in a PR-safe mode or equivalent build job and assert `dist/vstloader/runtime/bundle.js` contains dependency support plus the BoxedWine runtime has the new patch behavior. The release asset remains `vstloader-runtime.tar.gz`.

- [ ] **Step 6: Commit**

```bash
git add .github/workflows/native-buzz-universal-loader.yml .github/workflows/deploy.yml
git commit -m "ci: publish companion-aware universal runtime"
```

### Task 5: Rollout verification through prometheos-dev

**Files:**
- No production code change expected in `dahlgrenmartin/prometheos-dev`.
- Test/inspect: `scripts/vstloader-runtime.mjs` and its existing runtime bootstrap tests.

**Interfaces:**
- Consumes: updated `universal-runtime` GitHub Release asset from Task 4.
- Produces: verified local dev install of the new release by release/asset identity.

- [ ] **Step 1: Publish/update the `universal-runtime` asset**

Use the apps deployment workflow after merge. Record the release asset ID and composed runtime identity.

- [ ] **Step 2: Run the dev runtime bootstrap tests**

Expected: the release resolver sees a changed asset ID, downloads the new archive, validates `windows-dll`/`buzzloader` runtime markers, and replaces the previous cached runtime.

- [ ] **Step 3: Local smoke check**

Start `pnpm dev`, open `vstwrap-remote`, and verify the runtime reports the new release identity. Probe a dependency-free plugin and a synthetic dependency-bearing plugin; neither may show the legacy-runtime error.

- [ ] **Step 4: Final acceptance**

Confirm:
- focused wrapper tests/typecheck/build PASS;
- synthetic companion dependency gate PASS;
- dependency-bearing WebCLAP installs in Buzz;
- Robotplanet native Buzz audio gate PASS;
- no `alert is not defined` worker error; and
- no `vstpoc-jit-modules.zip` request during normal runtime boot.
