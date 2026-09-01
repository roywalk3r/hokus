/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#include "debug.h"

/* Sink-based disassembler: works in both the native CLI and the WASM build,
   since it writes through hkEmit instead of printf. */

static void emit(const char *s) { hkEmit(s); }

static void emitInt(long long n) {
    char buf[32];
    numberToString((double)n, buf, sizeof(buf));
    emit(buf);
}

static void emitPadded(const char *s, int width) {
    emit(s);
    int len = 0;
    while (s[len]) len++;
    while (len++ < width) emit(" ");
}

static void emitValueBrief(Value v) {
    if (IS_NIL(v)) { emit("nil"); return; }
    if (IS_BOOL(v)) { emit(AS_BOOL(v) ? "true" : "false"); return; }
    if (IS_NUMBER(v)) {
        char buf[32];
        numberToString(AS_NUMBER(v), buf, sizeof(buf));
        emit(buf);
        return;
    }
    {
            if (IS_STRING(v)) {
                ObjString *s = AS_STRING(v);
                emit("\"");
                for (int i = 0; i < s->length; i++) {
                    char c = s->chars[i];
                    /* escape so listings stay one instruction per line and
                       roundtrip-parseable (see tests/disasm_roundtrip.js) */
                    if (c == '\n') emit("\\n");
                    else if (c == '\t') emit("\\t");
                    else if (c == '\\') emit("\\\\");
                    else if (c == '"') emit("\\\"");
                    else { char b[2] = { c, 0 }; emit(b); }
                }
                emit("\"");
            } else if (IS_FUNCTION(v)) {
                ObjFunction *f = AS_FUNCTION(v);
                emit("<fn ");
                emit(f->name ? f->name->chars : "script");
                emit(">");
            } else {
                emit("<obj>");
            }
    }
}

static void disassembleInner(Chunk *chunk, const char *name) {
    emit("== ");
    emit(name);
    emit(" ==\n");
    for (int offset = 0; offset < chunk->count;) {
        offset = disassembleInstruction(chunk, offset);
    }
    for (int i = 0; i < chunk->constants.count; i++) {
        Value v = chunk->constants.values[i];
        if (IS_FUNCTION(v)) {
            ObjFunction *f = AS_FUNCTION(v);
            emit("\n");
            disassembleInner(f->chunk, f->name ? f->name->chars : "<script>");
        }
    }
}

void disassembleChunk(Chunk *chunk, const char *name) {
    disassembleInner(chunk, name);
}

static int simpleInstruction(const char *name, int offset) {
    emit(name);
    emit("\n");
    return offset + 1;
}

static int constantInstruction(const char *name, Chunk *chunk, int offset) {
    uint16_t constant = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    emitPadded(name, 16);
    emitPadded("", 4);
    emitInt(constant);
    emit(" ");
    emitValueBrief(chunk->constants.values[constant]);
    emit("\n");
    return offset + 3;
}

static int constant8Instruction(const char *name, Chunk *chunk, int offset) {
    uint8_t constant = chunk->code[offset + 1];
    emitPadded(name, 16);
    emitPadded("", 4);
    emitInt(constant);
    emit(" ");
    emitValueBrief(chunk->constants.values[constant]);
    emit("\n");
    return offset + 2;
}

static int localConst8Instruction(const char *name, Chunk *chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    uint8_t constant = chunk->code[offset + 2];
    emitPadded(name, 16);
    emitPadded("", 4);
    emitInt(slot);
    emit(" ");
    emitInt(constant);
    emit(" ");
    emitValueBrief(chunk->constants.values[constant]);
    emit("\n");
    return offset + 3;
}

