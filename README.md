# HOKUS

A complete programming language stack, built from the metal up in freestanding C —
and the *same VM* running natively on Linux and in the browser as raw WebAssembly.

```
source ─► lexer ─► Pratt parser ─► single-pass bytecode compiler ─► stack VM
                                                                  │
                                        native CLI (libc) ───────┴────── WASM (no libc)
```

## The layers

| Layer | Where | Notes |
|---|---|---|
| Bytecode VM | `src/vm.c` | stack machine, 64 call frames, call-by-value functions |
| Objects | `src/value.c` | heap strings (FNV-1a hashed interning key), custom `%.14g` formatter |
| Globals | `src/table.c` | open-addressing hash table with tombstones |
| Front end | `src/lexer.c`, `src/compiler.c` | Pratt parser, single-pass codegen, locals, jumps for `if`/`while`/`for`/`and`/`or` |
| Tooling | `src/debug.c` | disassembler (`--disasm`) |
| Native host | `src/main.c` | REPL + file runner |
| Browser host | `src/wasm_shim.c` | bump allocator over a static 4 MB heap, output captured in a static buffer — **zero imports, zero emscripten** |
| Webapp | `web/index.html` | playground; JS only shuttles bytes in/out, all logic is the C VM |

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

Numbers (doubles), booleans, nil, strings with `+` concat; globals & block-scoped
locals; `if/else`, `while`, `for`, `and`, `or`, `!`; first-class functions with
recursion; `print`. Deviations from stock Lox: no classes, closures, natives,
or string escapes (yet).

## Build & run

```bash
make                  # native: bin/hokus
make wasm             # web/hokus.wasm — clang --target=wasm32, no stdlib, no emscripten
bin/hokus examples/fib.hk
bin/hokus             # REPL
bin/hokus -d examples/strings.hk   # disassembler
npm run playground    # serve web/ at http://localhost:8000
```

## Verification — the external gate

Self-written golden tests (`tests/golden/`) are not the claim. The claim is the
**official Crafting Interpreters Lox test corpus** (Bob Nystrom, MIT) — a
third-party suite that knows nothing about this implementation:

```bash
./tests/fetch_lox.sh   # clones the corpus once
make test
```

Current: **60/60 in-subset tests pass**, 205 skipped with reasons reported
(classes, closures, `clock`, REPL-semantics files with no `print` — the skip
list is in `tests/lox_runner.js`, loud, not silent).

## Design notes

- **One allocator boundary.** Core code never calls `malloc` directly; everything
  goes through `HK_REALLOC`. Native builds map that to `realloc`, the WASM build
  to a bump allocator over a static heap — so the VM is the same object code in
  both worlds.
- **One output boundary.** `hkEmit` is a host-provided sink: `fputs` natively,
  a buffer the JS host drains in the browser. Compiler errors, runtime errors,
  and `print` all flow through it.
- **No GC (yet).** Objects live for the VM's lifetime; the WASM arena is
  reset per run. A mark-sweep collector over the shadow stack is the natural
  next layer.
