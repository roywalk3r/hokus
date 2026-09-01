# HOKUS Roadmap

Ground truth for "done": a feature is shipped when it passes the full gate
(`make test` — golden corpus, error goldens, disasm goldens, slice sweep,
wasm parity gate, differential fuzz, GC stress, and the external Lox corpus)
and appears in the README or the playground.

## Shipped

- **Compiler** — Pratt parser, full Lox (closures, classes, inheritance,
  `super`), 16-bit constant pool, compile-time folding of numeric literals.
- **VM** — NaN-boxed values (one 64-bit word per slot), computed-goto
  dispatch, `OP_INVOKE`/`OP_SUPER_INVOKE` fast paths, static methods
  (`class A { static m() {} }` on a dedicated class-side table, inherited,
  no `this` slot), string interning,
  mark-sweep GC with weak-key intern table, free-list allocator on the
  wasm bump heap.
- **CLI** — REPL (brace-depth-aware multi-line input for pasted
  fun/class blocks), `--disasm`, `--trace`, `--scan` (jlox token dump),
  `--slice N` budgeted execution, `HOKUS_STATS=1` allocation/GC counters.
- **WASM host** — freestanding C compiled with `clang --target=wasm32`,
  zero imports, 32 MB static heap; same core `.c` files as native.
- **Playground** — browser editor with live bytecode pane, execution trace,
  step-through debugger, and a status line showing allocs/GC/heap per run.
  Ships as a self-contained `hokus-artifact.html` (works from `file://`).
- **Gates** — official Crafting Interpreters corpus (247 pass / 14 smoke /
  0 fail / 4 documented skips), differential fuzzer (native vs wasm,
  byte-identical output, seeded valid-program generator incl. closures and
  error paths), GC collect-on-every-allocation stress build gated on the
  golden corpus and the closure/class/string/function subtrees),
  differential shapes incl. shared-upvalue closures and closures escaping
  class methods.
- **Disasm roundtrip gate** — `tests/disasm_roundtrip.js` assembles every
  `--disasm` listing back into bytecode and compares against the compiled
  chunk tree (`--emit-bin` oracle): code bytes exact, constants per
  referenced slot. Runs over goldens, examples, and the full Lox corpus.

- **Bytecode images** — `--emit-image` writes a `.hki` (magic `HKI1`,
  raw f64 bits, line table preserved); `hokus file.hki` runs it with no
  source. Gated on goldens, error-golden traces, and corpus parity sweeps.

- **Benchmark trendlines** — `tests/bench.js` times both hosts (native via
  an in-language `clock()` wrap, so compile+spawn stay out of the number),
  records `benchmarks/latest.json`, and `--check` fails beyond 1.5× the
  committed `benchmarks/baseline.json`.

- **Corpus mutation fuzz** — token-level mutants of the third-party corpus
  (delete/swap/duplicate/number-tweak), native vs wasm byte-parity (2 mutants
  × 265 files in the gate). Mutants run step-sliced on wasm with an
  instruction cap so infinite loops can't hang the gate.

- **Modules** — `import "lib.hk";` compiles the module inline into the
  importing unit (include semantics: its top-level declarations land in the
  same scope), top-level only, cycle-detected, resolved relative to the
  importing file. No new opcodes. Native host only — the freestanding WASM
  build has no filesystem and reports `import` as a compile error (gated).

- **Inline cache for OP_INVOKE** — monomorphic (offset, klass) → method cache
  per call site, 8 direct-mapped slots per chunk, marked through the
  function-blacken GC path. 200k method calls: 23.8 → 19.6 ms native; static
  calls (class-object receivers, no field-shadow check needed) sit at 12 ms,
  the cheapest dispatch in the VM.

- **`%` operator** — truncated (fmod-style) modulo at factor precedence,
  one opcode, libm-free shared implementation (`hkModulo`) so compile-time
  folding and the runtime agree bit-for-bit; `%` by zero is a runtime error
  (unlike `/`, which keeps IEEE inf).

## Next (in order)

1. **Field-offset cache for OP_GET_PROPERTY/OP_SET_PROPERTY** — the field-get
   bench is the remaining table-lookup hot spot; cache (klass, name) → stable
   field slot, invalidated on class-redefinition or table rehash. Wins are
   real only if tests/bench.js's field bench improves; baseline lives in
   benchmarks/baseline.json.

(Module namespaces are parked: true namespacing needs per-module
environments for functions that reference module globals — include
semantics is the shipped model.)

## Non-goals

- jlox compatibility shims (the 4 corpus skips are documented design
  divergences, not debt).
- A JIT — the computed-goto interpreter at NaN-boxed speed is the point.
