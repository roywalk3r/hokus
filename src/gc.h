/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#ifndef hokus_gc_h
#define hokus_gc_h

#include "vm.h"

/* Allocation-side hook: bumps the counter; collects when past threshold
   and a VM is mid-execution. Called from value.c's allocateObj. */
void gcOnAllocate(void);

/* Full mark-sweep pass. */
void gcCollect(VM *vm);

/* Free every remaining object (called from freeVM). */
void gcFreeAll(void);

#endif
