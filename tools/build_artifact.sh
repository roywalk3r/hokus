#!/usr/bin/env bash
# Builds hokus-artifact.html: the full playground as ONE file with the WASM
# module embedded as base64 — no server, no second file, works from file://.
set -euo pipefail
cd "$(dirname "$0")/.."

[ -f web/hokus.wasm ] || make wasm

B64=$(base64 -w 0 web/hokus.wasm)

awk -v b64="$B64" '
  /WebAssembly\.instantiateStreaming\(fetch\("hokus.wasm"\), \{\}\)/ {
    print "const wasmBytes = Uint8Array.from(atob(\"" b64 "\"), c => c.charCodeAt(0));"
    print "WebAssembly.instantiate(wasmBytes, {}).then(({ instance }) => {"
    next
  }
  { print }
' web/index.html > hokus-artifact.html

grep -q "atob" hokus-artifact.html || { echo "FAIL: injection point not found"; rm hokus-artifact.html; exit 1; }
echo "wrote hokus-artifact.html ($(stat -c%s hokus-artifact.html) bytes)"
