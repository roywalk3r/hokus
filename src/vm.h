/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#ifndef hokus_vm_h
#define hokus_vm_h

#include "chunk.h"

#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * 256)

typedef struct {
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;
    Value *consts; /* closure->function->chunk->constants.values, set once */
} CallFrame;

typedef struct {
    CallFrame frames[FRAMES_MAX];
    int frameCount;
    Value stack[STACK_MAX];
    Value *stackTop;
    /* Dense global-variable storage, indexed by compile-time slot (see
       value.h hkGlobalNames). EMPTY_VAL = declared-by-bytecode slot that no
       DEFINE_GLOBAL has run yet, which keeps undefined access a runtime
       error. Grows lazily at first touch of a slot. */
    Value *gslots;
    int gslotsCount;
    int gslotsCap;
    ObjUpvalue *openUpvalues;
    ObjString *initString; /* cached "init" root for construction calls */
    int trace;             /* per-instruction execution trace when non-zero */
    bool hadRuntimeError;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
    INTERPRET_PAUSED, /* instruction budget exhausted; call vmRunSlice again */
} InterpretResult;

void initVM(VM *vm);
void freeVM(VM *vm);
void resetVMStack(VM *vm);
InterpretResult interpret(VM *vm, const char *source, int sourceLen);
InterpretResult vmStart(VM *vm, const char *source, int sourceLen);
/* Prime frame 0 with an already-compiled function (bytecode images). */
InterpretResult vmStartFunction(VM *vm, ObjFunction *function);
InterpretResult vmRunSlice(VM *vm, int budget);
void runtimeError(VM *vm, const char *message);

#endif
