import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { Op, buildDependency, parseDependency } from "../runtime/protocol.js";

const shim = readFileSync(new URL("../wclap/vstloader.c", import.meta.url), "utf8");
const runtime = readFileSync(new URL("../runtime/runtime.js", import.meta.url), "utf8");

assert.equal(Op.DEPENDENCY, 5);
const sha = "0123456789abcdef".repeat(4);
const bytes = new Uint8Array([1, 2, 3, 4]);
const body = buildDependency({ name: "DSP.DLL", sha256: sha, bytes });
assert.equal(new DataView(body.buffer, body.byteOffset).getUint16(0, true), 7);
assert.equal(new DataView(body.buffer, body.byteOffset).getUint16(2, true), 0);
assert.equal(new DataView(body.buffer, body.byteOffset).getUint32(4, true), 4);
assert.equal(body.length, 40 + 7 + 4);
const parsed = parseDependency(body);
assert.equal(parsed.name, "DSP.DLL");
assert.equal(parsed.sha256, sha);
assert.deepEqual([...parsed.bytes], [1, 2, 3, 4]);

assert.throws(() => parseDependency(body.subarray(0, body.length - 1)), /dependency frame/);
assert.throws(() => buildDependency({ name: "../DSP.DLL", sha256: sha, bytes }), /dependency name/);
assert.throws(() => buildDependency({ name: "plugin.dll", sha256: sha, bytes }), /dependency name/);
assert.throws(() => buildDependency({ name: "DSP.DLL", sha256: "bad", bytes }), /dependency sha256/);

const names = new Set();
for (let i = 0; i < 64; i++) {
  const dep = parseDependency(buildDependency({ name: `d${i}.dll`, sha256: sha, bytes }));
  const key = dep.name.toLowerCase();
  assert.equal(names.has(key), false);
  names.add(key);
}
assert.equal(names.size, 64);
const duplicate = parseDependency(buildDependency({ name: "D0.DLL", sha256: sha, bytes }));
assert.equal(names.has(duplicate.name.toLowerCase()), true);

assert.match(shim, /#define MAX_DEPENDENCIES 64/);
assert.match(shim, /sendDependencies\(v\)[\s\S]*sendFrame\(v, VL_HELLO/);
assert.match(shim, /case VL_RESEND:[\s\S]*helloSent = false;[\s\S]*sendHello\(v\)/);
assert.match(shim, /loadDependencies\(plugin_path\)/);
assert.match(runtime, /case Op\.DEPENDENCY:[\s\S]*addDependency\(body\)/);
assert.match(runtime, /dependency integrity error:/);
assert.match(runtime, /this\.dependencies\.clear\(\);[\s\S]*this\.dependencyError/);
assert.match(runtime, /if \(this\.dependencyError\)[\s\S]*invalid companion dependency batch/);
assert.match(runtime, /await sha256Hex\(binary\)[\s\S]*plugin integrity error[\s\S]*await upload\(hello\.sha256, binary\)[\s\S]*await upload\([\s\S]*dep\.bytes[\s\S]*this\.channel = freeChannel\(\)[\s\S]*loadInto/);
console.log("dependency frame contract: ok");
