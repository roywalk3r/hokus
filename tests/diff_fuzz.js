#!/usr/bin/env node
// Differential fuzz: generate random *valid* Lox programs, run each under the
// native binary and the WASM module, and require byte-identical output + exit
// codes. Catches codegen/dispatch drift between the two hosts that garbage-
// input fuzzing (tests/fuzz.js) cannot.
// Usage: node tests/diff_fuzz.js ./bin/hokus web/hokus.wasm [rounds]
import { readFileSync, writeFileSync, unlinkSync } from "node:fs";
import { execFileSync } from "node:child_process";

const [native, wasmPath] = process.argv.slice(2);
const ROUNDS = Number(process.argv[4] ?? 200);

function lcg(seed) {
  let s = seed >>> 0;
  return () => (s = (s * 1664525 + 1013904223) >>> 0) / 0x100000000;
}

function genProgram(rnd) {
  const vars = [];
  let varCount = 0;
  const pick = () => rnd();
  const int = (n) => Math.floor(pick() * n);

  function numExpr(depth) {
    if (depth <= 0 || pick() < 0.4) {
      if (vars.length && pick() < 0.5) return vars[int(vars.length)];
      if (pick() < 0.15) return `(0 - ${int(100) + 1})`; /* negative literal */
      return pick() < 0.7 ? String(int(1000)) : `${int(100)}.${int(10)}`;
    }
    const ops = ["+", "-", "*", "/"];
    const op = ops[int(4)];
    // division by literal zero is legal IEEE-754, but keep results sane
    const rhs = op === "/" ? `(${int(9) + 1})` : numExpr(depth - 1);
    return `(${numExpr(depth - 1)} ${op} ${rhs})`;
  }

  function boolExpr(depth) {
    if (depth <= 0 || pick() < 0.5) {
      const ops = ["<", "<=", ">", ">=", "==", "!="];
      return `(${numExpr(1)} ${ops[int(6)]} ${numExpr(1)})`;
    }
    const op = pick() < 0.5 ? "and" : "or";
    return `(${boolExpr(depth - 1)} ${op} ${boolExpr(depth - 1)})`;
  }

  function strExpr() {
    const parts = ['"a"', '"bc"', '""', '"xyz"'];
    const a = parts[int(4)], b = parts[int(4)];
    return pick() < 0.5 ? a : `(${a} + ${b})`;
  }

  const lines = [];
  const funs = []; /* name -> arity, number-typed in/out only */

  function stmt(depth, indent) {
    if (depth > 0 && funs.length && pick() < 0.08) {
      const f = funs[int(funs.length)];
      const args = Array.from({ length: f.arity }, () => numExpr(1)).join(", ");
      lines.push(`${"  ".repeat(indent)}print ${f.name}(${args});`);
      return;
    }

    const k = pick();
    const pad = "  ".repeat(indent);
    if (k < 0.35) {
      lines.push(`${pad}print ${numExpr(2)};`);
    } else if (k < 0.5) {
      const name = `v${varCount++}`;
      const init = numExpr(2); /* before push: never reads its own name */
      lines.push(`${pad}var ${name} = ${init};`);
      vars.push(name);
      if (pick() < 0.5) lines.push(`${pad}print ${name};`);
    } else if (k < 0.65 && depth > 0) {
      const saved = vars.length;
      lines.push(`${pad}if (${boolExpr(2)}) {`);
      stmt(depth - 1, indent + 1);
      vars.length = saved;
      if (pick() < 0.5) { lines.push(`${pad}} else {`); stmt(depth - 1, indent + 1); vars.length = saved; }
      lines.push(`${pad}}`);
    } else if (k < 0.8 && depth > 0) {
      const i = `i${varCount++}`;
      const n = int(5) + 1;
      lines.push(`${pad}for (var ${i} = 0; ${i} < ${n}; ${i} = ${i} + 1) {`);
      const saved = vars.length;
      vars.push(i);
      stmt(depth - 1, indent + 1);
      vars.length = saved; // loop var leaves scope
      lines.push(`${pad}}`);
    } else if (k < 0.87 && depth > 0) {
      const i = `w${varCount++}`;
      lines.push(`${pad}var ${i} = 0;`);
      lines.push(`${pad}while (${i} < ${int(4) + 1}) {`);
      const saved = vars.length;
      lines.push(`${pad}  ${i} = ${i} + 1;`);
      stmt(depth - 1, indent + 1);
      vars.length = saved;
      lines.push(`${pad}}`);
    } else {
      lines.push(`${pad}print ${strExpr()};`);
    }
  }

  // zero to two top-level number functions, including a closure when nested
  if (pick() < 0.6) {
    const nf = pick() < 0.3 ? 2 : 1;
    for (let f = 0; f < nf; f++) {
      const name = `fn${f}`;
      const arity = int(3);
      const ps = Array.from({ length: arity }, (_, j) => `a${j}`);
      vars.push(...ps);
      lines.push(`fun ${name}(${ps.join(", ")}) {`);
      lines.push(`  var r = ${numExpr(2)};`);
      if (pick() < 0.4) lines.push(`  r = r * ${numExpr(1)};`);
      lines.push(`  return r;`);
      lines.push(`}`);
      vars.length -= ps.length;
      funs.push({ name, arity });
    }
  }

  // occasionally: a class with numeric fields and methods
  if (pick() < 0.4) {
    lines.push("class Box {");
    lines.push("  init(v) { this.v = v; this.acc = 0; }");
    lines.push("  add(x) { this.acc = this.acc + x; return this.acc; }");
    lines.push("  total() { return this.v + this.acc; }");
    lines.push("}");
    if (pick() < 0.5) {
      lines.push("class Bag < Box {");
      lines.push("  double() { return super.total() * 2; }");
      lines.push("}");
      const inst = pick() < 0.5 ? "bag" : "boxB";
      lines.push(`var ${inst} = Bag(${int(50)});`);
      for (let c = 0; c < 1 + int(3); c++) {
        if (pick() < 0.4) lines.push(`print ${inst}.double();`);
        else if (pick() < 0.7) lines.push(`print ${inst}.add(${numExpr(1)});`);
        else lines.push(`print ${inst}.total();`);
      }
    } else {
      lines.push(`var box = Box(${int(50)});`);
      for (let c = 0; c < 1 + int(4); c++) {
        if (pick() < 0.5) lines.push(`print box.add(${numExpr(1)});`);
        else lines.push(`print box.total();`);
      }
    }
  }

  const n = 3 + int(8);
  for (let i = 0; i < n; i++) stmt(3, 0);
  return lines.join("\n") + "\n";
}

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {});
const vm = instance.exports;
const u8 = () => new Uint8Array(vm.memory.buffer);
function wasmRun(src) {
  const enc = new TextEncoder().encode(src);
  u8().set(enc, vm.hk_source_buffer());
  const code = vm.hk_run(enc.length);
  const out = new TextDecoder().decode(
    u8().slice(vm.hk_output(), vm.hk_output() + vm.hk_output_length()));
  return { code, out };
}

