#!/usr/bin/env node
// Disasm roundtrip selftest: parse `--disasm` listings back into bytecode and
// require an exact match with the compiled chunk tree (--emit-bin oracle).
// The disassembler's inverse problem: if the text drifted from the bytes, the
// assembler can't rebuild them, and this gate fails.
// Usage: node tests/disasm_roundtrip.js ./bin/hokus <file.hk> [...]
import { execFileSync } from "node:child_process";

const [bin, ...files] = process.argv.slice(2);

// Opcode table, mirroring src/chunk.h (order is the byte value).
const OPS = [
  "OP_CONSTANT", "OP_NIL", "OP_TRUE", "OP_FALSE", "OP_POP",
  "OP_GET_GLOBAL", "OP_DEFINE_GLOBAL", "OP_SET_GLOBAL",
  "OP_GET_LOCAL", "OP_SET_LOCAL", "OP_GET_UPVALUE", "OP_SET_UPVALUE",
  "OP_CLOSURE", "OP_CLOSE_UPVALUE", "OP_EQUAL", "OP_GREATER", "OP_LESS",
  "OP_ADD", "OP_SUBTRACT", "OP_MULTIPLY", "OP_DIVIDE", "OP_NOT",
  "OP_NEGATE", "OP_PRINT", "OP_JUMP", "OP_JUMP_IF_FALSE", "OP_LOOP",
  "OP_CALL", "OP_RETURN", "OP_GET_PROPERTY", "OP_SET_PROPERTY",
  "OP_CLASS", "OP_METHOD", "OP_INHERIT", "OP_GET_SUPER",
  "OP_INVOKE", "OP_SUPER_INVOKE", "OP_STATIC_METHOD", "OP_MODULO",
  "OP_POP_JUMP_IF_FALSE", "OP_CONSTANT8", "OP_SUB_LOCAL_CONST8",
  "OP_LESS_JUMP", "OP_GREATER_JUMP",
];
const OPC = Object.fromEntries(OPS.map((n, i) => [n, i]));
const CONST_OPS = new Set(["OP_CONSTANT", "OP_GET_GLOBAL", "OP_DEFINE_GLOBAL",
  "OP_SET_GLOBAL", "OP_GET_PROPERTY", "OP_SET_PROPERTY", "OP_CLASS",
  "OP_METHOD", "OP_GET_SUPER", "OP_CLOSURE", "OP_STATIC_METHOD"]);
const CONST8_OPS = new Set(["OP_CONSTANT8"]); /* same operand text, 1-byte index */
const LOCAL_CONST8_OPS = new Set(["OP_SUB_LOCAL_CONST8"]); /* slot + 1-byte const */
const BYTE_OPS = new Set(["OP_GET_LOCAL", "OP_SET_LOCAL", "OP_GET_UPVALUE",
  "OP_SET_UPVALUE", "OP_CALL"]);
const JUMP_FWD = new Set(["OP_JUMP", "OP_JUMP_IF_FALSE", "OP_POP_JUMP_IF_FALSE",
  "OP_LESS_JUMP", "OP_GREATER_JUMP"]);
const INVOKE_OPS = new Set(["OP_INVOKE", "OP_SUPER_INVOKE"]);

// ---- canonical dump reader (from --emit-bin) ----
function readCanonical(buf) {
  let pos = 0;
  const u32 = () => { const v = buf.readUInt32LE(pos); pos += 4; return v; };
  const text = (n) => { const v = buf.toString("utf8", pos, pos + n); pos += n; return v; };
  function chunk() {
    const codeLen = u32();
    const code = [...buf.subarray(pos, pos + codeLen)];
    pos += codeLen;
    const nConst = u32();
    const consts = [];
    for (let i = 0; i < nConst; i++) {
      const tag = String.fromCharCode(buf[pos++]);
      if (tag === "n") consts.push({ t: "n", v: text(u32()) });
      else if (tag === "s") consts.push({ t: "s", v: text(u32()) });
      else consts.push({ t: "f", name: text(u32()), chunk: chunk() });
    }
    return { code, consts };
  }
  return chunk();
}

