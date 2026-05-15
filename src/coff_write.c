// SPDX-License-Identifier: LGPL-2.1-or-later
// Write a COFF/PE relocatable object file (.o) from an ObjFile.
// Targets: Windows x86-64 (MinGW) and AArch64.
#ifdef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "obj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// COFF constants
// ---------------------------------------------------------------------------
#define IMAGE_FILE_MACHINE_AMD64  0x8664
#define IMAGE_FILE_MACHINE_ARM64  0xAA64
#define IMAGE_FILE_MACHINE_ARMNT  0x01C4

// Section characteristics
#define IMAGE_SCN_CNT_CODE               0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_MEM_EXECUTE            0x20000000
#define IMAGE_SCN_MEM_READ               0x40000000
#define IMAGE_SCN_MEM_WRITE              0x80000000
#define IMAGE_SCN_ALIGN_16BYTES          0x00500000

#define COFF_CHAR_TEXT  \
    (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_16BYTES)
#define COFF_CHAR_DATA  \
    (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_ALIGN_16BYTES)
#define COFF_CHAR_RDATA \
    (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_16BYTES)
#define COFF_CHAR_BSS   \
    (IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_ALIGN_16BYTES)

// x86-64 COFF relocation types
#define IMAGE_REL_AMD64_ADDR64  1
#define IMAGE_REL_AMD64_ADDR32  2
#define IMAGE_REL_AMD64_ADDR32NB 3
#define IMAGE_REL_AMD64_REL32   4
#define IMAGE_REL_AMD64_REL32_1 5
#define IMAGE_REL_AMD64_REL32_2 6
#define IMAGE_REL_AMD64_REL32_4 7
#define IMAGE_REL_AMD64_REL32_5 8
#define IMAGE_REL_AMD64_SECTION 10
#define IMAGE_REL_AMD64_SECREL  11
#define IMAGE_REL_AMD64_SECREL7 12

// AArch64 COFF relocation types
#define IMAGE_REL_ARM64_ADDR64           1
#define IMAGE_REL_ARM64_ADDR32           2
#define IMAGE_REL_ARM64_BRANCH26         3
#define IMAGE_REL_ARM64_PAGEBASE_REL21   4
#define IMAGE_REL_ARM64_PAGEOFFSET_12A   5
#define IMAGE_REL_ARM64_PAGEOFFSET_12L   6
#define IMAGE_REL_ARM64_SECREL           7
#define IMAGE_REL_ARM64_SECREL_LOW12A    8
#define IMAGE_REL_ARM64_SECREL_HIGH12A   9
#define IMAGE_REL_ARM64_SECREL_LOW12L   10
#define IMAGE_REL_ARM64_GOT_LD_PREL19   11
#define IMAGE_REL_ARM64_GOT_PAGE        12

// Symbol storage class
#define IMAGE_SYM_CLASS_EXTERNAL      2
#define IMAGE_SYM_CLASS_STATIC        3
#define IMAGE_SYM_CLASS_WEAK_EXTERNAL 104

// Symbol type
#define IMAGE_SYM_TYPE_FUNC           0x20

// ---------------------------------------------------------------------------
// Section descriptor
// ---------------------------------------------------------------------------
typedef struct {
    char            short_name[8]; // zero-padded 8-byte section name
    int             sec_id;        // SEC_TEXT / SEC_DATA / SEC_RODATA / SEC_BSS
    uint32_t        characteristics;
    size_t          raw_size;      // bytes in file
    size_t          virt_size;     // virtual size (.bss: bss_size, others: raw_size)
    ObjReloc       *relocs;
    int             reloc_count;
    // computed during layout:
    uint32_t        raw_data_ptr;
    uint32_t        reloc_ptr;
} CoffSec;

// ---------------------------------------------------------------------------
// String table builder
// ---------------------------------------------------------------------------
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} CStrtab;

static void cstrtab_init(CStrtab *t) {
    t->cap = 256;
    t->data = malloc(t->cap);
    t->len = 4; // reserve 4 bytes for the length prefix
    memset(t->data, 0, 4);
}

