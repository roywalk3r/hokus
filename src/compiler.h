/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#ifndef hokus_compiler_h
#define hokus_compiler_h

#include "value.h"

ObjFunction *compile(const char *source, int sourceLen);

#endif
