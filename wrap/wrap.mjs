// Wraps a 32-bit Windows plugin (VST2 .dll or VST3 .vst3) into a WebCLAP
// bundle for any host that supports prometheos.runtime/1:
//   node wrap/wrap.mjs <plugin.dll|plugin.vst3> --site <url serving a built site>
//        [--runtime <runtime page URL in the host>] [--dep <companion.dll>]... [--out <Name.wclap.tar.gz>]
// The plugin is described once in the runtime (headless Chromium: the site's
// runtime/index.html, which boots Boxedwine), and the bundle (wrap/bundle.js)
// gets the shim (build/vstloader.wasm), the binary and the frozen descriptor:
//   module.wasm, resources/plugin.dll, resources/vstloader.txt
// --runtime is the URL the host loads the runtime from (it must serve it on
// its own origin); a relative URL resolves against the host page.
// The same thing runs in a browser: the site's runtime/wrap.html.
// CHROMIUM=/path/to/chrome uses a preinstalled browser.
import { readFileSync, rmSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";
import { buildBundle, bundleFileName, checkBinary } from "./bundle.js";
import { normalizeDependencies } from "../runtime/dependencies.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const [dllPath, ...rest] = process.argv.slice(2);
if (!dllPath) {
  console.error("usage: node wrap/wrap.mjs <plugin.dll|plugin.vst3> --site <url> [--runtime <url>] [--dep <companion.dll>]... [--out <file.wclap.tar.gz>]");
  process.exit(2);
}
const opt = { site: "", runtime: "/vstloader/runtime/index.html", out: "", wasm: join(root, "build", "vstloader.wasm"), dep: [] };
for (let i = 0; i < rest.length; i += 2) {
  const key = rest[i].replace(/^--/, "");
  if (key === "dep") opt.dep.push(rest[i + 1]);
  else opt[key] = rest[i + 1];
}
if (!opt.site) throw new Error("--site is required (a served dist-mt/)");

const binary = readFileSync(dllPath);
checkBinary(binary, basename(dllPath));
const dependencies = await normalizeDependencies(opt.dep.map((path) => ({ name: basename(path), bytes: new Uint8Array(readFileSync(path)) })));

const browser = await chromium.launch({ executablePath: process.env.CHROMIUM || undefined });
let result;
try {
  const page = await browser.newPage();
  page.on("console", (m) => /\[vstloader\]/.test(m.text()) && console.log(m.text()));
  await page.goto(`${opt.site.replace(/\/$/, "")}/runtime/index.html?boot=1`);
  await page.waitForFunction(() => window.vstloaderRuntime?.state.phase === "ready" || window.vstloaderRuntime?.state.phase === "failed", null, {
    timeout: 900000,
  });
  result = await page.evaluate(async ({ primary, deps }) => {
    const bytes = Uint8Array.from(atob(primary), (c) => c.charCodeAt(0));
    const dependencies = deps.map((d) => ({ name: d.name, sha256: d.sha256, bytes: Uint8Array.from(atob(d.data), (c) => c.charCodeAt(0)) }));
    return window.vstloaderRuntime.describeBinary(bytes, dependencies);
  }, { primary: binary.toString("base64"), deps: dependencies.map((d) => ({ name: d.name, sha256: d.sha256, data: Buffer.from(d.bytes).toString("base64") })) });
} finally {
  await browser.close();
}

if (result.error) throw new Error(`the runtime could not describe ${dllPath}: ${result.error}`);
const { sha256, describe } = result;
const bundle = await buildBundle({
  wasm: readFileSync(opt.wasm),
  plugin: binary,
  describe,
  sha256,
  runtime: opt.runtime,
  fileName: basename(dllPath),
  dependencies,
});
const out = resolve(opt.out || bundleFileName(describe, basename(dllPath)));
rmSync(out, { force: true });
writeFileSync(out, bundle);
console.log(JSON.stringify({ out, sha256, format: describe.format, name: describe.name, params: describe.params.length,
  synth: describe.synth, inPorts: describe.inPorts.length, outPorts: describe.outPorts.length, latency: describe.latency }));
