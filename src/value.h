#ifndef hokus_value_h
#define hokus_value_h

#include "common.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjFunction ObjFunction;
typedef struct ObjNative ObjNative;
struct Chunk;

typedef enum {
    VAL_NIL,
    VAL_BOOL,
    VAL_NUMBER,
    VAL_OBJ,
} ValueType;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        Obj *obj;
    } as;
} Value;

#define NIL_VAL         ((Value){VAL_NIL, {.number = 0}})
#define BOOL_VAL(v)     ((Value){VAL_BOOL, {.boolean = v}})
#define NUMBER_VAL(v)   ((Value){VAL_NUMBER, {.number = v}})
#define OBJ_VAL(o)      ((Value){VAL_OBJ, {.obj = (Obj *)(o)}})

#define IS_NIL(v)       ((v).type == VAL_NIL)
#define IS_BOOL(v)      ((v).type == VAL_BOOL)
#define IS_NUMBER(v)    ((v).type == VAL_NUMBER)
#define IS_OBJ(v)       ((v).type == VAL_OBJ)
#define IS_STRING(v)    (IS_OBJ(v) && ((Obj *)(v).as.obj)->type == OBJ_STRING)
#define IS_FUNCTION(v)  (IS_OBJ(v) && ((Obj *)(v).as.obj)->type == OBJ_FUNCTION)

#define AS_BOOL(v)      ((v).as.boolean)
#define AS_NUMBER(v)    ((v).as.number)
#define AS_OBJ(v)       ((v).as.obj)
#define AS_STRING(v)    ((ObjString *)(v).as.obj)
#define AS_FUNCTION(v)  ((ObjFunction *)(v).as.obj)
#define IS_NATIVE(v)    (IS_OBJ(v) && ((Obj *)(v).as.obj)->type == OBJ_NATIVE)
#define AS_NATIVE(v)    (((ObjNative *)(v).as.obj)->fn)
#define IS_CLOSURE(v)   (IS_OBJ(v) && ((Obj *)(v).as.obj)->type == OBJ_CLOSURE)
#define AS_CLOSURE(v)   ((ObjClosure *)(v).as.obj)
#define IS_CLASS(v)     (IS_OBJ(v) && ((Obj *)(v).as.obj)->type == OBJ_CLASS)
#define AS_CLASS(v)     ((ObjClass *)(v).as.obj)
#define IS_INSTANCE(v)  (IS_OBJ(v) && ((Obj *)(v).as.obj)->type == OBJ_INSTANCE)
#define AS_INSTANCE(v)  ((ObjInstance *)(v).as.obj)
#define IS_BOUND_METHOD(v) (IS_OBJ(v) && ((Obj *)(v).as.obj)->type == OBJ_BOUND_METHOD)
#define AS_BOUND_METHOD(v) ((ObjBoundMethod *)(v).as.obj)

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

void printValue(Value value);         /* writes through the VM output sink */
void numberToString(double value, char *buf, int bufSize);
double parseDouble(const char *s, int length);

#endif
