// The vstloader WebCLAP inside buzz-remote's real engine, in a browser page:
// buzz-remote's AudioWorklet bundle, its WebCLAP install (parseWebClapArchive),
// its PluginRuntimeHost (prometheos.runtime/1) and this repository's runtime
// page. The page sends the worklet what buzz-remote's EngineBridge sends
// (graph, song, sequence, transport); EngineBridge and the React app are not
// loaded. Built against a buzz-remote checkout by build.mjs, driven by run.mjs.
import { parseWebClapArchive } from "@/engine/webclap/archive";
import { makeWebClapBackendSource } from "@/engine/webclap/packageSource";
import { PluginRuntimeHost } from "@/engine/webclap/runtimeHost";
import type { WebClapPackage } from "@/engine/webclap/types";
import type { FromWorkletMessage, GraphMachine, ToWorkletMessage } from "@/engine/protocol";
import { buildSchedule } from "@/lib/schedule";
import { machineClassById, replaceWebVstMachineClasses } from "@/lib/machine-classes";
import { DeterministicProjectCodec } from "@/persistence/projectCodec";
import { MAX_BUSSES, MPF_STATE, type Connection, type Machine, type MachineParameter, type Pattern, type Song } from "@/types/song";

const RATE = 48000;
const L = 2048;
const B = 256;
// vstbridge_abi.h (one channel; the plugin's channel has the device's layout).
const CTL = { requestSeq: 8, responseSeq: 12, underruns: 44, skipped: 48, maxProcessUs: 56, turnUs: 128 };
const SLOTS = 64;
const SLOTS_OFFSET = 4096;
const REQUEST_BYTES = 2048;
const RING_FRAMES = 8192;
const IN_RINGS_OFFSET = SLOTS_OFFSET + SLOTS * REQUEST_BYTES;
const RUNTIME_URI = "/vstloader/runtime/index.html";

const out = document.getElementById("log")!;
function log(message: string): void {
  out.textContent += `${message}\n`;
  console.log(`[harness] ${message}`);
}

interface Engine {
  ctx: AudioContext;
  node: AudioWorkletNode;
  recorder: AudioWorkletNode;
  runtimes: PluginRuntimeHost;
  send(message: ToWorkletMessage, transfer?: Transferable[]): void;
  request(machineId: string, bytes: Uint8Array): Promise<Uint8Array>;
  captureData(machineId: string): Promise<Uint8Array | null>;
  errors: string[];
}

let engine: Engine | null = null;
const installed: WebClapPackage[] = [];

async function startEngine(): Promise<Engine> {
  if (engine) return engine;
  const ctx = new AudioContext({ sampleRate: RATE, latencyHint: "interactive" });
  await ctx.audioWorklet.addModule("buzz-worklet.js");
  await ctx.audioWorklet.addModule("recorder-worklet.js");
  const node = new AudioWorkletNode(ctx, "buzz-processor", {
    numberOfInputs: 0,
    numberOfOutputs: MAX_BUSSES,
    outputChannelCount: Array.from({ length: MAX_BUSSES }, () => 2),
  });
  const recorder = new AudioWorkletNode(ctx, "harness-recorder", {
    numberOfInputs: 1,
    numberOfOutputs: 1,
    channelCount: 2,
    channelCountMode: "explicit",
  });
  node.connect(recorder, 0, 0);
  recorder.connect(ctx.destination);
  const errors: string[] = [];
  const send = (message: ToWorkletMessage, transfer: Transferable[] = []) => node.port.postMessage(message, transfer);
  let nextId = 1;
  const pending = new Map<number, { resolve: (value: unknown) => void; reject: (error: Error) => void }>();
  const request = (machineId: string, bytes: Uint8Array) =>
    new Promise<Uint8Array>((resolve, reject) => {
      const requestId = nextId++;
      pending.set(requestId, { resolve: resolve as (value: unknown) => void, reject });
      const data = bytes.slice().buffer;
      send({ type: "machine-message", requestId, machineId, data }, [data]);
    });
  const captures = new Map<number, (data: Uint8Array | null) => void>();
  const captureData = (machineId: string) =>
    new Promise<Uint8Array | null>((resolve) => {
      const captureId = nextId++;
      captures.set(captureId, resolve);
      send({ type: "machine-data", machineId, revision: 0, data: new ArrayBuffer(0), captureId });
    });
  const runtimes = new PluginRuntimeHost({
    request,
    onError: (machineId, message) => errors.push(`${machineId}: ${message}`),
    onLog: (message) => log(`runtime: ${message}`),
  });
  node.port.onmessage = (e: MessageEvent<FromWorkletMessage>) => {
    const m = e.data;
    if (m.type === "machine-message") {
      const waiter = pending.get(m.requestId);
      pending.delete(m.requestId);
      if (m.error !== undefined || !m.data) waiter?.reject(new Error(m.error ?? "no reply"));
      else waiter?.resolve(new Uint8Array(m.data));
    } else if (m.type === "machine-runtime") {
      if (m.uri) runtimes.attach(m.machineId, m.instance, m.uri, m.memory ?? null);
      else runtimes.detach(m.machineId, m.instance);
    } else if (m.type === "machine-data" && "captureId" in m) {
      captures.get(m.captureId)?.(m.data ? new Uint8Array(m.data) : null);
      captures.delete(m.captureId);
    } else if (m.type === "machine-error") {
      errors.push(`${m.machineId}: ${m.message}`);
    } else if (m.type === "backend-status" && !m.ok) {
      errors.push(`backend ${m.backendId}: ${m.message}`);
    }
  };
  await ctx.resume();
  engine = { ctx, node, recorder, runtimes, send, request, captureData, errors };
  return engine;
}

