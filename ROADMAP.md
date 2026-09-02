# HOKUS Roadmap

Ground truth for "done": a feature is shipped when it passes the full gate
(`make test` — golden corpus, error goldens, disasm goldens, slice sweep,
wasm parity gate, differential fuzz, GC stress, and the external Lox corpus)
and appears in the README or the playground.

## Shipped

- **Compiler** — Pratt parser, full Lox (closures, classes, inheritance,
  `super`), 16-bit constant pool with a 2-byte `OP_CONSTANT8` short form for
  indices < 256 (the folder tracks instruction length by opcode, so folding
  works across both forms), compile-time folding of numeric literals.
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
  importing file. No new opcodes. Native resolves via the filesystem; the
  freestanding WASM build resolves via a preloaded module arena
  (`hk_add_module`/`hk_clear_modules`, exact-path match, persistent across
  runs) — an unregistered path stays a compile error (both gated).

- **Inline cache for OP_INVOKE** — monomorphic (offset, klass) → method cache
  per call site, 8 direct-mapped slots per chunk, marked through the
  function-blacken GC path. 200k method calls: 23.8 → 19.6 ms native; static
  calls (class-object receivers, no field-shadow check needed) sit at 12 ms,
  the cheapest dispatch in the VM.

- **`%` operator** — truncated (fmod-style) modulo at factor precedence,
  one opcode, libm-free shared implementation (`hkModulo`) so compile-time
  folding and the runtime agree bit-for-bit; `%` by zero is a runtime error
  (unlike `/`, which keeps IEEE inf).

- **`OP_POP_JUMP_IF_FALSE`** — statement-position conditions (`if`/`while`/
  `for`) compile to one fused branch-and-pop instead of JUMP_IF_FALSE+POP;
  `and`/`or` keep the old opcode because their short-circuit value must
  survive the jump. fib(30): ~2% native, ~7% WASM.

- **Cached locals window** — the dispatch loop reads locals through a cached
  `frame->slots` (`FRAME_SLOTS`), refreshed only where the frame can change
  (all call branches share a `called:` epilogue; OP_RETURN refreshes inline).
  ~26% on the 1M leaf-call bench, ~2% on fib(30), native; compiled out on
  wasm32, where the extra local measured slower.

- **`OP_SUB_LOCAL_CONST8`** — peephole fusing `local - numeric-literal`
  (GET_LOCAL + CONSTANT8 + SUBTRACT → one 3-byte op; rhs proven numeric at
  compile time so runtime checks only the slot). fib(28): ~12% native,
  ~16% WASM, interleaved A/B.

