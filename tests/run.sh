#!/usr/bin/env bash
# Golden tests (self-authored) + external Lox suite (third-party gate).
set -u
cd "$(dirname "$0")/.."
BIN=./bin/hokus
fail=0

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
    if [ $code -eq 70 ] && grep -q "Undefined variable" /tmp/hokus_out; then
        echo "PASS $name"
    else
        echo "FAIL $name (exit $code)"; cat /tmp/hokus_out; fail=1
    fi
done

# ---- external gate: Crafting Interpreters Lox test corpus ----
LOX=tests/external/craftinginterpreters/test
if [ -d "$LOX" ]; then
    echo "== external: Lox suite (supported subset) =="
    node tests/lox_runner.js "$BIN" "$LOX" || fail=1
else
    echo "== external Lox suite not present (run: ./tests/fetch_lox.sh) =="
fi

exit $fail