/** The runtime page's diagnostics (same origin). */
function runtime(): {
  state: { phase: string; bootSeconds: number };
  instances(): Array<Record<string, number | string>>;
  channel(id: string): { buffer: SharedArrayBuffer; base: number } | null;
} | null {
  const frame = document.querySelector<HTMLIFrameElement>('iframe[title="Plugin runtime"]');
  return (frame?.contentWindow as unknown as { vstloaderRuntime?: ReturnType<typeof runtime> })?.vstloaderRuntime ?? null;
}

function runtimeInstance(machineId: string) {
  return runtime()?.instances().find((i) => String(i.id).startsWith(`${machineId}#`) && Number(i.generation) > 0) ?? null;
}

/** Installs a wrapped .wclap.tar.gz as buzz-remote's package layer does. */
async function install(file: string): Promise<WebClapPackage> {
  const e = await startEngine();
  const bytes = new Uint8Array(await (await fetch(`plugins/${file}`)).arrayBuffer());
  const started = performance.now();
  const pkg = await parseWebClapArchive(bytes, { kind: "local", location: file });
  installed.push(pkg);
  replaceWebVstMachineClasses(
    installed.flatMap((p) =>
      p.manifest.classes.map((cls) => ({
        machineClass: structuredClone(cls.descriptor),
        packageId: p.manifest.packageId,
        archiveSha256: p.archiveSha256,
        classUid: cls.classUid,
        availability: "live" as const,
        format: "webclap" as const,
      })),
    ),
    [],
  );
  const source = makeWebClapBackendSource(pkg);
  const fetched = new Map<string, ArrayBuffer>();
  for (const id of source.artifacts) fetched.set(id, await source.loadArtifact(id));
  e.send({ type: "backend", payload: { ...source.pack(fetched), sourceId: source.id } });
  log(`installed ${file}: ${pkg.manifest.classes[0]!.classId} in ${((performance.now() - started) / 1000).toFixed(1)} s`);
  return pkg;
}

// ---- songs ---------------------------------------------------------------------------

function buzzNote(key: number): number {
  return ((Math.floor(key / 12) - 1) << 4) | ((key % 12) + 1);
}

function machine(id: string, classId: string, tracks: number, x: number): Machine {
  const cls = machineClassById(classId);
  if (!cls) throw new Error(`no class ${classId}`);
  return {
    id,
    classId,
    name: cls.name,
    x,
    y: 100,
    tracks,
    globalValues: cls.globalParameters.map((p) => p.defValue),
    trackValues: Array.from({ length: tracks }, () => cls.trackParameters.map((p) => p.noValue)),
    patterns: [],
    muted: false,
    soloed: false,
  };
}

