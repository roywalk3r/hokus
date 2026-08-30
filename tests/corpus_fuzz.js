#!/usr/bin/env node
// Corpus-mutation fuzz: mutate real third-party Lox programs (token-level
// delete/swap/dup/number-tweak), then require native and WASM hosts to agree
// byte-for-byte on output and exit status — whatever the mutant does, even a
// compile error. Where diff_fuzz.js generates well-formed programs, this gate
// explores the error surface around real ones.
// Usage: node tests/corpus_fuzz.js ./bin/hokus web/hokus.wasm [roundsPerFile]
import { readFileSync, writeFileSync, unlinkSync, readdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join } from "node:path";

const [native, wasmPath] = process.argv.slice(2);
const ROUNDS = Number(process.argv[4] ?? 3);
const CORPUS = "tests/external/craftinginterpreters/test";

function* walk(dir) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) yield* walk(p);
    else if (e.name.endsWith(".lox")) yield p;
  }
}

function lcg(seed) {
  let s = seed >>> 0;
  return () => (s = (s * 1664525 + 1013904223) >>> 0) / 0x100000000;
}

function tokenize(src) {
  // crude splitter good enough for mutation: words, strings, punctuation
  return src.match(/"(?:[^"\\]|\\.)*"|\w+|[^\s\w"]/g) ?? [];
}

function mutate(tokens, rnd) {
  const t = tokens.slice();
  const n = t.length;
  const kind = Math.floor(rnd() * 4);
  if (kind === 0 && n > 2) t.splice(Math.floor(rnd() * n), 1); // delete
  else if (kind === 1 && n > 2) { // swap adjacent
    const i = Math.floor(rnd() * (n - 1));
    [t[i], t[i + 1]] = [t[i + 1], t[i]];
  } else if (kind === 2 && n > 1) { // duplicate a span of 1-4 tokens
    const i = Math.floor(rnd() * n);
    const len = 1 + Math.floor(rnd() * 4);
    t.splice(i, 0, ...t.slice(i, Math.min(n, i + len)));
  } else { // tweak first numeric literal
    const i = t.findIndex((x) => /^\d/.test(x));
    if (i >= 0) t[i] = String(1 + Math.floor(rnd() * 97));
  }
  return t.join(" ");
}

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {});
const vm = instance.exports;
const u8 = () => new Uint8Array(vm.memory.buffer);
function wasmRun(src) {
  // step-sliced with an instruction cap: mutants can infinite-loop, and a
  // sync hk_run would hang forever. Cap matches the native timeout path.
  const enc = new TextEncoder().encode(src);
  u8().set(enc, vm.hk_source_buffer());
  let raw = vm.hk_prepare(enc.length, 0); // 0 = READY (not done!), 65 compile error
  const CAP = 200_000_000;
  let spent = 0;
  if (raw !== 65) {
    do { raw = vm.hk_step(10000); spent += 10000; }
    while (raw === 2 && spent < CAP); // 2 = paused, 0 = done, 70 = runtime error
    if (spent >= CAP && raw === 2) raw = -1; // out of budget, like native timeout
  }
  // shim speaketh sysexits (65/70), norm() speaketh InterpretResult (1/2)
  const code = raw === 65 ? 1 : raw === 70 ? 2 : raw === 0 ? 0 : raw;
  const out = new TextDecoder().decode(
    u8().slice(vm.hk_output(), vm.hk_output() + vm.hk_output_length()));
  return { code, out };
}
const norm = (code) => (code === 65 ? 1 : code === 70 ? 2 : code);

let total = 0, mismatches = 0;
const tmp = `/tmp/hokus_corpus_fuzz_${process.pid}.hk`;
for (const file of walk(CORPUS)) {
  const raw0 = readFileSync(file, "utf8");
  // clock() is wall/CPU time — inherently host-dependent, excluded like diff_fuzz
  if (/\bclock\b/.test(raw0)) continue;
  const tokens = tokenize(raw0);
  if (tokens.length < 3) continue;
  for (let i = 0; i < ROUNDS; i++) {
    const src = mutate(tokens, lcg(total * 2654435761 + i * 97 + 5)) + "\n";
    total++;
    writeFileSync(tmp, src);
    let nat;
    try {
      nat = { code: 0, out: execFileSync(native, [tmp], { encoding: "utf8", timeout: 10000 }) };
    } catch (e) {
      nat = { code: e.status ?? -1, out: (e.stdout ?? "") + (e.stderr ?? "") };
    }
    if (nat.code === -1) continue; // runaway mutant: only compared when it terminates
    const was = wasmRun(src);
    if (was.code === -1) {
      console.log(`MISMATCH ${file}#${i}: native finished, wasm hit instruction cap`);
      mismatches++;
      continue;
    }
    if (norm(nat.code) !== was.code || nat.out !== was.out) {
      mismatches++;
      if (mismatches <= 3) {
        console.log(`MISMATCH ${file}#${i}: native(${nat.code}) vs wasm(${was.code})`);
        console.log("--- source ---\n" + src.slice(0, 400));
        console.log("--- native ---\n" + nat.out.slice(0, 200));
        console.log("--- wasm ---\n" + was.out.slice(0, 200));
      }
    }
  }
}
unlinkSync(tmp);
console.log(`corpus fuzz: ${total} mutants, ${mismatches} mismatches`);
process.exit(mismatches ? 1 : 0);
