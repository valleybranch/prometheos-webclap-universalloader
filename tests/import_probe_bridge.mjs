import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";

const bridge = readFileSync(new URL("../host/bridge.cpp", import.meta.url), "utf8");
const pluginInstance = readFileSync(new URL("../host/plugin_instance.cpp", import.meta.url), "utf8");
const formatLoaders = ["vst2", "vst3", "buzz"].map((name) =>
  readFileSync(new URL(`../host/${name}_instance.cpp`, import.meta.url), "utf8"),
);
assert.match(bridge, /case\s+VSTB_OP_PROBE_IMPORTS\s*:/);
assert.match(bridge, /probePeImports\(payload,\s*probe,\s*error\)/);
assert.match(bridge, /importProbeJson\(probe\)/);

const abi = JSON.parse(readFileSync(new URL("../include/vstbridge_abi.json", import.meta.url), "utf8"));
assert.equal(abi.ops.PROBE_IMPORTS, 8);
assert.match(pluginInstance, /missing-direct-dependency:/);
assert.match(pluginInstance, /loader-dependency-failure:/);
assert.match(pluginInstance, /plugin-initialization-failure:/);
for (const loader of formatLoaders) {
  assert.match(loader, /LoadLibraryExA\([^;]+LOAD_WITH_ALTERED_SEARCH_PATH\)/);
  assert.match(loader, /classifyPluginLoadFailure\(path, GetLastError\(\)\)/);
  assert.doesNotMatch(loader, /error\s*=\s*"LoadLibrary failed/);
}

const env = { ...process.env, WINEARCH: "win32", WINEDEBUG: "-all" };
execFileSync("wine", ["build/pe_imports_test.exe"], { stdio: "inherit", env });

const json = execFileSync(
  "wine",
  ["build/pe_imports_test.exe", "--json", "build\\companion_plugin.dll"],
  { encoding: "utf8", env },
).trim();
const probe = JSON.parse(json);
assert.deepEqual(probe.missing, ["COMPANION_DEP.DLL"]);
console.log("import probe bridge contract: ok");
