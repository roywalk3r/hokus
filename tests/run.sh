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
else
    echo "== wasm gate skipped (run: make wasm) =="
fi

# ---- external gate: Crafting Interpreters Lox test corpus ----
LOX=tests/external/craftinginterpreters/test
if [ -d "$LOX" ]; then
    echo "== external: Lox suite (supported subset) =="
    node tests/lox_runner.js "$BIN" "$LOX" || fail=1
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
