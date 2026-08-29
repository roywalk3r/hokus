#include "vm.h"
#include "compiler.h"
#include "gc.h"

extern VM *hkActiveVM; /* gc.c gate: collections only mid-execution */

HkEmitFn hkEmit;

static void defaultEmit(const char *s) { (void)s; }

void initVM(VM *vm) {
    if (!hkEmit) hkEmit = defaultEmit;
    vm->frameCount = 0;
    vm->openUpvalues = NULL;
    vm->hadRuntimeError = false;
    initTable(&vm->globals);
    objectsInit();
    vm->initString = copyString("init", 4);
    resetVMStack(vm);
}

void resetVMStack(VM *vm) {
    vm->stackTop = vm->stack;
    vm->frameCount = 0;
}

void freeVM(VM *vm) {
    gcFreeAll();
    freeTable(&vm->globals);
}

void runtimeError(VM *vm, const char *message) {
    hkEmit(message);
    hkEmit("\n");
    CallFrame *frame = &vm->frames[vm->frameCount - 1];
    int line = frame->closure->function->chunk->lines[
        (int)(frame->ip - frame->closure->function->chunk->code) - 1];
    char buf[64];
    buf[0] = '['; buf[1] = 'l'; buf[2] = 'i'; buf[3] = 'n'; buf[4] = 'e'; buf[5] = ' ';
    numberToString(line, buf + 6, sizeof(buf) - 10);
    int n = 6;
    while (buf[n]) n++;
    buf[n++] = ' '; buf[n++] = 'i'; buf[n++] = 'n'; buf[n++] = ' ';
    buf[n++] = 's'; buf[n++] = 'c'; buf[n++] = 'r'; buf[n++] = 'i';
    buf[n++] = 'p'; buf[n++] = 't'; buf[n++] = ']'; buf[n] = '\0';
    hkEmit(buf);
    hkEmit("\n");
    resetVMStack(vm);
    vm->hadRuntimeError = true;
}

static void push(VM *vm, Value value) { *vm->stackTop = value; vm->stackTop++; }
static Value pop(VM *vm) { vm->stackTop--; return *vm->stackTop; }
static Value peek(VM *vm, int distance) { return vm->stackTop[-1 - distance]; }

/* Message builders for arity errors must not clobber the sink mid-line, so
   they use a static buffer and numberToString. */
static void arityError(VM *vm, int expected, int got) {
    char buf[96];
    int n = 0;
    const char *pre = "Expected ";
    while (*pre) buf[n++] = *pre++;
    numberToString(expected, buf + n, (int)sizeof(buf) - n - 24);
    while (buf[n]) n++;
    const char *mid = " arguments but got ";
    while (*mid) buf[n++] = *mid++;
    numberToString(got, buf + n, (int)sizeof(buf) - n - 4);
    while (buf[n]) n++;
    buf[n++] = '.'; buf[n] = '\0';
    runtimeError(vm, buf);
}

static bool callClosure(VM *vm, ObjClosure *closure, int argCount,
                        CallFrame **frameOut) {
    if (argCount != closure->function->arity) {
        arityError(vm, closure->function->arity, argCount);
        return false;
    }
    if (vm->frameCount == FRAMES_MAX) {
        runtimeError(vm, "Stack overflow.");
        return false;
    }
    CallFrame *newFrame = &vm->frames[vm->frameCount++];
    newFrame->closure = closure;
    newFrame->ip = closure->function->chunk->code;
    newFrame->slots = vm->stackTop - argCount - 1;
    *frameOut = newFrame;
    return true;
}

static bool callValue(VM *vm, int argCount, CallFrame **frameOut) {
    Value callee = peek(vm, argCount);
    if (IS_OBJ(callee)) {
        switch (AS_OBJ(callee)->type) {
            case OBJ_NATIVE: {
                Value result = AS_NATIVE(callee)(argCount, vm->stackTop - argCount);
                vm->stackTop -= argCount + 1; /* args + callee */
                push(vm, result);
                return true;
            }
            case OBJ_CLOSURE:
                return callClosure(vm, AS_CLOSURE(callee), argCount, frameOut);
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
                vm->stackTop[-argCount - 1] = bound->receiver; /* slot 0 = this */
                return callClosure(vm, bound->method, argCount, frameOut);
            }
            case OBJ_CLASS: {
                ObjClass *klass = AS_CLASS(callee);
                vm->stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
                Value *initializer;
                if (tableGet(klass->methods, vm->initString, &initializer)) {
                    return callClosure(vm, AS_CLOSURE(*initializer), argCount, frameOut);
                }
                if (argCount != 0) {
                    arityError(vm, 0, argCount);
                    return false;
                }
                return true;
            }
            default:
                break;
        }
    }
    runtimeError(vm, "Can only call functions and classes.");
    return false;
}

