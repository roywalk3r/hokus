/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#include "value.h"
#include "chunk.h"
#include "table.h"

static int hk_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void *hk_memcpy(void *dst, const void *src, size_t n) {
    char *d = dst;
    const char *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void initValueArray(ValueArray *array) {
    array->count = 0;
    array->capacity = 0;
    array->values = NULL;
}

void writeValueArray(ValueArray *array, Value value) {
    if (array->capacity < array->count + 1) {
        int old = array->capacity;
        array->capacity = old < 8 ? 8 : old * 2;
        array->values = HK_REALLOC(array->values, old * (int)sizeof(Value),
                                   array->capacity * (int)sizeof(Value));
    }
    array->values[array->count] = value;
    array->count++;
}

void freeValueArray(ValueArray *array) {
    HK_FREE(array->values, array->capacity * (int)sizeof(Value));
    initValueArray(array);
}

#include "gc.h"

/* GC state — declared extern in gc.c. */
Obj *hkObjects = NULL;
int hkGcCounter = 0;
int hkGcThreshold = 0;
int hkAllocTotal = 0;
int hkGcCollections = 0;
VM *hkActiveVM = NULL;

static Obj *allocateObj(size_t size, ObjType type) {
    gcOnAllocate();
    Obj *obj = HK_REALLOC(NULL, 0, size);
    obj->type = type;
    obj->isMarked = false;
    obj->next = hkObjects;
    hkObjects = obj;
    return obj;
}

static int hkGlobalCap = 0;

void objectsInit(void) {
    hkObjects = NULL;
    hkGcCounter = 0;
    hkGcThreshold = 0;
    hkAllocTotal = 0;
    hkGcCollections = 0;
    hkActiveVM = NULL;
    initTable(&hkStrings);
#ifdef HK_WASM
    /* wasm runs on a bump arena rewound between initVM() calls: HK_FREE-ing a
       stale arena pointer would corrupt the free list (the rewind already
       reclaimed it), but the bookkeeping must still reset or hkGlobalSlot
       reads detached pointers next round. */
    initTable(&hkGlobalNames);
    hkGlobalNameList = NULL;
    hkGlobalCap = 0;
    hkGlobalCount = 0;
#else
    hkGlobalsFree();
#endif
}

static uint32_t hashString(const char *key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

/* Global intern table — weak keys: gc.c's sweep drops unmarked entries.
   Interning is process-global (not per-VM), matching hkObjects ownership. */
Table hkStrings;

/* See value.h for why this is process-global. hkGlobalNameList entries are
   interned strings, so as long as gc.c marks them the intern table keeps
   the key side alive too. */
Table hkGlobalNames;
ObjString **hkGlobalNameList = NULL;
int hkGlobalCount = 0;

int hkGlobalSlot(ObjString *name) {
    Value *existing;
    if (tableGet(&hkGlobalNames, name, &existing))
        return (int)AS_NUMBER(*existing);
    int slot = hkGlobalCount;
    if (slot >= hkGlobalCap) {
        int oldCap = hkGlobalCap;
        hkGlobalCap = oldCap < 8 ? 8 : oldCap * 2;
        hkGlobalNameList = HK_REALLOC(hkGlobalNameList,
            (size_t)oldCap * sizeof(ObjString *),
            (size_t)hkGlobalCap * sizeof(ObjString *));
    }
    hkGlobalNameList[slot] = name;
    /* mark valid before tableSet: if a future allocation path ever collects
       mid-slot-assign, slot < hkGlobalCount already covers this entry */
    hkGlobalCount++;
    tableSet(&hkGlobalNames, name, NUMBER_VAL((double)slot));
    return slot;
}

void hkGlobalsFree(void) {
    freeTable(&hkGlobalNames);
    HK_FREE(hkGlobalNameList, (size_t)hkGlobalCap * sizeof(ObjString *));
    hkGlobalNameList = NULL;
    hkGlobalCap = 0;
    hkGlobalCount = 0;
}

static ObjString *allocateString(char *chars, int length, uint32_t hash) {
    ObjString *interned = tableFindString(&hkStrings, chars, length, hash);
    if (interned != NULL) {
        if (chars != interned->chars) HK_FREE(chars, length + 1);
        return interned;
    }
    ObjString *string = (ObjString *)allocateObj(sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->hash = hash;
    string->chars = chars;
    tableSet(&hkStrings, string, NIL_VAL);
    return string;
}

ObjString *takeString(char *chars, int length) {
    return allocateString(chars, length, hashString(chars, length));
}

ObjString *copyString(const char *chars, int length) {
    uint32_t hash = hashString(chars, length);
    ObjString *interned = tableFindString(&hkStrings, chars, length, hash);
    if (interned != NULL) return interned;
    char *heap = HK_REALLOC(NULL, 0, length + 1);
    hk_memcpy(heap, chars, length);
    heap[length] = '\0';
    return allocateString(heap, length, hash);
}

ObjFunction *newFunction(void) {
    ObjFunction *fn = (ObjFunction *)allocateObj(sizeof(ObjFunction), OBJ_FUNCTION);
    fn->arity = 0;
    fn->upvalueCount = 0;
    fn->chunk = NULL;
    fn->name = NULL;
    return fn;
}

ObjUpvalue *newUpvalue(Value *slot) {
    ObjUpvalue *upvalue = (ObjUpvalue *)allocateObj(sizeof(ObjUpvalue), OBJ_UPVALUE);
    upvalue->location = slot;
    upvalue->closed = NIL_VAL;
    upvalue->next = NULL;
    return upvalue;
}

ObjClosure *newClosure(ObjFunction *function) {
    ObjUpvalue **upvalues = HK_REALLOC(NULL, 0,
                                       sizeof(ObjUpvalue *) * function->upvalueCount);
    for (int i = 0; i < function->upvalueCount; i++) upvalues[i] = NULL;
    ObjClosure *closure = (ObjClosure *)allocateObj(sizeof(ObjClosure), OBJ_CLOSURE);
    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalueCount = function->upvalueCount;
    return closure;
}

ObjNative *newNative(NativeFn fn) {
    ObjNative *native = (ObjNative *)allocateObj(sizeof(ObjNative), OBJ_NATIVE);
    native->fn = fn;
    return native;
}

ObjClass *newClass(ObjString *name) {
    ObjClass *klass = (ObjClass *)allocateObj(sizeof(ObjClass), OBJ_CLASS);
    klass->name = name;
    klass->methods = HK_REALLOC(NULL, 0, sizeof(Table));
    initTable(klass->methods);
    klass->statics = HK_REALLOC(NULL, 0, sizeof(Table));
    initTable(klass->statics);
    return klass;
}

ObjInstance *newInstance(ObjClass *klass) {
    ObjInstance *instance = (ObjInstance *)allocateObj(sizeof(ObjInstance), OBJ_INSTANCE);
    instance->klass = klass;
    instance->fields = HK_REALLOC(NULL, 0, sizeof(Table));
    initTable(instance->fields);
    return instance;
}

ObjBoundMethod *newBoundMethod(Value receiver, ObjClosure *method) {
    ObjBoundMethod *bound = (ObjBoundMethod *)allocateObj(sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);
    bound->receiver = receiver;
    bound->method = method;
    return bound;
}

/* Freestanding strtod for decimal literals (digits [. digits]). */
double parseDouble(const char *s, int length) {
    double result = 0;
    int i = 0;
    while (i < length && s[i] >= '0' && s[i] <= '9') {
        result = result * 10 + (s[i] - '0');
        i++;
    }
    if (i < length && s[i] == '.') {
        i++;
        double frac = 0, scale = 1;
        while (i < length && s[i] >= '0' && s[i] <= '9') {
            frac = frac * 10 + (s[i] - '0');
            scale *= 10;
            i++;
        }
        result += frac / scale;
    }
    return result;
}

/* Minimal freestanding %.14g: 14 significant digits, rounded, zeros trimmed. */
void numberToString(double value, char *buf, int bufSize) {
    if (value != value) { hk_memcpy(buf, "nan", 4); return; }
    int i = 0;
    if (value < 0 || (value == 0 && 1.0 / value < 0)) { /* -0.0 is not < 0 */
        buf[i++] = '-';
        value = -value;
    }
    if (value >= 1e15) { hk_memcpy(buf + i, "big", 4); return; }

    long long ip = (long long)value;
    double frac = value - (double)ip;

    /* count integer digits to budget significant digits */
    int intDigits = 0;
    for (long long t = ip; t > 0; t /= 10) intDigits++;
    int sigBudget = 14 - intDigits; /* digits available for the fraction */

    /* generate fraction digits */
    char fd[24];
    int m = 0;
    while (m < sigBudget && m < 20 && frac > 1e-15) {
        frac *= 10;
        int d = (int)frac;
        fd[m++] = (char)('0' + d);
        frac -= d;
    }
    /* round: look at the next digit */
    frac *= 10;
    if ((int)frac >= 5) {
        int k = m - 1;
        while (k >= 0 && fd[k] == '9') { fd[k] = '0'; k--; }
        if (k >= 0) fd[k]++;
        else { ip++; m = 0; } /* carried into the integer part */
    }
    /* trim trailing zeros */
    while (m > 0 && fd[m - 1] == '0') m--;

    /* emit integer part */
    char tmp[24];
    int t = 0;
    if (ip == 0) {
        buf[i++] = '0';
    } else {
        while (ip > 0) { tmp[t++] = (char)('0' + ip % 10); ip /= 10; }
        while (t > 0 && i + m + 2 < bufSize) buf[i++] = tmp[--t];
    }
    if (m > 0 && i + m + 2 < bufSize) {
        buf[i++] = '.';
        for (int k = 0; k < m; k++) buf[i++] = fd[k];
    }
    buf[i] = '\0';
}

bool valuesEqual(Value a, Value b) {
    /* Numbers are bitwise-distinct NaN-space encodings; everything else is a
       unique 64-bit token (interned strings, singleton nil/true/false). */
    if (IS_NUMBER(a) && IS_NUMBER(b)) return AS_NUMBER(a) == AS_NUMBER(b);
    return a == b;
}

int hk_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

void printValue(Value value) {
    char buf[64];
    if (IS_BOOL(value))      hkEmit(AS_BOOL(value) ? "true" : "false");
    else if (IS_NIL(value))  hkEmit("nil");
    else if (IS_NUMBER(value)) {
        numberToString(AS_NUMBER(value), buf, sizeof(buf));
        hkEmit(buf);
    }
    else {
            if (IS_STRING(value)) {
                hkEmit(AS_STRING(value)->chars); /* always null-terminated */
            } else if (IS_CLASS(value)) {
                hkEmit(AS_CLASS(value)->name->chars);
            } else if (IS_INSTANCE(value)) {
                hkEmit(AS_INSTANCE(value)->klass->name->chars);
                hkEmit(" instance");
            } else if (IS_BOUND_METHOD(value)) {
                ObjFunction *f = AS_BOUND_METHOD(value)->method->function;
                hkEmit("<fn ");
                if (f->name) hkEmit(f->name->chars);
                hkEmit(">");
            } else if (IS_NATIVE(value)) {
                hkEmit("<native fn>");
            } else if (IS_CLOSURE(value)) {
                ObjFunction *f = AS_CLOSURE(value)->function;
                if (f->name) {
                    hkEmit("<fn ");
                    hkEmit(f->name->chars);
                    hkEmit(">");
                } else {
                    hkEmit("<script>");
                }
            } else if (IS_FUNCTION(value)) {
                ObjFunction *f = AS_FUNCTION(value);
                if (f->name) {
                    hkEmit("<fn ");
                    hkEmit(f->name->chars);
                    hkEmit(">");
                } else {
                    hkEmit("<script>");
                }
            }
    }
    (void)hk_strlen;
}
