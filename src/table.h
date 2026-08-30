/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#ifndef hokus_table_h
#define hokus_table_h

#include "value.h"

typedef struct {
    ObjString *key;
    Value value;
} Entry;

typedef struct Table {
    int count;
    int capacity;
    Entry *entries;
} Table;

void initTable(Table *table);
void freeTable(Table *table);
bool tableGet(Table *table, ObjString *key, Value **valueOut);
bool tableSet(Table *table, ObjString *key, Value value); /* true if new key */
bool tableDelete(Table *table, ObjString *key);
void tableAddAll(Table *from, Table *to);
/* interning: probe by raw chars; drop keys whose object went unmarked */
ObjString *tableFindString(Table *table, const char *chars, int length, uint32_t hash);
void tableRemoveWhite(Table *table);

#endif
