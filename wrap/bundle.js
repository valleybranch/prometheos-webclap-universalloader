import { assertX86Pe, normalizeDependencies } from "../runtime/dependencies.js";

// Universal Windows-plugin WebCLAP bundle builder. VST2/VST3 retain the
// original vstloader descriptor; Buzz DLLs add their raw machine metadata while
// using the same runtime and shared Boxedwine instance.

export function checkBinary(bytes, fileName = "") {
  assertX86Pe(bytes, fileName || "the file");
  return /\.vst3$/i.test(fileName) ? "vst3" : "windows-dll";
}

const clean = (text) => String(text ?? "").replace(/[\t\r\n]/g, " ").trim();

export function descriptor({ sha256, describe, runtime, fileName = "", dependencies = [] }) {
  const stem = fileName.replace(/^.*[\\/]/, "").replace(/\.(dll|vst3)$/i, "");
  const version = describe.versionString || (describe.vendorVersion ? String(describe.vendorVersion) : "") || "1.0.0";
  const isBuzz = describe.format === "buzz";
  const buzzType = (n) => ["note", "switch", "byte", "word"][n] || "byte";
  const buzzLayout = isBuzz ? JSON.stringify({
    name: describe.name || stem || "Buzz machine",
    author: describe.vendor || "",
    type: describe.synth ? "generator" : "effect",
    minTracks: describe.minTracks | 0,
    maxTracks: describe.maxTracks | 0,
    globals: (describe.params || []).map((p) => ({
      type: buzzType(p.buzzType), name: p.name || "", description: p.description || p.display || "",
      minValue: p.minValue | 0, maxValue: p.maxValue | 0, noValue: p.noValue | 0,
      flags: p.flags | 0, defValue: p.defValue | 0,
    })),
    tracks: (describe.trackParams || []).map((p) => ({
      type: buzzType(p.buzzType), name: p.name || "", description: p.description || "",
      minValue: p.minValue | 0, maxValue: p.maxValue | 0, noValue: p.noValue | 0,
      flags: p.flags | 0, defValue: p.defValue | 0,
    })),
    attributes: describe.attributes || [],
  }) : "";
  const lines = [
    `id=prometheos.${isBuzz ? "buzzloader" : "vstloader"}.${sha256.slice(0, 16)}`,
    `name=${clean(describe.name) || stem || "Windows plugin"}`,
    `vendor=${clean(describe.vendor)}`,
    `version=${clean(version)}`,
    `format=${describe.format || "vst2"}`,
    `sha256=${sha256}`,
    `runtime=${runtime}`,
    "dll=plugin.dll",
    `synth=${describe.synth ? 1 : 0}`,
    `inPorts=${describe.inPorts.length}`,
    `outPorts=${describe.outPorts.length}`,
    `latency=${Math.max(0, describe.latency | 0)}`,
    "bridgeLatency=2048",
    "block=256",
    ...dependencies.map((d) => `dependency=${d.name}\t${d.sha256}\tresources/deps/${d.name}`),
    ...(isBuzz ? [
      `buzzName=${clean(describe.name)}`,
      `buzzLayout=${buzzLayout}`,
      `minTracks=${describe.minTracks | 0}`,
      `maxTracks=${describe.maxTracks | 0}`,
      ...((describe.trackParams || []).map((p) => `buzzTrack=${clean(p.name)}\t${p.buzzType}\t${p.minValue}\t${p.maxValue}\t${p.noValue}\t${p.defValue}`)),
      ...((describe.attributes || []).map((a) => `buzzAttribute=${clean(a.name)}\t${a.minValue}\t${a.maxValue}\t${a.defValue}`)),
    ] : []),
    ...describe.params.map((p) => `param=${clean(p.name)}\t${clean(p.label)}\t${Number(p.value).toFixed(6)}`),
  ];
  return `${lines.join("\n")}\n`;
}

export function tar(files) {
  const encoder = new TextEncoder();
  let size = 1024;
  for (const { bytes } of files) size += 512 + Math.ceil(bytes.length / 512) * 512;
  const out = new Uint8Array(size);
  let offset = 0;
  const put = (text, at, length) => out.set(encoder.encode(text).subarray(0, length), offset + at);
  const octal = (value, length) => value.toString(8).padStart(length - 1, "0");
  for (const { name, bytes } of files) {
    if (encoder.encode(name).length >= 100) throw new Error(`tar name too long: ${name}`);
    put(name, 0, 100); put(octal(0o644, 8), 100, 8); put(octal(0, 8), 108, 8); put(octal(0, 8), 116, 8);
    put(octal(bytes.length, 12), 124, 12); put(octal(0, 12), 136, 12); put("        ", 148, 8);
    out[offset + 156] = 0x30; put("ustar\u000000", 257, 8);
    let sum = 0; for (let i = 0; i < 512; i++) sum += out[offset + i];
    put(`${octal(sum, 7)}\u0000 `, 148, 8);
    out.set(bytes, offset + 512);
    offset += 512 + Math.ceil(bytes.length / 512) * 512;
  }
  return out;
}

export async function gzip(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream("gzip"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

export async function buildBundle({ wasm, plugin, describe, sha256, runtime, fileName, dependencies = [] }) {
  const deps = await normalizeDependencies(dependencies);
  const text = new TextEncoder().encode(descriptor({ sha256, describe, runtime, fileName, dependencies: deps }));
  return gzip(tar([
    { name: "module.wasm", bytes: wasm },
    { name: "resources/plugin.dll", bytes: plugin },
    ...deps.map((d) => ({ name: `resources/deps/${d.name}`, bytes: d.bytes })),
    { name: "resources/vstloader.txt", bytes: text },
  ]));
}

export function bundleFileName(describe, fileName = "") {
  const stem = clean(describe.name) || fileName.replace(/^.*[\\/]/, "").replace(/\.(dll|vst3)$/i, "") || "plugin";
  return `${stem.replace(/[^A-Za-z0-9._ -]+/g, "_").trim() || "plugin"}.wclap.tar.gz`;
}
