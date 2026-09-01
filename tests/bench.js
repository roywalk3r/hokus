#!/usr/bin/env node
// Benchmarks + trendlines. Usage:
//   node tests/bench.js                          run both hosts, print, write benchmarks/latest.json
//   node tests/bench.js --check <baseline.json>  also fail if any bench > 1.5x baseline
// Native timing uses a clock() wrap inside the language, so process-spawn and
// compile time never pollute the number: t0 is the first executed statement.
import { readFileSync, writeFileSync, mkdirSync, unlinkSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { dirname } from "node:path";

const args = process.argv.slice(2);
const checkIdx = args.indexOf("--check");
const baselinePath = checkIdx >= 0 ? args[checkIdx + 1] : null;
const NATIVE = "bin/hokus";
const OUT = "benchmarks/latest.json";

const PROGRAMS = {
  "fib(28) recursion": "fun fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\nprint fib(28);\n",
  "1M leaf calls (call path)":
    /* locals, not globals: keep the per-iteration cost on the call path,
       not on two interned-name globals-table lookups */
    "fun f(a) { return a + 1; }\n"
    + "fun bench() {\n    var s = 0;\n"
    + "    for (var i = 0; i < 1000000; i = i + 1) s = f(s);\n    return s;\n}\n"
    + "print bench();\n",
  "200k method calls (OP_INVOKE)":
    "class Counter {\n    init() { this.n = 0; }\n    bump() { this.n = this.n + 1; return this.n; }\n}\n"
    + "var c = Counter();\nvar last = 0;\n"
    + "for (var i = 0; i < 200000; i = i + 1) last = c.bump();\nprint last;\n",
  "200k static calls (class receiver)":
    "class Math2 {\n    static sqr(x) { return x * x; }\n}\nvar last = 0;\n"
    + "for (var i = 0; i < 200000; i = i + 1) last = Math2.sqr(7);\nprint last;\n",
  "100k field gets (interned keys)":
    "class P { init() { this.x = 1; } }\nvar p = P();\nvar s = 0;\n"
    + "for (var i = 0; i < 100000; i = i + 1) s = s + p.x;\nprint s;\n",
  "300k field get/set churn":
    "class P { init() { this.x = 1; this.y = 2; this.z = 3; } }\nvar p = P();\nvar a = 0;\n"
    + "for (var i = 0; i < 300000; i = i + 1) {\n"
    + "    a = p.x; a = p.y; a = p.z;\n"
    + "    p.x = a; p.y = a; p.z = a;\n"
    + "}\nprint a;\n",
  "2k string concat (+ GC)":
    "var s = \"\";\nfor (var i = 0; i < 2000; i = i + 1) s = s + \"ab\";\nprint s;\n",
};

const { instance } = await WebAssembly.instantiate(readFileSync("web/hokus.wasm"), {});
const vm = instance.exports;

function wasmMs(src) {
  const bytes = new TextEncoder().encode(src);
  let best = Infinity;
  // one untimed warm-up: the wasm engine's first runs miss JIT tiers and can
  // read ~2x slow, which flaked the trendline against a lucky baseline
  for (let i = -1; i < 5; i++) {
    new Uint8Array(vm.memory.buffer).set(bytes, vm.hk_source_buffer());
    const t0 = performance.now();
    const rc = vm.hk_run(bytes.length);
    if (i >= 0) best = Math.min(best, performance.now() - t0);
    if (rc !== 0) throw new Error(`wasm run failed rc=${rc}`);
  }
  return best;
}

function nativeMs(src) {
  const wrapped = "var bench_t0 = clock();\n" + src + "\nprint clock() - bench_t0;\n";
  const tmp = `/tmp/hokus_bench_${process.pid}.hk`;
  writeFileSync(tmp, wrapped);
  try {
    let best = Infinity;
    for (let i = 0; i < 3; i++) {
      const out = execFileSync(NATIVE, [tmp], { encoding: "utf8" });
      const lines = out.trim().split("\n");
      best = Math.min(best, Number(lines[lines.length - 1]) * 1000);
    }
    return best;
  } finally {
    try { unlinkSync(tmp); } catch {}
  }
}

const results = {};
for (const [label, src] of Object.entries(PROGRAMS)) {
  const wasm = wasmMs(src), native = nativeMs(src);
  results[label] = { wasm_ms: +wasm.toFixed(2), native_ms: +native.toFixed(2) };
  console.log(`${label}: wasm ${wasm.toFixed(1)} ms, native ${native.toFixed(1)} ms`);
}
mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, JSON.stringify({ when: new Date().toISOString(), results }, null, 2) + "\n");

if (baselinePath) {
  const base = JSON.parse(readFileSync(baselinePath, "utf8")).results;
  const FACTOR = 1.5;
  let slow = 0;
  for (const [label, m] of Object.entries(results)) {
    for (const host of ["wasm_ms", "native_ms"]) {
      const b = base[label]?.[host];
      if (b == null) continue;
      // gate runs share a loaded machine (the fuzz sweep just finished); a
      // tripped row gets one re-measure — a real regression survives it,
      // a load spike doesn't
      const got = m[host] > b * FACTOR
        ? (host === "wasm_ms" ? wasmMs : nativeMs)(PROGRAMS[label])
        : m[host];
      if (got > b * FACTOR) {
        console.log(`SLOW ${label} ${host}: ${got.toFixed(2)} ms vs baseline ${b} ms (> ${FACTOR}x)`);
        slow++;
      }
    }
  }
  if (slow) { console.error(`${slow} regressions vs ${baselinePath}`); process.exit(1); }
  console.log(`all benches within ${FACTOR}x of ${baselinePath}`);
}
