#ifndef VM_COMPILER_H
#define VM_COMPILER_H
#include "core/ast.h"
#include "vm/bytecode.h"

XSProto *compile_program(Node *program);

/* Cross-file `use` loader sets this to 1 before compiling an imported
   file so top-level let/var/const land in globals (not just locals).
   Reset to 0 after. */
extern int g_compile_for_module;

#endif
