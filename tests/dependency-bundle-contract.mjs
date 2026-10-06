import { readFileSync } from "node:fs";
import { strict as assert } from "node:assert";
import { normalizeDependencies } from "../runtime/dependencies.js";
import { descriptor, tar } from "../wrap/bundle.js";

const primary = new Uint8Array(readFileSync("build/companion_plugin.dll"));
const dep = new Uint8Array(readFileSync("build/companion_dep.dll"));
const norm = await normalizeDependencies([{ name: "COMPANION_DEP.DLL", bytes: dep }]);
assert.equal(norm.length, 1);
assert.match(norm[0].sha256, /^[0-9a-f]{64}$/);
await assert.rejects(() => normalizeDependencies([{ name: "../bad.dll", bytes: dep }]), /invalid companion/);
await assert.rejects(() => normalizeDependencies([{ name: "plugin.dll", bytes: dep }]), /invalid companion/);
await assert.rejects(() => normalizeDependencies([{ name: "DSP.DLL", bytes: dep }, { name: "dsp.dll", bytes: dep }]), /duplicate/);

const describe = { format:"vst2", name:"Fixture", vendor:"", synth:false, inPorts:[], outPorts:[], latency:0, params:[] };
const base = descriptor({ sha256:"a".repeat(64), describe, runtime:"/runtime", fileName:"fixture.dll" });
const withDeps = descriptor({ sha256:"a".repeat(64), describe, runtime:"/runtime", fileName:"fixture.dll", dependencies:norm });
assert.equal(base.includes("dependency="), false);
assert.match(withDeps, new RegExp(`dependency=COMPANION_DEP\\.DLL\\t${norm[0].sha256}\\tresources/deps/COMPANION_DEP\\.DLL`));
const archive = tar([{name:"resources/deps/COMPANION_DEP.DLL",bytes:dep}]);
assert.ok(archive.length > dep.length);
assert.ok(primary.length > 0);
console.log("PASS dependency bundle contract");
