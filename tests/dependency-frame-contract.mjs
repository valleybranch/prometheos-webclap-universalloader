import assert from "node:assert/strict";
import { Op, buildDependency, parseDependency } from "../runtime/protocol.js";

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
console.log("dependency frame contract: ok");