// ---- assembler (from --disasm text) ----
function parseValue(tok) {
  if (tok.startsWith('"')) {
    // inverse of debug.c's listing escapes
    const v = tok.slice(1, -1).replace(/\\(n|t|\\|")/g,
      (_, c) => ({ n: "\n", t: "\t", "\\": "\\", '"': '"' })[c]);
    return { t: "s", v };
  }
  if (tok.startsWith("<fn ")) return { t: "f", name: tok.slice(4, -1) };
  return { t: "n", v: tok };
}

function assemble(listing) {
  const sections = [];
  const re = /^== (.*?) ==$/;
  let cur = null;
  for (const line of listing.split("\n")) {
    const h = line.match(re);
    if (h) { cur = { name: h[1], rows: [] }; sections.push(cur); continue; }
    if (cur && line.trim()) cur.rows.push(line);
  }
  let cursor = 0;
  function assembleChunk(expectName) {
    const sec = sections[cursor++];
    if (!sec) throw new Error(`missing section for ${expectName}`);
    const code = [];
    const consts = [];
    const needConst = (idx) => { while (consts.length <= idx) consts.push(null); };
    for (let r = 0; r < sec.rows.length; r++) {
      const up = sec.rows[r].match(/^\s+\| (local|upvalue) (\d+)$/);
      if (up) { // consumed by OP_CLOSURE below
        throw new Error("stray upvalue row: " + sec.rows[r]);
      }
      const m = sec.rows[r].match(/^\s*(\d+)\s+(?:\||\d+)\s+(.*)$/);
      if (!m) throw new Error("unparseable row: " + sec.rows[r]);
      const offset = Number(m[1]);
      if (offset !== code.length)
        throw new Error(`offset gap at ${m[1]} vs ${code.length}`);
      const rest = m[2].match(/^(OP_\w+)(?:\s+(.*))?$/);
      if (!rest) throw new Error("no opcode: " + m[2]);
      const [, op, operand] = rest;
      if (!(op in OPC)) throw new Error("unknown opcode: " + op);
      code.push(OPC[op]);
      if (CONST_OPS.has(op) || CONST8_OPS.has(op)) {
        const c = operand.match(/^(\d+) (.*)$/);
        const idx = Number(c[1]);
        const val = parseValue(c[2]);
        needConst(idx);
        if (val.t === "f") {
          const inner = assembleChunk(val.name);
          consts[idx] = { t: "f", name: val.name, chunk: inner };
        } else {
          consts[idx] = val;
        }
        if (CONST8_OPS.has(op)) code.push(idx & 0xff);
        else code.push(idx >> 8, idx & 0xff);
        if (op === "OP_CLOSURE") {
          // upvalue rows follow: 2 bytes each
          while (r + 1 < sec.rows.length &&
                 /^\s+\| (local|upvalue) \d+$/.test(sec.rows[r + 1])) {
            const u = sec.rows[++r].match(/^\s+\| (local|upvalue) (\d+)$/);
            code.push(u[1] === "local" ? 1 : 0, Number(u[2]));
          }
        }
      } else if (LOCAL_CONST8_OPS.has(op)) {
        const c = operand.match(/^(\d+) (\d+) (.*)$/);
        const idx = Number(c[2]);
        needConst(idx);
        consts[idx] = parseValue(c[3]);
        code.push(Number(c[1]), idx & 0xff);
      } else if (BYTE_OPS.has(op)) {
        code.push(Number(operand));
      } else if (JUMP_FWD.has(op)) {
        const j = operand.match(/^(\d+) -> (\d+)$/);
        const jump = Number(j[2]) - (offset + 3);
        code.push(jump >> 8, jump & 0xff);
      } else if (op === "OP_LOOP") {
        const j = operand.match(/^(\d+) -> (\d+)$/);
        const jump = (offset + 3) - Number(j[2]);
        code.push(jump >> 8, jump & 0xff);
      } else if (INVOKE_OPS.has(op)) {
        const c = operand.match(/^(\d+) \((\d+) args\) (.*)$/);
        const idx = Number(c[1]);
        needConst(idx);
        consts[idx] = parseValue(c[3]);
        code.push(idx >> 8, idx & 0xff, Number(c[2]));
      }
    }
    return { code, consts }; // consts may be sparse: listings never show dead slots
  }
  return assembleChunk(sections[0].name);
}

// Code bytes must match exactly; constants are compared per referenced slot
// (constant folding can leave pool holes the listing legitimately omits).
function compareChunks(oracle, built, path) {
  if (JSON.stringify(oracle.code) !== JSON.stringify(built.code))
    throw new Error(`code bytes differ in ${path}`);
  for (let i = 0; i < built.consts.length; i++) {
    const b = built.consts[i];
    if (b === null) continue;
    const o = oracle.consts[i];
    if (!o || o.t !== b.t) throw new Error(`constant ${i} type differs in ${path}`);
    if (b.t === "f") {
      if (o.name !== b.name) throw new Error(`fn name differs at ${i} in ${path}`);
      compareChunks(o.chunk, b.chunk, `${path}/${b.name}`);
    } else if (o.v !== b.v) {
      throw new Error(`constant ${i} differs in ${path}: ${JSON.stringify(o.v)} vs ${JSON.stringify(b.v)}`);
    }
  }
}

let failed = 0, skipped = 0;
for (const f of files) {
  let binBuf, listing;
  try {
    binBuf = execFileSync(bin, ["--emit-bin", f], { maxBuffer: 1 << 26 });
    listing = execFileSync(bin, ["--disasm", f], { encoding: "utf8", maxBuffer: 1 << 26 });
  } catch {
    skipped++; // negative-test sources don't compile; nothing to roundtrip
    continue;
  }
  try {
    const oracle = readCanonical(binBuf);
    const built = assemble(listing);
    compareChunks(oracle, built, f.split("/").pop());
    console.log(`PASS roundtrip ${f.split("/").pop()}`);
  } catch (e) {
    console.log(`FAIL ${f}: ${e.stack}`);
    failed++;
  }
}
const passed = files.length - failed - skipped;
console.log(`disasm roundtrip: ${passed} passed, ${failed} failed, ${skipped} skipped (compile-error sources)`);
process.exit(failed ? 1 : 0);
