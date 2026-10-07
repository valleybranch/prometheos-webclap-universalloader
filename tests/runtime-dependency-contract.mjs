import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const runtime = readFileSync(new URL("../runtime/runtime.js", import.meta.url), "utf8");
const deps = readFileSync(new URL("../runtime/dependencies.js", import.meta.url), "utf8");

assert.match(runtime, /function guestDir\(sha256\) \{ return `C:\\\\winvst\\\\\$\{sha256\}`; \}/);
assert.match(runtime, /function guestPath\(sha256\) \{ return `\$\{guestDir\(sha256\)\}\\\\plugin\.dll`; \}/);
assert.match(runtime, /async function probeBinary\(binary, dependencies = \[\]\)/);
assert.match(runtime, /normalizeDependencies\(dependencies\)/);
assert.match(runtime, /OP_PROBE_IMPORTS/);
assert.match(runtime, /missing-direct-dependency:/);
assert.match(runtime, /async function describeBinary\(binary, dependencies = \[\]\)/);
assert.match(runtime, /dep\.sha256/);
assert.match(deps, /dependency integrity error:/);
assert.match(deps, /assertX86Pe\(bytes, name\)/);
assert.match(deps, /name\.includes\(":"\)/);
console.log("runtime dependency contract: ok");
