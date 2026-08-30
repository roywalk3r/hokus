/* HOKUS — freestanding Lox-class bytecode VM. MIT License; see LICENSE. */
#ifndef hokus_compiler_h
#define hokus_compiler_h

#include "value.h"

ObjFunction *compile(const char *source, int sourceLen);

/* `import "path";` resolves through the host: NULL (or a NULL hook) means the
   module can't be provided — the compiler reports it as a compile error.
   The host retains ownership of the returned source for the process life. */
typedef const char *(*HkReadModuleFn)(const char *path);
extern HkReadModuleFn hkReadModule;

#endif