const MASTER: Machine = {
  id: "master", classId: "master", name: "Master", x: 400, y: 100, tracks: 0,
  globalValues: [16384, 126, 4], trackValues: [], patterns: [], muted: false, soloed: false,
};

function edge(id: string, from: string, to: string): Connection {
  return { id, from, to, amp: 16384, pan: 8192 };
}

/** Four 8-note chords per 16 rows, each held three rows, at varied velocities. */
function chordPattern(m: Machine, voices: number): Pattern {
  const cls = machineClassById(m.classId)!;
  const perTrack = cls.trackParameters.length;
  const data: number[] = [];
  for (let row = 0; row < 16; row++) {
    for (const p of cls.globalParameters) data.push(p.noValue);
    for (let track = 0; track < m.tracks; track++) {
      const chord = Math.floor(row / 4);
      const base = 36 + ((chord * 5) % 12);
      const voicing = [0, 7, 12, 16, 19, 24, 28, 31][track % 8]!;
      let note = 0;
      let velocity = 0xff;
      if (track < voices && row % 4 === 0) {
        note = buzzNote(base + voicing);
        velocity = 0x40 + ((chord * 13 + track * 5) % 0x40);
      } else if (track < voices && row % 4 === 3) {
        note = 255;
      }
      const cells = [note, velocity];
      for (let i = 0; i < perTrack; i++) data.push(cells[i] ?? cls.trackParameters[i]!.noValue);
    }
  }
  return { id: `${m.id}-chords`, machineId: m.id, name: "Chords", rows: 16, data };
}

function buzzPattern(m: Machine): Pattern {
  const cls = machineClassById(m.classId)!;
  const gNote = cls.globalParameters.findIndex((p) => p.type === "note");
  const tNote = cls.trackParameters.findIndex((p) => p.type === "note");
  const gTrigger = cls.globalParameters.findIndex(
    (p) => p.type !== "note" && !(p.flags & MPF_STATE) && p.maxValue > p.minValue,
  );
  const tTrigger = cls.trackParameters.findIndex(
    (p) => p.type !== "note" && !(p.flags & MPF_STATE) && p.maxValue > p.minValue,
  );
  const data: number[] = [];
  for (let row = 0; row < 16; row++) {
    const hit = row % 4 === 0;
    const release = row % 4 === 3;
    for (let i = 0; i < cls.globalParameters.length; i++) {
      const p = cls.globalParameters[i]!;
      let value = p.noValue;
      if (i === gNote && (hit || release)) value = hit ? buzzNote(48 + ((row / 4) | 0) * 2) : 255;
      else if (i === gTrigger && hit) value = p.defValue !== p.noValue ? p.defValue : p.maxValue;
      data.push(value);
    }
    for (let track = 0; track < m.tracks; track++) {
      for (let i = 0; i < cls.trackParameters.length; i++) {
        const p = cls.trackParameters[i]!;
        let value = p.noValue;
        if (track === 0 && i === tNote && (hit || release)) value = hit ? buzzNote(48 + ((row / 4) | 0) * 2) : 255;
        else if (track === 0 && i === tTrigger && hit) value = p.defValue !== p.noValue ? p.defValue : p.maxValue;
        data.push(value);
      }
    }
  }
  if (gNote < 0 && tNote < 0 && gTrigger < 0 && tTrigger < 0)
    throw new Error(`${cls.name}: no note or trigger parameter for Buzz sound test`);
  return { id: `${m.id}-buzz-test`, machineId: m.id, name: "Buzz native test", rows: 16, data };
}

function song(machines: Machine[], connections: Connection[], patterns: Pattern[]): Song {
  return {
    name: "vstloader harness",
    bpm: 126,
    tpb: 4,
    length: 16,
    loop: true,
    machines,
    connections,
    sequences: patterns.map((p) => ({ id: `seq-${p.machineId}`, machineId: p.machineId, events: [{ tick: 0, kind: "pattern" as const, patternId: p.id }] })),
    patterns,
    timedEventLanes: [],
    waves: [],
    controllers: [],
    controlRoutes: [],
    controllerPatterns: [],
    controllerSequences: [],
  } as Song;
}

