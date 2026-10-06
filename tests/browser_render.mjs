// Renders plugins in headless Chromium through the demo page, one emulator boot
// for all of them, and checks each result:
//   node tests/browser_render.mjs <base url> <screenshot dir> <plugin file>...
import { chromium } from "playwright";
import { readFileSync } from "node:fs";
import { basename } from "node:path";

const [base, shots, ...plugins] = process.argv.slice(2);
const companionPrimary = process.env.COMPANION_PRIMARY;
const companionDep = process.env.COMPANION_DEP;
// CHROMIUM=/path/to/chrome uses a preinstalled browser instead of Playwright's own.
const browser = await chromium.launch({
  args: ["--autoplay-policy=no-user-gesture-required"],
  executablePath: process.env.CHROMIUM || undefined,
});
const page = await browser.newPage({ viewport: { width: 1100, height: 1000 } });
page.on("pageerror", (e) => console.log("pageerror:", e.message));
await page.goto(`${base}/index.html`);
await page.waitForSelector("#plugin option", { state: "attached" });
let failures = 0;

if (companionPrimary && companionDep) {
  const primary = readFileSync(companionPrimary).toString("base64");
  const depBytes = readFileSync(companionDep).toString("base64");
  const depName = basename(companionDep);
  const result = await page.evaluate(async ({ primary, depName, depBytes }) => {
    const decode = (value) => Uint8Array.from(atob(value), (ch) => ch.charCodeAt(0));
    const runtime = window.vstloaderRuntime;
    if (!runtime) throw new Error("vstloader runtime diagnostics are unavailable on demo page");
    const first = await runtime.probeBinary(decode(primary), []);
    const second = await runtime.probeBinary(decode(primary), [{ name: depName, bytes: decode(depBytes) }]);
    return { first: first.probe, second: second.probe };
  }, { primary, depName, depBytes });
  const missing = result.first.missing.map((name) => name.toLowerCase());
  const ok = missing.length === 1 && missing[0] === "companion_dep.dll" && result.second.missing.length === 0;
  failures += ok ? 0 : 1;
  console.log(JSON.stringify({ companionProbe: true, ok, missing: result.first.missing, resolvedMissing: result.second.missing }));
}

for (const plugin of plugins) {
  await page.evaluate(() => { window.vstPocResult = undefined; });
  await page.selectOption("#plugin", plugin);
  const started = Date.now();
  await page.click("#render");
  const result = await page
    .waitForFunction(() => window.vstPocResult, null, { timeout: 25 * 60 * 1000, polling: 1000 })
    .then((h) => h.jsonValue());
  const r = result.report || {};
  const ok = Boolean(r.ok && r.peak > 0.01 && r.nonFinite === 0);
  failures += ok ? 0 : 1;
  console.log(JSON.stringify({
    plugin, ok, seconds: (Date.now() - started) / 1000, boot: result.bootSeconds, error: result.error,
    name: r.name, format: r.format, peak: r.peak, rms: r.rms, nonFinite: r.nonFinite,
    renderMs: r.renderMs, initMs: r.initMs, processMs: r.processMs, realtimeFactor: r.realtimeFactor,
    params: (r.params || []).length,
  }));
  if (shots) await page.screenshot({ path: `${shots}/${plugin.replace(/\W+/g, "_")}.png`, fullPage: true });
}
await browser.close();
process.exit(failures ? 1 : 0);
