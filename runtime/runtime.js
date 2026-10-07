// The vstloader runtime: the page a WebCLAP host loads for prometheos.runtime/1.
// It boots Boxedwine (multithreaded build, vsthost --bridge) in a nested
// frame, loads each plugin instance the host attaches into its own
// /dev/vstbridge channel, and starts two relay workers per instance that move
// blocks between the instance's channel (in the plugin module's shared memory)
// and the bridge channel (in Boxedwine's). Control (upload, load, state) runs
// here, off the audio path.
//
// Host messages (protocol RUNTIME): attach {instance, memory}, frame
// {instance, data}, detach {instance}; and, for wrapping a .dll, describe
// {requestId, data}. Frames: wclap/vstloader_protocol.h (protocol.js).
import { BridgeRegion, ControlClient, VSTB } from "./vstbridge.js";
import { HELLO_BYTES, Op, frame, parseDependency, parseHello } from "./protocol.js";
import { normalizeDependencies, sha256Hex } from "./dependencies.js";

const RUNTIME = "prometheos-runtime/1";
const C = VSTB.constants;
const E = VSTB.enums;
const CTL = VSTB.channel_ctl;
/** Bridge channels for instances; the last one describes binaries for the wrapper. */
const DESCRIBE_CHANNEL = C.MAX_CHANNELS;
const UPLOAD_PIECE = 1 << 20;
const params = new URLSearchParams(location.search);

const state = {
  phase: "idle",
  bootSeconds: 0,
  error: "",
  log: [],
};
let emulator = null; // { region, control }
let booting = null;
const uploaded = new Map(); // sha256 -> Promise
const instances = new Map(); // instance id -> Instance
const channelsInUse = new Set();

function log(message, level = "info", instance) {
  state.log.push(`${new Date().toISOString().slice(11, 23)} ${message}`);
  if (state.log.length > 200) state.log.shift();
  console[level === "error" ? "error" : "log"](`[vstloader] ${message}`);
  toHost({ type: "log", level, message, ...(instance ? { instance } : {}) });
}

function toHost(message, transfer = []) {
  if (parent !== window) parent.postMessage({ protocol: RUNTIME, ...message }, location.origin, transfer);
}

// ---- the emulator -----------------------------------------------------------------

function boot() {
  booting ??= bootOnce().catch((error) => {
    booting = null;
    state.phase = "failed";
    state.error = String(error?.message ?? error);
    throw error;
  });
  return booting;
}

