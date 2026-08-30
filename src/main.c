/* Native host: REPL, file runner, --disasm. Native-only (libc). */
#ifndef HK_WASM

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "debug.h"
#include "image.h"
#include "lexer.h"
#include "vm.h"

static void stdoutEmit(const char *s) { fputs(s, stdout); }

#include <time.h>
double hkClockSeconds(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }

static char *readFile(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(65);
    }
    size_t capacity = 4096, count = 0;
    char *buffer = malloc(capacity);
    size_t read;
    while (buffer != NULL && (read = fread(buffer + count, 1, capacity - count, file)) > 0) {
        count += read;
        if (count == capacity) {
            capacity *= 2;
            buffer = realloc(buffer, capacity);
        }
    }
    if (buffer == NULL || ferror(file)) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }
    buffer[count] = '\0';
    fclose(file);
    return buffer;
}

/* --emit-bin oracle: canonical serialization of a compiled chunk tree.
   Numbers go out as numberToString text so the roundtrip assembler compares
   exactly what the disassembler printed — never a re-decoded double. */
static void emitU32(uint32_t v) {
    unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24 };
    fwrite(b, 1, 4, stdout);
}

static void emitBinChunk(ObjFunction *fn) {
    Chunk *ch = fn->chunk;
    emitU32((uint32_t)ch->count);
    fwrite(ch->code, 1, (size_t)ch->count, stdout);
    emitU32((uint32_t)ch->constants.count);
    for (int i = 0; i < ch->constants.count; i++) {
        Value v = ch->constants.values[i];
        if (IS_NUMBER(v)) {
            char buf[32];
            numberToString(AS_NUMBER(v), buf, sizeof(buf));
            fputc('n', stdout);
            emitU32((uint32_t)strlen(buf));
            fputs(buf, stdout);
        } else if (IS_STRING(v)) {
            ObjString *s = AS_STRING(v);
            fputc('s', stdout);
            emitU32((uint32_t)s->length);
            fwrite(s->chars, 1, (size_t)s->length, stdout);
        } else {
            ObjFunction *f = AS_FUNCTION(v);
            fputc('f', stdout);
            emitU32(f->name ? (uint32_t)f->name->length : 0);
            if (f->name) fwrite(f->name->chars, 1, (size_t)f->name->length, stdout);
            emitBinChunk(f);
        }
    }
}

static void emitBinary(const char *source) {
    ObjFunction *fn = compile(source, (int)strlen(source));
    if (fn == NULL) exit(65);
    emitBinChunk(fn);
}

static void runFile(VM *vm, const char *path, bool disasm, bool scan, int slice) {
    char *source = readFile(path);
    if (scan) {
        lexDump(source);
    } else if (disasm) {
        ObjFunction *fn = compile(source, (int)strlen(source));
        if (fn == NULL) exit(65);
        disassembleChunk(fn->chunk, path);
    } else if (slice > 0) {
        if (vmStart(vm, source, (int)strlen(source)) == INTERPRET_COMPILE_ERROR) exit(65);
        InterpretResult result = INTERPRET_PAUSED;
        while (result == INTERPRET_PAUSED) result = vmRunSlice(vm, slice);
        if (result == INTERPRET_COMPILE_ERROR) exit(65);
        if (result == INTERPRET_RUNTIME_ERROR) exit(70);
    } else {
        InterpretResult result = interpret(vm, source, (int)strlen(source));
        if (result == INTERPRET_COMPILE_ERROR) exit(65);
        if (result == INTERPRET_RUNTIME_ERROR) exit(70);
    }
    if (getenv("HOKUS_STATS")) {
        extern int hkAllocTotal, hkGcCollections;
        fprintf(stderr, "stats: %d allocations, %d GC collections\n",
                hkAllocTotal, hkGcCollections);
    }
    free(source);
}

/* Net open-bracket count for one physical line, string- and comment-aware —
   lets the REPL accumulate multi-line blocks without a full lex. */
