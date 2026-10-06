import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const patch = readFileSync(new URL("../patches/boxedwine/0005-worker-safe-messagebox.patch", import.meta.url), "utf8");
assert.match(patch, /typeof alert === ["']function["']/);
assert.match(patch, /console\.error\(/);
assert.doesNotMatch(patch, /^\+\s*alert\(UTF8ToString\(/m);
assert.match(patch, /UTF8ToString\(\$0\).*UTF8ToString\(\$1\)/s);
console.log("boxedwine messagebox contract: ok");
