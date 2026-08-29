#!/usr/bin/env node
// Runs the official Crafting Interpreters Lox test corpus against HOKUS,
// restricted to the HOKUS language subset (no classes/closures/etc.).
// Parse `// expect: <line>` annotations and diff program stdout against them.
// Files containing expected-error markers or unsupported features are SKIPPED
// and reported as such — never silently.
import { execFileSync } from "node:child_process";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";

const [bin, root] = process.argv.slice(2);

// Directories whose semantics HOKUS deliberately does not implement.
const SKIP_DIRS = new Set([
  "benchmark", "class", "closure", "constructor", "field", "inheritance",
  "method", "super", "this", "regression", "limit",
  "scanning", // asserts the scanner's token dump — jlox-implementation-specific
]);

// Feature tokens that place a file outside the subset.
const UNSUPPORTED = [
  /\bclass\b/, /\bthis\b/, /\bsuper\b/, /\bstatic\b/,
  /closure/i, /local_recursion/, // closures not in HOKUS subset
];

function* walk(dir) {
  for (const entry of readdirSync(dir)) {
    const p = join(dir, entry);
    if (statSync(p).isDirectory()) yield* walk(p);
    else if (entry.endsWith(".lox")) yield p;
  }
}

let pass = 0, fail = 0, skip = 0;
const failures = [];

for (const path of walk(root)) {
  const rel = relative(root, path);
  const top = rel.split("/")[0];
  const src = readFileSync(path, "utf8");

  let why = null;
  if (SKIP_DIRS.has(top)) why = `subset (${top}/)`;
  else if (UNSUPPORTED.some((re) => re.test(src) || re.test(rel)))
    why = "unsupported feature";
  else if (src.includes("expect error") || src.includes("Error at") ||
           src.includes("expect runtime error"))
    why = "error-annotation test";
  else if (!/\bprint\b/.test(src))
    // jlox-REPL tests (expressions/, AST dumping): a bytecode VM has no
    // implicit value printing, so there is no observable output to diff.
    why = "no observable output (REPL-semantics test)";
  if (why) { skip++; continue; }

  const expected = [...src.matchAll(/\/\/ expect: ?(.*)/g)]
    .map((m) => m[1]).join("\n");

  let out = "", code = 0;
  try {
    out = execFileSync(bin, [path], { encoding: "utf8", timeout: 10_000 });
  } catch (e) {
    code = e.status ?? 1;
    out = (e.stdout ?? "") + (e.stderr ?? "");
  }

  if (code === 0 && out.trimEnd() === expected.trimEnd()) {
    pass++;
  } else {
    fail++;
    failures.push({ rel, expected, out, code });
  }
}

console.log(`lox suite: ${pass} passed, ${fail} failed, ${skip} skipped (out of subset)`);
for (const f of failures.slice(0, 10)) {
  console.log(`\nFAIL ${f.rel} (exit ${f.code})`);
  console.log("  expected:", JSON.stringify(f.expected.slice(0, 200)));
  console.log("  actual:  ", JSON.stringify(f.out.trimEnd().slice(0, 200)));
}
process.exit(fail ? 1 : 0);
