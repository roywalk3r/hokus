/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#ifndef hokus_debug_h
#define hokus_debug_h

#include "chunk.h"

void disassembleChunk(Chunk *chunk, const char *name);
int disassembleInstruction(Chunk *chunk, int offset);

#endif
