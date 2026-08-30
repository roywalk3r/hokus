/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#include "chunk.h"
#include "gc.h"
#include "table.h"

/* value.c owns the object list; these are its GC interfaces. */
extern Obj *hkObjects;              /* head of every allocated object */
extern int hkGcCounter;             /* allocations since last GC */
extern int hkGcThreshold;           /* collect when counter exceeds this */
extern VM *hkActiveVM;              /* non-NULL only while run() is executing */

#define GC_INITIAL_THRESHOLD 1024

static Value *grayStack = NULL;
static int grayCount = 0;
static int grayCapacity = 0;

static void markValue(Value value);
static void markObject(Obj *obj);

static void grayPush(Value value) {
    if (!IS_OBJ(value)) return;
    ((Obj *)AS_OBJ(value))->isMarked = true;
    if (grayCapacity < grayCount + 1) {
        int old = grayCapacity;
        grayCapacity = old < 8 ? 8 : old * 2;
        grayStack = HK_REALLOC(grayStack, old * (int)sizeof(Value),
                               grayCapacity * (int)sizeof(Value));
    }
    grayStack[grayCount++] = value;
}

static void markValue(Value value) {
    if (IS_OBJ(value)) markObject(AS_OBJ(value));
}

static void markChunkConstants(Chunk *chunk) {
    for (int i = 0; i < chunk->constants.count; i++) {
        markValue(chunk->constants.values[i]);
    }
}

static void markTable(Table *table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry *entry = &table->entries[i];
        if (entry->key != NULL) {
            markObject((Obj *)entry->key);
            markValue(entry->value);
        }
    }
}

static void blacken(Obj *obj) {
    switch (obj->type) {
        case OBJ_STRING:
        case OBJ_NATIVE:
            break; /* no outgoing references */
        case OBJ_UPVALUE:
            markValue(((ObjUpvalue *)obj)->closed);
            break;
        case OBJ_FUNCTION: {
            ObjFunction *fn = (ObjFunction *)obj;
            if (fn->name) markObject((Obj *)fn->name);
            if (fn->chunk) markChunkConstants(fn->chunk);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *closure = (ObjClosure *)obj;
            markObject((Obj *)closure->function);
            for (int i = 0; i < closure->upvalueCount; i++) {
                if (closure->upvalues[i]) markObject((Obj *)closure->upvalues[i]);
            }
            break;
        }
        case OBJ_CLASS: {
            ObjClass *klass = (ObjClass *)obj;
            markObject((Obj *)klass->name);
            markTable(klass->methods);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance *instance = (ObjInstance *)obj;
            markObject((Obj *)instance->klass);
            markTable(instance->fields);
            break;
        }
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod *bound = (ObjBoundMethod *)obj;
            markValue(bound->receiver);
            markObject((Obj *)bound->method);
            break;
        }
    }
}

static void markObject(Obj *obj) {
    if (obj == NULL || obj->isMarked) return;
    grayPush(OBJ_VAL(obj));
}

static void markRoots(VM *vm) {
    for (Value *slot = vm->stack; slot < vm->stackTop; slot++) {
        markValue(*slot);
    }
    for (int i = 0; i < vm->frameCount; i++) {
        markObject((Obj *)vm->frames[i].closure);
    }
    if (vm->initString) markObject((Obj *)vm->initString);
    for (ObjUpvalue *upvalue = vm->openUpvalues; upvalue != NULL;
         upvalue = upvalue->next) {
        markObject((Obj *)upvalue);
    }
    for (int i = 0; i < vm->globals.capacity; i++) {
        Entry *entry = &vm->globals.entries[i];
        if (entry->key != NULL) {
            markObject((Obj *)entry->key);
            markValue(entry->value);
        }
    }
}

static void traceReferences(void) {
    while (grayCount > 0) {
        blacken(AS_OBJ(grayStack[--grayCount]));
    }
}

static void freeObject(Obj *obj) {
    switch (obj->type) {
        case OBJ_STRING: {
            ObjString *s = (ObjString *)obj;
            HK_FREE(s->chars, s->length + 1);
            HK_FREE(s, sizeof(ObjString));
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction *fn = (ObjFunction *)obj;
            if (fn->chunk) {
                freeChunk(fn->chunk);
                HK_FREE(fn->chunk, sizeof(Chunk));
            }
            HK_FREE(fn, sizeof(ObjFunction));
            break;
        }
        case OBJ_NATIVE:
            HK_FREE(obj, sizeof(ObjNative));
            break;
        case OBJ_CLOSURE: {
            ObjClosure *c = (ObjClosure *)obj;
            HK_FREE(c->upvalues, c->upvalueCount * (int)sizeof(ObjUpvalue *));
            HK_FREE(c, sizeof(ObjClosure));
            break;
        }
        case OBJ_UPVALUE:
            HK_FREE(obj, sizeof(ObjUpvalue));
            break;
        case OBJ_CLASS: {
            ObjClass *klass = (ObjClass *)obj;
            freeTable(klass->methods);
            HK_FREE(klass->methods, sizeof(Table));
            HK_FREE(klass, sizeof(ObjClass));
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance *instance = (ObjInstance *)obj;
            freeTable(instance->fields);
            HK_FREE(instance->fields, sizeof(Table));
            HK_FREE(instance, sizeof(ObjInstance));
            break;
        }
        case OBJ_BOUND_METHOD:
            HK_FREE(obj, sizeof(ObjBoundMethod));
            break;
    }
}

static void sweep(void) {
    /* Weak keys must die before their objects, or the table dangles. */
    tableRemoveWhite(&hkStrings);
    Obj *previous = NULL;
    Obj *object = hkObjects;
    while (object != NULL) {
        if (object->isMarked) {
            object->isMarked = false;
            previous = object;
            object = object->next;
        } else {
            Obj *unreached = object;
            object = object->next;
            if (previous != NULL) previous->next = object;
            else hkObjects = object;
            freeObject(unreached);
        }
    }
}

void gcCollect(VM *vm) {
    markRoots(vm);
    traceReferences();
    sweep();
    hkGcCounter = 0;
    hkGcThreshold *= 2;
}

void gcFreeAll(void) {
    Obj *object = hkObjects;
    while (object != NULL) {
        Obj *next = object->next;
        freeObject(object);
        object = next;
    }
    hkObjects = NULL;
    freeTable(&hkStrings);
    initTable(&hkStrings);
}

void gcOnAllocate(void) {
    if (hkGcThreshold == 0) hkGcThreshold = GC_INITIAL_THRESHOLD;
    hkGcCounter++;
    if (hkGcCounter > hkGcThreshold && hkActiveVM != NULL) {
        gcCollect(hkActiveVM);
    }
}
