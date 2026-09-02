/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#ifndef hokus_chunk_h
#define hokus_chunk_h

#include "common.h"
#include "value.h"

typedef enum {
    OP_CONSTANT,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_GET_GLOBAL,
    OP_DEFINE_GLOBAL,
    OP_SET_GLOBAL,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_CLOSURE,
    OP_CLOSE_UPVALUE,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_NOT,
    OP_NEGATE,
    OP_PRINT,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_CALL,
    OP_RETURN,
    OP_GET_PROPERTY,
    OP_SET_PROPERTY,
    OP_CLASS,
    OP_METHOD,
    OP_INHERIT,
    OP_GET_SUPER,
    OP_INVOKE,
    OP_SUPER_INVOKE,
    /* appended last: bytecode goldens and .hki images index these by value */
    OP_STATIC_METHOD,
    OP_MODULO,
    OP_POP_JUMP_IF_FALSE,
    OP_CONSTANT8,
    OP_SUB_LOCAL_CONST8,
    OP_LESS_JUMP,
    OP_GREATER_JUMP,
    OP_SET_LOCAL_POP,
    OP_SET_GLOBAL_POP,
    OP_ADD_LOCAL_CONST8_SET,
    OP_RETURN_LOCAL,
    OP_LESS_LOCAL_CONST8_JUMP,
    OP_ADD_LOCAL_CONST8,
    OP_ADD_LOCAL_CONST8_SET_LOOP,
    OP_ADD_LOCAL_CONST8_RETURN,
    OP_LESS_LOCAL_CONST8_LOOP,
    OP_ADD_LOCAL_CONST8_SET_LESS_LOOP,
    OP_GET_LOCAL_PROPERTY,
    OP_SET_PROPERTY_POP,
    OP_ADD_LOCAL_LOCAL,
    OP_ADD_LOCAL_LOCAL_SET,
    OP_MUL_LOCAL_CONST8,
    OP_MUL_LOCAL_CONST8_SET,
    OP_ADD_LOCAL_PROPERTY_CONST8_SET,
    OP_RETURN_LOCAL_PROPERTY,
    OP_SUB_LOCAL_CONST8_SET,
    OP_GREATER_LOCAL_CONST8_JUMP,
} OpCode;

typedef struct ObjClass ObjClass;
typedef struct ObjClosure ObjClosure;

/* Monomorphic inline cache for OP_INVOKE: one slot per (opcode offset & 7).
   Runtime-only — images don't serialize it, GC marks through it. */
#define IC_SLOTS 8
typedef struct {
    uint32_t offset;   /* opcode offset this slot caches */
    ObjClass *klass;   /* NULL = empty */
    ObjClosure *method;
} ICEntry;

typedef struct Chunk {
    int count;
    int capacity;
    uint8_t *code;
    int *lines;
    ValueArray constants;
    ICEntry ic[IC_SLOTS];
} Chunk;

void initChunk(Chunk *chunk);
void writeChunk(Chunk *chunk, uint8_t byte, int line);
int addConstant(Chunk *chunk, Value value);
void freeChunk(Chunk *chunk);

#endif
