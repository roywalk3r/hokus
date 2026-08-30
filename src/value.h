/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#ifndef hokus_value_h
#define hokus_value_h

#include "common.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjFunction ObjFunction;
typedef struct ObjNative ObjNative;
struct Chunk;

/* NaN-boxed values: every Value is one 64-bit word. Numbers are raw doubles;
   non-numbers hide in the unused quiet-NaN exponent range — nil/true/false as
   QNAN|tag, heap objects as sign|QNAN|pointer. Halves the VM stack's memory
   and turns number boxing/unboxing into moves. */
typedef uint64_t Value;

#define SIGN_BIT    ((uint64_t)0x8000000000000000)
#define QNAN        ((uint64_t)0x7ffc000000000000)

#define TAG_NIL     1
#define TAG_FALSE   2
#define TAG_TRUE    3

#define NIL_VAL     ((Value)(QNAN | TAG_NIL))
#define TRUE_VAL    ((Value)(QNAN | TAG_TRUE))
#define FALSE_VAL   ((Value)(QNAN | TAG_FALSE))

static inline Value numToValue(double num) {
    union { uint64_t bits; double num; } u;
    u.num = num;
    return u.bits;
}
static inline double valueToNum(Value value) {
    union { uint64_t bits; double num; } u;
    u.bits = value;
    return u.num;
}

#define BOOL_VAL(v)     ((v) ? TRUE_VAL : FALSE_VAL)
#define NUMBER_VAL(v)   (numToValue(v))
#define OBJ_VAL(o)      ((Value)(SIGN_BIT | QNAN | (uint64_t)(uintptr_t)(o)))

#define IS_NIL(v)       ((v) == NIL_VAL)
#define IS_BOOL(v)      (((v) | 1) == TRUE_VAL)
#define IS_NUMBER(v)    (((v) & QNAN) != QNAN)
#define IS_OBJ(v)       (((v) & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT))
#define IS_STRING(v)    (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING)
#define IS_FUNCTION(v)  (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_FUNCTION)

#define AS_BOOL(v)      ((v) == TRUE_VAL)
#define AS_NUMBER(v)    (valueToNum(v))
#define AS_OBJ(v)       ((Obj *)(uintptr_t)((v) & ~(SIGN_BIT | QNAN)))
#define AS_STRING(v)    ((ObjString *)AS_OBJ(v))
#define AS_FUNCTION(v)  ((ObjFunction *)AS_OBJ(v))
#define IS_NATIVE(v)    (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_NATIVE)
#define AS_NATIVE(v)    (((ObjNative *)AS_OBJ(v))->fn)
#define IS_CLOSURE(v)   (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_CLOSURE)
#define AS_CLOSURE(v)   ((ObjClosure *)AS_OBJ(v))
#define IS_CLASS(v)     (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_CLASS)
#define AS_CLASS(v)     ((ObjClass *)AS_OBJ(v))
#define IS_INSTANCE(v)  (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_INSTANCE)
#define AS_INSTANCE(v)  ((ObjInstance *)AS_OBJ(v))
#define IS_BOUND_METHOD(v) (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_BOUND_METHOD)
#define AS_BOUND_METHOD(v) ((ObjBoundMethod *)AS_OBJ(v))

typedef enum {
    OBJ_STRING, OBJ_FUNCTION, OBJ_NATIVE, OBJ_CLOSURE, OBJ_UPVALUE,
    OBJ_CLASS, OBJ_INSTANCE, OBJ_BOUND_METHOD,
} ObjType;

typedef Value (*NativeFn)(int argCount, Value *args);
typedef struct ObjUpvalue ObjUpvalue;
typedef struct ObjClosure ObjClosure;
typedef struct ObjClass ObjClass;
typedef struct ObjInstance ObjInstance;
struct Table;

struct Obj {
    ObjType type;
    bool isMarked;
    Obj *next;
};

struct ObjNative {
    Obj obj;
    NativeFn fn;
};

struct ObjString {
    Obj obj;
    int length;
    uint32_t hash;
    char *chars;
};

struct ObjUpvalue {
    Obj obj;
    Value *location;   /* stack slot while open, &closed once closed */
    Value closed;
    ObjUpvalue *next;  /* VM open-upvalue list */
};

struct ObjFunction {
    Obj obj;
    int arity;
    int upvalueCount;
    struct Chunk *chunk;
    ObjString *name;
};

struct ObjClosure {
    Obj obj;
    ObjFunction *function;
    ObjUpvalue **upvalues;
    int upvalueCount;
};

struct ObjClass {
    Obj obj;
    ObjString *name;
    struct Table *methods; /* ObjString name -> ObjClosure */
};

struct ObjInstance {
    Obj obj;
    ObjClass *klass;
    struct Table *fields;
};

struct ObjBoundMethod {
    Obj obj;
    Value receiver;
    ObjClosure *method;
};
typedef struct ObjBoundMethod ObjBoundMethod;

typedef struct {
    int count;
    int capacity;
    Value *values;
} ValueArray;

void initValueArray(ValueArray *array);
void writeValueArray(ValueArray *array, Value value);
void freeValueArray(ValueArray *array);

/* The VM owns one global object list; these link new objects into it. */
void objectsInit(void);
ObjString *copyString(const char *chars, int length);
ObjString *takeString(char *chars, int length);
ObjFunction *newFunction(void);
ObjNative *newNative(NativeFn fn);
ObjClosure *newClosure(ObjFunction *function);
ObjUpvalue *newUpvalue(Value *slot);
ObjClass *newClass(ObjString *name);
ObjInstance *newInstance(ObjClass *klass);
ObjBoundMethod *newBoundMethod(Value receiver, ObjClosure *method);

/* Host-provided time source for the clock() native (seconds). */
double hkClockSeconds(void);

bool valuesEqual(Value a, Value b);
int hk_strncmp(const char *a, const char *b, int n);
typedef void (*HkEmitFn)(const char *);
extern HkEmitFn hkEmit; /* set by the host (native CLI or WASM shim) */

struct Table;
extern struct Table hkStrings; /* global string intern table, weak keys */

void printValue(Value value);         /* writes through the VM output sink */
void numberToString(double value, char *buf, int bufSize);
double parseDouble(const char *s, int length);

#endif
