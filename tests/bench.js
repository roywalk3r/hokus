#!/usr/bin/env node
// Benchmarks for the WASM build. Usage: node tests/bench.js [web/hokus.wasm]
// Compare native with: time bin/hokus - (same sources are printed below).
import { readFileSync } from "node:fs";

const wasmPath = process.argv[2] ?? "web/hokus.wasm";
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {});
const vm = instance.exports;

const PROGRAMS = {
  "fib(28) recursion": "fun fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\nprint fib(28);\n",
  "200k method calls (OP_INVOKE)":
    "class Counter {\n    init() { this.n = 0; }\n    bump() { this.n = this.n + 1; return this.n; }\n}\n"
    + "var c = Counter();\nvar last = 0;\n"
    + "for (var i = 0; i < 200000; i = i + 1) last = c.bump();\nprint last;\n",
};

for (const [label, src] of Object.entries(PROGRAMS)) {
  const bytes = new TextEncoder().encode(src);
  for (let i = 0; i < 3; i++) {
    new Uint8Array(vm.memory.buffer).set(bytes, vm.hk_source_buffer());
    const t0 = performance.now();
    const rc = vm.hk_run(bytes.length);
    const ms = performance.now() - t0;
    if (rc !== 0) { console.error(`${label}: run failed rc=${rc}`); process.exit(1); }
    console.log(`${label}  run ${i + 1}: ${ms.toFixed(1)} ms`);
  }
}
