#include "vm.h"
#include "compiler.h"

HkEmitFn hkEmit;

static void defaultEmit(const char *s) { (void)s; }

void initVM(VM *vm) {
    if (!hkEmit) hkEmit = defaultEmit;
    vm->frameCount = 0;
    vm->hadRuntimeError = false;
    initTable(&vm->globals);
    objectsInit();
    resetVMStack(vm);
}

void resetVMStack(VM *vm) {
    vm->stackTop = vm->stack;
    vm->frameCount = 0;
}

void freeVM(VM *vm) {
    freeTable(&vm->globals);
}

void runtimeError(VM *vm, const char *message) {
    hkEmit(message);
    hkEmit("\n");
    CallFrame *frame = &vm->frames[vm->frameCount - 1];
    int line = frame->function->chunk->lines[
        (int)(frame->ip - frame->function->chunk->code) - 1];
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

static InterpretResult run(VM *vm) {
    CallFrame *frame = &vm->frames[vm->frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->function->chunk->constants.values[READ_BYTE()])
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
                    runtimeError(vm, "Undefined variable.");
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
                    runtimeError(vm, "Undefined variable.");
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
                Value callee = peek(vm, argCount);
                if (!IS_FUNCTION(callee)) {
                    runtimeError(vm, "Can only call functions.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjFunction *function = AS_FUNCTION(callee);
                if (argCount != function->arity) {
                    runtimeError(vm, "Wrong number of arguments.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm->frameCount == FRAMES_MAX) {
                    runtimeError(vm, "Stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                CallFrame *newFrame = &vm->frames[vm->frameCount++];
                newFrame->function = function;
                newFrame->ip = function->chunk->code;
                newFrame->slots = vm->stackTop - argCount - 1;
                frame = newFrame;
                break;
            }
            case OP_RETURN: {
                Value result = pop(vm);
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

InterpretResult interpret(VM *vm, const char *source, int sourceLen) {
    ObjFunction *function = compile(source, sourceLen);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;

    push(vm, OBJ_VAL(function));
    CallFrame *frame = &vm->frames[vm->frameCount++];
    frame->function = function;
    frame->ip = function->chunk->code;
    frame->slots = vm->stack;

    /* Function chunks stay alive for the VM's lifetime: globals hold
       ObjFunctions pointing into them. Freed with the VM in freeVM(). */
    return run(vm);
}