static int bracketDelta(const char *s) {
    int delta = 0;
    bool inStr = false, esc = false;
    for (const char *c = s; *c; c++) {
        if (inStr) {
            if (esc) esc = false;
            else if (*c == '\\') esc = true;
            else if (*c == '"') inStr = false;
            continue;
        }
        if (*c == '/' && c[1] == '/') break;
        if (*c == '"') inStr = true;
        else if (*c == '{' || *c == '(') delta++;
        else if (*c == '}' || *c == ')') delta--;
    }
    return delta;
}

#define REPL_MAX 16384

static void repl(VM *vm) {
    char acc[REPL_MAX];
    size_t accLen = 0;
    int depth = 0;
    char line[2048];
    for (;;) {
        printf(depth > 0 ? ". " : "> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        depth += bracketDelta(line);
        size_t lineLen = strlen(line);
        if (accLen + lineLen >= REPL_MAX) {
            fprintf(stderr, "repl: input too long\n");
            accLen = 0;
            depth = 0;
            continue;
        }
        memcpy(acc + accLen, line, lineLen + 1);
        accLen += lineLen;
        if (depth > 0) continue;
        if (accLen > 0) {
            interpret(vm, acc, (int)accLen);
            vm->hadRuntimeError = false; /* keep the session alive past one bad line */
        }
        accLen = 0;
        depth = 0;
    }
}

int main(int argc, char **argv) {
    hkEmit = stdoutEmit;
    bool disasm = false;
    bool trace = false;
    bool scan = false;
    bool emitBin = false;
    bool emitImage = false;
    int slice = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disasm") == 0 || strcmp(argv[i], "-d") == 0) {
            disasm = true;
        } else if (strcmp(argv[i], "--trace") == 0 || strcmp(argv[i], "-t") == 0) {
            trace = true;
        } else if (strcmp(argv[i], "--scan") == 0 || strcmp(argv[i], "-s") == 0) {
            scan = true;
        } else if (strcmp(argv[i], "--emit-bin") == 0) {
            emitBin = true;
        } else if (strcmp(argv[i], "--emit-image") == 0) {
            emitImage = true;
        } else if (strcmp(argv[i], "--slice") == 0 && i + 1 < argc) {
            slice = atoi(argv[++i]);
            if (slice < 1) slice = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "usage: hokus [flags] [script.hk]\n"
                "  (no script)      REPL\n"
                "  -d, --disasm     compile and print bytecode listing\n"
                "  -t, --trace      print each instruction as it executes\n"
                "  -s, --scan       token dump (jlox format), no execution\n"
                "  --slice N        execute in N-instruction budgeted slices\n"
                "  --emit-image     write a .hki bytecode image to stdout\n"
                "  (a .hki file)    run a bytecode image, no source needed\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "hokus: unknown flag '%s' (try --help)\n", argv[i]);
            return 64;
        } else {
            path = argv[i];
        }
    }

    VM vm;
    initVM(&vm);
    vm.trace = trace;
    if (path == NULL) {
        repl(&vm);
    } else if (emitBin) {
        emitBinary(readFile(path));
    } else if (emitImage) {
        char *source = readFile(path);
        ObjFunction *fn = compile(source, (int)strlen(source));
        if (fn == NULL) { free(source); return 65; }
        writeImage(fn, stdout);
        free(source);
    } else if (strstr(path, ".hki") != NULL) {
        FILE *in = fopen(path, "rb");
        if (in == NULL) {
            fprintf(stderr, "Could not open file \"%s\".\n", path);
            return 74;
        }
        ObjFunction *fn = readImage(in);
        fclose(in);
        if (fn == NULL) {
            fprintf(stderr, "hokus: \"%s\" is not a valid bytecode image\n", path);
            return 65;
        }
        vmStartFunction(&vm, fn);
        InterpretResult result = vmRunSlice(&vm, -1);
        if (result == INTERPRET_RUNTIME_ERROR) return 70;
    } else {
        runFile(&vm, path, disasm, scan, slice);
    }
    freeVM(&vm);
    return 0;
}

#endif /* HK_WASM */
