#!/usr/bin/env node
// Benchmark: recursive fib(28) under the WASM build.
// Usage: node tests/bench.js [web/hokus.wasm]   (compare native with: time bin/hokus ...)
import { readFileSync, writeFileSync } from "node:fs";

const wasmPath = process.argv[2] ?? "web/hokus.wasm";
const src = "fun fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\nprint fib(28);\n";

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {});
const vm = instance.exports;
const bytes = new TextEncoder().encode(src);

for (let i = 0; i < 3; i++) {
  new Uint8Array(vm.memory.buffer).set(bytes, vm.hk_source_buffer());
  const t0 = performance.now();
  const rc = vm.hk_run(bytes.length);
  const ms = performance.now() - t0;
  if (rc !== 0) { console.error("run failed", rc); process.exit(1); }
  console.log(`run ${i + 1}: ${ms.toFixed(1)} ms`);
}
