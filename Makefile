CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
SRC_CORE = src/chunk.c src/value.c src/table.c src/lexer.c src/compiler.c src/vm.c

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

test: $(BIN)
	./tests/run.sh

clean:
	rm -rf bin web/hokus.wasm

.PHONY: all wasm test clean
