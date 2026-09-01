/* Freestanding WASM host: bump allocator over a static heap, output captured
   into a static buffer. No libc, no imports — JS reads results from memory. */
#ifdef HK_WASM

#include "vm.h"
#include "compiler.h"
#include "debug.h"

/* Bump heap; HK_FREE is a no-op here, so GC recycles reachability only —
   size this for a full session of stepped execution, not one run. */
#define HEAP_SIZE (32 * 1024 * 1024)
#define OUT_SIZE  (256 * 1024)
#define SRC_SIZE  (256 * 1024)

static uint8_t heap[HEAP_SIZE];
static size_t heapUsed;
static char outBuf[OUT_SIZE];
static int outLen;
static char srcBuf[SRC_SIZE];
static VM theVM;

/* Small-block free list: 16-byte granularity up to 256 bytes. The bump heap
   has no coalescing, but GC churn is dominated by same-size ObjStrings, so a
   size-classed list recycles the blocks sweep() actually releases. */
#define FL_CLASSES 16
static void *freeList[FL_CLASSES];

static size_t bucketOf(size_t size) { return (size >> 4) - 1; }

/* Module store for `import` on the no-filesystem host: JS stages name/source
   into fixed buffers, hk_add_module copies both into a persistent arena
   (untouched by hkHeapReset, so modules survive across runs). Lookup is by
   exact path string — the playground has no directories. */
#define MOD_MAX  16
#define MOD_NAME 64
#define MOD_SRC  (32 * 1024)
static char modStageName[MOD_NAME];
static char modStageSrc[MOD_SRC];
static char modNames[MOD_MAX][MOD_NAME];
static char modArena[MOD_MAX][MOD_SRC];
static int modCount;

static bool pathEq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static const char *wasmReadModule(const char *path) {
    for (int i = 0; i < modCount; i++) {
        if (pathEq(modNames[i], path)) return modArena[i];
    }
    return NULL;
}

static void bufferEmit(const char *s);

static void hkHeapReset(void) {
    heapUsed = 0;
    for (int i = 0; i < FL_CLASSES; i++) freeList[i] = NULL;
}

void *hk_alloc(size_t size) {
    size = (size + 15) & ~(size_t)15; /* 16-byte align, free-list node space */
    if (size >= 16 && size <= FL_CLASSES * 16) {
        size_t b = bucketOf(size);
        if (freeList[b] != NULL) {
            void *p = freeList[b];
            freeList[b] = *(void **)p;
            return p;
        }
    }
    if (heapUsed + size > HEAP_SIZE) {
        bufferEmit("fatal: VM heap exhausted\n");
        __builtin_trap(); /* wasm `unreachable` — surfaces as RuntimeError in JS */
    }
    void *ptr = heap + heapUsed;
    heapUsed += size;
    return ptr;
}

void hk_free_bump(void *ptr, size_t size) {
    if (ptr == NULL) return;
    size = (size + 15) & ~(size_t)15;
    if (size < 16 || size > FL_CLASSES * 16) return; /* big blocks stay bumped */
    size_t b = bucketOf(size);
    *(void **)ptr = freeList[b];
    freeList[b] = ptr;
}

void *hk_realloc_bump(void *ptr, size_t oldSize, size_t newSize) {
    void *fresh = hk_alloc(newSize);
    if (!fresh) return (void *)0;
    if (ptr) {
        uint8_t *d = fresh;
        const uint8_t *s = ptr;
        for (size_t i = 0; i < oldSize && i < newSize; i++) d[i] = s[i];
        hk_free_bump(ptr, oldSize);
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

char *hk_module_name_buffer(void) { return modStageName; }
char *hk_module_src_buffer(void) { return modStageSrc; }
int hk_module_src_capacity(void) { return MOD_SRC; }

/* 0 ok, 1 rejected (full / oversized). Copies out of the staging buffers. */
int hk_add_module(int nameLen, int srcLen) {
    if (modCount >= MOD_MAX) return 1;
    if (nameLen <= 0 || nameLen >= MOD_NAME || srcLen <= 0 || srcLen >= MOD_SRC) return 1;
    for (int i = 0; i < nameLen; i++) modNames[modCount][i] = modStageName[i];
    modNames[modCount][nameLen] = '\0';
    for (int i = 0; i < srcLen; i++) modArena[modCount][i] = modStageSrc[i];
    modArena[modCount][srcLen] = '\0';
    modCount++;
    return 0;
}

void hk_clear_modules(void) { modCount = 0; }

static int runInner(int sourceLength, int trace) {
    hkHeapReset();
    outLen = 0;
    virtualClock += 1;
    srcBuf[sourceLength < SRC_SIZE ? sourceLength : SRC_SIZE - 1] = '\0';
    hkEmit = bufferEmit;
    hkReadModule = wasmReadModule;
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
    hkHeapReset();
    outLen = 0;
    virtualClock += 1;
    srcBuf[sourceLength < SRC_SIZE ? sourceLength : SRC_SIZE - 1] = '\0';
    hkEmit = bufferEmit;
    hkReadModule = wasmReadModule;
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

/* ---- runtime statistics (reset per run/prep) ---- */

extern int hkAllocTotal;    /* value.c */
extern int hkGcCollections; /* gc.c */

int hk_stat_allocs(void) { return hkAllocTotal; }
int hk_stat_gc_runs(void) { return hkGcCollections; }
int hk_stat_heap_bytes(void) { return (int)heapUsed; }

/* Compile-only: emits the bytecode listing to the output buffer.
   Returns 0 on success, 65 on compile error. */
int hk_disasm(int sourceLength) {
    hkHeapReset();
    outLen = 0;
    srcBuf[sourceLength < SRC_SIZE ? sourceLength : SRC_SIZE - 1] = '\0';
    hkEmit = bufferEmit;
    hkReadModule = wasmReadModule;
    objectsInit(); /* heap was rewound: intern table/object list must not dangle */
    ObjFunction *fn = compile(srcBuf, sourceLength);
    outBuf[outLen] = '\0';
    if (fn == NULL) return 65;
    disassembleChunk(fn->chunk, "web");
    outBuf[outLen] = '\0';
    return 0;
}

#endif
