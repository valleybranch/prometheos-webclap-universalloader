import assert from "node:assert/strict";
import { descriptor } from "../wrap/bundle.js";

const describe = {
  format: "buzz",
  name: "Test Buzz",
  vendor: "Prometheos",
  vendorVersion: 15,
  synth: true,
  minTracks: 1,
  maxTracks: 8,
  inPorts: [],
  outPorts: [{ name: "Main" }],
  latency: 0,
  params: [{ name: "Cutoff", label: "", value: 0.5 }],
  trackParams: [{ name: "Note", buzzType: 0, minValue: 1, maxValue: 156, noValue: 0, defValue: 0 }],
  attributes: [{ name: "Mode", minValue: 0, maxValue: 2, defValue: 1 }],
};

const text = descriptor({
  sha256: "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef",
  describe,
  runtime: "/universalloader/runtime/index.html",
  fileName: "TestBuzz.dll",
});

assert.match(text, /^id=prometheos\.buzzloader\.1234567890abcdef$/m);
assert.match(text, /^format=buzz$/m);
assert.match(text, /^buzzName=Test Buzz$/m);
assert.match(text, /^minTracks=1$/m);
assert.match(text, /^maxTracks=8$/m);
assert.match(text, /^buzzTrack=Note\t0\t1\t156\t0\t0$/m);
assert.match(text, /^buzzAttribute=Mode\t0\t2\t1$/m);
console.log("buzz loader descriptor contract: ok");
