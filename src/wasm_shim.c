/* Freestanding WASM host: bump allocator over a static heap, output captured
   into a static buffer. No libc, no imports — JS reads results from memory. */
#ifdef HK_WASM

#include "vm.h"
#include "compiler.h"
#include "debug.h"

#define HEAP_SIZE (16 * 1024 * 1024)
#define OUT_SIZE  (256 * 1024)
#define SRC_SIZE  (256 * 1024)

static uint8_t heap[HEAP_SIZE];
static size_t heapUsed;
static char outBuf[OUT_SIZE];
static int outLen;
static char srcBuf[SRC_SIZE];
static VM theVM;

static void bufferEmit(const char *s);

void *hk_alloc(size_t size) {
    size = (size + 7) & ~(size_t)7; /* 8-byte align */
    if (heapUsed + size > HEAP_SIZE) {
        bufferEmit("fatal: VM heap exhausted\n");
        __builtin_trap(); /* wasm `unreachable` — surfaces as RuntimeError in JS */
    }
    void *ptr = heap + heapUsed;
    heapUsed += size;
    return ptr;
}

void *hk_realloc_bump(void *ptr, size_t oldSize, size_t newSize) {
    void *fresh = hk_alloc(newSize);
    if (!fresh) return (void *)0;
    if (ptr) {
        uint8_t *d = fresh;
        const uint8_t *s = ptr;
        for (size_t i = 0; i < oldSize && i < newSize; i++) d[i] = s[i];
    }
    return fresh;
}

/* No wall clock in the freestanding build: count runs as virtual seconds. */
static double virtualClock;
double hkClockSeconds(void) { return virtualClock / 1000.0; }

static void bufferEmit(const char *s) {
    while (*s && outLen < OUT_SIZE - 2) outBuf[outLen++] = *s++;
}

/* ---- exports (no name mangling issues in C) ---- */

char *hk_source_buffer(void) { return srcBuf; }
int hk_source_capacity(void) { return SRC_SIZE; }

static int runInner(int sourceLength, int trace) {
    heapUsed = 0;
    outLen = 0;
    virtualClock += 1;
    srcBuf[sourceLength < SRC_SIZE ? sourceLength : SRC_SIZE - 1] = '\0';
    hkEmit = bufferEmit;
    initVM(&theVM);
    theVM.trace = trace;
    InterpretResult result = interpret(&theVM, srcBuf, sourceLength);
    outBuf[outLen] = '\0';
    return (int)result;
}

int hk_run(int sourceLength) { return runInner(sourceLength, 0); }

/* Same as hk_run, with per-instruction execution trace interleaved. */
int hk_run_trace(int sourceLength) { return runInner(sourceLength, 1); }

/* ---- debugger: prepare once, then step in slices (heap NOT reset) ---- */

/* Compile + prime the VM; trace on. Returns 0 ready, 65 compile error. */
int hk_prepare(int sourceLength, int trace) {
    heapUsed = 0;
    outLen = 0;
    virtualClock += 1;
    srcBuf[sourceLength < SRC_SIZE ? sourceLength : SRC_SIZE - 1] = '\0';
    hkEmit = bufferEmit;
    initVM(&theVM);
    theVM.trace = trace;
    outBuf[outLen] = '\0';
    return vmStart(&theVM, srcBuf, sourceLength) == INTERPRET_COMPILE_ERROR
        ? 65 : 0;
}

/* Run at most `budget` instructions of the prepared program.
   Returns 0 done, 2 paused (more to run), 70 runtime error. */
int hk_step(int budget) {
    InterpretResult r = vmRunSlice(&theVM, budget);
    outBuf[outLen] = '\0';
    if (r == INTERPRET_PAUSED) return 2;
    if (r == INTERPRET_RUNTIME_ERROR) return 70;
    return 0;
}

char *hk_output(void) { return outBuf; }
int hk_output_length(void) { return outLen; }

/* Compile-only: emits the bytecode listing to the output buffer.
   Returns 0 on success, 65 on compile error. */
int hk_disasm(int sourceLength) {
    heapUsed = 0;
    outLen = 0;
    srcBuf[sourceLength < SRC_SIZE ? sourceLength : SRC_SIZE - 1] = '\0';
    hkEmit = bufferEmit;
    objectsInit(); /* heap was rewound: intern table/object list must not dangle */
    ObjFunction *fn = compile(srcBuf, sourceLength);
    outBuf[outLen] = '\0';
    if (fn == NULL) return 65;
    disassembleChunk(fn->chunk, "web");
    outBuf[outLen] = '\0';
    return 0;
}

#endif