async function bootOnce() {
  state.phase = "booting";
  const started = performance.now();
  const query = new URLSearchParams({
    root: "vstpoc-prefix",
    overlay: "TinyCore15Wine11.0",
    app: "vstpoc.zip",
    p: "vsthost.exe",
    args: "--bridge",
    storage: "memory",
    sound: "false",
    "server-jit-cache": "false",
    ...(params.get("jit-record") === "true" ? { "jit-record": "true" } : {}),
  });
  const base = params.get("boxedwine") ?? "../boxedwine/";
  const frameEl = document.createElement("iframe");
  frameEl.title = "Boxedwine";
  Object.assign(frameEl.style, { position: "fixed", width: "0", height: "0", border: "0", visibility: "hidden" });
  frameEl.src = `${base.replace(/\/?$/, "/")}boxedwine.html?${query}`;
  document.body.appendChild(frameEl);
  for (;;) {
    if (performance.now() - started > 15 * 60 * 1000) throw new Error("Boxedwine did not start in time");
    const mod = frameEl.contentWindow?.Module;
    if (mod && typeof mod._vstbridge_region === "function" && mod.HEAPU8) {
      const buffer = mod.HEAPU8.buffer;
      if (Object.prototype.toString.call(buffer) !== "[object SharedArrayBuffer]") {
        throw new Error("Boxedwine's memory is not shared: the page needs cross-origin isolation");
      }
      const region = new BridgeRegion(buffer, mod._vstbridge_region());
      if (region.valid && region.serving) {
        const control = new ControlClient(region);
        const pong = await control.request(E.OP_PING, 0, "", 60000);
        if (pong.status !== E.STATUS_OK) throw new Error("vsthost --bridge did not answer");
        emulator = { region, control };
        state.phase = "ready";
        state.bootSeconds = (performance.now() - started) / 1000;
        log(`Boxedwine ready after ${state.bootSeconds.toFixed(1)} s`);
        return emulator;
      }
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
}

function guestDir(sha256) { return `C:\\winvst\\${sha256}`; }
function guestPath(sha256) { return `${guestDir(sha256)}\\plugin.dll`; }

/** Writes a plugin binary into the guest once per session (PUT_FILE pieces). */
function upload(sha256, binary, path = guestPath(sha256)) {
  let done = uploaded.get(sha256);
  if (!done) {
    done = (async () => {
      const { control } = await boot();
      const pathBytes = new TextEncoder().encode(path);
      for (let offset = 0; offset < binary.length || offset === 0; offset += UPLOAD_PIECE) {
        const piece = binary.subarray(offset, offset + UPLOAD_PIECE);
        const message = new Uint8Array(12 + pathBytes.length + piece.length);
        const view = new DataView(message.buffer);
        view.setUint32(0, offset, true);
        view.setUint32(4, binary.length, true);
        view.setUint32(8, pathBytes.length, true);
        message.set(pathBytes, 12);
        message.set(piece, 12 + pathBytes.length);
        const reply = await control.request(E.OP_PUT_FILE, 0, message);
        if (reply.status !== E.STATUS_OK) throw new Error(`uploading the plugin failed: ${reply.text}`);
        if (binary.length === 0) break;
      }
    })();
    uploaded.set(sha256, done);
    done.catch(() => uploaded.delete(sha256));
  }
  return done;
}

async function loadInto(channel, sha256, sampleRate, block) {
  const { control } = await boot();
  const reply = await control.request(
    E.OP_LOAD,
    channel,
    `rate=${sampleRate} block=${block} warmup=1 path=${guestPath(sha256)}`,
  );
  if (reply.status !== E.STATUS_OK) throw new Error(`the plugin did not load: ${reply.text}`);
  return JSON.parse(reply.text);
}

function freeChannel() {
  for (let channel = 1; channel < DESCRIBE_CHANNEL; channel++) {
    if (!channelsInUse.has(channel)) {
      channelsInUse.add(channel);
      return channel;
    }
  }
  throw new Error(`at most ${DESCRIBE_CHANNEL - 1} Windows plugins can run at once`);
}

// ---- instances ----------------------------------------------------------------------

class Instance {
  constructor(id, memory) {
    this.id = id;
    this.memory = memory;
    this.hello = null;
    this.channel = 0;
    this.describe = null;
    this.relays = [];
    this.stop = null;
    this.closed = false;
    this.queue = Promise.resolve();
    this.loadedAt = 0;
    this.dependencies = new Map();
    this.dependencyError = null;
  }

  /** Runs control work for this instance one step at a time. */
  run(what, body) {
    this.queue = this.queue.then(async () => {
      if (this.closed) return;
      try {
        await body();
      } catch (error) {
        const message = `${what}: ${error?.message ?? error}`;
        log(message, "error", this.id);
        this.send(Op.ERROR, new TextEncoder().encode(message));
      }
    });
    return this.queue;
  }

  send(op, body = new Uint8Array(0)) {
    const data = frame(op, body);
    toHost({ type: "frame", instance: this.id, data: data.buffer }, [data.buffer]);
  }

  receive(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const op = view.getUint32(0, true);
    const body = bytes.subarray(4);
    switch (op) {
      case Op.DEPENDENCY:
        return this.run("receiving a companion dependency", () => this.addDependency(body));
      case Op.HELLO:
        return this.run("loading the plugin", () => this.start(body));
      case Op.SET_STATE:
        return this.run("restoring the plugin's state", () => this.setState(body.slice()));
      case Op.GET_STATE:
        return this.run("reading the plugin's state", () => this.reportState());
      case Op.BYE:
        return this.close();
      default:
        log(`unknown frame op ${op}`, "error", this.id);
    }
  }

  async addDependency(body) {
    if (this.dependencyError) throw new Error(this.dependencyError);
    try {
      const dep = parseDependency(body);
      const key = dep.name.toLowerCase();
      if (this.dependencies.size >= 64 && !this.dependencies.has(key)) throw new Error("too many companion dependencies");
      if (this.dependencies.has(key)) throw new Error(`duplicate companion dependency: ${dep.name}`);
      const actual = await sha256Hex(dep.bytes);
      if (actual !== dep.sha256) throw new Error(`dependency integrity error: ${dep.name}`);
      this.dependencies.set(key, dep);
    } catch (error) {
      this.dependencies.clear();
      this.dependencyError = String(error?.message ?? error);
      throw error;
    }
  }

  async start(body) {
    if (!this.memory) throw new Error("the plugin's memory is not shared (it needs a threads build)");
    if (this.dependencyError) {
      const error = this.dependencyError;
      this.dependencyError = null;
      this.dependencies.clear();
      throw new Error(`invalid companion dependency batch: ${error}`);
    }
    // A HELLO consumes the dependency frames that preceded it. Snapshot and
    // clear them before validation/loading so a failed HELLO can be retried
    // with the shim's freshly re-sent dependency batch.
    const dependencies = [...this.dependencies.values()];
    this.dependencies.clear();
    if (body.length < HELLO_BYTES) throw new Error("short hello");
    const hello = parseHello(body);
    if (body.length !== HELLO_BYTES + hello.dllSize) throw new Error("malformed hello");
    const binary = body.subarray(HELLO_BYTES);
    if (!/^[0-9a-f]{64}$/.test(hello.sha256)) throw new Error("invalid plugin sha256");
    const actual = await sha256Hex(binary);
    if (actual !== hello.sha256) throw new Error("plugin integrity error");
    if (this.channel) await this.unload();
    this.hello = hello;
    const started = performance.now();
    await upload(hello.sha256, binary);
    for (const dep of dependencies)
      await upload(`${hello.sha256}:${dep.name.toLowerCase()}:${dep.sha256}`, dep.bytes, `${guestDir(hello.sha256)}\\${dep.name}`);
    this.channel = freeChannel();
    this.describe = await loadInto(this.channel, hello.sha256, hello.sampleRate, hello.blockFrames);
    this.loadedAt = performance.now();
    if (this.pendingState) {
      const reply = await emulator.control.request(E.OP_SET_STATE, this.channel, this.pendingState);
      if (reply.status !== E.STATUS_OK) throw new Error(reply.text);
      this.pendingState = null;
    }
    this.attachStream();
    log(`${this.describe.name}: loaded on channel ${this.channel} in ${((this.loadedAt - started) / 1000).toFixed(1)} s`, "info", this.id);
    await this.reportState();
    this.dependencyError = null;
  }

  /** Resets the plugin's channel, starts the relays, and lets the plugin start its stream. */
  attachStream() {
    const hello = this.hello;
    const buffer = this.memory.buffer;
    const pctl = new Int32Array(buffer, hello.channelBase, C.CHANNEL_CTL_BYTES / 4);
    for (const field of ["requestSeq", "responseSeq", "underruns", "skipped", "maxProcessUs", "lastProcessUs"]) {
      Atomics.store(pctl, CTL[field] / 4, 0);
    }
    for (let i = 0; i < C.SLOTS; i++) Atomics.store(pctl, CTL.doneBlock / 4 + i, 0);
    Atomics.store(pctl, CTL.blockFrames / 4, hello.blockFrames);
    Atomics.store(pctl, CTL.sampleRate / 4, hello.sampleRate);
    Atomics.store(pctl, CTL.inPorts / 4, this.describe.inPorts.length);
    Atomics.store(pctl, CTL.outPorts / 4, this.describe.outPorts.length);
    Atomics.store(pctl, CTL.pluginLatency / 4, Math.max(0, this.describe.latency | 0));
    const bridgeBase = emulator.region.base + C.CHANNELS_OFFSET + (this.channel - 1) * C.CHANNEL_BYTES;
    this.stop = new SharedArrayBuffer(4);
    const job = {
      plugin: { buffer, base: hello.channelBase },
      bridge: { buffer: emulator.region.buffer, base: bridgeBase },
      stop: this.stop,
      block: hello.blockFrames,
      inPorts: Math.min(hello.inPorts, this.describe.inPorts.length),
      outPorts: Math.min(hello.outPorts, this.describe.outPorts.length),
    };
    this.relays = ["forward", "backward"].map((mode) => {
      const worker = new Worker(new URL("./relay-worker.js", import.meta.url), { type: "module", name: `relay ${mode} ${this.id}` });
      worker.postMessage({ ...job, mode });
      return worker;
    });
    Atomics.store(pctl, CTL.state / 4, E.STATE_READY);
    Atomics.add(pctl, CTL.generation / 4, 1);
  }

  async setState(bytes) {
    if (!this.channel) {
      this.pendingState = bytes;
      return;
    }
    const reply = await emulator.control.request(E.OP_SET_STATE, this.channel, bytes);
    if (reply.status !== E.STATUS_OK) throw new Error(reply.text);
    await this.reportState();
  }

  async reportState() {
    if (!this.channel) return;
    const reply = await emulator.control.request(E.OP_GET_STATE, this.channel);
    if (reply.status !== E.STATUS_OK) throw new Error(reply.text);
    this.send(Op.STATE, reply.bytes);
  }

  stats() {
    if (!this.hello || !this.memory) return null;
    const pctl = new Int32Array(this.memory.buffer, this.hello.channelBase, C.CHANNEL_CTL_BYTES / 4);
    const word = (field) => Atomics.load(pctl, CTL[field] / 4);
    return {
      channel: this.channel,
      channelBase: this.hello.channelBase,
      name: this.describe?.name ?? "",
      requestSeq: word("requestSeq"),
      responseSeq: word("responseSeq"),
      underruns: word("underruns"),
      skipped: word("skipped"),
      maxProcessUs: word("maxProcessUs"),
      generation: word("generation"),
    };
  }

  async unload() {
    if (this.stop) Atomics.store(new Int32Array(this.stop), 0, 1);
    for (const worker of this.relays) setTimeout(() => worker.terminate(), 500);
    this.relays = [];
    if (this.hello && this.memory) {
      const pctl = new Int32Array(this.memory.buffer, this.hello.channelBase, C.CHANNEL_CTL_BYTES / 4);
      Atomics.store(pctl, CTL.state / 4, E.STATE_CLOSED);
    }
    const channel = this.channel;
    this.channel = 0;
    if (channel && emulator) {
      await emulator.control.request(E.OP_UNLOAD, channel).catch(() => undefined);
      channelsInUse.delete(channel);
    }
  }

  close() {
    if (this.closed) return this.queue;
    const done = this.run("unloading the plugin", () => this.unload());
    this.closed = true;
    instances.delete(this.id);
    return done;
  }
}

// ---- describe (for wrapping a .dll) --------------------------------------------------

async function probeBinary(binary, dependencies = []) {
  const sha256 = await sha256Hex(binary);
  const deps = await normalizeDependencies(dependencies);
  await upload(sha256, binary);
  for (const dep of deps) await upload(`${sha256}:${dep.name.toLowerCase()}:${dep.sha256}`, dep.bytes, `${guestDir(sha256)}\\${dep.name}`);
  const { control } = await boot();
  const reply = await control.request(E.OP_PROBE_IMPORTS, 0, guestPath(sha256));
  if (reply.status !== E.STATUS_OK) throw new Error(`probing plugin imports failed: ${reply.text}`);
  return { sha256, dependencies: deps, probe: JSON.parse(reply.text) };
}

async function describeBinary(binary, dependencies = []) {
  const result = await probeBinary(binary, dependencies);
  if (result.probe.missing.length) throw new Error(`missing-direct-dependency:${result.probe.missing.join(",")}`);
  const describe = await loadInto(DESCRIBE_CHANNEL, result.sha256, 48000, 256);
  await emulator.control.request(E.OP_UNLOAD, DESCRIBE_CHANNEL).catch(() => undefined);
  return { sha256: result.sha256, describe };
}

// ---- host messages -----------------------------------------------------------------------

window.addEventListener("message", (event) => {
  if (event.source !== parent || event.origin !== location.origin) return;
  const message = event.data;
  if (message?.protocol !== RUNTIME) return;
  switch (message.type) {
    case "attach": {
      if (!instances.has(message.instance)) instances.set(message.instance, new Instance(message.instance, message.memory));
      void boot().catch((error) => log(`Boxedwine failed: ${error.message}`, "error", message.instance));
      break;
    }
    case "frame":
      instances.get(message.instance)?.receive(new Uint8Array(message.data));
      break;
    case "detach":
      void instances.get(message.instance)?.close();
      break;
    case "describe":
      describeBinary(new Uint8Array(message.data)).then(
        (result) => toHost({ type: "described", requestId: message.requestId, ...result }),
        (error) => toHost({ type: "described", requestId: message.requestId, error: String(error?.message ?? error) }),
      );
      break;
  }
});

/** Diagnostics for tests and the wrapper page. */
window.vstloaderRuntime = {
  state,
  boot,
  describeBinary,
  probeBinary,
  instances: () => [...instances.values()].map((instance) => ({ id: instance.id, ...instance.stats() })),
  /** An instance's channel in its plugin's memory (tests capture requests from it). */
  channel: (id) => {
    const instance = instances.get(id);
    return instance?.hello && instance.memory ? { buffer: instance.memory.buffer, base: instance.hello.channelBase } : null;
  },
};

toHost({ type: "ready" });
if (params.get("boot") === "1") void boot();
