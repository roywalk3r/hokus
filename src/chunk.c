/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#include "chunk.h"

void initChunk(Chunk *chunk) {
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    initValueArray(&chunk->constants);
}

static int growCapacity(int capacity) {
    return capacity < 8 ? 8 : capacity * 2;
}

void writeChunk(Chunk *chunk, uint8_t byte, int line) {
    if (chunk->capacity < chunk->count + 1) {
        int old = chunk->capacity;
        chunk->capacity = growCapacity(old);
        chunk->code = HK_REALLOC(chunk->code, old, chunk->capacity);
        chunk->lines = HK_REALLOC(chunk->lines, old * (int)sizeof(int),
                                  chunk->capacity * (int)sizeof(int));
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

int addConstant(Chunk *chunk, Value value) {
    writeValueArray(&chunk->constants, value);
    return chunk->constants.count - 1;
}

void freeChunk(Chunk *chunk) {
    HK_FREE(chunk->code, chunk->capacity);
    HK_FREE(chunk->lines, chunk->capacity * (int)sizeof(int));
    freeValueArray(&chunk->constants);
    initChunk(chunk);
}
