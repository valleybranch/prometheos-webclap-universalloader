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

export function frame(op, body = new Uint8Array(0)) {
  const out = new Uint8Array(4 + body.length);
  new DataView(out.buffer).setUint32(0, op, true);
  out.set(body, 4);
  return out;
}