static uint32_t cstrtab_add(CStrtab *t, const char *s) {
    size_t n = strlen(s) + 1;
    if (t->len + n > t->cap) {
        while (t->cap < t->len + n)
            t->cap *= 2;
        t->data = realloc(t->data, t->cap);
    }
    uint32_t off = (uint32_t)t->len;
    memcpy(t->data + t->len, s, n);
    t->len += n;
    // update length prefix (little-endian)
    uint32_t total = (uint32_t)t->len;
    memcpy(t->data, &total, 4);
    return off;
}

// ---------------------------------------------------------------------------
// COFF symbol entry descriptor (used to build the symbol table in memory)
// ---------------------------------------------------------------------------
typedef struct {
    char     short_name[8];   // inline name (zero-padded)
    bool     long_name;       // true → short_name[4..7] = strtab offset
    uint32_t strtab_off;      // valid only if long_name
    uint32_t value;
    int16_t  section_number;
    uint16_t type;
    uint8_t  storage_class;
    uint8_t  num_aux;         // 0 or 1
    // aux section entry (valid only when num_aux > 0)
    uint32_t aux_length;
    uint16_t aux_num_relocs;
    uint16_t aux_num_linenums;
    uint16_t aux_number;
} CoffSymRec;

// ---------------------------------------------------------------------------
// Dynamic array of CoffSymRec
// ---------------------------------------------------------------------------
typedef struct {
    CoffSymRec *data;
    int         len;
    int         cap;
} SymArr;

static void symarr_push(SymArr *a, CoffSymRec s) {
    if (a->len == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->data = realloc(a->data, (size_t)a->cap * sizeof(CoffSymRec));
    }
    a->data[a->len++] = s;
}

// ---------------------------------------------------------------------------
// Low-level write helpers (little-endian)
// ---------------------------------------------------------------------------
static void w8(FILE *f, uint8_t v)  { fputc(v, f); }
static void w16(FILE *f, uint16_t v) {
    w8(f, (uint8_t)v);
    w8(f, (uint8_t)(v >> 8));
}
static void w32(FILE *f, uint32_t v) {
    w16(f, (uint16_t)v);
    w16(f, (uint16_t)(v >> 16));
}
static void wbuf(FILE *f, const void *buf, size_t n) { fwrite(buf, 1, n, f); }
static void wzeros(FILE *f, size_t n) {
    uint8_t z[64];
    memset(z, 0, sizeof(z));
    while (n >= 64) { fwrite(z, 1, 64, f); n -= 64; }
    if (n) fwrite(z, 1, n, f);
}

// ---------------------------------------------------------------------------
// Write a COFF symbol entry (18 bytes)
// ---------------------------------------------------------------------------
static void write_coff_sym(FILE *f, const CoffSymRec *s) {
    wbuf(f, s->short_name, 8);
    w32(f, s->value);
    w16(f, (uint16_t)s->section_number);
    w16(f, s->type);
    w8(f, s->storage_class);
    w8(f, s->num_aux);
}

// ---------------------------------------------------------------------------
// Write a COFF aux section symbol entry (18 bytes)
// ---------------------------------------------------------------------------
static void write_coff_aux_section(FILE *f, const CoffSymRec *s) {
    w32(f, s->aux_length);
    w16(f, s->aux_num_relocs);
    w16(f, s->aux_num_linenums);
    w32(f, 0); // checksum
    w16(f, s->aux_number);
    w8(f, 0);  // selection
    wzeros(f, 3); // reserved
}

// ---------------------------------------------------------------------------
// Map ELF relocation type → COFF relocation type
// ---------------------------------------------------------------------------
static uint16_t reloc_to_coff_x86_64(uint32_t elf_type) {
    switch (elf_type) {
    case R_X86_64_64:        return IMAGE_REL_AMD64_ADDR64;
    case R_X86_64_32:        return IMAGE_REL_AMD64_ADDR32;
    case R_X86_64_32S:       return IMAGE_REL_AMD64_ADDR32;
    case R_X86_64_PC32:      return IMAGE_REL_AMD64_REL32;
    case R_X86_64_PLT32:     return IMAGE_REL_AMD64_REL32;
    case R_X86_64_GOTPCREL:  return IMAGE_REL_AMD64_REL32;
    case R_X86_64_GOT32:     return IMAGE_REL_AMD64_ADDR32;
    case R_X86_64_PC64:      return IMAGE_REL_AMD64_ADDR64;
    default:                 return 0;
    }
}

