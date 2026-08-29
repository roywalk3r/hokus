CC      ?= cc
CFLAGS  ?= -std=c11 -O3 -Wall -Wextra
SRC_CORE = src/chunk.c src/value.c src/table.c src/lexer.c src/compiler.c src/vm.c src/gc.c

BIN = bin/hokus

all: $(BIN)

$(BIN): $(SRC_CORE) src/debug.c src/main.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $^

# --- freestanding WASM build (no emscripten, no libc) ---
WASM_CC    = clang
WASM_FLAGS = --target=wasm32 -O2 -nostdlib -fno-builtin -DHK_WASM \
             -Wl,--no-entry -Wl,--export-all -Wl,--allow-undefined

web/hokus.wasm: $(SRC_CORE) src/debug.c src/wasm_shim.c
	@mkdir -p web
	$(WASM_CC) $(WASM_FLAGS) -o $@ $^

wasm: web/hokus.wasm

# single-file playground with the WASM module embedded (works from file://)
hokus-artifact.html: web/hokus.wasm web/index.html tools/build_artifact.sh
	./tools/build_artifact.sh

artifact: hokus-artifact.html

test: $(BIN) web/hokus.wasm
	./tests/run.sh

clean:
	rm -rf bin web/hokus.wasm hokus-artifact.html

.PHONY: all wasm artifact test clean