- **`OP_LESS_JUMP` / `OP_GREATER_JUMP`** — bare comparisons in statement
  conditions (if/while/for) fuse with their discard-branch; operands pop
  directly, no bool round-trip. `and`/`or` keep OP_JUMP_IF_FALSE.
  fib(28): ~6% native, ~4% WASM; leaf calls ~3.5% native (all A/B'd).

- **`OP_SET_LOCAL_POP` / `OP_SET_GLOBAL_POP`** — assignment in discard
  position (expression statements, for-increments) fuses SET_x+POP.
  Fusion marks are instruction-start marks (setStmtStart/cmpJumpStart),
  never positional byte guesses — an operand byte can alias an opcode
  (this bit twice). Leaf native ~5%, fib ~2%.

- **`OP_ADD_LOCAL_CONST8_SET`** — slot += numeric constant (for-increments,
  accumulators) as one op. Leaf: ~11% native, ~12% WASM. (fib WASM reads
  ~3% slower despite never executing the op — jump-table layout effect,
  noted in the commit.)

- **`OP_ADD_LOCAL_CONST8`** — symmetric sibling of the SUB fusion:
  `local + const` (leaf-callee `return a + 1` shapes) as one 3-byte op.
  Compile-verified numeric rhs; a non-number slot keeps OP_ADD's exact
  "two numbers or two strings" error (no concat path fires — rhs is
  never a string). Leaf: ~9% native, ~5% WASM (interleaved A/B); fib
  flat native, ±3% WASM layout noise as before. emitExprPop folds the
  resulting ADD_LOCAL_CONST8; SET_LOCAL(same slot) tail back into
  OP_ADD_LOCAL_CONST8_SET, keeping `i = i + 1` at one dispatch (that
  fold-back alone: leaf ~5% native / ~6% WASM).

- **`OP_LESS_LOCAL_CONST8_JUMP`** — `local < const` branch condition
  (fib's `n < 2`, every `for (i < n)`/while bound) as one 5-byte op:
  slot + const8 + jump16, no stack round-trip. fib(28): ~16% native,
  ~14% WASM; leaf: ~7% both (interleaved A/B, best-of). Shipped behind
  the and/or guard below — see the fix commit 3b49011: and/or patch
  their end-jumps at the operand tail, so any rewind fusion must refuse
  to cross `andOrTarget` or it strands those targets mid-instruction.

- **`OP_ADD_LOCAL_CONST8_SET_LOOP`** — a for-increment (or trailing
  while-body statement) in the fused `i = i + k` form joins its OP_LOOP
  back-edge: one op, increment-then-jump in the original order. New
  compiler mark (incSetStart) tracks the increment's instruction start;
  mark + bytes + adjacency verified, and/or guard as with all rewinds.
  Leaf: ~6% native, ~3% WASM; fib flat (interleaved A/B, best-of).

- **`OP_ADD_LOCAL_CONST8_RETURN`** — `return local + const;` (leaf-callee
  `return a + 1` shapes) as one op; the VM computes, pushes, and shares the
  OP_RETURN epilogue via a computed-goto `goto`. Rewind 3 + emit 3 is
  length-neutral, so no jump-target guard is needed (same accident that
  makes OP_RETURN_LOCAL guard-free). Leaf: ~2.7% native, ~4% WASM;
  wasm fib flat; native fib reads ~+1.7% — verified pure jump-table layout
  (a binary with the op in the table but the fusion disabled, emitting
  byte-identical fib code, shows the identical +1.7%).

- **For-loop rotation** — `for (init; local < const; increment)` compiles
  bottom-tested: [OP_JUMP→cond][body][increment][OP_LESS_LOCAL_CONST8_LOOP
  →body]. The entry jump is paid once and the steady state has no
  unconditional jumps (previously one per iteration, plus a separate body
  back-edge). rotateForLoop swaps increment/body en bloc — jump distances
  inside a moved span are preserved, so no and/or guard is needed; the
  shape check (condition emitted *as* the fused LC8J, adjacent to
  loopStart) is the whole precondition. Leaf: ~11% native, ~11% WASM
  (interleaved A/B); fib flat.

- **`OP_ADD_LOCAL_CONST8_SET_LESS_LOOP`** — mega-fuse of the rotated
  for-loop tail: when the increment is already `OP_ADD_LOCAL_CONST8_SET` on
  the condition's slot, the entry `LC8J` doubles as the exit check, the
  `OP_JUMP` over the increment is dropped, and the bottom becomes one
  6-byte op (slot += k; if slot < N loop). Length-neutral rewrite (3-byte
  JUMP + 3-byte increment → 6-byte op). Leaf: ~7% native, ~9% WASM
  (interleaved A/B, two sessions each); fib flat both hosts.

- **Per-frame constants pointer** — `CallFrame.consts` caches
  `closure->function->chunk->constants.values`, set once when the frame is
  primed (callClosure / vmStartFunction). Every constant-touching op
  (OP_CONSTANT8, all LC8 fusions, READ_CONSTANT names) drops a 5-hop
  pointer chain to one load. Unlike the rejected VM-wide cached-constants
  window, there is nothing to reload on call/return — the pointer is a
  field, stable for the frame's lifetime (pools don't grow mid-run).
  fib(28): ~8% native both A/B sessions, ~3-6% WASM (5/6 sessions; one
  drift outlier up); leaf flat-to-better (no regression anywhere).

## Next (in order)

1. **Call-path work, remaining** — after the slots-window, sub-fusion, and
   compare-branch wins, fib(28) is ~28 ms native; folding CALL+RETURN
   (tail calls) or register-window frame setup are what's left. Any
   candidate must move the fib/leaf benches under interleaved A/B;
   baseline lives in benchmarks/baseline.json.

(Measured and rejected: a monomorphic callee cache for OP_CALL sharing the
chunk IC array — both natives' `callValue` type switch and wasm's br_table
are already cheaper than the cache's site-offset math plus IC loads/stores;
interleaved A/B put fib ~2% slower native and ~3% slower wasm. The earlier
non-interleaved "+4% native" reading was machine drift — A/B before keeping
any call-path candidate.)

(Measured and rejected: a cached-constants window (FRAME_CONSTS, same
refresh discipline as the slots window). fib(28) gained ~3% native but the
1M leaf-call bench lost ~2.4% — the extra reload on every call/return costs
more in call-dominated code than the saved derefs in constant-heavy code.
Mixed A/B = not a win.)

(Measured and rejected: fusing `local(...)` calls into OP_CALL_LOCAL8 —
recursion compiles to GET_LOCAL 0 + CALL, so fib hits it 2M times per run,
but the arg-shift loop and the duplicated callValue site cost more than the
saved dispatch: interleaved A/B ~7% slower native, ~1% slower wasm. A
GET_LOCAL followed by a push is already just a dispatch plus an array
load. Retried once with the compiler lifting the GET_LOCAL out of the
byte stream and a single shared callValue (no duplicated call body):
native fib still ~4% slower across repeat interleaved A/B, wasm fib
gains inconsistent between sessions (one +11%, one +4%) and leaf wasm
moved against non-executing layout. Mixed/noisy = not a win; the callee
store under the args fundamentally can't beat one array load. Dead end.)

(Measured and rejected: a field-offset cache for OP_GET_PROPERTY /
OP_SET_PROPERTY — interned-name tableGet on a capacity-8 table is already a
hash-mask plus one probe; the cache's extra site-offset math and validation
branches made a six-field-churn microbench ~3% *slower*. The semantic
regression golden (`tests/golden/fields.hk`) written for the experiment is
kept as coverage.)

(Measured and rejected: RETURN consuming the caller's result-discard op —
the return epilogue peeked at the caller's next byte and wrote SET_LOCAL_POP
results straight into the caller slot (POP got skipped outright), saving a
dispatch plus the push/pop round-trip. Interleaved A/B: leaf −2%/−7%
(native/wasm) but fib +1%/+2% native — every miss of the peek taxes the
return-heavy recursion path. Mixed A/B = not a win; reverted. Lesson: any
branch added to the shared return epilogue is paid by all 1.66M fib
returns, matching or exceeding what the hit path saves.)

(Measured and rejected: OP_CALL_GLOBAL — fusing `name(args)` with a global
callee into one 4-byte op (compiler slides the arg bytecode left over the
3-byte OP_GET_GLOBAL, VM shifts args up one slot and plants the callee
underneath, sharing callValue). Unlike CALL_LOCAL8 this replaces a hash
tableGet, but the saved dispatch + push still lose to the shift: in-language
clock() interleaved A/B (best-of-9): leaf 39.5 → 41.8 ms native (+6%),
wasm leaf flat-to-worse across repeated harness runs, fib noise both hosts.
The callee-store-under-args pattern is now confirmed dead for both callee
kinds — anything wanting it must eliminate the shift, not redecorate it.
Harness note: whole-process /usr/bin/time at 10 ms resolution read this as
"flat 30/30" when in-language clock() showed +6% — A/B native runs must use
the clock() wrap, not wall-clock process timing.)

(Module namespaces are parked: true namespacing needs per-module
environments for functions that reference module globals — include
semantics is the shipped model.)

## Non-goals

- jlox compatibility shims (the 4 corpus skips are documented design
  divergences, not debt).
- A JIT — the computed-goto interpreter at NaN-boxed speed is the point.
