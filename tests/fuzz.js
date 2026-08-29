#!/usr/bin/env node
// Fuzz gate: feed deterministic garbage into the WASM build's full pipeline.
// The VM must never report anything except the defined result codes.
// Usage: node tests/fuzz.js <path/to/hokus.wasm>
import { readFileSync } from "node:fs";

const [wasmPath] = process.argv.slice(2);
const vm = (await WebAssembly.instantiate(readFileSync(wasmPath), {})).instance.exports;
const buf = new Uint8Array(vm.memory.buffer);
const srcPtr = vm.hk_source_buffer();

const OK = new Set([0, 1, 2, 65, 70]);
let anomalies = 0;

// 50 rounds of pseudo-random bytes (deterministic LCG — reproducible failures)
let state = 0x2f6e2b1;
const rnd = () => (state = (state * 1103515245 + 12345) & 0x7fffffff);
for (let i = 0; i < 50; i++) {
  const n = 100 + (rnd() % 4000);
  const bytes = new Uint8Array(n);
  for (let j = 0; j < n; j++) bytes[j] = (rnd() % 255) + 1;
  buf.set(bytes, srcPtr);
  const code = vm.hk_run(n);
  if (!OK.has(code)) { anomalies++; console.log(`round ${i}: unexpected code ${code}`); }
}
for (let round = 0; round < 2; round++) {
  // pathological nesting and truncation at every prefix length of a program
  const deep = new TextEncoder().encode("print " + "(".repeat(800) + "1" + ")".repeat(800) + ";");
  buf.set(deep, srcPtr);
  const code = vm.hk_run(deep.length);
  if (code !== 0) { anomalies++; console.log(`deep nesting: code ${code}`); }
  const prog = new TextEncoder().encode('class A { m(x) { return x; } } print A().m("ok");');
  for (let n = 1; n < prog.length; n += 7) {
    buf.set(prog.subarray(0, n), srcPtr);
    const c = vm.hk_run(n);
    if (!OK.has(c)) { anomalies++; console.log(`prefix ${n}: code ${c}`); }
  }
}

console.log(`fuzz: ${anomalies === 0 ? "PASS" : `FAIL (${anomalies} anomalies)`}`);
process.exit(anomalies ? 1 : 0);