static int localConst8JumpInstruction(const char *name, Chunk *chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    uint8_t constant = chunk->code[offset + 2];
    uint16_t jump = (uint16_t)((chunk->code[offset + 3] << 8) | chunk->code[offset + 4]);
    emitPadded(name, 16);
    emitPadded("", 4);
    emitInt(slot);
    emit(" ");
    emitInt(constant);
    emit(" ");
    emitValueBrief(chunk->constants.values[constant]);
    emit(" | ");
    emitInt(offset);
    emit(" -> ");
    emitInt(offset + 5 + jump);
    emit("\n");
    return offset + 5;
}

static int invokeInstruction(const char *name, Chunk *chunk, int offset) {
    uint16_t constant = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    uint8_t argCount = chunk->code[offset + 3];
    emitPadded(name, 16);
    emitPadded("", 4);
    emitInt(constant);
    emit(" (");
    emitInt(argCount);
    emit(" args) ");
    emitValueBrief(chunk->constants.values[constant]);
    emit("\n");
    return offset + 4;
}

static int byteInstruction(const char *name, Chunk *chunk, int offset) {
    emitPadded(name, 16);
    emitPadded("", 4);
    emitInt(chunk->code[offset + 1]);
    emit("\n");
    return offset + 2;
}

static int jumpInstruction(const char *name, int sign, Chunk *chunk, int offset) {
    uint16_t jump = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    emitPadded(name, 16);
    emitPadded("", 4);
    emitInt(offset);
    emit(" -> ");
    emitInt(offset + 3 + sign * jump);
    emit("\n");
    return offset + 3;
}

static int closureInstruction(Chunk *chunk, int offset) {
    uint16_t constant = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    emitPadded("OP_CLOSURE", 16);
    emitPadded("", 4);
    emitInt(constant);
    emit(" ");
    Value v = chunk->constants.values[constant];
    emitValueBrief(v);
    emit("\n");
    offset += 3;
    int upvalues = IS_FUNCTION(v) ? AS_FUNCTION(v)->upvalueCount : 0;
    for (int i = 0; i < upvalues; i++) {
        int isLocal = chunk->code[offset];
        int index = chunk->code[offset + 1];
        emitPadded("", 8);
        emit("| ");
        emit(isLocal ? "local" : "upvalue");
        emit(" ");
        emitInt(index);
        emit("\n");
        offset += 2;
    }
    return offset;
}

