/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#ifndef hokus_image_h
#define hokus_image_h

#ifndef HK_WASM

#include <stdio.h>

#include "chunk.h"
#include "value.h"

void writeImage(ObjFunction *fn, FILE *out);
ObjFunction *readImage(FILE *in); /* NULL on bad magic or truncated stream */

#endif /* HK_WASM */
#endif /* hokus_image_h */
