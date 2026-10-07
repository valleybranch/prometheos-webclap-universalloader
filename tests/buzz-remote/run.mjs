// Runs the vstloader WebCLAP inside buzz-remote's engine (harness.ts) in
// headless Chromium:
//   node tests/buzz-remote/build.mjs --buzz <prometheos-apps>/apps/buzz-remote
//   node tests/buzz-remote/run.mjs [--scenarios song,null,bzw] [--plugin Dexed]
//        [--seconds 600] [--capture 20] [--null-seconds 10] [--out dir]
// song: the wrapped plugin (build/wraps/<plugin>.wclap.tar.gz; "<name>-vst3" for a
//   wrapped <name>.vst3) as a machine playing
//   8-voice chords for --seconds; the first --capture seconds of the master
//   output and of the requests its channel published are replayed offline by
//   vsthost --replay (tests/identity.mjs --align 1, dist-st/).
// null: buzz-remote's FM synth dry + through the wrapped PoC Invert.
// bzw: Dexed's state through buzz-remote's project codec and back.
// Serves out/ and dist-st/ with cross-origin isolation.
// CHROMIUM=/path/to/chrome uses a preinstalled browser.
import { execFileSync, spawn } from "node:child_process";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../..");
const opt = {
  scenarios: "song,null,bzw",
  plugin: "Dexed",
  seconds: "600",
  capture: "20",
  "null-seconds": "10",
  out: join(here, "out", "runs"),
  port: "8097",
};
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i += 2) opt[args[i].replace(/^--/, "")] = args[i + 1];
const scenarios = new Set(opt.scenarios.split(","));
mkdirSync(opt.out, { recursive: true });

const servers = [];
function serve(port, directory) {
  servers.push(spawn("python3", [join(root, "serve.py"), String(port), directory], { stdio: "ignore" }));
  return `http://127.0.0.1:${port}`;
}
const base = serve(Number(opt.port), join(here, "out"));
const replayBase = serve(Number(opt.port) + 1, join(root, "dist-st"));
await new Promise((r) => setTimeout(r, 1000));

const t0 = Date.now();
const say = (o) => console.log(JSON.stringify({ t: (Date.now() - t0) / 1000, ...o }));
const browser = await chromium.launch({
  executablePath: process.env.CHROMIUM || undefined,
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const results = {};
const consoleLog = [];
try {
  const page = await browser.newPage({ viewport: { width: 1100, height: 900 } });
  page.on("pageerror", (e) => say({ pageerror: e.message }));
  page.on("console", (m) => {
    const t = m.text().replace(/\x1b\[1C/g, " ").replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "");
    consoleLog.push(`${((Date.now() - t0) / 1000).toFixed(3)} ${t}`);
    if (/^\[harness\]|^\[vstloader\]|vsthost:|err:|error|panic/i.test(t) && !/callHandler/.test(t)) say({ console: t });
  });
  await page.goto(`${base}/harness.html`);
  await page.waitForFunction(() => window.vstloaderHarness, null, { timeout: 60000 });
  if (!(await page.evaluate(() => crossOriginIsolated))) throw new Error("the page is not cross-origin isolated");

  async function download(key) {
    const size = await page.evaluate((k) => window.vstloaderHarness.captureSize(k), key);
    const parts = [];
    for (let at = 0; at < size; at += 4 << 20) {
      parts.push(Buffer.from(await page.evaluate(([k, o]) => window.vstloaderHarness.chunk(k, o, 4 << 20), [key, at]), "base64"));
    }
    return Buffer.concat(parts);
  }

  for (const plugin of opt.plugin.split(",")) {
    if (!scenarios.has("song")) break;
    const file = `${plugin}.wclap.tar.gz`;
    // The binary the offline replay loads: <name>.dll, or <name>.vst3 for a "-vst3" bundle.
    const binary = plugin.endsWith("-vst3") ? `${plugin.slice(0, -5)}.vst3` : `${plugin}.dll`;
    const r = await page.evaluate(([f, s, c]) => window.vstloaderHarness.songScenario(f, s, c), [file, Number(opt.seconds), Number(opt.capture)]);
    say({ song: plugin, ...r, timeline: undefined });
    const dir = join(opt.out, `song-${plugin}`);
    mkdirSync(dir, { recursive: true });
    const header = Buffer.alloc(32);
    header.write("VSRP", 0, "ascii");
    header.writeUInt32LE(1, 4);
    header.writeUInt32LE(r.sampleRate, 8);
    header.writeUInt32LE(r.capture.block, 12);
    header.writeUInt32LE(r.capture.inPorts, 16);
    header.writeUInt32LE(r.capture.outPorts, 20);
    header.writeUInt32LE(r.capture.blocks, 24);
    header.writeUInt32LE(1, 28); // warm-up, as the bridge's LOAD does
    writeFileSync(join(dir, "capture.bin"), Buffer.concat([header, await download(`${file}:requests`)]));
    writeFileSync(join(dir, "realtime.f32"), await download(`${file}:output`));
    writeFileSync(join(dir, "realtime.json"), JSON.stringify({ plugin: binary, ...r }, null, 2));
    let identity;
    try {
      const output = execFileSync("node", [join(root, "tests/identity.mjs"), replayBase, dir, "--align", "1"], {
        encoding: "utf8",
        timeout: 1900_000,
      });
      identity = { pass: true, ...JSON.parse(output.trim().split("\n").pop()) };
    } catch (error) {
      const last = String(error.stdout ?? "").trim().split("\n").pop();
      identity = { pass: false, ...(last?.startsWith("{") ? JSON.parse(last) : { error: String(error.stderr || error.message).slice(-2000) }) };
    }
    say({ identity: plugin, ...identity });
    results[`song:${plugin}`] = { ...r, identity };
  }
  if (scenarios.has("companion")) {
    results.companion = await page.evaluate(() => window.vstloaderHarness.companionScenario());
    say({ companion: results.companion });
  }
  if (scenarios.has("null")) {
    results.null = await page.evaluate((s) => window.vstloaderHarness.nullScenario(s), Number(opt["null-seconds"]));
    say({ null: results.null });
  }
  if (scenarios.has("bzw")) {
    results.bzw = await page.evaluate(() => window.vstloaderHarness.bzwScenario());
    say({ bzw: results.bzw });
  }
  await new Promise((r) => setTimeout(r, 3000));
  await page.screenshot({ path: join(opt.out, "harness.png"), fullPage: true });
} finally {
  writeFileSync(join(opt.out, "results.json"), JSON.stringify(results, null, 2));
  writeFileSync(join(opt.out, "console.log"), consoleLog.join("\n"));
  await browser.close();
  for (const server of servers) server.kill();
}
