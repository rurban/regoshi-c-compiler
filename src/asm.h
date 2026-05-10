// SPDX-License-Identifier: LGPL-2.1-or-later
// Built-in assembler: convert rcc-generated assembly text to an object file.
// Supports the exact subset of ARM64 and x86-64 that rcc emits.
#ifndef ASM_H
#define ASM_H

#include "obj.h"

// State for line-by-line assembly (usable from codegen.c)
typedef struct AsmState AsmState;
struct AsmState {
    ObjFile *obj;
    int cur_sec;
    int lineno;
    const char *filename;

    struct Fixup {
        size_t patch_off;
        int section;
        char label[128];
        int kind;
        int64_t addend;
    } fixups[512];
    int nfixups;

    struct LocalSym {
        char name[128];
        int section;
        size_t offset;
    } locals[2048];
    int nlocals;
};

// Initialize AsmState for line-by-line assembly into an ObjFile
void asm_init(AsmState *as, ObjFile *obj, const char *filename);

// Assemble a single line of assembly text into the ObjFile.
// Returns 0 on success, -1 on error.
int asm_assemble_line(AsmState *as, const char *line);

// Assemble an already-trimmed line (no leading/trailing whitespace)
int asm_assemble_trimmed_line(AsmState *as, const char *trimmed);

// Resolve pending fixups after all lines have been assembled.
// Returns 0 on success, -1 on error (unresolved fixups).
int asm_finish(AsmState *as);

// Assemble the assembly text file at `asm_path` and write an ELF or Mach-O
// object file to `obj_path`. Returns 0 on success, -1 on error.
int assemble_file(const char *asm_path, const char *obj_path);

#endif // ASM_H