static uint16_t reloc_to_coff_arm64(uint32_t elf_type) {
    switch (elf_type) {
    case R_AARCH64_ABS64:                  return IMAGE_REL_ARM64_ADDR64;
    case R_AARCH64_ABS32:                  return IMAGE_REL_ARM64_ADDR32;
    case R_AARCH64_JUMP26:                 return IMAGE_REL_ARM64_BRANCH26;
    case R_AARCH64_CALL26:                 return IMAGE_REL_ARM64_BRANCH26;
    case R_AARCH64_ADR_PREL_PG_HI21:       return IMAGE_REL_ARM64_PAGEBASE_REL21;
    case R_AARCH64_ADD_ABS_LO12_NC:        return IMAGE_REL_ARM64_PAGEOFFSET_12A;
    case R_AARCH64_LDST64_ABS_LO12_NC:     return IMAGE_REL_ARM64_PAGEOFFSET_12L;
    case R_AARCH64_LDST32_ABS_LO12_NC:     return IMAGE_REL_ARM64_PAGEOFFSET_12L;
    case R_AARCH64_LDST16_ABS_LO12_NC:     return IMAGE_REL_ARM64_PAGEOFFSET_12L;
    case R_AARCH64_LDST8_ABS_LO12_NC:      return IMAGE_REL_ARM64_PAGEOFFSET_12L;
    case R_AARCH64_ADR_GOT_PAGE:           return IMAGE_REL_ARM64_GOT_PAGE;
    case R_AARCH64_LD64_GOT_LO12_NC:       return IMAGE_REL_ARM64_GOT_LD_PREL19;
    default:                               return 0;
    }
}

// ---------------------------------------------------------------------------
// Determine the byte width of an ELF relocation addend for patching
// ---------------------------------------------------------------------------
static int addend_byte_width(uint32_t elf_type) {
    switch (elf_type) {
    case R_X86_64_64:
    case R_X86_64_PC64:
    case R_AARCH64_ABS64:
        return 8;
    default:
        return 4;
    }
}

// ---------------------------------------------------------------------------
// Patch relocation addends into a copy of the section data.
// COFF uses REL format (addend stored in-place), unlike ELF RELA.
// ---------------------------------------------------------------------------
static void patch_addends(uint8_t *buf, size_t len,
                          ObjReloc *relocs, int reloc_count) {
    for (int i = 0; i < reloc_count; i++) {
        ObjReloc *r = &relocs[i];
        // Skip PC-relative relocations: the in-place addend is 0 (placeholder)
        // and the linker will compute the correct displacement.
        if (r->type == R_X86_64_PC32 || r->type == R_X86_64_PLT32 ||
            r->type == R_X86_64_GOTPCREL || r->type == R_X86_64_PC64 ||
            r->type == R_AARCH64_CALL26 || r->type == R_AARCH64_JUMP26 ||
            r->type == R_AARCH64_ADR_PREL_PG_HI21)
            continue;
        uint64_t off = r->offset;
        int width = addend_byte_width(r->type);
        if (off + (size_t)width > len)
            continue;
        if (width == 8) {
            uint64_t v = (uint64_t)(int64_t)r->addend;
            memcpy(buf + off, &v, 8);
        } else {
            uint32_t v = (uint32_t)(int32_t)r->addend;
            memcpy(buf + off, &v, 4);
        }
    }
}

// ---------------------------------------------------------------------------
// Build the short name for a user symbol.
// If the name is ≤ 8 bytes, zero-pad into short_name[8].
// Otherwise set first 4 bytes = 0, next 4 bytes = strtab offset.
// ---------------------------------------------------------------------------
static void fill_short_name(char short_name[8], const char *name,
                            CStrtab *strtab, bool *long_name,
                            uint32_t *strtab_off) {
    size_t n = strlen(name);
    if (n <= 8) {
        memset(short_name, 0, 8);
        memcpy(short_name, name, n);
        *long_name = false;
        *strtab_off = 0;
    } else {
        memset(short_name, 0, 4);
        uint32_t off = cstrtab_add(strtab, name);
        *long_name = true;
        *strtab_off = off;
        memcpy(short_name + 4, &off, 4);
    }
}

