export function assertX86Pe(bytes, fileName = "the file") {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.length < 0x40 || view.getUint16(0, true) !== 0x5a4d) throw new Error(`${fileName} is not a Windows binary`);
  const pe = view.getUint32(0x3c, true);
  if (pe + 6 > bytes.length || view.getUint32(pe, true) !== 0x4550) throw new Error(`${fileName} is not a Windows binary`);
  const machine = view.getUint16(pe + 4, true);
  if (machine === 0x8664) throw new Error(`${fileName} is a 64-bit binary; only 32-bit (x86) binaries run`);
  if (machine !== 0x14c) throw new Error(`${fileName} is not an x86 binary`);
}

export async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

export async function normalizeDependencies(dependencies = []) {
  const seen = new Set();
  const out = [];
  for (const dep of dependencies) {
    const name = String(dep?.name ?? "");
    if (!name || name !== name.replace(/^.*[\\/]/, "") || name.includes(":") || !/\.dll$/i.test(name) || /^plugin\.dll$/i.test(name))
      throw new Error(`invalid companion DLL name: ${name || "(empty)"}`);
    const key = name.toLowerCase();
    if (seen.has(key)) throw new Error(`duplicate companion DLL name: ${name}`);
    seen.add(key);
    const bytes = dep.bytes instanceof Uint8Array ? dep.bytes : new Uint8Array(dep.bytes ?? 0);
    assertX86Pe(bytes, name);
    const actual = await sha256Hex(bytes);
    if (dep.sha256 && String(dep.sha256).toLowerCase() !== actual) throw new Error(`dependency integrity error: ${name}`);
    out.push({ name, sha256: actual, bytes });
  }
  out.sort((a, b) => a.name.toLowerCase().localeCompare(b.name.toLowerCase()) || a.name.localeCompare(b.name));
  return out;
}
