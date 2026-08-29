/* Native host: REPL, file runner, --disasm. Native-only (libc). */
#ifndef HK_WASM

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "debug.h"
#include "vm.h"

static void stdoutEmit(const char *s) { fputs(s, stdout); }

static char *readFile(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(65);
    }
    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);
    char *buffer = malloc(fileSize + 1);
    if (buffer == NULL || fread(buffer, sizeof(char), fileSize, file) != fileSize) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }
    buffer[fileSize] = '\0';
    fclose(file);
    return buffer;
}

static void runFile(VM *vm, const char *path, bool disasm) {
    char *source = readFile(path);
    if (disasm) {
        ObjFunction *fn = compile(source, (int)strlen(source));
        if (fn == NULL) exit(65);
        disassembleChunk(fn->chunk, path);
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
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disasm") == 0 || strcmp(argv[i], "-d") == 0) {
            disasm = true;
        } else {
            path = argv[i];
        }
    }

    VM vm;
    initVM(&vm);
    if (path == NULL) {
        repl(&vm);
    } else {
        runFile(&vm, path, disasm);
    }
    freeVM(&vm);
    return 0;
}

#endif /* HK_WASM */
