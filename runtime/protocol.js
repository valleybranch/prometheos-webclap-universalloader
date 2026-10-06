// JavaScript twin of wclap/vstloader_protocol.h: frames between the vstloader
// WebCLAP and its runtime. Little-endian; every frame starts with a uint32 op.

export const Op = Object.freeze({
  HELLO: 1,
  SET_STATE: 2,
  GET_STATE: 3,
  BYE: 4,
  DEPENDENCY: 5,
  STATE: 101,
  ERROR: 102,
  RESEND: 103,
});

/** sizeof(vl_hello): nine uint32, sha256[64], reserved[3]. */
export const HELLO_BYTES = 9 * 4 + 64 + 3 * 4;

export function parseHello(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, HELLO_BYTES);
  const u32 = (i) => view.getUint32(i * 4, true);
  return {
    channelBase: u32(0),
    channelIndex: u32(1),
    sampleRate: u32(2),
    blockFrames: u32(3),
    bridgeLatency: u32(4),
    pluginLatency: u32(5),
    inPorts: u32(6),
    outPorts: u32(7),
    dllSize: u32(8),
    sha256: new TextDecoder().decode(bytes.subarray(36, 100)),
  };
}

const DEPENDENCY_HEADER_BYTES = 40;

function validDependencyName(name) {
  return name.length > 0 && name.length <= 0xffff && !/[\\/:]/.test(name) && /\.dll$/i.test(name) && !/^plugin\.dll$/i.test(name);
}

export function buildDependency({ name, sha256, bytes }) {
  if (!validDependencyName(name)) throw new Error(`invalid dependency name: ${name || "(empty)"}`);
  if (!/^[0-9a-f]{64}$/i.test(sha256)) throw new Error(`invalid dependency sha256: ${sha256}`);
  const nameBytes = new TextEncoder().encode(name);
  if (nameBytes.length !== name.length) throw new Error(`invalid dependency name: ${name}`);
  bytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const out = new Uint8Array(DEPENDENCY_HEADER_BYTES + nameBytes.length + bytes.length);
  const view = new DataView(out.buffer);
  view.setUint16(0, nameBytes.length, true);
  view.setUint16(2, 0, true);
  view.setUint32(4, bytes.length, true);
  for (let i = 0; i < 32; i++) out[8 + i] = parseInt(sha256.slice(i * 2, i * 2 + 2), 16);
  out.set(nameBytes, DEPENDENCY_HEADER_BYTES);
  out.set(bytes, DEPENDENCY_HEADER_BYTES + nameBytes.length);
  return out;
}

export function parseDependency(body) {
  if (body.length < DEPENDENCY_HEADER_BYTES) throw new Error("short dependency frame");
  const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
  const nameBytes = view.getUint16(0, true);
  const reserved = view.getUint16(2, true);
  const dataBytes = view.getUint32(4, true);
  if (reserved !== 0 || nameBytes === 0 || DEPENDENCY_HEADER_BYTES + nameBytes + dataBytes !== body.length)
    throw new Error("malformed dependency frame");
  const rawName = body.subarray(DEPENDENCY_HEADER_BYTES, DEPENDENCY_HEADER_BYTES + nameBytes);
  const name = new TextDecoder("utf-8", { fatal: true }).decode(rawName);
  if (!validDependencyName(name) || new TextEncoder().encode(name).length !== nameBytes)
    throw new Error(`invalid dependency name: ${name}`);
  const sha256 = [...body.subarray(8, 40)].map((b) => b.toString(16).padStart(2, "0")).join("");
  return { name, sha256, bytes: body.subarray(DEPENDENCY_HEADER_BYTES + nameBytes).slice() };
}

export function frame(op, body = new Uint8Array(0)) {
  const out = new Uint8Array(4 + body.length);
  new DataView(out.buffer).setUint32(0, op, true);
  out.set(body, 4);
  return out;
}
