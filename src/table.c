/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#include "table.h"

#define TABLE_MAX_LOAD 0.75

void initTable(Table *table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void freeTable(Table *table) {
    HK_FREE(table->entries, table->capacity * (int)sizeof(Entry));
    initTable(table);
}

static Entry *findEntry(Entry *entries, int capacity, ObjString *key) {
    uint32_t index = key->hash % (uint32_t)capacity;
    Entry *tombstone = NULL;
    for (;;) {
        Entry *entry = &entries[index];
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) {
                return tombstone ? tombstone : entry; /* empty */
            }
            if (!tombstone) tombstone = entry;
        } else if (entry->key == key ||
                   (entry->key->hash == key->hash &&
                    entry->key->length == key->length &&
                    hk_strncmp(entry->key->chars, key->chars, key->length) == 0)) {
            return entry;
        }
        index = (index + 1) % (uint32_t)capacity;
    }
}

bool tableGet(Table *table, ObjString *key, Value **valueOut) {
    if (table->count == 0) return false;
    Entry *entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;
    *valueOut = &entry->value;
    return true;
}

static void adjustCapacity(Table *table, int capacity) {
    Entry *entries = HK_REALLOC(NULL, 0, capacity * (int)sizeof(Entry));
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].value = NIL_VAL;
    }
    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        Entry *entry = &table->entries[i];
        if (entry->key == NULL) continue;
        Entry *dest = findEntry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }
    HK_FREE(table->entries, table->capacity * (int)sizeof(Entry));
    table->entries = entries;
    table->capacity = capacity;
}

bool tableSet(Table *table, ObjString *key, Value value) {
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = table->capacity < 8 ? 8 : table->capacity * 2;
        adjustCapacity(table, capacity);
    }
    Entry *entry = findEntry(table->entries, table->capacity, key);
    bool isNew = entry->key == NULL;
    if (isNew && IS_NIL(entry->value)) table->count++;
    entry->key = key;
    entry->value = value;
    return isNew;
}

bool tableDelete(Table *table, ObjString *key) {
    if (table->count == 0) return false;
    Entry *entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;
    entry->key = NULL;
    entry->value = BOOL_VAL(true); /* tombstone */
    return true;
}

void tableAddAll(Table *from, Table *to) {
    for (int i = 0; i < from->capacity; i++) {
        Entry *entry = &from->entries[i];
        if (entry->key != NULL) tableSet(to, entry->key, entry->value);
    }
}

ObjString *tableFindString(Table *table, const char *chars, int length, uint32_t hash) {
    if (table->count == 0) return NULL;
    uint32_t index = hash % (uint32_t)table->capacity;
    for (;;) {
        Entry *entry = &table->entries[index];
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) return NULL; /* empty, not tombstone */
        } else if (entry->key->length == length && entry->key->hash == hash &&
                   hk_strncmp(entry->key->chars, chars, length) == 0) {
            return entry->key;
        }
        index = (index + 1) % (uint32_t)table->capacity;
    }
}

void tableRemoveWhite(Table *table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry *entry = &table->entries[i];
        if (entry->key != NULL && !entry->key->obj.isMarked) {
            tableDelete(table, entry->key);
        }
    }
}
