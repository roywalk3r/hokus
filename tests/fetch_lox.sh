#!/usr/bin/env bash
# Fetch the official Lox test corpus (Crafting Interpreters, (c) Bob Nystrom, MIT)
# as an EXTERNAL correctness gate — tests we did not write.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tests/external
if [ ! -d tests/external/craftinginterpreters ]; then
    git clone --depth 1 https://github.com/munificent/craftinginterpreters tests/external/craftinginterpreters
fi
echo "Lox test corpus at tests/external/craftinginterpreters/test"
