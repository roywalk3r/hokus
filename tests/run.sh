#!/usr/bin/env bash
# Golden tests (self-authored) + external Lox suite (third-party gate).
set -u
cd "$(dirname "$0")/.."
BIN=./bin/hokus
fail=0

echo "== cli flags =="
$BIN --help >/dev/null 2>&1 && echo "PASS --help" || { echo "FAIL --help"; fail=1; }
$BIN --unknown-flag >/dev/null 2>&1
[ $? -eq 64 ] && echo "PASS unknown-flag-exits-64" || { echo "FAIL unknown-flag"; fail=1; }
$BIN --trace examples/fib.hk 2>&1 | grep -q 'OP_LOOP' && echo "PASS --trace" || { echo "FAIL --trace"; fail=1; }
HOKUS_STATS=1 $BIN examples/fib.hk 2>&1 | grep -qx 'stats: 8 allocations, 0 GC collections' \
    && echo "PASS stats" || { echo "FAIL stats"; fail=1; }

echo "== golden tests =="
for f in tests/golden/*.hk; do
    name=$(basename "$f" .hk)
    if $BIN "$f" > /tmp/hokus_out 2>&1 && diff -u "tests/golden/$name.expected" /tmp/hokus_out > /tmp/hokus_diff; then
        echo "PASS $name"
    else
        echo "FAIL $name"; cat /tmp/hokus_diff; fail=1
    fi
done

echo "== runtime error tests =="
for f in tests/golden_errors/*.err.hk; do
    name=$(basename "$f" .err.hk)
    $BIN "$f" > /tmp/hokus_out 2>&1
    code=$?
    expected="$f.expected"
    if [ -f "$expected" ]; then
        if [ $code -eq 70 ] && diff -q "$expected" /tmp/hokus_out >/dev/null; then
            echo "PASS $name"
        else
            echo "FAIL $name (exit $code, trace mismatch)"; diff "$expected" /tmp/hokus_out; fail=1
        fi
    elif [ $code -eq 70 ] && grep -q "Undefined variable" /tmp/hokus_out; then
        echo "PASS $name"
    else
        echo "FAIL $name (exit $code)"; cat /tmp/hokus_out; fail=1
    fi
done

echo "== examples run clean =="
for f in examples/*.hk; do
    if $BIN "$f" >/dev/null 2>&1; then
        echo "PASS $(basename "$f")"
    else
        echo "FAIL $f"; fail=1
    fi
done

echo "== GC stress (collect on every allocation) =="
if [ -x ./bin/hokus-gcstress ]; then
    for f in tests/golden/*.hk; do
        name=$(basename "$f" .hk)
        if ./bin/hokus-gcstress "$f" > /tmp/hokus_out 2>&1 && diff -q "tests/golden/$name.expected" /tmp/hokus_out >/dev/null; then
            echo "PASS gcstress:$name"
        else
            echo "FAIL gcstress:$name"; fail=1
        fi
    done
    for f in tests/golden_errors/*.err.hk; do
        name=$(basename "$f" .err.hk)
        ./bin/hokus-gcstress "$f" > /tmp/hokus_out 2>&1
        if [ $? -eq 70 ]; then echo "PASS gcstress:$name"; else echo "FAIL gcstress:$name"; fail=1; fi
    done
    # GC-sensitive corpus subtrees (upvalues, tables, concat) — benchmark dirs
    # excluded: collect-per-allocate on millions of allocs never terminates.
    for sub in closure class string function; do
        if node tests/lox_runner.js ./bin/hokus-gcstress "tests/external/craftinginterpreters/test/$sub" | grep -q ' 0 failed'; then
            echo "PASS gcstress-corpus:$sub"
        else
            echo "FAIL gcstress-corpus:$sub"; fail=1
        fi
    done
else
    echo "SKIP gcstress (run: make gcstress)"
fi

echo "== disassembler goldens =="
for f in tests/golden_disasm/*.hk; do
    name=$(basename "$f" .hk)
    if $BIN --disasm "$f" > /tmp/hokus_out 2>&1 && diff -q "tests/golden_disasm/$name.expected" /tmp/hokus_out >/dev/null; then
        echo "PASS disasm-$name"
    else
        echo "FAIL disasm-$name"; fail=1
    fi
done

echo "== disasm roundtrip (assemble listings, compare to compiled tree) =="
node tests/disasm_roundtrip.js $BIN tests/golden/*.hk tests/golden_disasm/*.hk examples/*.hk \
    | tail -1 | grep -q ", 0 failed," \
    && echo "PASS disasm-roundtrip" || { echo "FAIL disasm-roundtrip"; fail=1; }

echo "== bytecode images (.hki: emit then run, source-free) =="
imgfail=0
for f in tests/golden/*.hk; do
    name=$(basename "$f" .hk)
    if $BIN --emit-image "$f" > /tmp/hokus_img.hki && $BIN /tmp/hokus_img.hki > /tmp/hokus_out 2>&1 \
        && diff -q "tests/golden/$name.expected" /tmp/hokus_out >/dev/null; then
        :
    else
        echo "FAIL img:$name"; imgfail=1
    fi
done
[ $imgfail -eq 0 ] && echo "PASS images:goldens" || fail=1
for f in tests/golden_errors/*.err.hk; do
    name=$(basename "$f" .err.hk)
    $BIN --emit-image "$f" > /tmp/hokus_img.hki 2>/dev/null
    $BIN /tmp/hokus_img.hki > /tmp/hokus_out 2>&1
    if [ $? -eq 70 ] && [ ! -f "$f.expected" ] || diff -q "$f.expected" /tmp/hokus_out >/dev/null 2>&1; then
        :
    else
        echo "FAIL img:$name (error trace drifted)"; fail=1
    fi
done
echo "PASS images:error-traces"

echo "== slice-equivalence tests =="
for f in tests/golden/*.hk; do
    name=$(basename "$f" .hk)
    if $BIN --slice 1 "$f" > /tmp/hokus_out 2>&1 && diff -q "tests/golden/$name.expected" /tmp/hokus_out > /dev/null; then
        echo "PASS $name (slice=1)"
    else
        echo "FAIL $name (slice=1)"; fail=1
    fi
done

# ---- WASM gate: freestanding build vs golden corpus + API surface ----
if [ -f web/hokus.wasm ]; then
    echo "== wasm gate =="
    node tests/wasm_runner.js web/hokus.wasm tests/golden || fail=1
    echo "== fuzz gate =="
    node tests/fuzz.js web/hokus.wasm || fail=1
    echo "== differential fuzz (native vs wasm) =="
    node tests/diff_fuzz.js $BIN web/hokus.wasm 200 || fail=1
    echo "== benchmark trendline =="
    if [ -f benchmarks/baseline.json ]; then
        node tests/bench.js --check benchmarks/baseline.json > /tmp/hokus_bench_log 2>&1 \
            && echo "PASS bench-trendline" || { echo "FAIL bench-trendline"; cat /tmp/hokus_bench_log; fail=1; }
    else
        echo "SKIP bench-trendline (no benchmarks/baseline.json)"
    fi
else
    echo "== wasm gate skipped (run: make wasm) =="
fi

# ---- external gate: Crafting Interpreters Lox test corpus ----
LOX=tests/external/craftinginterpreters/test
if [ -d "$LOX" ]; then
    echo "== external: Lox suite (supported subset) =="
    node tests/lox_runner.js "$BIN" "$LOX" || fail=1
    echo "== external: corpus-wide disasm roundtrip =="
    node tests/disasm_roundtrip.js "$BIN" $(find "$LOX" -name '*.lox' | sort) \
        | tail -1 | grep -q ", 0 failed," \
        && echo "PASS corpus-roundtrip" || { echo "FAIL corpus-roundtrip"; fail=1; }
    echo "== external: image-run parity (closure/call/class subtrees) =="
    imgparity=0
    for f in "$LOX"/closure/*.lox "$LOX"/call/*.lox "$LOX"/class/*.lox; do
        o1=$($BIN "$f" 2>&1); c1=$?
        [ $c1 -eq 65 ] && continue
        $BIN --emit-image "$f" > /tmp/hokus_img.hki 2>/dev/null || continue
        o2=$($BIN /tmp/hokus_img.hki 2>&1); c2=$?
        if [ "$o1" != "$o2" ] || [ $c1 -ne $c2 ]; then
            echo "FAIL img-parity:$f"; imgparity=1
        fi
    done
    [ $imgparity -eq 0 ] && echo "PASS image-parity" || fail=1
else
    echo "== external Lox suite not present (run: ./tests/fetch_lox.sh) =="
fi

if [ -d "$LOX" ] && [ "${HOKUS_SLICE_SWEEP:-0}" = "1" ]; then
    echo "== external Lox suite at --slice 17 =="
    HOKUS_SLICE=17 node tests/lox_runner.js "$BIN" "$LOX" || fail=1
fi

if [ -f hokus-artifact.html ]; then
    echo "== artifact freshness =="
    if grep -oP 'atob\("\K[^"]+' hokus-artifact.html | head -1 | base64 -d | cmp -s - web/hokus.wasm; then
        echo "PASS artifact matches web/hokus.wasm"
    else
        echo "FAIL artifact embeds a stale wasm build (run: make artifact)"; fail=1
    fi
fi

exit $fail
