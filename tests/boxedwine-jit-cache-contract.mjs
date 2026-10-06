import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const patch = readFileSync(new URL("../patches/boxedwine/0006-server-jit-cache-switch.patch", import.meta.url), "utf8");
const runtime = readFileSync(new URL("../runtime/runtime.js", import.meta.url), "utf8");

assert.match(patch, /Config\.serverJitCacheEnabled\s*=\s*true/);
assert.match(patch, /getParameter\(["']server-jit-cache["']\)/);
assert.match(patch, /Config\.serverJitCacheEnabled\s*=\s*getServerJitCacheEnabled\(\)/);
assert.match(patch, /function fetchServerJitCache\(callback\)[\s\S]*if \(!Config\.serverJitCacheEnabled\)[\s\S]*callback\(\)[\s\S]*return/);
assert.match(patch, /fetch\(baseUrl \+ getJitCacheZipFilename\(\)\)/);
assert.match(runtime, /["']server-jit-cache["']:\s*["']false["']/);
assert.doesNotMatch(patch, /^[+-].*getJitRecord/m);
console.log("boxedwine JIT cache contract: ok");
