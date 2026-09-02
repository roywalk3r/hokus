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

- **`OP_GET_LOCAL_PROPERTY` / `OP_SET_PROPERTY_POP`** — `local.field` reads
  (`this.n` twice per method call) fuse the GET_LOCAL receiver read into
  the property op (mark + bytes + adjacency, and/or guard); statement-
  position stores fuse SET_PROPERTY+POP (length-neutral). An early version
  that refactored plain OP_GET_PROPERTY through a shared helper cost churn
  ~3% WASM — the shipped version leaves that op body byte-identical and
  duplicates the logic in the fused op. Method calls: ~4-6% native, ~7-9%
  WASM; get/set churn: ~3% native; leaf/field/static flat. fib(28) native
  reads ~+4% — verified jump-table layout (fib has no property ops, so its
  bytes are identical in all three binaries: prev 19.43, fusion-disabled
  twin 21.39, shipped 20.24 — the op-table growth alone moves it; the
  shipped build is closer to prev than the twin). The golden_disasm
  `classes` listing picked up the fused forms (single intended diff).

- **While-loop mega-fuse** — `while (local < const)` bodies ending in the
  fused `i = i + k` on the condition's slot get the same mega op as rotated
  for-loops, but with no rotation at all: a while loop's natural shape is
  already `[LC8J exit check][body][increment][back-edge]`, so the transform
  deletes the 3-byte increment and replaces OP_LOOP with
  OP_ADD_LOCAL_CONST8_SET_LESS_LOOP (net +3 bytes; patchJump lands on the
  same exit address either way — jump distances inside the body are
  untouched). Guard set mirrors the for-mega: mark/bytes/adjacency, slot
  must equal the condition slot (else plain LOOP), and/or tail-rewind
  refusal. New bench (`1M while iterations`): −9% native, −15..26% WASM
  across two interleaved best-of sessions; fib/leaf/invoke controls flat
  within noise (focused fib-wasm interleave: +1.7%, inside the documented
  ±3% wasm layout band — the change adds no opcode, the jump table is
  byte-identical, only the compiler's while arm moves).

- **While-loop rotation (non-mega tails)** — `while (local < const)` bodies
  that don't end in the fused increment rotate the same way for-loops do:
  the entry `LC8J` (5 bytes) becomes a one-time `OP_JUMP` (3 bytes) to the
  bottom check, the body slides left 2 bytes en bloc (internal jump
  distances preserved — the exitJump operand is deliberately NOT patched
  afterward, since its bytes now hold relocated body code), and the bottom
  gets `OP_LESS_LOCAL_CONST8_LOOP` back to the body. Length-neutral
  (5+3 → 3+5). Scoped-local tails (`var` in the body ⇒ trailing `OP_POP`)
  land here instead of the mega path, which is why both matter. New bench
  (`1M while+calls`): −4.8% native, −5.7%/−4.5% WASM across two
  interleaved sessions; fib/leaf/mega-while controls flat.

- **While-mega past scope-close POPs** — a body-scoped `var` leaves
  `OP_POP`(s) between the trailing increment and the loop end, which used
  to miss the mega guard and fall back to rotation (one extra dispatch per
  iteration). The increment commutes with the POP run (different slots,
  nothing allocates between; jump targets never land inside a single-byte
  POP run, so a 3-byte slide is distance-preserving), so the compiler now
  slides the inc past the POPs and applies the mega fuse on top. New bench
  (`1M while scoped-var`): −4.3%/−5.0% native, −8.4%/−9.5% WASM across two
  interleaved sessions; while/while+calls/fib controls flat both hosts.

- **`OP_ADD_LOCAL_LOCAL` / `OP_ADD_LOCAL_LOCAL_SET`** — `b + c` with both
  operands locals (expression form, 3 bytes: two adjacent GET_LOCALs at the
  chunk tail, byte-verified, and/or guard) and `a = b + c;` as one 4-byte
  statement op (ADD_LOCAL_LOCAL + SET_LOCAL fold in emitExprPop; dst may
  differ from both sources). String pairs route through the shared
  concatenate in OP_ADD's own check order (strings first), error text
  byte-identical. Biggest single win of the fusion series: 1M-iteration
  while accumulator −51% native / −40% WASM, scoped-var variant −32% /
  −15% (two interleaved sessions). Controls: fib native flat; fib/leaf
  WASM read +4-8% but hold byte-identical code — 3-way prev/twin/cur with
  a fusion-disabled twin (ops in the table, compiler fusion off) shows the
  twin regresses exactly as much as the shipped build (fib wasm 30.1 /
  32.3 / 32.2), so this is jump-table growth, not the op bodies. The
  `classes` disasm golden picked up the fused form (single intended diff).

- **`OP_MUL_LOCAL_CONST8` / `OP_MUL_LOCAL_CONST8_SET`** — `local * const`
  (`t = i * 2` scaling shapes) via the existing generic tryFuseLocalConstOp
  (expression form, 3 bytes) plus a statement fold in emitExprPop
  (dst independent, 4 bytes — the general-dst form ADD_LOCAL_LOCAL_SET
  established). Scoped-var while bench: −17% native, −19..31% WASM across
  two interleaved sessions. The mega-while accumulator (no multiply) reads
  ~+13% slower native with byte-identical code in all three binaries —
  3-way prev/twin/cur with a fusion-disabled twin (ops in the table,
  fusion sites off) puts twin ≈ cur (5.69 / 6.59 / 6.57), i.e. pure
  jump-table growth again; fib/leaf flat. Third instance of the
  documented layout-attribution precedent.

- **`OP_ADD_LOCAL_PROPERTY_CONST8_SET`** — `r.n = r.n + k` (same local
  receiver, same property, numeric const — every `this.x = this.x + 1`
  method-body accumulator) as one 5-byte op. The five-instruction tail
  (GET_LOCAL recv; GET_LOCAL_PROPERTY; CONSTANT8; ADD; SET_PROPERTY) is
  verified with two instruction-start marks: the SET_PROPERTY anchor plus
  a new propSetRecvStart snapshot taken in dot() before the rhs compiles
  (localGetStart is clobbered by the rhs's own receiver GET_LOCAL/fusion).
  Non-neutral 12→5 rewind, and/or guard as always. The VM routes the read
  through doGetProperty and re-checks IS_INSTANCE for the store, so every
  slow path (class-receiver statics, method binding, "Only instances have
  fields/properties.", "Undefined property", the ADD type error) is
  byte-identical to the unfused chain — verified by stash-cycle output
  diffs across six probe programs. 200k-method-call bench: −9% native
  (focused best-of-11 interleave: 16.59 → 15.10; full-harness native runs
  that session read +1.4/+1.8% — drift, confirmed by the focused
  interleave), −9..15% WASM across two interleaved full sessions and the
  3-way (invoke wasm 18.43 / 18.68 / 15.95 prev/twin/cur — twin ≈ prev on
  the target itself, since the twin emits the unfused tail). Controls with
  byte-identical code (getset/fib/leaf): flat within the layout band both
  hosts, twin ≈ cur.

- **`OP_RETURN_LOCAL_PROPERTY`** — `return local.prop;` (the getter idiom:
  every `return this.n`) as one 4-byte op; doGetProperty leaves the value
  on the stack top, which is exactly what the shared RETURN epilogue pops
  (`goto op_OP_RETURN`, same as ADD_LOCAL_CONST8_RETURN). Mark discipline:
  dot()'s property fusion now records propGetStart — a plain localGetStart
  anchor is impossible because the fusion rewinds the receiver GET_LOCAL
  the mark was pointing at. Method-call bench: WASM −3..5% across four
  interleaved 3-way/full sessions (invoke 16.43/16.25/15.86 and
  16.00/15.82/15.49 prev/twin/cur — twin ≈ prev confirms the fusion is the
  delta; one contradicted session self-flagged via twin ≠ prev on
  byte-identical leaf, a noise tell); native flat (±1% across focused
  best-of-11/13 interleaves — one saved dispatch is below the native
  measurement floor at ~80 ns/call), controls flat both hosts.

- **`OP_SUB_LOCAL_CONST8_SET` + `OP_GREATER_LOCAL_CONST8_JUMP`** —
  `i = i - k;` decrement statements (mirror of the += fold: same-slot
  SUB_LOCAL_CONST8; SET_LOCAL → one 3-byte op, sets the incSetStart mark so
  downstream loop fusions see it) and `local > const` branch conditions
  (emitCondJump's deep pattern relaxed from LESS-only to LESS|GREATER with a
  ternary opcode pick — one arm, both comparisons). New countdown bench
  (1M `while (i > 0) { s += i; i = i - 1; }`): native 14.79 → 8.21 ms (−44%),
  WASM 24.71 → 15.34 ms (−38%), 9/6-round interleaved A/B. Control fib moved
  +3.6% native / +5.7% WASM in the first session; the 3-way twin control
  resolved it as noise, not layout and not regression: twin (cur layout,
  fusion arms disabled) came in anomalously slow on BOTH hosts (24.79 native
  / 37.23 wasm vs prev 19.83 / 31.13) while cur ≈ prev (20.56 / 30.89). If
  the gap were layout, cur would track the twin; it doesn't. Third kind of
  twin tell recorded: twin anomaly ≠ prev/cur spread flags a poisoned twin
  build, not a noisy session.

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

(Measured and rejected: OP_GET_GLOBAL_PROPERTY — the global-receiver sibling
of the OP_GET_LOCAL_PROPERTY win (`a = p.x` with p a global: GET_GLOBAL +
GET_PROPERTY adjacent, fused at emission, no slide, no order change). Two
interleaved A/B sessions agreed: the target get/set-churn microbench gained
≤1% native but *lost* ~1-1.5% WASM, and invoke/leaf — which never execute
the op — read ~+1-4% slower on both hosts across sessions. A global load's
hash-mask probe is short enough that saving one dispatch doesn't cover the
br_table/dispatch-loop size tax on wasm, and the non-executing regressions
are consistent (not single-session drift), so even the layout-attribution
precedent can't save it — the target bench itself doesn't win. Reverted;
the sibling INVOKE variant is skipped by design (it would need the rejected
CALL_GLOBAL en-bloc slide).)

(Measured and skipped without building: an inline cache on OP_GET_GLOBAL.
Decomposed the global-vs-local loop gap first, native best-of-11 over 1M
iterations: local fused 3.04 ms, local with fusion defeated (`i = 1 + i`,
disasm-confirmed unfused) 9.36 ms, global 25.28 ms. That brackets the
22.2 ms gap at ~50% table lookup (~3.7 ns / ~13 cycles per masked hit)
and ~50% dispatches the fuser cannot collapse. An IC only shortens the
lookup half — ceiling ~7 ms of 22 — while adding per-site cache storage,
a rehash generation counter, and GC rules for the cached Entry*. Slot
resolution takes both halves for the same effort, because a uint8 global
slot is structurally what a local already is, so the whole existing
fusion family applies to it unchanged. Go there instead.)

(Module namespaces are parked: true namespacing needs per-module
environments for functions that reference module globals — include
semantics is the shipped model.)

## Non-goals

- jlox compatibility shims (the 4 corpus skips are documented design
  divergences, not debt).
- A JIT — the computed-goto interpreter at NaN-boxed speed is the point.