int disassembleInstruction(Chunk *chunk, int offset) {
    char obuf[16];
    numberToString(offset, obuf, sizeof(obuf));
    int di = 0;
    while (obuf[di]) di++;
    int padNeeded = 4 - di;
    while (padNeeded-- > 0) emit(" ");
    emit(obuf);
    emit(" ");

    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        emit("   | ");
    } else {
        char lbuf[16];
        numberToString(chunk->lines[offset], lbuf, sizeof(lbuf));
        int llen = 0; while (lbuf[llen]) llen++;
        int lpad = 4 - llen;
        while (lpad-- > 0) emit(" ");
        emit(lbuf);
        emit(" ");
    }

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_CONSTANT:      return constantInstruction("OP_CONSTANT", chunk, offset);
        case OP_CONSTANT8:     return constant8Instruction("OP_CONSTANT8", chunk, offset);
        case OP_SUB_LOCAL_CONST8:
            return localConst8Instruction("OP_SUB_LOCAL_CONST8", chunk, offset);
        case OP_NIL:           return simpleInstruction("OP_NIL", offset);
        case OP_TRUE:          return simpleInstruction("OP_TRUE", offset);
        case OP_FALSE:         return simpleInstruction("OP_FALSE", offset);
        case OP_POP:           return simpleInstruction("OP_POP", offset);
        case OP_GET_GLOBAL:    return constantInstruction("OP_GET_GLOBAL", chunk, offset);
        case OP_DEFINE_GLOBAL: return constantInstruction("OP_DEFINE_GLOBAL", chunk, offset);
        case OP_SET_GLOBAL:    return constantInstruction("OP_SET_GLOBAL", chunk, offset);
        case OP_SET_GLOBAL_POP: return constantInstruction("OP_SET_GLOBAL_POP", chunk, offset);
        case OP_GET_LOCAL:     return byteInstruction("OP_GET_LOCAL", chunk, offset);
        case OP_SET_LOCAL:     return byteInstruction("OP_SET_LOCAL", chunk, offset);
        case OP_SET_LOCAL_POP: return byteInstruction("OP_SET_LOCAL_POP", chunk, offset);
        case OP_RETURN_LOCAL:  return byteInstruction("OP_RETURN_LOCAL", chunk, offset);
        case OP_LESS_LOCAL_CONST8_JUMP:
            return localConst8JumpInstruction("OP_LESS_LOCAL_CONST8_JUMP", chunk, offset);
        case OP_ADD_LOCAL_CONST8_SET:
            return localConst8Instruction("OP_ADD_LOCAL_CONST8_SET", chunk, offset);
        case OP_GET_UPVALUE:   return byteInstruction("OP_GET_UPVALUE", chunk, offset);
        case OP_SET_UPVALUE:   return byteInstruction("OP_SET_UPVALUE", chunk, offset);
        case OP_CLOSE_UPVALUE: return simpleInstruction("OP_CLOSE_UPVALUE", offset);
        case OP_CLOSURE:       return closureInstruction(chunk, offset);
        case OP_EQUAL:         return simpleInstruction("OP_EQUAL", offset);
        case OP_GREATER:       return simpleInstruction("OP_GREATER", offset);
        case OP_LESS:          return simpleInstruction("OP_LESS", offset);
        case OP_ADD:           return simpleInstruction("OP_ADD", offset);
        case OP_SUBTRACT:      return simpleInstruction("OP_SUBTRACT", offset);
        case OP_MULTIPLY:      return simpleInstruction("OP_MULTIPLY", offset);
        case OP_DIVIDE:        return simpleInstruction("OP_DIVIDE", offset);
        case OP_NOT:           return simpleInstruction("OP_NOT", offset);
        case OP_NEGATE:        return simpleInstruction("OP_NEGATE", offset);
        case OP_PRINT:         return simpleInstruction("OP_PRINT", offset);
        case OP_JUMP:          return jumpInstruction("OP_JUMP", 1, chunk, offset);
        case OP_JUMP_IF_FALSE: return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
        case OP_LOOP:          return jumpInstruction("OP_LOOP", -1, chunk, offset);
        case OP_CALL:          return byteInstruction("OP_CALL", chunk, offset);
        case OP_RETURN:        return simpleInstruction("OP_RETURN", offset);
        case OP_GET_PROPERTY:  return constantInstruction("OP_GET_PROPERTY", chunk, offset);
        case OP_SET_PROPERTY:  return constantInstruction("OP_SET_PROPERTY", chunk, offset);
        case OP_CLASS:         return constantInstruction("OP_CLASS", chunk, offset);
        case OP_METHOD:        return constantInstruction("OP_METHOD", chunk, offset);
        case OP_STATIC_METHOD: return constantInstruction("OP_STATIC_METHOD", chunk, offset);
        case OP_MODULO:        return simpleInstruction("OP_MODULO", offset);
        case OP_POP_JUMP_IF_FALSE: return jumpInstruction("OP_POP_JUMP_IF_FALSE", 1, chunk, offset);
        case OP_LESS_JUMP:    return jumpInstruction("OP_LESS_JUMP", 1, chunk, offset);
        case OP_GREATER_JUMP: return jumpInstruction("OP_GREATER_JUMP", 1, chunk, offset);
        case OP_INHERIT:       return simpleInstruction("OP_INHERIT", offset);
        case OP_GET_SUPER:     return constantInstruction("OP_GET_SUPER", chunk, offset);
        case OP_INVOKE:        return invokeInstruction("OP_INVOKE", chunk, offset);
        case OP_SUPER_INVOKE:  return invokeInstruction("OP_SUPER_INVOKE", chunk, offset);
        default:
            emit("<unknown opcode>");
            emit("\n");
            return offset + 1;
    }
}