static void undefinedVarError(VM *vm, ObjString *name) {
    char buf[96];
    int n = 0;
    const char *pre = "Undefined variable '";
    while (*pre) buf[n++] = *pre++;
    for (int i = 0; i < name->length && n < 80; i++) buf[n++] = name->chars[i];
    buf[n++] = '\''; buf[n++] = '.'; buf[n] = '\0';
    runtimeError(vm, buf);
}

static bool bindMethod(VM *vm, ObjClass *klass, ObjString *name) {
    Value *method;
    if (!tableGet(klass->methods, name, &method)) {
        char buf[96];
        int n = 0;
        const char *pre = "Undefined property '";
        while (*pre) buf[n++] = *pre++;
        for (int i = 0; i < name->length && n < 80; i++) buf[n++] = name->chars[i];
        buf[n++] = '\''; buf[n++] = '.'; buf[n] = '\0';
        runtimeError(vm, buf);
        return false;
    }
    /* receiver is still on the stack during allocation, so the GC roots it */
    ObjBoundMethod *bound = newBoundMethod(peek(vm, 0), AS_CLOSURE(*method));
    pop(vm);
    push(vm, OBJ_VAL(bound));
    return true;
}

static bool isFalsey(Value value) {
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate(VM *vm) {
    ObjString *b = AS_STRING(pop(vm));
    ObjString *a = AS_STRING(pop(vm));
    int length = a->length + b->length;
    char *chars = HK_REALLOC(NULL, 0, length + 1);
    for (int i = 0; i < a->length; i++) chars[i] = a->chars[i];
    for (int i = 0; i < b->length; i++) chars[a->length + i] = b->chars[i];
    chars[length] = '\0';
    push(vm, OBJ_VAL(takeString(chars, length)));
}

/* Upvalues: sorted open list by stack address; closing moves the value
   into the ObjUpvalue itself so it outlives the stack frame. */
static ObjUpvalue *captureUpvalue(VM *vm, Value *local) {
    ObjUpvalue *prevUpvalue = NULL;
    ObjUpvalue *upvalue = vm->openUpvalues;
    while (upvalue != NULL && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }
    if (upvalue != NULL && upvalue->location == local) return upvalue;
    ObjUpvalue *created = newUpvalue(local);
    created->next = upvalue;
    if (prevUpvalue == NULL) vm->openUpvalues = created;
    else prevUpvalue->next = created;
    return created;
}

static void closeUpvalues(VM *vm, Value *last) {
    while (vm->openUpvalues != NULL && vm->openUpvalues->location >= last) {
        ObjUpvalue *upvalue = vm->openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm->openUpvalues = upvalue->next;
    }
}

static InterpretResult run(VM *vm) {
    CallFrame *frame = &vm->frames[vm->frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk->constants.values[READ_SHORT()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_OP(valueType, op)                                     \
    do {                                                             \
        if (!IS_NUMBER(peek(vm, 0)) || !IS_NUMBER(peek(vm, 1))) {    \
            runtimeError(vm, "Operands must be numbers.");           \
            return INTERPRET_RUNTIME_ERROR;                          \
        }                                                            \
        double b = AS_NUMBER(pop(vm));                               \
        double a = AS_NUMBER(pop(vm));                               \
        push(vm, valueType(a op b));                                 \
    } while (false)

    for (;;) {
        uint8_t instruction = READ_BYTE();
        switch (instruction) {
            case OP_CONSTANT: push(vm, READ_CONSTANT()); break;
            case OP_NIL:   push(vm, NIL_VAL); break;
            case OP_TRUE:  push(vm, BOOL_VAL(true)); break;
            case OP_FALSE: push(vm, BOOL_VAL(false)); break;
            case OP_POP:   pop(vm); break;
            case OP_GET_GLOBAL: {
                ObjString *name = READ_STRING();
                Value *value;
                if (!tableGet(&vm->globals, name, &value)) {
                    undefinedVarError(vm, name);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, *value);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString *name = READ_STRING();
                tableSet(&vm->globals, name, peek(vm, 0));
                pop(vm);
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString *name = READ_STRING();
                if (tableSet(&vm->globals, name, peek(vm, 0))) {
                    tableDelete(&vm->globals, name);
                    undefinedVarError(vm, name);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_GET_LOCAL:
                push(vm, frame->slots[READ_BYTE()]);
                break;
            case OP_SET_LOCAL:
                frame->slots[READ_BYTE()] = peek(vm, 0);
                break;
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                push(vm, *frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = peek(vm, 0);
                break;
            }
            case OP_CLOSURE: {
                ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure *closure = newClosure(function);
                push(vm, OBJ_VAL(closure));
                for (int i = 0; i < closure->upvalueCount; i++) {
                    uint8_t isLocal = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    closure->upvalues[i] = isLocal
                        ? captureUpvalue(vm, frame->slots + index)
                        : frame->closure->upvalues[index];
                }
                break;
            }
            case OP_CLOSE_UPVALUE:
                closeUpvalues(vm, vm->stackTop - 1);
                pop(vm);
                break;
            case OP_EQUAL: {
                Value b = pop(vm), a = pop(vm);
                push(vm, BOOL_VAL(valuesEqual(a, b)));
                break;
            }
            case OP_GREATER:  BINARY_OP(BOOL_VAL, >); break;
            case OP_LESS:     BINARY_OP(BOOL_VAL, <); break;
            case OP_ADD: {
                if (IS_STRING(peek(vm, 0)) && IS_STRING(peek(vm, 1))) {
                    concatenate(vm);
                } else if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
                    double b = AS_NUMBER(pop(vm)), a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a + b));
                } else {
                    runtimeError(vm, "Operands must be two numbers or two strings.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
            case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
            case OP_DIVIDE:   BINARY_OP(NUMBER_VAL, /); break;
            case OP_NOT:      push(vm, BOOL_VAL(isFalsey(pop(vm)))); break;
            case OP_NEGATE:
                if (!IS_NUMBER(peek(vm, 0))) {
                    runtimeError(vm, "Operand must be a number.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, NUMBER_VAL(-AS_NUMBER(pop(vm))));
                break;
            case OP_PRINT:
                printValue(pop(vm));
                hkEmit("\n");
                break;
            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                frame->ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (isFalsey(peek(vm, 0))) frame->ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                frame->ip -= offset;
                break;
            }
            case OP_CALL: {
                int argCount = READ_BYTE();
                if (!callValue(vm, argCount, &frame)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_CLASS: {
                push(vm, OBJ_VAL(newClass(READ_STRING())));
                break;
            }
            case OP_METHOD: {
                ObjString *name = READ_STRING();
                ObjClass *klass = AS_CLASS(peek(vm, 1));
                tableSet(klass->methods, name, peek(vm, 0));
                pop(vm); /* closure */
                break;
            }
            case OP_INHERIT: {
                if (!IS_CLASS(peek(vm, 1))) {
                    runtimeError(vm, "Superclass must be a class.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass *subclass = AS_CLASS(peek(vm, 0));
                tableAddAll(AS_CLASS(peek(vm, 1))->methods, subclass->methods);
                pop(vm); /* subclass */
                break;
            }
            case OP_GET_PROPERTY: {
                ObjString *name = READ_STRING();
                if (!IS_INSTANCE(peek(vm, 0))) {
                    runtimeError(vm, "Only instances have properties.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance *instance = AS_INSTANCE(peek(vm, 0));
                Value *value;
                if (tableGet(instance->fields, name, &value)) {
                    pop(vm);
                    push(vm, *value);
                    break;
                }
                if (!bindMethod(vm, instance->klass, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SET_PROPERTY: {
                ObjString *name = READ_STRING();
                if (!IS_INSTANCE(peek(vm, 1))) {
                    runtimeError(vm, "Only instances have fields.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance *instance = AS_INSTANCE(peek(vm, 1));
                tableSet(instance->fields, name, peek(vm, 0));
                Value value = pop(vm);
                pop(vm);
                push(vm, value);
                break;
            }
            case OP_GET_SUPER: {
                ObjString *name = READ_STRING();
                ObjClass *superclass = AS_CLASS(pop(vm));
                if (!bindMethod(vm, superclass, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_RETURN: {
                Value result = pop(vm);
                closeUpvalues(vm, frame->slots);
                vm->frameCount--;
                if (vm->frameCount == 0) {
                    pop(vm); /* script */
                    return INTERPRET_OK;
                }
                vm->stackTop = frame->slots;
                push(vm, result);
                frame = &vm->frames[vm->frameCount - 1];
                break;
            }
        }
        if (vm->hadRuntimeError) return INTERPRET_RUNTIME_ERROR;
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

static Value clockNative(int argCount, Value *args) {
    (void)argCount; (void)args;
    return NUMBER_VAL(hkClockSeconds());
}

InterpretResult interpret(VM *vm, const char *source, int sourceLen) {
    tableSet(&vm->globals, copyString("clock", 5),
             OBJ_VAL(newNative(clockNative)));
    ObjFunction *function = compile(source, sourceLen);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;

    ObjClosure *closure = newClosure(function);
    push(vm, OBJ_VAL(closure));
    CallFrame *frame = &vm->frames[vm->frameCount++];
    frame->closure = closure;
    frame->ip = function->chunk->code;
    frame->slots = vm->stack;

    /* Function chunks stay alive for the VM's lifetime: globals hold
       ObjFunctions pointing into them. Freed with the VM in freeVM(). */
    hkActiveVM = vm; /* GC armed only while bytecode is executing */
    InterpretResult result = run(vm);
    hkActiveVM = NULL;
    return result;
}