// ---------------------------------------------------------------------------
// Main COFF writer
// ---------------------------------------------------------------------------
int coff_write(ObjFile *obj, const char *path) {
#ifdef ARCH_ARM64
    uint16_t machine = IMAGE_FILE_MACHINE_ARM64;
#elif defined(__aarch64__)
    uint16_t machine = IMAGE_FILE_MACHINE_ARM64;
#else
    uint16_t machine = IMAGE_FILE_MACHINE_AMD64;
#endif

    // -------------------------------------------------------------------
    // Enumerate sections
    // -------------------------------------------------------------------
    CoffSec sections[4];
    int num_sec = 0;

    // .text
    memset(sections[num_sec].short_name, 0, 8);
    memcpy(sections[num_sec].short_name, ".text", 5);
    sections[num_sec].sec_id = SEC_TEXT;
    sections[num_sec].characteristics = COFF_CHAR_TEXT;
    sections[num_sec].raw_size = obj->text.len;
    sections[num_sec].virt_size = obj->text.len;
    sections[num_sec].relocs = obj->text_relocs;
    sections[num_sec].reloc_count = obj->text_reloc_count;
    num_sec++;

    // .data
    if (obj->data.len > 0 || obj->data_reloc_count > 0) {
        memset(sections[num_sec].short_name, 0, 8);
        memcpy(sections[num_sec].short_name, ".data", 5);
        sections[num_sec].sec_id = SEC_DATA;
        sections[num_sec].characteristics = COFF_CHAR_DATA;
        sections[num_sec].raw_size = obj->data.len;
        sections[num_sec].virt_size = obj->data.len;
        sections[num_sec].relocs = obj->data_relocs;
        sections[num_sec].reloc_count = obj->data_reloc_count;
        num_sec++;
    }

    // .rdata
    if (obj->rodata.len > 0 || obj->rodata_reloc_count > 0) {
        memset(sections[num_sec].short_name, 0, 8);
        memcpy(sections[num_sec].short_name, ".rdata", 6);
        sections[num_sec].sec_id = SEC_RODATA;
        sections[num_sec].characteristics = COFF_CHAR_RDATA;
        sections[num_sec].raw_size = obj->rodata.len;
        sections[num_sec].virt_size = obj->rodata.len;
        sections[num_sec].relocs = obj->rodata_relocs;
        sections[num_sec].reloc_count = obj->rodata_reloc_count;
        num_sec++;
    }

    // .bss
    if (obj->bss_size > 0) {
        memset(sections[num_sec].short_name, 0, 8);
        memcpy(sections[num_sec].short_name, ".bss", 4);
        sections[num_sec].sec_id = SEC_BSS;
        sections[num_sec].characteristics = COFF_CHAR_BSS;
        sections[num_sec].raw_size = 0; // no data in object file
        sections[num_sec].virt_size = obj->bss_size;
        sections[num_sec].relocs = NULL;
        sections[num_sec].reloc_count = 0;
        num_sec++;
    }

    // Build SEC_* → COFF section index (1-based) map
    int coff_sec_idx[SEC_NUM];
    memset(coff_sec_idx, 0, sizeof(coff_sec_idx));
    for (int i = 0; i < num_sec; i++)
        coff_sec_idx[sections[i].sec_id] = i + 1;

    // -------------------------------------------------------------------
    // Build string table and symbol table
    // -------------------------------------------------------------------
    CStrtab strtab;
    cstrtab_init(&strtab);

    SymArr syms = {NULL, 0, 0};
    int *sym_map = calloc((size_t)(obj->sym_count + 1), sizeof(int));

    // Section symbols (each with 1 aux entry)
    for (int i = 0; i < num_sec; i++) {
        CoffSymRec ss = {0};
        memcpy(ss.short_name, sections[i].short_name, 8);
        ss.long_name = false;
        ss.section_number = (int16_t)(i + 1);
        ss.type = 0;
        ss.storage_class = IMAGE_SYM_CLASS_STATIC;
        ss.num_aux = 1;
        ss.aux_length = (uint32_t)sections[i].virt_size;
        ss.aux_num_relocs = (uint16_t)sections[i].reloc_count;
        ss.aux_num_linenums = 0;
        ss.aux_number = (uint16_t)(i + 1);
        symarr_push(&syms, ss);
    }

    // Local user symbols first
    for (int i = 0; i < obj->sym_count; i++) {
        ObjSym *os = &obj->syms[i];
        if (os->bind != SB_LOCAL)
            continue;
        sym_map[i] = syms.len;
        CoffSymRec es = {0};
        fill_short_name(es.short_name, os->name, &strtab,
                        &es.long_name, &es.strtab_off);
        es.value = (uint32_t)os->offset;
        es.section_number = (int16_t)coff_sec_idx[os->section];
        es.type = (os->type == ST_FUNC) ? IMAGE_SYM_TYPE_FUNC : 0;
        es.storage_class = IMAGE_SYM_CLASS_STATIC;
        symarr_push(&syms, es);
    }

    // Global/weak user symbols
    for (int i = 0; i < obj->sym_count; i++) {
        ObjSym *os = &obj->syms[i];
        if (os->bind == SB_LOCAL)
            continue;
        sym_map[i] = syms.len;
        CoffSymRec es = {0};
        fill_short_name(es.short_name, os->name, &strtab,
                        &es.long_name, &es.strtab_off);
        es.value = (uint32_t)os->offset;
        es.section_number = (int16_t)coff_sec_idx[os->section];
        es.type = (os->type == ST_FUNC) ? IMAGE_SYM_TYPE_FUNC : 0;
        es.storage_class = (os->bind == SB_WEAK) ? IMAGE_SYM_CLASS_WEAK_EXTERNAL
                                                 : IMAGE_SYM_CLASS_EXTERNAL;
        symarr_push(&syms, es);
    }

    // Build coff_sym_idx: syms.data index → COFF symbol table index (counting aux)
    int *coff_sym_idx = calloc((size_t)syms.len, sizeof(int));
    int ci = 0;
    for (int j = 0; j < syms.len; j++) {
        coff_sym_idx[j] = ci;
        ci += 1 + syms.data[j].num_aux;
    }

    // -------------------------------------------------------------------
    // Compute file layout
    // -------------------------------------------------------------------
    // Header (20) + section headers (40 * num_sec)
    uint32_t off = 20 + (uint32_t)num_sec * 40;

    // Section raw data pointers
    for (int i = 0; i < num_sec; i++) {
        sections[i].raw_data_ptr = off;
        off += (uint32_t)sections[i].raw_size;
    }

    // Relocation pointers
    for (int i = 0; i < num_sec; i++) {
        sections[i].reloc_ptr = off;
        off += (uint32_t)sections[i].reloc_count * 10; // 10 bytes per COFF reloc
    }

    // Symbol table (count includes aux entries)
    int total_sym_count = syms.len;
    for (int i = 0; i < syms.len; i++)
        total_sym_count += syms.data[i].num_aux;

    uint32_t symtab_off = off;
    uint32_t symtab_size = (uint32_t)total_sym_count * 18;

    // -------------------------------------------------------------------
    // Copy section data and patch addends
    // -------------------------------------------------------------------
    uint8_t *text_copy = NULL;
    uint8_t *data_copy = NULL;
    uint8_t *rodata_copy = NULL;
    int text_idx = -1, data_idx = -1, rodata_idx = -1;

    // Find section indices
    for (int i = 0; i < num_sec; i++) {
        if (sections[i].sec_id == SEC_TEXT)
            text_idx = i;
        else if (sections[i].sec_id == SEC_DATA)
            data_idx = i;
        else if (sections[i].sec_id == SEC_RODATA)
            rodata_idx = i;
    }

    if (text_idx >= 0 && sections[text_idx].raw_size > 0) {
        text_copy = malloc(sections[text_idx].raw_size);
        memcpy(text_copy, obj->text.data, sections[text_idx].raw_size);
        patch_addends(text_copy, sections[text_idx].raw_size,
                      sections[text_idx].relocs, sections[text_idx].reloc_count);
    }
    if (data_idx >= 0 && sections[data_idx].raw_size > 0) {
        data_copy = malloc(sections[data_idx].raw_size);
        memcpy(data_copy, obj->data.data, sections[data_idx].raw_size);
        patch_addends(data_copy, sections[data_idx].raw_size,
                      sections[data_idx].relocs, sections[data_idx].reloc_count);
    }
    if (rodata_idx >= 0 && sections[rodata_idx].raw_size > 0) {
        rodata_copy = malloc(sections[rodata_idx].raw_size);
        memcpy(rodata_copy, obj->rodata.data, sections[rodata_idx].raw_size);
        patch_addends(rodata_copy, sections[rodata_idx].raw_size,
                      sections[rodata_idx].relocs, sections[rodata_idx].reloc_count);
    }

    // -------------------------------------------------------------------
    // Write file
    // -------------------------------------------------------------------
    FILE *f = fopen(path, "wb");
    if (!f) {
    free(sym_map);
    free(coff_sym_idx);
        free(syms.data);
        free(strtab.data);
        free(text_copy);
        free(data_copy);
        free(rodata_copy);
        return -1;
    }

    // --- File header (20 bytes) ---
    w16(f, machine);
    w16(f, (uint16_t)num_sec);
    w32(f, 0);            // timestamp
    w32(f, symtab_off);   // symtab_offset
    w32(f, (uint32_t)total_sym_count); // num_symbols (includes aux entries)
    w16(f, 0);            // opt_hdr_size (0 for object files)
    w16(f, 0);            // characteristics

    // --- Section headers (40 bytes each) ---
    for (int i = 0; i < num_sec; i++) {
        wbuf(f, sections[i].short_name, 8);
        w32(f, (uint32_t)sections[i].virt_size);
        w32(f, 0); // virtual_addr
        w32(f, (uint32_t)sections[i].raw_size);
        w32(f, sections[i].raw_data_ptr);
        w32(f, sections[i].reloc_ptr);
        w32(f, 0); // linenum_ptr
        w16(f, (uint16_t)sections[i].reloc_count);
        w16(f, 0); // num_linenums
        w32(f, sections[i].characteristics);
    }

    // --- Section raw data ---
    for (int i = 0; i < num_sec; i++) {
        size_t sz = sections[i].raw_size;
        if (sz == 0)
            continue;
        if (sections[i].sec_id == SEC_TEXT)
            wbuf(f, text_copy, sz);
        else if (sections[i].sec_id == SEC_DATA)
            wbuf(f, data_copy, sz);
        else if (sections[i].sec_id == SEC_RODATA)
            wbuf(f, rodata_copy, sz);
        // .bss has raw_size 0, handled above
    }

    // --- Relocations ---
    for (int i = 0; i < num_sec; i++) {
        for (int j = 0; j < sections[i].reloc_count; j++) {
            ObjReloc *r = &sections[i].relocs[j];
            int sym_idx = (r->sym_idx >= 0 && r->sym_idx < obj->sym_count)
                ? coff_sym_idx[sym_map[r->sym_idx]] : 0;
            uint16_t coff_type;
#ifdef ARCH_ARM64
            coff_type = reloc_to_coff_arm64(r->type);
#elif defined(__aarch64__)
            coff_type = reloc_to_coff_arm64(r->type);
#else
            coff_type = reloc_to_coff_x86_64(r->type);
#endif
            w32(f, (uint32_t)r->offset);
            w32(f, (uint32_t)sym_idx);
            w16(f, coff_type);
        }
    }

    // --- Symbol table (aux entries immediately follow their owning symbol) ---
    for (int i = 0; i < syms.len; i++) {
        write_coff_sym(f, &syms.data[i]);
        if (syms.data[i].num_aux > 0)
            write_coff_aux_section(f, &syms.data[i]);
    }

    // --- String table ---
    wbuf(f, strtab.data, strtab.len);

    fclose(f);

    free(sym_map);
    free(coff_sym_idx);
    free(syms.data);
    free(strtab.data);
    free(text_copy);
    free(data_copy);
    free(rodata_copy);
    return 0;
}
#endif /* _WIN32 */