// Error rounds: every program ends in a deliberate runtime error after some
// valid computation — gate the error paths (message + clox trace + line no).
function genErrorProgram(rnd) {
  const src = genProgram(rnd);
  const kind = Math.floor(rnd() * 5);
  const tails = [
    "print nope_undefined;\n",
    "clock(1);\n",
    "var x = 3; x();\n",
    'print 1 + "s";\n',
    "class E {} var e = E(); print e.missing;\n",
  ];
  return src + tails[kind];
}

let mismatches = 0;
/* Native uses sysexits (65/70), the shim returns raw InterpretResult (1/2). */
const norm = (code) => (code === 65 ? 1 : code === 70 ? 2 : code);
for (let round = 0; round < ROUNDS; round++) {
  const src = genProgram(lcg(round * 7919 + 13));
  let nat;
  const tmp = `/tmp/hokus_diff_fuzz_${process.pid}.hk`;
  writeFileSync(tmp, src);
  try {
    nat = { code: 0, out: execFileSync(native, [tmp], { encoding: "utf8" }) };
  } catch (e) {
    nat = { code: e.status ?? -1, out: (e.stdout ?? "") + (e.stderr ?? "") };
  }
  const was = wasmRun(src);
  if (norm(nat.code) !== was.code || nat.out !== was.out) {
    mismatches++;
    if (mismatches <= 3) {
      console.log(`MISMATCH round ${round}: native(${nat.code}) vs wasm(${was.code})`);
      console.log("--- source ---\n" + src);
      console.log("--- native ---\n" + nat.out.slice(0, 300));
      console.log("--- wasm ---\n" + was.out.slice(0, 300));
    }
  }
}
const ERROR_ROUNDS = Math.max(20, ROUNDS / 2 | 0);
for (let round = 0; round < ERROR_ROUNDS; round++) {
  const src = genErrorProgram(lcg(round * 104729 + 7));
  let nat;
  const tmp = `/tmp/hokus_diff_fuzz_${process.pid}.hk`;
  writeFileSync(tmp, src);
  try {
    nat = { code: 0, out: execFileSync(native, [tmp], { encoding: "utf8" }) };
  } catch (e) {
    nat = { code: e.status ?? -1, out: (e.stdout ?? "") + (e.stderr ?? "") };
  }
  const was = wasmRun(src);
  if (norm(nat.code) !== was.code || nat.out !== was.out) {
    mismatches++;
    if (mismatches <= 3) {
      console.log(`MISMATCH error-round ${round}: native(${nat.code}) vs wasm(${was.code})`);
      console.log("--- native ---\n" + nat.out.slice(-200));
      console.log("--- wasm ---\n" + was.out.slice(-200));
    }
  }
}
console.log(`diff fuzz: ${ROUNDS} rounds + ${ERROR_ROUNDS} error rounds, ${mismatches} mismatches`);
unlinkSync(`/tmp/hokus_diff_fuzz_${process.pid}.hk`);
process.exit(mismatches ? 1 : 0);