function stateOf(params: readonly MachineParameter[]): (number | null)[] {
  return params.map((p) => (p.type === "note" || !(p.flags & MPF_STATE) ? null : p.noValue));
}

function topologicalOrder(s: Song): string[] {
  const indegree = new Map(s.machines.map((m) => [m.id, 0]));
  for (const c of s.connections) indegree.set(c.to, (indegree.get(c.to) ?? 0) + 1);
  const queue = [...indegree].filter(([, d]) => d === 0).map(([id]) => id);
  const order: string[] = [];
  while (queue.length) {
    const id = queue.shift()!;
    order.push(id);
    for (const c of s.connections.filter((c) => c.from === id)) {
      const d = indegree.get(c.to)! - 1;
      indegree.set(c.to, d);
      if (d === 0) queue.push(c.to);
    }
  }
  return order;
}

/** What EngineBridge sends for a song (graph, tempo, schedule). */
function sendSong(e: Engine, s: Song): void {
  const machines: GraphMachine[] = s.machines.map((m) => {
    const cls = machineClassById(m.classId);
    const pkg = installed.find((p) => p.manifest.classes.some((c) => c.classId === m.classId));
    return {
      id: m.id,
      classId: m.classId,
      tracks: m.tracks,
      muted: m.muted,
      globalValues: m.globalValues,
      trackValues: m.trackValues,
      ...(cls ? { globalState: stateOf(cls.globalParameters), trackState: stateOf(cls.trackParameters) } : {}),
      ...(pkg ? { backendSourceId: `webclap:${pkg.archiveSha256}` } : {}),
      ...(m.classId === "master" ? { busIndex: 0 } : {}),
    };
  });
  e.send({ type: "graph", machines, connections: s.connections.map((c) => ({ ...c })), order: topologicalOrder(s), masterId: "master" });
  e.send({ type: "song", bpm: s.bpm, tpb: s.tpb });
  e.send({ type: "sequence", schedule: buildSchedule(s) });
}

