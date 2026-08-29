#!/usr/bin/env node
// WASM gate: exercises the freestanding build's exported surface against the
// golden corpus plus API-level checks (trace, disasm, step slicing).
// Usage: node tests/wasm_runner.js <path/to/hokus.wasm> <golden dir>
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";

const [wasmPath, goldenDir] = process.argv.slice(2);

const instance = await WebAssembly
  .instantiate(readFileSync(wasmPath), {})
  .then((r) => r.instance);
const vm = instance.exports;

const u8 = () => new Uint8Array(vm.memory.buffer);
function load(src) {
  const enc = new TextEncoder().encode(src);
  u8().set(enc, vm.hk_source_buffer());
  return enc.length;
}
function read() {
  return new TextDecoder().decode(
    u8().slice(vm.hk_output(), vm.hk_output() + vm.hk_output_length()));
}

let pass = 0, fail = 0;
const failures = [];
function check(name, ok, detail = "") {
  if (ok) pass++;
  else { fail++; failures.push(`${name}: ${detail}`); }
}

// 1. golden programs produce identical output under WASM as under native
for (const f of readdirSync(goldenDir).filter((f) => f.endsWith(".hk"))) {
  const src = readFileSync(join(goldenDir, f), "utf8");
  const expected = readFileSync(join(goldenDir, f.replace(/\.hk$/, ".expected")), "utf8");
  const code = vm.hk_run(load(src));
  check(`run ${f}`, code === 0 && read().trimEnd() === expected.trimEnd(),
    `exit ${code}, got ${JSON.stringify(read().slice(0, 120))}`);
}

// 2. trace mode interleaves instruction lines with program output
{
  const code = vm.hk_run_trace(load("print 40 + 2;"));
  const out = read();
  check("trace", code === 0 && out.includes("OP_ADD") && out.includes("42\n"),
    JSON.stringify(out.slice(0, 120)));
}

// 3. disassembler emits listing, incl. class opcodes
{
  const code = vm.hk_disasm(load("class A { m() {} } print A;"));
  const out = read();
  check("disasm", code === 0 && out.includes("OP_CLASS") && out.includes("OP_METHOD"),
    JSON.stringify(out.slice(0, 120)));
}

// 3b. method-call sites compile to OP_INVOKE; super calls to OP_SUPER_INVOKE
{
  const code = vm.hk_disasm(load(
    "class A { m(x) { return x; } } class B < A { go(a) { a.m(1); return super.m(2); } }"));
  const out = read();
  check("invoke opcodes", code === 0 &&
    out.includes("OP_INVOKE") && out.includes("OP_SUPER_INVOKE"),
    JSON.stringify(out.slice(0, 200)));
}

// 4. step API: sliced execution produces identical output to a full run
{
  const src = "for (var i = 0; i < 10; i = i + 1) print i * i;";
  vm.hk_run(load(src));
  const full = read();
  const rc0 = vm.hk_prepare(load(src), 0);
  let rc = 2, slices = 0;
  while (rc === 2 && slices < 1000) { rc = vm.hk_step(17); slices++; }
  check("step slices", rc0 === 0 && rc === 0 && read() === full && slices > 1,
    `rc ${rc}, slices ${slices}, got ${JSON.stringify(read().slice(0, 120))}`);
}

// 5. compile error surfaced by hk_run (exit 1=compile in shim convention)
{
  const code = vm.hk_run(load("{ var a = a; }"));
  check("compile error", code !== 0 &&
    read().includes("Can't read local variable in its own initializer."),
    `exit ${code}`);
}

// 6. runtime error mid-slice returns 70 with named variable
{
  vm.hk_prepare(load("print 1; bogus;"), 0);
  let rc = 2, n = 0;
  while (rc === 2 && n < 100) { rc = vm.hk_step(50); n++; }
  const out = read();
  check("runtime error", rc === 70 && out.includes("1\n") &&
    out.includes("Undefined variable 'bogus'."), `rc ${rc}`);
}

// 6b. runtime errors carry a clox-style stack trace back to script
{
  const code = vm.hk_run(load("fun a() { b(); } fun b() { undefined_fn(); } a();"));
  const out = read();
  check("stack trace", code === 2 && out.includes("Undefined variable 'undefined_fn'.") &&
    out.includes("in b()") && out.includes("in a()") && out.includes("in script"),
    JSON.stringify(out.slice(0, 200)));
}

console.log(`wasm gate: ${pass} passed, ${fail} failed`);
for (const f of failures) console.log("FAIL", f);
process.exit(fail ? 1 : 0);
