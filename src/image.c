/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
/* Bytecode images: persist a compiled chunk tree to bytes (magic "HKI2") and
   load it back into fresh VM objects, source-free. Raw f64 bits (not the
   text used by --emit-bin) so -0 and full precision roundtrip exactly.
   Lines ride along so runtime error traces from images match source runs.
   Native-only (stdio host); the WASM build never loads images.
   HKI2: global operands are VM-wide slot indices, so the image carries the
   slot-name table between the magic and the root function. Loading requires
   an empty slot map (fresh process) — slot identities would otherwise
   collide with names a REPL already assigned. */
#include "image.h"

#include <string.h>

typedef struct {
    FILE *file;
    bool ok; /* sticky failure flag: once short-read/EOF, load is dead */
} ImageIO;

static void putU32(ImageIO *io, uint32_t v) {
    unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24 };
    fwrite(b, 1, 4, io->file);
}

static void putBytes(ImageIO *io, const void *data, size_t len) {
    fwrite(data, 1, len, io->file);
}

static uint32_t getU32(ImageIO *io) {
    unsigned char b[4];
    if (fread(b, 1, 4, io->file) != 4) { io->ok = false; return 0; }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void getBytes(ImageIO *io, void *dst, size_t len) {
    if (fread(dst, 1, len, io->file) != len) io->ok = false;
}

static void writeFn(ImageIO *io, ObjFunction *fn) {
    putU32(io, (uint32_t)fn->arity);
    putU32(io, (uint32_t)fn->upvalueCount);
    putU32(io, fn->name ? (uint32_t)fn->name->length : 0);
    if (fn->name) putBytes(io, fn->name->chars, (size_t)fn->name->length);
    Chunk *ch = fn->chunk;
    putU32(io, (uint32_t)ch->count);
    putBytes(io, ch->code, (size_t)ch->count);
    for (int i = 0; i < ch->count; i++) putU32(io, (uint32_t)ch->lines[i]);
    putU32(io, (uint32_t)ch->constants.count);
    for (int i = 0; i < ch->constants.count; i++) {
        Value v = ch->constants.values[i];
        if (IS_NUMBER(v)) {
            double d = AS_NUMBER(v);
            unsigned char b[8];
            memcpy(b, &d, 8);
            putBytes(io, "n", 1);
            putBytes(io, b, 8);
        } else if (IS_STRING(v)) {
            ObjString *s = AS_STRING(v);
            putBytes(io, "s", 1);
            putU32(io, (uint32_t)s->length);
            putBytes(io, s->chars, (size_t)s->length);
        } else {
            putBytes(io, "f", 1);
            writeFn(io, AS_FUNCTION(v));
        }
    }
}

static char *getBytesAlloc(ImageIO *io, uint32_t len) {
    char *buf = HK_REALLOC(NULL, 0, len + 1);
    if (buf == NULL) { io->ok = false; return NULL; }
    getBytes(io, buf, len);
    if (!io->ok) { HK_FREE(buf, len + 1); return NULL; }
    buf[len] = '\0';
    return buf;
}

static ObjFunction *readFn(ImageIO *io) {
    uint32_t arity = getU32(io);
    uint32_t upvalues = getU32(io);
    uint32_t nameLen = getU32(io);
    if (!io->ok) return NULL;
    ObjFunction *fn = newFunction();
    fn->chunk = HK_REALLOC(NULL, 0, sizeof(Chunk)); /* compiler.c convention */
    if (fn->chunk == NULL) return NULL;
    initChunk(fn->chunk);
    fn->arity = (int)arity;
    fn->upvalueCount = (int)upvalues;
    if (nameLen > 0) {
        char *name = getBytesAlloc(io, nameLen);
        if (name == NULL) return NULL;
        fn->name = copyString(name, (int)nameLen);
        HK_FREE(name, nameLen + 1);
    }
    uint32_t count = getU32(io);
    if (!io->ok) return NULL;
    uint8_t *codeBuf = HK_REALLOC(NULL, 0, count);
    uint32_t *lineBuf = HK_REALLOC(NULL, 0, count * sizeof(uint32_t));
    if (codeBuf == NULL || lineBuf == NULL) return NULL;
    getBytes(io, codeBuf, count); /* same layout as writeFn: code, then lines */
    for (uint32_t i = 0; i < count; i++) lineBuf[i] = getU32(io);
    if (!io->ok) { HK_FREE(codeBuf, count); HK_FREE(lineBuf, count * sizeof(uint32_t)); return NULL; }
    for (uint32_t i = 0; i < count; i++) writeChunk(fn->chunk, codeBuf[i], (int)lineBuf[i]);
    HK_FREE(codeBuf, count);
    HK_FREE(lineBuf, count * sizeof(uint32_t));
    uint32_t nConst = getU32(io);
    if (!io->ok) return NULL;
    for (uint32_t i = 0; i < nConst; i++) {
        char tag;
        getBytes(io, &tag, 1);
        if (!io->ok) return NULL;
        if (tag == 'n') {
            double d;
            getBytes(io, &d, 8);
            if (!io->ok) return NULL;
            addConstant(fn->chunk, NUMBER_VAL(d));
        } else if (tag == 's') {
            uint32_t len = getU32(io);
            if (!io->ok) return NULL;
            char *buf = getBytesAlloc(io, len);
            if (buf == NULL) return NULL;
            /* interning keeps identity semantics identical to compiled code */
            addConstant(fn->chunk, OBJ_VAL(copyString(buf, (int)len)));
            HK_FREE(buf, len + 1);
        } else if (tag == 'f') {
            ObjFunction *inner = readFn(io);
            if (inner == NULL) return NULL;
            addConstant(fn->chunk, OBJ_VAL(inner));
        } else {
            return NULL;
        }
    }
    return io->ok ? fn : NULL;
}

void writeImage(ObjFunction *fn, FILE *out) {
    ImageIO io = { out, true };
    putBytes(&io, "HKI2", 4);
    putU32(&io, (uint32_t)hkGlobalCount);
    for (int i = 0; i < hkGlobalCount; i++) {
        ObjString *s = hkGlobalNameList[i];
        putU32(&io, (uint32_t)s->length);
        putBytes(&io, s->chars, (size_t)s->length);
    }
    writeFn(&io, fn);
}

ObjFunction *readImage(FILE *in) {
    ImageIO io = { in, true };
    char magic[4];
    getBytes(&io, magic, 4);
    if (!io.ok || memcmp(magic, "HKI2", 4) != 0) return NULL;
    if (hkGlobalCount != 0) return NULL; /* slot identities must not collide */
    uint32_t slots = getU32(&io);
    for (uint32_t i = 0; i < slots; i++) {
        uint32_t len = getU32(&io);
        if (!io.ok || len > (1u << 20)) { io.ok = false; return NULL; }
        char buf[256];
        char *chars = buf;
        if (len >= sizeof(buf)) {
            chars = HK_REALLOC(NULL, 0, len + 1);
            if (!chars) { io.ok = false; return NULL; }
        }
        getBytes(&io, chars, len);
        if (!io.ok) { if (chars != buf) HK_FREE(chars, len + 1); return NULL; }
        chars[len] = '\0';
        /* slot index comes back identical: the map was empty, and slots
           allocate densely in order */
        hkGlobalSlot(copyString(chars, len));
        if (chars != buf) HK_FREE(chars, len + 1);
    }
    return readFn(&io);
}