async function waitFor<T>(test: () => T | null | undefined | false, timeoutMs: number, what: string): Promise<T> {
  const until = performance.now() + timeoutMs;
  for (;;) {
    const value = test();
    if (value) return value;
    if (engine?.errors.length) throw new Error(`${what}: ${engine.errors.join("; ")}`);
    if (performance.now() > until) throw new Error(`timed out waiting for ${what}`);
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
}

function record(e: Engine, frames: number): Promise<Float32Array> {
  return new Promise((resolve) => {
    e.recorder.port.onmessage = (m: MessageEvent) => {
      if ((m.data as { type: string }).type === "recorded") resolve((m.data as { samples: Float32Array }).samples);
    };
    e.recorder.port.postMessage({ type: "record", frames });
  });
}

function channelOf(machineId: string) {
  const info = runtimeInstance(machineId);
  const channel = info && runtime()!.channel(String(info.id));
  if (!channel) throw new Error(`no runtime channel for ${machineId}`);
  return { ctl: new Int32Array(channel.buffer, channel.base, 256), bytes: new Uint8Array(channel.buffer, channel.base), base: channel.base, buffer: channel.buffer };
}

/** Copies every request the machine's channel publishes (record + inputs), as vsthost --replay reads them. */
function captureRequests(machineId: string, blocks: number, inPorts: number) {
  const ch = channelOf(machineId);
  const recordBytes = REQUEST_BYTES + inPorts * 2 * B * 4;
  const bytes = new Uint8Array(blocks * recordBytes);
  let next = 0;
  let lost = 0;
  const timer = setInterval(() => {
    const published = Atomics.load(ch.ctl, CTL.requestSeq / 4);
    if (published - next > SLOTS) lost++;
    while (next < published && next < blocks) {
      const at = next * recordBytes;
      const slot = SLOTS_OFFSET + (next % SLOTS) * REQUEST_BYTES;
      bytes.set(ch.bytes.subarray(slot, slot + REQUEST_BYTES), at);
      const frame = (next * B) % RING_FRAMES;
      for (let c = 0; c < inPorts * 2; c++) {
        const ring = IN_RINGS_OFFSET + c * RING_FRAMES * 4 + frame * 4;
        bytes.set(ch.bytes.subarray(ring, ring + B * 4), at + REQUEST_BYTES + c * B * 4);
      }
      next++;
    }
  }, 20);
  return {
    done: () => waitFor(() => next >= blocks, 600_000, "request capture").then(() => {
      clearInterval(timer);
      return { bytes, blocks, recordBytes, lost };
    }),
  };
}

/** Lag episodes (guest more than 4 blocks behind) and device turns over 10 ms, every 10 ms. */
function watchChannel(machineId: string) {
  const ch = channelOf(machineId);
  const origin = performance.now();
  const episodes: Array<{ atSeconds: number; ms: number; maxLagBlocks: number }> = [];
  const slowTurns: Array<{ block: number; atSeconds: number; turnMs: number }> = [];
  let open: (typeof episodes)[number] | null = null;
  let done = Atomics.load(ch.ctl, CTL.responseSeq / 4);
  const timer = setInterval(() => {
    const now = (performance.now() - origin) / 1000;
    const lag = Atomics.load(ch.ctl, CTL.requestSeq / 4) - Atomics.load(ch.ctl, CTL.responseSeq / 4);
    if (lag > 4) {
      if (!open) episodes.push((open = { atSeconds: now, ms: 0, maxLagBlocks: lag }));
      open.maxLagBlocks = Math.max(open.maxLagBlocks, lag);
      open.ms = (now - open.atSeconds) * 1000;
    } else open = null;
    for (; done < Atomics.load(ch.ctl, CTL.responseSeq / 4); done++) {
      const turn = Atomics.load(ch.ctl, CTL.turnUs / 4 + (done % SLOTS));
      if (turn > 10_000 && slowTurns.length < 200) slowTurns.push({ block: done, atSeconds: now, turnMs: turn / 1000 });
    }
  }, 10);
  return {
    stop: () => {
      clearInterval(timer);
      return { episodes, slowTurns, underruns: Atomics.load(ch.ctl, CTL.underruns / 4), skipped: Atomics.load(ch.ctl, CTL.skipped / 4), maxProcessUs: Atomics.load(ch.ctl, CTL.maxProcessUs / 4) };
    },
    underruns: () => Atomics.load(ch.ctl, CTL.underruns / 4),
  };
}

// ---- scenarios -------------------------------------------------------------------------

const results: Record<string, unknown> = {};
const captures: Record<string, Uint8Array> = {};

/** The wrapped plugin as a machine playing 8-voice chords; captures for the identity check. */
async function songScenario(file: string, seconds: number, captureSeconds: number): Promise<unknown> {
  const e = await startEngine();
  const pkg = installed.find((p) => p.source.kind === "local" && p.source.location === file) ?? (await install(file));
  const installedClass = pkg.manifest.classes[0]!;
  const classId = installedClass.classId;
  const cls = machineClassById(classId)!;
  const tracks = installedClass.buzz ? cls.defaultTracks : 8;
  const inst = machine("vst", classId, tracks, 100);
  const pattern = installedClass.buzz ? buzzPattern(inst) : chordPattern(inst, 8);
  const s = song([inst, MASTER], [edge("c1", "vst", "master")], [pattern]);
  const loadStarted = performance.now();
  sendSong(e, s);
  await waitFor(() => runtimeInstance("vst"), 600_000, "the plugin to load in the runtime");
  const loadSeconds = (performance.now() - loadStarted) / 1000;
  const info = runtimeInstance("vst")!;
  log(`${file}: streaming after ${loadSeconds.toFixed(1)} s (bridge channel ${info.channel}, Boxedwine up in ${runtime()!.state.bootSeconds.toFixed(1)} s)`);
  const watch = watchChannel("vst");
  const blocks = Math.ceil((captureSeconds * RATE) / B);
  const requests = captureRequests("vst", blocks, 0);
  const recording = record(e, Math.ceil((captureSeconds + 2) * RATE));
  e.send({ type: "transport", action: "play" });
  const started = performance.now();
  const [samples, captured] = await Promise.all([recording, requests.done()]);
  captures[`${file}:output`] = new Uint8Array(samples.buffer);
  captures[`${file}:requests`] = captured.bytes;
  const timeline: Array<{ seconds: number; underruns: number }> = [];
  while ((performance.now() - started) / 1000 < seconds) {
    await new Promise((resolve) => setTimeout(resolve, 10_000));
    timeline.push({ seconds: Math.round((performance.now() - started) / 1000), underruns: watch.underruns() });
    log(`${file}: ${((performance.now() - started) / 1000).toFixed(0)} s played, ${watch.underruns()} underrun blocks`);
  }
  e.send({ type: "transport", action: "stop" });
  const watched = watch.stop();
  let peak = 0, sumSquares = 0, nonFinite = 0, nonZero = 0;
  for (const sample of samples) {
    if (!Number.isFinite(sample)) { nonFinite++; continue; }
    const a = Math.abs(sample);
    if (a > peak) peak = a;
    if (a > 1e-7) nonZero++;
    sumSquares += sample * sample;
  }
  const audio = { peak, rms: Math.sqrt(sumSquares / Math.max(1, samples.length)), nonZero, nonFinite };
  if (installedClass.buzz && (nonFinite !== 0 || peak <= 1e-6 || nonZero < 100))
    throw new Error(`${file}: native Buzz render failed sound check: ${JSON.stringify(audio)}`);
  const result = {
    file, classId, loadSeconds, audio, bootSeconds: runtime()!.state.bootSeconds, playedSeconds: (performance.now() - started) / 1000,
    sampleRate: e.ctx.sampleRate, latency: L, block: B,
    underrunBlocks: watched.underruns, skipped: watched.skipped, maxProcessUs: watched.maxProcessUs,
    stalls: watched.episodes, slowTurns: watched.slowTurns, timeline,
    capture: { blocks: captured.blocks, recordBytes: captured.recordBytes, lostWhileCapturing: captured.lost, inPorts: 0, outPorts: 1, block: B, frames: samples.length / 2 },
    errors: [...e.errors],
  };
  results[`song:${file}`] = result;
  sendSong(e, song([MASTER], [], []));
  await waitFor(() => !runtimeInstance("vst"), 60_000, "the instance to go");
  return result;
}

/** Compensation: a generator dry and through the PoC Invert (out = -in). */
async function nullScenario(seconds: number): Promise<unknown> {
  const e = await startEngine();
  const pkg = installed.find((p) => p.source.kind === "local" && p.source.location === "PoCInvert.wclap.tar.gz") ?? (await install("PoCInvert.wclap.tar.gz"));
  const fm = machine("fm", "fmsynth", 4, 50);
  const inv = machine("inv", pkg.manifest.classes[0]!.classId, 0, 200);
  const connections = [edge("dry", "fm", "master"), edge("send", "fm", "inv"), edge("wet", "inv", "master")];
  const s = song([fm, inv, MASTER], connections, [chordPattern(fm, 4)]);
  sendSong(e, s);
  await waitFor(() => runtimeInstance("inv"), 600_000, "the invert to load");
  const watch = watchChannel("inv");
  e.send({ type: "transport", action: "play" });
  await new Promise((resolve) => setTimeout(resolve, 2000));
  const summarize = (samples: Float32Array) => {
    const peak = [0, 0];
    const nonZero = [0, 0];
    for (let i = 0; i < samples.length; i++) {
      const v = samples[i]!;
      if (v !== 0) nonZero[i & 1]!++;
      peak[i & 1] = Math.max(peak[i & 1]!, Math.abs(v));
    }
    return { frames: samples.length / 2, peak, nonZeroSamples: nonZero };
  };
  const compensated = summarize(await record(e, Math.round(seconds * RATE)));
  sendSong(e, { ...s, machines: [fm, { ...inv, muted: true }, MASTER] });
  await new Promise((resolve) => setTimeout(resolve, 500));
  const dryOnly = summarize(await record(e, RATE * 2));
  sendSong(e, s);
  e.send({ type: "delay-compensation", enabled: false });
  await new Promise((resolve) => setTimeout(resolve, 1000));
  const uncompensated = summarize(await record(e, RATE * 2));
  e.send({ type: "delay-compensation", enabled: true });
  e.send({ type: "transport", action: "stop" });
  const watched = watch.stop();
  const result = { seconds, compensated, dryOnly, uncompensated, invertUnderruns: watched.underruns, errors: [...e.errors] };
  results.null = result;
  sendSong(e, song([MASTER], [], []));
  await waitFor(() => !runtimeInstance("inv"), 60_000, "the invert to go");
  return result;
}

/**
 * .bzw round trip: Dexed with two changed parameters, its state captured the
 * way a save does (clap.state through the machine's data), the song encoded
 * and decoded by buzz-remote's project codec, and the machine reopened from
 * it in a fresh instance.
 */
async function bzwScenario(): Promise<unknown> {
  const e = await startEngine();
  const pkg = installed.find((p) => p.source.kind === "local" && p.source.location === "Dexed.wclap.tar.gz") ?? (await install("Dexed.wclap.tar.gz"));
  const classId = pkg.manifest.classes[0]!.classId;
  const cls = machineClassById(classId)!;
  const inst = machine("dexed", classId, 8, 100);
  const s = song([inst, MASTER], [edge("c1", "dexed", "master")], []);
  sendSong(e, s);
  await waitFor(() => runtimeInstance("dexed"), 600_000, "Dexed to load");
  e.send({ type: "transport", action: "play" });
  const changed = [
    { index: cls.globalParameters.findIndex((p) => /cutoff/i.test(p.name)), word: 0x3000 },
    { index: cls.globalParameters.findIndex((p) => /reso/i.test(p.name)), word: 0xc000 },
  ].filter((c) => c.index >= 0);
  const before = await e.captureData("dexed");
  for (const c of changed) {
    e.send({ type: "param", machineId: "dexed", track: null, index: c.index, value: c.word });
    inst.globalValues[c.index] = c.word;
  }
  // The plugin asks its runtime for a fresh state after parameter changes (at most every ~0.5 s).
  await new Promise((resolve) => setTimeout(resolve, 1500));
  const state = (await e.captureData("dexed"))!;
  e.send({ type: "transport", action: "stop" });
  const saved: Song = { ...s, machines: s.machines.map((m) => (m.id === "dexed" ? { ...m, data: state } : m)) };
  const codec = new DeterministicProjectCodec();
  const blob = await codec.encode({ song: saved, saveMode: "portable" });
  sendSong(e, song([MASTER], [], []));
  await waitFor(() => !runtimeInstance("dexed"), 60_000, "Dexed to go");

  const decoded = await codec.decode(blob);
  const reloaded = decoded.song.machines.find((m) => m.id === "dexed")!;
  // A fresh machine id, restored from the decoded data before the graph creates it.
  const data = new Uint8Array(reloaded.data!).slice().buffer;
  e.send({ type: "machine-data", machineId: "dexed2", revision: 1, data }, [data]);
  const s2 = { ...decoded.song, machines: decoded.song.machines.map((m) => (m.id === "dexed" ? { ...m, id: "dexed2" } : m)), connections: [edge("c1", "dexed2", "master")] };
  sendSong(e, s2);
  await waitFor(() => runtimeInstance("dexed2"), 600_000, "Dexed to reopen");
  await new Promise((resolve) => setTimeout(resolve, 1500));
  const after = (await e.captureData("dexed2"))!;
  const same = (a: Uint8Array | null, b: Uint8Array | null) => !!a && !!b && a.length === b.length && a.every((x, i) => x === b[i]);
  const result = {
    blobBytes: blob.size,
    stateBytes: state.length,
    stateChangedByParams: !same(before, state),
    dataRoundTrip: reloaded.data?.length === state.length,
    sameStateAfterReopen: same(after, state),
    errors: [...e.errors],
  };
  results.bzw = result;
  sendSong(e, song([MASTER], [], []));
  await waitFor(() => !runtimeInstance("dexed2"), 60_000, "Dexed to go");
  return result;
}

function chunk(key: string, offset: number, length: number): string {
  const bytes = captures[key]!.subarray(offset, offset + length);
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

Object.assign(window, {
  vstloaderHarness: {
    install,
    songScenario,
    nullScenario,
    bzwScenario,
    results,
    captureSize: (key: string) => captures[key]?.length ?? 0,
    chunk,
    runtimeUri: RUNTIME_URI,
  },
});
log("ready");
