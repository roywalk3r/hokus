#!/usr/bin/env node
// Runs the official Crafting Interpreters Lox test corpus against HOKUS,
// restricted to the HOKUS language subset (no static methods/etc.).
// Parse `// expect: <line>` annotations and diff program stdout against them.
// Error-annotation tests are verified against compiler/VM diagnostics:
//   // [line N] Error at 'x': MSG   → exit 65, output shows [line N] + MSG
//   // [c line N] Error at end: MSG → exit 65 (c = compile)
//   // expect runtime error: MSG    → exit 70, ordered expectations then MSG
// Files containing unsupported features are SKIPPED — never silently.
import { execFileSync } from "node:child_process";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";

const [bin, root] = process.argv.slice(2);

// Individual files HOKUS deliberately diverges on.
const SKIP_FILES = new Map([
  // HOKUS widened constant operands to 16 bits (65536 slots); these assert
  // the corpus's 8-bit-of-constants failure mode.
  ["limit/no_reuse_constants.lox", "16-bit constant pool by design"],
  ["limit/too_many_constants.lox", "16-bit constant pool by design"],
  // jlox REPL/AST-printing chapter scaffolding, not program semantics.
  ["expressions/evaluate.lox", "REPL-semantics test"],
  ["expressions/parse.lox", "REPL-semantics test"],
]);

function* walk(dir) {
  for (const entry of readdirSync(dir)) {
    const p = join(dir, entry);
    if (statSync(p).isDirectory()) yield* walk(p);
    else if (entry.endsWith(".lox")) yield p;
  }
}

// True if each needle occurs in haystack, in order.
function orderedContains(haystack, needles) {
  let pos = 0;
  for (const n of needles) {
    const at = haystack.indexOf(n, pos);
    if (at < 0) return false;
    pos = at + n.length;
  }
  return true;
}

let pass = 0, fail = 0, skip = 0, smoke = 0;
const failures = [];

for (const path of walk(root)) {
  const rel = relative(root, path);
  const top = rel.split("/")[0];
  const src = readFileSync(path, "utf8");

  let why = null;
  if (SKIP_FILES.has(rel)) why = SKIP_FILES.get(rel);
  if (why) { skip++; continue; }

  const expects = [...src.matchAll(/\/\/ expect: ?(.*)/g)].map((m) => m[1]);
  // Matches `// [line N] Error: M`, `// [line N] Error at 'x': M`,
  // `// [c line N] Error at end: M`, `// Error at 'x': M`.
  // Deliberately ignores `[java line N]` (jlox-specific diagnostics).
  const compileErrs = [...src.matchAll(/\/\/ (?:\[(?:c )?line (\d+)\] )?Error(?: at (?:'[^']*'|end))?: ?(.*)/g)]
    .map((m) => ({ line: m[1] ? +m[1] : null, msg: m[2] }));
  const runtimeErrs = [...src.matchAll(/\/\/ expect runtime error: ?(.*)/g)].map((m) => m[1]);

  let out = "", code = 0;
  try {
    // scanning/ asserts the token stream: run in --scan mode
    const args = top === "scanning" ? ["--scan", path] : [path];
    // zoo_batch/zoo are clock-bounded benchmarks that run ~10s by design
    out = execFileSync(bin, args, { encoding: "utf8", timeout: 30_000 });
  } catch (e) {
    code = e.status ?? 1;
    out = (e.stdout ?? "") + (e.stderr ?? "");
  }

  if (compileErrs.length > 0) {
    // Expect a compile error: exit 65, and every annotated (line, message)
    // must appear in the output.
    let ok = code === 65;
    for (const ce of compileErrs) {
      if (ce.line !== null && !out.includes(`[line ${ce.line}]`)) ok = false;
      if (!out.includes(ce.msg)) ok = false;
    }
    if (ok) pass++;
    else {
      fail++;
      failures.push({
        rel, code,
        expected: `exit 65 + ` + compileErrs.map((c) => `[line ${c.line}] ${c.msg}`).join("; "),
        out,
      });
    }
    continue;
  }

  if (runtimeErrs.length > 0) {
    // Expect a runtime error: exit 70, any `// expect:` lines print first (in
    // order), then the runtime error message.
    const ok = code === 70 &&
      orderedContains(out, [...expects, ...runtimeErrs]);
    if (ok) pass++;
    else {
      fail++;
      failures.push({
        rel, code,
        expected: `exit 70 + ` + [...expects, ...runtimeErrs].join(" | "),
        out,
      });
    }
    continue;
  }

  if (expects.length === 0) {
    // Smoke test: no output contract, but the program must not crash or error.
    if (code === 0) smoke++;
    else {
      fail++;
      failures.push({ rel, expected: "<exit 0>", out, code });
    }
    continue;
  }

  if (code === 0 && out.trimEnd() === expects.join("\n").trimEnd()) {
    pass++;
  } else {
    fail++;
    failures.push({ rel, expected: expects.join("\n"), out, code });
  }
}

console.log(`lox suite: ${pass} passed, ${smoke} smoke, ${fail} failed, ${skip} skipped (out of subset)`);
for (const f of failures.slice(0, 10)) {
  console.log(`\nFAIL ${f.rel} (exit ${f.code})`);
  console.log("  expected:", JSON.stringify(f.expected.slice(0, 200)));
  console.log("  actual:  ", JSON.stringify(f.out.trimEnd().slice(0, 200)));
}
process.exit(fail ? 1 : 0);
