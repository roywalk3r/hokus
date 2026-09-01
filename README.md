# HOKUS

A complete programming language stack, built from the metal up in freestanding C —
and the *same VM* running natively on Linux and in the browser as raw WebAssembly.

```
source ─► lexer ─► Pratt parser ─► single-pass bytecode compiler ─► stack VM
                                                                  │
                                        native CLI (libc) ───────┴────── WASM (no libc)
```

![Playground: source, live bytecode disassembly, and VM output](docs/playground-run.png)

## The layers

| Layer | Where | Notes |
|---|---|---|
| Bytecode VM | `src/vm.c` | stack machine, 64 call frames, call-by-value functions |
| Objects | `src/value.c` | heap strings interned in a global weak-key table (FNV-1a), custom `%.14g` formatter |
| Globals | `src/table.c` | open-addressing hash table with tombstones |
| Front end | `src/lexer.c`, `src/compiler.c` | Pratt parser, single-pass codegen, locals, jumps for `if`/`while`/`for`/`and`/`or` |
| Tooling | `src/debug.c` | disassembler (`--disasm`) |
| Native host | `src/main.c` | REPL + file runner |
| Images | `src/image.c` | `.hki` bytecode images: compile once, run without source (line table preserved for error traces) |
| Browser host | `src/wasm_shim.c` | bump allocator over a static 16 MB heap, output captured in a static buffer — **zero imports, zero emscripten** |
| Webapp | `web/index.html` | playground: source editor, live bytecode pane, execution trace, **step-through debugger**; JS only shuttles bytes in/out, all logic is the C VM |
| GC | `src/gc.c` | mark-sweep over the VM's root set; the WASM arena sweeps by unlinking |

## Language

```lox
fun mod(a, b) {
    while (a >= b) a = a - b;
    return a;
}
for (var i = 1; i <= 20; i = i + 1) {
    if (mod(i, 15) == 0) print "FizzBuzz";
    else if (mod(i, 3) == 0) print "Fizz";
    else print i;
}
```

Numbers (doubles), booleans, nil, strings with escapes and `+` concat; globals &
block-scoped locals; `if/else`, `while`, `for`, `and`, `or`, `!`; first-class
functions with recursion and **closures**; **classes** with methods, initializers,
`this`, inheritance, and `super`; **static methods** (`static m() {}` — stored
on the class object, inherited, no `this`); `print`; a `clock()` native.
Deviations from stock Lox: `%` — truncated modulo on numbers
(`print 17 % 5;` → `2`, `x % 0` is a runtime error), since the freestanding
build can't rely on libm. Otherwise none (the corpus's `static` tests are
upstream of this book chapter, so statics are golden-tested locally).

Programs span files with `import "lib.hk";` — include semantics (the module's
top-level declarations compile right into the importing unit), resolved
relative to the importing file, top-level only, circular imports rejected at
compile time. Native host only: the freestanding WASM build has no filesystem
and reports `Cannot import module "..."` as a compile error.

## Performance

Recursive `fib(28)` (~1.03M calls): **~35 ms native**, **~46 ms raw WASM** in Node
(warm; ~90 ms cold on first instantiation) — dispatch is a computed-goto jump table
(labels as values) on both hosts, statement conditions compile to a fused
`OP_POP_JUMP_IF_FALSE` (branch+pop in one op), and values are NaN-boxed 64-bit
words, so boxing a number is a move, not a heap round-trip. A 200k-iteration method-call loop runs
**~20 ms native, ~27 ms WASM** — `OP_INVOKE`/`OP_SUPER_INVOKE` dispatch directly
without allocating a bound-method object per call (~24% faster than the
GET_PROPERTY+CALL path). The no-libc WASM build tracks within **~1.3× of native** —
same C object code, same clang. Reproduce: `node tests/bench.js`.

## Build & run

```bash
make                  # native: bin/hokus
make wasm             # web/hokus.wasm — clang --target=wasm32, no stdlib, no emscripten
bin/hokus examples/fib.hk
bin/hokus             # REPL (multi-line aware: paste whole fun/class blocks)
bin/hokus -d examples/strings.hk   # disassembler
bin/hokus -t examples/fib.hk       # execution trace
bin/hokus -s examples/fib.hk       # token dump (jlox format)
bin/hokus --slice 100 examples/fib.hk   # run in 100-instruction slices
HOKUS_STATS=1 bin/hokus examples/fib.hk # allocation + GC stats on stderr
bin/hokus --emit-image examples/fib.hk > fib.hki   # compile to bytecode image
bin/hokus fib.hki            # run with no source in sight
npm run playground    # serve web/ at http://localhost:8000
make artifact         # hokus-artifact.html — whole playground, one file, WASM embedded
```

## Verification — the external gate

Self-written golden tests (`tests/golden/`) are not the claim. The claim is the
**official Crafting Interpreters Lox test corpus** (Bob Nystrom, MIT) — a
third-party suite that knows nothing about this implementation:

```bash
./tests/fetch_lox.sh   # clones the corpus once
make test
```

Current: **247 passed, 14 smoke, 0 failed, 4 skipped** — the skips are documented
divergences (16-bit constant pool by design; jlox REPL-semantics files). Scan-mode
(`--scan`, jlox token-dump format) covers the corpus's `scanning/` chapter.

The same `make test` also gates the **WASM boundary** (`tests/wasm_runner.js`):
every golden program runs identically under raw WASM, and the exported API surface
(`hk_run`, `hk_run_trace`, `hk_disasm`, `hk_prepare`/`hk_step`) is checked —
including the property that budgeted step slices produce byte-identical output to
a full run. Natively, `--slice N` runs any program in N-instruction budgeted
slices, so the re-entrant interpreter is gated on both hosts. `HOKUS_SLICE_SWEEP=1
./tests/run.sh` re-runs the entire corpus at `--slice 17`, proving the pause/resume
interpreter byte-identical across all 266 third-party programs, not just goldens.
The gate also runs a **disassembly roundtrip** (`tests/disasm_roundtrip.js`):
every `--disasm` listing — goldens, examples, and the full corpus — is
*assembled back* into bytecode and required to match the compiled chunk tree
(`--emit-bin` oracle), so the disassembler can never drift from the bytes it
describes.

## Design notes

- **One allocator boundary.** Core code never calls `malloc` directly; everything
  goes through `HK_REALLOC`. Native builds map that to `realloc`, the WASM build
  to a bump allocator over a static heap — so the VM is the same object code in
  both worlds.
- **One output boundary.** `hkEmit` is a host-provided sink: `fputs` natively,
  a buffer the JS host drains in the browser. Compiler errors, runtime errors,
  and `print` all flow through it.
- **Mark-sweep GC.** Collection runs against the active VM's stack, frames, and
  globals; on the WASM host, sweeping only unlinks (the bump arena never frees),
  reset wholesale between runs.
- **Re-entrant interpreter.** All execution state lives in the `VM` struct, so the
  run loop is `vmStart` + `vmRunSlice(budget)` — pause/resume at instruction
  granularity. That's what makes the browser's step-through debugger a thin shell
  over `hk_step`, not a separate interpreter.

## License

MIT — see [LICENSE](LICENSE).
