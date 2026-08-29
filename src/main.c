/* Native host: REPL, file runner, --disasm. Native-only (libc). */
#ifndef HK_WASM

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "debug.h"
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
    free(source);
}

static void repl(VM *vm) {
    char line[2048];
    for (;;) {
        printf("> ");
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        interpret(vm, line, (int)strlen(line));
        vm->hadRuntimeError = false; /* keep the session alive past one bad line */
    }
}

int main(int argc, char **argv) {
    hkEmit = stdoutEmit;
    bool disasm = false;
    bool trace = false;
    bool scan = false;
    int slice = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disasm") == 0 || strcmp(argv[i], "-d") == 0) {
            disasm = true;
        } else if (strcmp(argv[i], "--trace") == 0 || strcmp(argv[i], "-t") == 0) {
            trace = true;
        } else if (strcmp(argv[i], "--scan") == 0 || strcmp(argv[i], "-s") == 0) {
            scan = true;
        } else if (strcmp(argv[i], "--slice") == 0 && i + 1 < argc) {
            slice = atoi(argv[++i]);
            if (slice < 1) slice = 1;
        } else {
            path = argv[i];
        }
    }

    VM vm;
    initVM(&vm);
    vm.trace = trace;
    if (path == NULL) {
        repl(&vm);
    } else {
        runFile(&vm, path, disasm, scan, slice);
    }
    freeVM(&vm);
    return 0;
}

#endif /* HK_WASM */
