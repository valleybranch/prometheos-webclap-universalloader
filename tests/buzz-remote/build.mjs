// Builds the buzz-remote harness into tests/buzz-remote/out/ against a
// buzz-remote checkout (its AudioWorklet bundle and engine modules), with the
// wrapped plugins and this repository's built site mounted at /vstloader/
// (runtime/ and boxedwine/, the multithreaded build):
//   node tests/buzz-remote/build.mjs --buzz <prometheos-apps>/apps/buzz-remote
//        [--site dist-mt] [--plugins <dir with *.wclap.tar.gz>]
// esbuild is taken from the buzz-remote checkout's node_modules.
import { cpSync, existsSync, mkdirSync, readdirSync, rmSync, symlinkSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../..");
const opt = { buzz: "", site: "dist-mt", plugins: join(root, "build", "wraps") };
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i += 2) opt[args[i].replace(/^--/, "")] = args[i + 1];
if (!opt.buzz) throw new Error("--buzz <prometheos-apps>/apps/buzz-remote is required");
const buzz = resolve(opt.buzz);
const require = createRequire(join(root, "package.json"));
const { build } = require("esbuild");

const out = join(here, "out");
rmSync(out, { recursive: true, force: true });
mkdirSync(join(out, "plugins"), { recursive: true });
const common = { bundle: true, format: "esm", target: "es2022", alias: { "@": join(buzz, "src") }, logLevel: "warning" };
await build({ ...common, entryPoints: [join(here, "harness.ts")], outfile: join(out, "harness.js") });
await build({ ...common, entryPoints: [join(buzz, "src/engine/buzz-worklet.ts")], outfile: join(out, "buzz-worklet.js") });
cpSync(join(here, "harness.html"), join(out, "harness.html"));
cpSync(join(here, "recorder-worklet.js"), join(out, "recorder-worklet.js"));
for (const name of existsSync(opt.plugins) ? readdirSync(opt.plugins) : []) {
  if (name.endsWith(".wclap.tar.gz")) cpSync(join(opt.plugins, name), join(out, "plugins", name));
}
const site = resolve(root, opt.site);
if (!existsSync(join(site, "runtime", "index.html"))) throw new Error(`no runtime in ${site} (build.sh with DIST=${opt.site})`);
symlinkSync(site, join(out, "vstloader"));
console.log(`built ${out}`);
