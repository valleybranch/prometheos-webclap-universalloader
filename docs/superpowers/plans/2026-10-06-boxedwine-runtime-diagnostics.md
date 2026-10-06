# BoxedWine Runtime Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent BoxedWine worker-side message-box crashes and disable unnecessary server JIT-cache probing in the PrometheOS universal runtime.

**Architecture:** Patch the pinned BoxedWine source, not only its page shell, so SDL's Emscripten message-box fallback is worker-safe. Add one explicit BoxedWine configuration switch for server JIT-cache discovery and have the PrometheOS runtime disable that switch at boot while preserving BoxedWine's default behavior for other users.

**Tech Stack:** BoxedWine C/C++, SDL Emscripten backend, Emscripten pthread workers, JavaScript runtime boot page, Playwright network assertions.

**Spec:** `docs/superpowers/specs/2026-10-06-native-windows-companion-dlls-design.md`

## Global Constraints

- Do not hide underlying Wine/plugin errors; only prevent the browser worker from crashing while reporting them.
- BoxedWine's default behavior remains server JIT-cache probing enabled.
- PrometheOS explicitly disables server JIT-cache probing.
- Do not publish an empty `vstpoc-jit-modules.zip`.
- Preserve existing `jit-record=true` recording behavior.
- Apply changes through the repository's pinned BoxedWine patch series; do not edit generated `dist/` output.
- The native Buzz Robotplanet audio gate must remain green.

## Review Focus

- SDL message boxes invoked from a pthread worker must log instead of throwing when `alert` is absent; Task 1 adds a worker-context source contract test.
- SDL message boxes on a normal browser main thread must still call `alert` when available; Task 1 tests both branches.
- Disabling server JIT cache must not disable ordinary WASM JIT execution; Task 2 checks boot/render still succeeds.
- `jit-record=true` must continue to expose recording behavior despite server cache discovery being disabled; Task 2 adds a query-param regression assertion.
- A PrometheOS boot must issue zero requests ending in `-jit-modules.zip`; Task 3 records and checks all browser requests.

---

### Task 1: Worker-safe SDL message boxes

**Files:**
- Create: `patches/boxedwine/0005-worker-safe-messagebox.patch`
- Create: `tests/boxedwine-messagebox-contract.mjs`
- Modify: `build.sh` only if the patch application list is explicit there

**Interfaces:**
- Produces BoxedWine SDL Emscripten behavior: construct the same message string, call `alert(message)` only when `typeof alert === "function"`, otherwise call `console.error(message)` when console exists.

- [ ] **Step 1: Write the failing patch contract test**

The test extracts the SDL hunk from patch 0005 and asserts it contains both `typeof alert === "function"` and a console fallback, and does not contain an unconditional `alert(UTF8ToString(...))`.

- [ ] **Step 2: Run test and verify RED**

Run: `node tests/boxedwine-messagebox-contract.mjs`  
Expected: FAIL because patch 0005 does not exist.

- [ ] **Step 3: Create patch 0005 against the pinned BoxedWine commit**

Modify `lib/sdl2/src/video/SDL_video.c` inside `SDL_ShowSimpleMessageBox`'s Emscripten branch. Keep return value and non-Emscripten code unchanged.

- [ ] **Step 4: Apply the patch series to a clean BoxedWine checkout**

Run the same patch application commands used by `.github/workflows/release.yml`.  
Expected: all patches 0001–0005 apply without fuzz/rejects.

- [ ] **Step 5: Run contract test**

Run: `node tests/boxedwine-messagebox-contract.mjs`  
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add patches/boxedwine/0005-worker-safe-messagebox.patch tests/boxedwine-messagebox-contract.mjs build.sh
git commit -m "fix: make BoxedWine message boxes worker safe"
```

### Task 2: Explicit server JIT-cache discovery switch

**Files:**
- Create: `patches/boxedwine/0006-server-jit-cache-switch.patch`
- Modify: `runtime/runtime.js`
- Create: `tests/boxedwine-jit-cache-contract.mjs`

**Interfaces:**
- Produces BoxedWine URL parameter: `server-jit-cache=false`.
- Produces BoxedWine config field: `Config.serverJitCacheEnabled`, default `true`.
- PrometheOS runtime boot URL must set `server-jit-cache=false`.

- [ ] **Step 1: Write failing switch contract tests**

Assert patch 0006 defines a config parser for `server-jit-cache`, default true, and short-circuits `fetchServerJitCache(callback)` before `fetch(...-jit-modules.zip)` when false. Assert PrometheOS runtime's BoxedWine boot URL contains `server-jit-cache=false`.

- [ ] **Step 2: Run tests and verify RED**

Run: `node tests/boxedwine-jit-cache-contract.mjs`  
Expected: FAIL because no switch exists.

- [ ] **Step 3: Implement patch 0006**

In `project/emscripten/boxedwine-shell.js`, add `Config.serverJitCacheEnabled = true`, parse `server-jit-cache=true|false`, and make `fetchServerJitCache(callback)` call `callback()` immediately when disabled. Do not alter `getJitRecord()`, persistence APIs, or cache import code.

- [ ] **Step 4: Pass the switch from `runtime/runtime.js`**

When constructing the nested BoxedWine URL, append `server-jit-cache=false`. Do not change the public WebCLAP runtime URI.

- [ ] **Step 5: Verify patch application and contract**

Run the clean patch-series application plus `node tests/boxedwine-jit-cache-contract.mjs`.  
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add patches/boxedwine/0006-server-jit-cache-switch.patch runtime/runtime.js tests/boxedwine-jit-cache-contract.mjs
git commit -m "fix: disable server JIT cache probing in PrometheOS"
```

### Task 3: Browser regression gate for worker errors and JIT requests

**Files:**
- Modify: `tests/browser_render.mjs`
- Modify: `.github/workflows/release.yml`

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces CI assertions that no page/worker error contains `alert is not defined` and no request URL ends in `-jit-modules.zip`.

- [ ] **Step 1: Add browser request/error capture**

Attach Playwright `page.on("request")`, `page.on("pageerror")`, and console error collection before runtime boot.

- [ ] **Step 2: Write failing assertions**

At test completion, assert zero request URLs matching `/-jit-modules\.zip(?:\?|$)/` and zero captured errors containing `ReferenceError: alert is not defined`.

- [ ] **Step 3: Run browser test**

Run the browser-render command from the release workflow against a build with Tasks 1–2.  
Expected: PASS and normal plugin render remains non-silent.

- [ ] **Step 4: Run `jit-record=true` smoke path**

Boot BoxedWine once with `jit-record=true&server-jit-cache=false` and assert startup succeeds and recording controls/state are still initialized. No server JIT ZIP request is allowed.

- [ ] **Step 5: Run full release workflow**

Expected: synthetic plugins, existing VST fixtures, and Robotplanet native Buzz render all PASS.

- [ ] **Step 6: Commit**

```bash
git add tests/browser_render.mjs .github/workflows/release.yml
git commit -m "test: gate BoxedWine worker and JIT cache behavior"
```
