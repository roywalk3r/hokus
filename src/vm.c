/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#include "vm.h"
#include "compiler.h"
#include "debug.h"
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
    vm->trace = 0;
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
    /* erroring frame first, then the callers — same "[line N] in name()" format */
    for (int i = vm->frameCount - 1; i >= 0; i--) {
        CallFrame *caller = &vm->frames[i];
        ObjFunction *fn = caller->closure->function;
        int callerLine = fn->chunk->lines[
            (int)(caller->ip - fn->chunk->code) - 1];
        char line[64];
        line[0] = '['; line[1] = 'l'; line[2] = 'i'; line[3] = 'n';
        line[4] = 'e'; line[5] = ' ';
        numberToString(callerLine, line + 6, sizeof(line) - 10);
        int m = 6;
        while (line[m]) m++;
        line[m++] = ']'; line[m++] = ' '; line[m++] = 'i'; line[m++] = 'n';
        line[m++] = ' '; line[m] = '\0';
        hkEmit(line);
        if (fn->name == NULL) {
            hkEmit("script\n");
        } else {
            hkEmit(fn->name->chars);
            hkEmit("()\n");
        }
    }
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

static bool invoke(VM *vm, ObjString *name, int argCount, CallFrame **frameOut) {
    Value receiver = peek(vm, argCount);
    /* A.m(...) — statics are plain functions: slot 0 holds the closure, no this */
    if (IS_CLASS(receiver)) {
        Value *method;
        if (!tableGet(AS_CLASS(receiver)->statics, name, &method)) {
            char buf[96];
            int n = 0;
            const char *pre = "Undefined property '";
            while (*pre) buf[n++] = *pre++;
            for (int i = 0; i < name->length && n < 80; i++) buf[n++] = name->chars[i];
            buf[n++] = '\''; buf[n++] = '.'; buf[n] = '\0';
            runtimeError(vm, buf);
            return false;
        }
        vm->stackTop[-argCount - 1] = *method;
        return callClosure(vm, AS_CLOSURE(*method), argCount, frameOut);
    }
    if (!IS_INSTANCE(receiver)) {
        runtimeError(vm, "Only instances have methods.");
        return false;
    }
    ObjInstance *instance = AS_INSTANCE(receiver);
    Value *field;
    if (tableGet(instance->fields, name, &field)) {
        /* a field shadows methods: call whatever it holds (fails cleanly if not callable) */
        vm->stackTop[-argCount - 1] = *field;
        return callValue(vm, argCount, frameOut);
    }
    Value *method;
    if (!tableGet(instance->klass->methods, name, &method)) {
        char buf[96];
        int n = 0;
        const char *pre = "Undefined property '";
        while (*pre) buf[n++] = *pre++;
        for (int i = 0; i < name->length && n < 80; i++) buf[n++] = name->chars[i];
        buf[n++] = '\''; buf[n++] = '.'; buf[n] = '\0';
        runtimeError(vm, buf);
        return false;
    }
    return callClosure(vm, AS_CLOSURE(*method), argCount, frameOut);
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

/* budget: max instructions to execute (-1 = unlimited); returning
   INTERPRET_PAUSED leaves VM state intact for a later vmRunSlice call */
static InterpretResult run(VM *vm, int budget) {
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
        if (budget == 0) return INTERPRET_PAUSED;
        if (budget > 0) budget--;
        if (vm->trace) {
            hkEmit("          ");
            for (Value *slot = vm->stack; slot < vm->stackTop; slot++) {
                hkEmit("[ ");
                printValue(*slot);
                hkEmit(" ]");
            }
            hkEmit("\n");
            disassembleInstruction(
                frame->closure->function->chunk,
                (int)(frame->ip - frame->closure->function->chunk->code));
        }

#if defined(__GNUC__) || defined(__clang__)
    static void *jumpTable[] = {
        &&op_OP_CONSTANT, &&op_OP_NIL, &&op_OP_TRUE, &&op_OP_FALSE,
        &&op_OP_POP, &&op_OP_GET_GLOBAL, &&op_OP_DEFINE_GLOBAL,
        &&op_OP_SET_GLOBAL, &&op_OP_GET_LOCAL, &&op_OP_SET_LOCAL,
        &&op_OP_GET_UPVALUE, &&op_OP_SET_UPVALUE, &&op_OP_CLOSURE,
        &&op_OP_CLOSE_UPVALUE, &&op_OP_EQUAL, &&op_OP_GREATER,
        &&op_OP_LESS, &&op_OP_ADD, &&op_OP_SUBTRACT, &&op_OP_MULTIPLY,
        &&op_OP_DIVIDE, &&op_OP_NOT, &&op_OP_NEGATE, &&op_OP_PRINT,
        &&op_OP_JUMP, &&op_OP_JUMP_IF_FALSE, &&op_OP_LOOP, &&op_OP_CALL,
        &&op_OP_RETURN, &&op_OP_GET_PROPERTY, &&op_OP_SET_PROPERTY,
        &&op_OP_CLASS, &&op_OP_METHOD, &&op_OP_INHERIT, &&op_OP_GET_SUPER,
        &&op_OP_INVOKE, &&op_OP_SUPER_INVOKE, &&op_OP_STATIC_METHOD,
        &&op_OP_MODULO,
    };
    goto *jumpTable[READ_BYTE()];
#endif

            op_OP_CONSTANT: push(vm, READ_CONSTANT()); continue;
            op_OP_NIL:   push(vm, NIL_VAL); continue;
            op_OP_TRUE:  push(vm, BOOL_VAL(true)); continue;
            op_OP_FALSE: push(vm, BOOL_VAL(false)); continue;
            op_OP_POP:   pop(vm); continue;
            op_OP_GET_GLOBAL: {
                ObjString *name = READ_STRING();
                Value *value;
                if (!tableGet(&vm->globals, name, &value)) {
                    undefinedVarError(vm, name);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, *value);
                continue;
            }
            op_OP_DEFINE_GLOBAL: {
                ObjString *name = READ_STRING();
                tableSet(&vm->globals, name, peek(vm, 0));
                pop(vm);
                continue;
            }
            op_OP_SET_GLOBAL: {
                ObjString *name = READ_STRING();
                if (tableSet(&vm->globals, name, peek(vm, 0))) {
                    tableDelete(&vm->globals, name);
                    undefinedVarError(vm, name);
                    return INTERPRET_RUNTIME_ERROR;
                }
                continue;
            }
            op_OP_GET_LOCAL:
                push(vm, frame->slots[READ_BYTE()]);
                continue;
            op_OP_SET_LOCAL:
                frame->slots[READ_BYTE()] = peek(vm, 0);
                continue;
            op_OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                push(vm, *frame->closure->upvalues[slot]->location);
                continue;
            }
            op_OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = peek(vm, 0);
                continue;
            }
            op_OP_CLOSURE: {
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
                continue;
            }
            op_OP_CLOSE_UPVALUE:
                closeUpvalues(vm, vm->stackTop - 1);
                pop(vm);
                continue;
            op_OP_EQUAL: {
                Value b = pop(vm), a = pop(vm);
                push(vm, BOOL_VAL(valuesEqual(a, b)));
                continue;
            }
            op_OP_GREATER:  BINARY_OP(BOOL_VAL, >); continue;
            op_OP_LESS:     BINARY_OP(BOOL_VAL, <); continue;
            op_OP_ADD: {
                if (IS_STRING(peek(vm, 0)) && IS_STRING(peek(vm, 1))) {
                    concatenate(vm);
                } else if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
                    double b = AS_NUMBER(pop(vm)), a = AS_NUMBER(pop(vm));
                    push(vm, NUMBER_VAL(a + b));
                } else {
                    runtimeError(vm, "Operands must be two numbers or two strings.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                continue;
            }
            op_OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); continue;
            op_OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); continue;
            op_OP_DIVIDE:   BINARY_OP(NUMBER_VAL, /); continue;
            op_OP_NOT:      push(vm, BOOL_VAL(isFalsey(pop(vm)))); continue;
            op_OP_NEGATE:
                if (!IS_NUMBER(peek(vm, 0))) {
                    runtimeError(vm, "Operand must be a number.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, NUMBER_VAL(-AS_NUMBER(pop(vm))));
                continue;
            op_OP_PRINT:
                printValue(pop(vm));
                hkEmit("\n");
                continue;
            op_OP_JUMP: {
                uint16_t offset = READ_SHORT();
                frame->ip += offset;
                continue;
            }
            op_OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (isFalsey(peek(vm, 0))) frame->ip += offset;
                continue;
            }
            op_OP_LOOP: {
                uint16_t offset = READ_SHORT();
                frame->ip -= offset;
                continue;
            }
            op_OP_CALL: {
                int argCount = READ_BYTE();
                if (!callValue(vm, argCount, &frame)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                continue;
            }
            op_OP_INVOKE: {
                /* opcode offset identifies this call site for the IC */
                uint32_t siteOff = (uint32_t)(frame->ip -
                    frame->closure->function->chunk->code - 1);
                ObjString *name = READ_STRING();
                int argCount = READ_BYTE();
                Value receiver = peek(vm, argCount);
                if (IS_CLASS(receiver)) {
                    /* statics: key the IC on the class object itself */
                    ObjClass *klass = AS_CLASS(receiver);
                    Chunk *ch = frame->closure->function->chunk;
                    ICEntry *ic = &ch->ic[siteOff & (IC_SLOTS - 1)];
                    if (ic->offset == siteOff && ic->klass == klass) {
                        vm->stackTop[-argCount - 1] = OBJ_VAL(ic->method);
                        if (!callClosure(vm, ic->method, argCount, &frame)) {
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        continue;
                    }
                    Value *method;
                    if (tableGet(klass->statics, name, &method)) {
                        ic->offset = siteOff;
                        ic->klass = klass;
                        ic->method = AS_CLOSURE(*method);
                        /* call what we just looked up — don't pay invoke()
                           a second lookup on the fill path */
                        vm->stackTop[-argCount - 1] = *method;
                        if (!callClosure(vm, AS_CLOSURE(*method), argCount,
                                &frame)) {
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        continue;
                    }
                    if (!invoke(vm, name, argCount, &frame)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    continue;
                }
                if (IS_INSTANCE(receiver)) {
                    ObjInstance *instance = AS_INSTANCE(receiver);
                    Chunk *ch = frame->closure->function->chunk;
                    ICEntry *ic = &ch->ic[siteOff & (IC_SLOTS - 1)];
                    bool shadowed = false;
                    if (instance->fields->count > 0) {
                        Value *field;
                        shadowed = tableGet(instance->fields, name, &field);
                    }
                    /* a hit saves the method-table lookup; shadow check stays */
                    if (!shadowed && ic->offset == siteOff &&
                            ic->klass == instance->klass) {
                        if (!callClosure(vm, ic->method, argCount, &frame)) {
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        continue;
                    }
                    if (!shadowed) {
                        Value *method;
                        if (tableGet(instance->klass->methods, name, &method)) {
                            ic->offset = siteOff;
                            ic->klass = instance->klass;
                            ic->method = AS_CLOSURE(*method);
                            if (!callClosure(vm, ic->method, argCount,
                                    &frame)) {
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            continue;
                        }
                    }
                }
                if (!invoke(vm, name, argCount, &frame)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                continue;
            }
            op_OP_SUPER_INVOKE: {
                ObjString *name = READ_STRING();
                int argCount = READ_BYTE();
                ObjClass *superclass = AS_CLASS(pop(vm));
                Value *method;
                if (!tableGet(superclass->methods, name, &method)) {
                    char buf[96];
                    int n = 0;
                    const char *pre = "Undefined property '";
                    while (*pre) buf[n++] = *pre++;
                    for (int i = 0; i < name->length && n < 80; i++) buf[n++] = name->chars[i];
                    buf[n++] = '\''; buf[n++] = '.'; buf[n] = '\0';
                    runtimeError(vm, buf);
                    return INTERPRET_RUNTIME_ERROR;
                }
                /* receiver ('this') is already in slot 0 of the callee's argument window */
                if (!callClosure(vm, AS_CLOSURE(*method), argCount, &frame)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                continue;
            }
            op_OP_CLASS: {
                push(vm, OBJ_VAL(newClass(READ_STRING())));
                continue;
            }
            op_OP_METHOD: {
                ObjString *name = READ_STRING();
                ObjClass *klass = AS_CLASS(peek(vm, 1));
                tableSet(klass->methods, name, peek(vm, 0));
                pop(vm); /* closure */
                continue;
            }
            op_OP_STATIC_METHOD: {
                ObjString *name = READ_STRING();
                ObjClass *klass = AS_CLASS(peek(vm, 1));
                tableSet(klass->statics, name, peek(vm, 0));
                pop(vm); /* closure */
                continue;
            }
            /* custom block (not BINARY_OP): divide-by-zero is a runtime error
               here, while OP_DIVIDE deliberately yields inf per IEEE */
            op_OP_MODULO: {
                if (!IS_NUMBER(peek(vm, 0)) || !IS_NUMBER(peek(vm, 1))) {
                    runtimeError(vm, "Operands must be numbers.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                double b = AS_NUMBER(pop(vm));
                double a = AS_NUMBER(pop(vm));
                if (b == 0) {
                    runtimeError(vm, "Modulo by zero.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, NUMBER_VAL(hkModulo(a, b)));
                continue;
            }
            op_OP_INHERIT: {
                if (!IS_CLASS(peek(vm, 1))) {
                    runtimeError(vm, "Superclass must be a class.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass *subclass = AS_CLASS(peek(vm, 0));
                ObjClass *superclass = AS_CLASS(peek(vm, 1));
                tableAddAll(superclass->methods, subclass->methods);
                tableAddAll(superclass->statics, subclass->statics);
                pop(vm); /* subclass */
                continue;
            }
            op_OP_GET_PROPERTY: {
                ObjString *name = READ_STRING();
                /* class objects expose their statics as properties */
                if (IS_CLASS(peek(vm, 0))) {
                    ObjClass *klass = AS_CLASS(peek(vm, 0));
                    Value *method;
                    if (!tableGet(klass->statics, name, &method)) {
                        /* stock-Lox error: corpus asserts this exact message */
                        runtimeError(vm, "Only instances have properties.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    pop(vm);
                    push(vm, *method);
                    continue;
                }
                if (!IS_INSTANCE(peek(vm, 0))) {
                    runtimeError(vm, "Only instances have properties.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance *instance = AS_INSTANCE(peek(vm, 0));
                Value *value;
                if (tableGet(instance->fields, name, &value)) {
                    pop(vm);
                    push(vm, *value);
                    continue;
                }
                if (!bindMethod(vm, instance->klass, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                continue;
            }
            op_OP_SET_PROPERTY: {
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
                continue;
            }
            op_OP_GET_SUPER: {
                ObjString *name = READ_STRING();
                ObjClass *superclass = AS_CLASS(pop(vm));
                if (!bindMethod(vm, superclass, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                continue;
            }
            op_OP_RETURN: {
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
                continue;
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

/* Compile source and prime the VM's first frame without executing anything.
   Returns INTERPRET_OK (ready to slice) or INTERPRET_COMPILE_ERROR. */
InterpretResult vmStartFunction(VM *vm, ObjFunction *function) {
    tableSet(&vm->globals, copyString("clock", 5),
             OBJ_VAL(newNative(clockNative)));
    ObjClosure *closure = newClosure(function);
    push(vm, OBJ_VAL(closure));
    CallFrame *frame = &vm->frames[vm->frameCount++];
    frame->closure = closure;
    frame->ip = function->chunk->code;
    frame->slots = vm->stack;
    return INTERPRET_OK;
}

InterpretResult vmStart(VM *vm, const char *source, int sourceLen) {
    ObjFunction *function = compile(source, sourceLen);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;
    return vmStartFunction(vm, function);
}

/* Execute up to `budget` instructions (-1 = unlimited).
    Frame/stack state lives in the VM, so slicing is re-entrant. */
InterpretResult vmRunSlice(VM *vm, int budget) {
    hkActiveVM = vm; /* GC armed only while bytecode is executing */
    InterpretResult result = run(vm, budget);
    hkActiveVM = NULL;
    return result;
}

InterpretResult interpret(VM *vm, const char *source, int sourceLen) {
    /* Function chunks stay alive for the VM's lifetime: globals hold
       ObjFunctions pointing into them. Freed with the VM in freeVM(). */
    InterpretResult started = vmStart(vm, source, sourceLen);
    if (started == INTERPRET_COMPILE_ERROR) return started;
    return vmRunSlice(vm, -1);
}
