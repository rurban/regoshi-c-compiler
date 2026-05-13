// SPDX-License-Identifier: LGPL-2.1-or-later
// Write an ELF64 relocatable object file (.o) from an ObjFile.
// Targets: Linux x86-64 and AArch64.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "obj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// ELF64 constants and structures
// ---------------------------------------------------------------------------
#define EI_NIDENT    16
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1
#define ELFOSABI_NONE 0
#define ET_REL        1
#define EM_X86_64    62
#define EM_AARCH64  183

#define SHT_NULL      0
#define SHT_PROGBITS  1
#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_RELA      4
#define SHT_NOBITS    8

#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_MERGE     0x10
#define SHF_INFO_LINK 0x40

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STV_DEFAULT 0
#define SHN_UNDEF   0

#define ELF64_ST_INFO(b, t) (uint8_t)(((b)<<4)|((t)&0xf))
#define ELF64_R_INFO(s, t)  (((uint64_t)(s)<<32)|((uint64_t)(t)&0xffffffffULL))

// ---------------------------------------------------------------------------
// String table builder
// ---------------------------------------------------------------------------
typedef struct {
    char *data;
    size_t len, cap;
} Strtab;

static void strtab_init(Strtab *t) {
    t->cap = 256;
    t->data = malloc(t->cap);
    t->len = 0;
    t->data[t->len++] = '\0';
}

static uint32_t strtab_add(Strtab *t, const char *s) {
    size_t n = strlen(s) + 1;
    if (t->len + n > t->cap) {
        while (t->cap < t->len + n) t->cap *= 2;
        t->data = realloc(t->data, t->cap);
    }
    uint32_t idx = (uint32_t)t->len;
    memcpy(t->data + t->len, s, n);
    t->len += n;
    return idx;
}

// ---------------------------------------------------------------------------
// Low-level write helpers (always little-endian)
// ---------------------------------------------------------------------------
static void w8(FILE *f, uint8_t v) { fputc(v, f); }
static void w16(FILE *f, uint16_t v) {
    w8(f, (uint8_t)v);
    w8(f, v >> 8);
}
static void w32(FILE *f, uint32_t v) {
    w16(f, (uint16_t)v);
    w16(f, (uint16_t)(v >> 16));
}
static void w64(FILE *f, uint64_t v) {
    w32(f, (uint32_t)v);
    w32(f, (uint32_t)(v >> 32));
}
static void wi64(FILE *f, int64_t v) { w64(f, (uint64_t)v); }
static void wbuf(FILE *f, const void *buf, size_t n) { fwrite(buf, 1, n, f); }
static void wzeros(FILE *f, uint64_t n) {
    uint8_t z[64];
    memset(z, 0, sizeof(z));
    while (n >= 64) {
        fwrite(z, 1, 64, f);
        n -= 64;
    }
    if (n) fwrite(z, 1, (size_t)n, f);
}

static void write_ehdr(FILE *f, uint8_t em,
                       uint64_t shoff, uint16_t shnum, uint16_t shstrndx) {
    uint8_t ident[EI_NIDENT] = {
        0x7f, 'E', 'L', 'F', ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_NONE,
        0, 0, 0, 0, 0, 0, 0, 0};
    wbuf(f, ident, EI_NIDENT);
    w16(f, ET_REL);
    w16(f, em);
    w32(f, EV_CURRENT);
    w64(f, 0); // e_entry
    w64(f, 0); // e_phoff
    w64(f, shoff);
    w32(f, 0); // e_flags
    w16(f, 64); // e_ehsize
    w16(f, 0); // e_phentsize
    w16(f, 0); // e_phnum
    w16(f, 64); // e_shentsize
    w16(f, shnum);
    w16(f, shstrndx);
}

static void write_shdr(FILE *f, uint32_t name, uint32_t type, uint64_t flags,
                       uint64_t offset, uint64_t size, uint32_t link,
                       uint32_t info, uint64_t align, uint64_t entsize) {
    w32(f, name);
    w32(f, type);
    w64(f, flags);
    w64(f, 0); // sh_addr=0
    w64(f, offset);
    w64(f, size);
    w32(f, link);
    w32(f, info);
    w64(f, align);
    w64(f, entsize);
}

static void write_sym(FILE *f, uint32_t name, uint8_t info, uint8_t other,
                      uint16_t shndx, uint64_t value, uint64_t size) {
    w32(f, name);
    w8(f, info);
    w8(f, other);
    w16(f, shndx);
    w64(f, value);
    w64(f, size);
}

static uint64_t align16(uint64_t x) { return (x + 15) & ~(uint64_t)15; }

// ---------------------------------------------------------------------------
// ELF symbol array builder (locals first, then globals/weaks)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} ESym;

typedef struct {
    ESym *data;
    int len, cap;
} ESymArr;

static void esym_push(ESymArr *a, ESym s) {
    if (a->len == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->data = realloc(a->data, (size_t)a->cap * sizeof(ESym));
    }
    a->data[a->len++] = s;
}

// ---------------------------------------------------------------------------
// Main ELF writer
// ---------------------------------------------------------------------------
int elf_write(ObjFile *obj, const char *path) {
#ifdef __aarch64__
    uint8_t em = EM_AARCH64;
#else
    uint8_t em = EM_X86_64;
#endif

    Strtab symstrtab, shstrtab;
    strtab_init(&symstrtab);
    strtab_init(&shstrtab);

    // Section name strings
    uint32_t shn_empty = strtab_add(&shstrtab, "");
    uint32_t shn_text = strtab_add(&shstrtab, ".text");
    uint32_t shn_data = strtab_add(&shstrtab, ".data");
    uint32_t shn_bss = strtab_add(&shstrtab, ".bss");
    uint32_t shn_rodata = strtab_add(&shstrtab, ".rodata");
    uint32_t shn_note = strtab_add(&shstrtab, ".note.GNU-stack");
    uint32_t shn_rela_txt = strtab_add(&shstrtab, ".rela.text");
    uint32_t shn_rela_dat = strtab_add(&shstrtab, ".rela.data");
    uint32_t shn_rela_rod = strtab_add(&shstrtab, ".rela.rodata");
    uint32_t shn_symtab = strtab_add(&shstrtab, ".symtab");
    uint32_t shn_strtab = strtab_add(&shstrtab, ".strtab");
    uint32_t shn_shstrtab = strtab_add(&shstrtab, ".shstrtab");
    (void)shn_empty;

    // -----------------------------------------------------------------------
    // Build ELF symbol table (locals first, then globals)
    // -----------------------------------------------------------------------
    int *sym_map = calloc((size_t)(obj->sym_count + 1), sizeof(int));
    ESymArr ea = {NULL, 0, 0};

    // Index 0: null symbol
    ESym null_sym = {0};
    esym_push(&ea, null_sym);

    // Section symbols
    ESym ssym = {0};
    ssym.info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
    ssym.shndx = 1;
    esym_push(&ea, ssym); // .text
    ssym.shndx = 2;
    esym_push(&ea, ssym); // .data
    ssym.shndx = 3;
    esym_push(&ea, ssym); // .bss
    ssym.shndx = 4;
    esym_push(&ea, ssym); // .rodata

    // Local user symbols
    for (int i = 0; i < obj->sym_count; i++) {
        ObjSym *os = &obj->syms[i];
        if (os->bind != SB_LOCAL) continue;
        sym_map[i] = ea.len;
        ESym es = {0};
        es.name = strtab_add(&symstrtab, os->name);
        es.info = ELF64_ST_INFO(STB_LOCAL,
                                os->type == ST_FUNC ? STT_FUNC : os->type == ST_OBJECT ? STT_OBJECT
                                                                                       : STT_NOTYPE);
        es.shndx = os->section == SEC_TEXT ? 1 : os->section == SEC_DATA ? 2
            : os->section == SEC_BSS                                     ? 3
            : os->section == SEC_RODATA                                  ? 4
                                                                         : SHN_UNDEF;
        es.value = os->offset;
        es.size = os->size;
        esym_push(&ea, es);
    }
    int first_global = ea.len;

    // Global/weak user symbols
    for (int i = 0; i < obj->sym_count; i++) {
        ObjSym *os = &obj->syms[i];
        if (os->bind == SB_LOCAL) continue;
        sym_map[i] = ea.len;
        ESym es = {0};
        es.name = strtab_add(&symstrtab, os->name);
        uint8_t bind = os->bind == SB_WEAK ? STB_WEAK : STB_GLOBAL;
        es.info = ELF64_ST_INFO(bind,
                                os->type == ST_FUNC ? STT_FUNC : os->type == ST_OBJECT ? STT_OBJECT
                                                                                       : STT_NOTYPE);
        es.shndx = os->section == SEC_TEXT ? 1 : os->section == SEC_DATA ? 2
            : os->section == SEC_BSS                                     ? 3
            : os->section == SEC_RODATA                                  ? 4
                                                                         : SHN_UNDEF;
        es.value = os->offset;
        es.size = os->size;
        esym_push(&ea, es);
    }

    // -----------------------------------------------------------------------
    // Fixed section layout (section indices 0..5 always present)
    // -----------------------------------------------------------------------
    // 0=NULL, 1=.text, 2=.data, 3=.bss, 4=.rodata, 5=.note.GNU-stack
    // then optional .rela.text, .rela.data, .rela.rodata
    // then .symtab, .strtab, .shstrtab
    bool has_rela_text = obj->text_reloc_count > 0;
    bool has_rela_data = obj->data_reloc_count > 0;
    bool has_rela_rodata = obj->rodata_reloc_count > 0;

    int shidx = 6;
    int sh_rela_text_idx = has_rela_text ? shidx++ : -1;
    int sh_rela_data_idx = has_rela_data ? shidx++ : -1;
    int sh_rela_rodata_idx = has_rela_rodata ? shidx++ : -1;
    int sh_symtab_idx = shidx++;
    int sh_strtab_idx = shidx++;
    int sh_shstrtab_idx = shidx++;
    int nshdr = shidx;
    (void)sh_rela_text_idx;
    (void)sh_rela_data_idx;
    (void)sh_rela_rodata_idx;

    // -----------------------------------------------------------------------
    // File layout
    // -----------------------------------------------------------------------
    uint64_t text_off = align16(64); // 64 = sizeof ELF header
    uint64_t text_size = obj->text.len;
    uint64_t data_off = align16(text_off + text_size);
    uint64_t data_size = obj->data.len;
    uint64_t rodata_off = align16(data_off + data_size);
    uint64_t rodata_size = obj->rodata.len;
    uint64_t note_off = align16(rodata_off + rodata_size);

    uint64_t rela_txt_off = align16(note_off); // .note is empty
    uint64_t rela_txt_size = (uint64_t)obj->text_reloc_count * 24;
    uint64_t rela_dat_off = align16(rela_txt_off + rela_txt_size);
    uint64_t rela_dat_size = (uint64_t)obj->data_reloc_count * 24;
    uint64_t rela_rod_off = align16(rela_dat_off + rela_dat_size);
    uint64_t rela_rod_size = (uint64_t)obj->rodata_reloc_count * 24;

    uint64_t symtab_off = align16(rela_rod_off + rela_rod_size);
    uint64_t symtab_size = (uint64_t)ea.len * 24;
    uint64_t strtab_off = align16(symtab_off + symtab_size);
    uint64_t strtab_size = symstrtab.len;
    uint64_t shstrtab_off = align16(strtab_off + strtab_size);
    uint64_t shstrtab_size = shstrtab.len;
    uint64_t shoff = align16(shstrtab_off + shstrtab_size);

    // -----------------------------------------------------------------------
    // Write file
    // -----------------------------------------------------------------------
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(sym_map);
        free(ea.data);
        free(symstrtab.data);
        free(shstrtab.data);
        return -1;
    }

    write_ehdr(f, em, shoff, (uint16_t)nshdr, (uint16_t)sh_shstrtab_idx);

    wzeros(f, text_off - 64);
    if (text_size) wbuf(f, obj->text.data, text_size);
    wzeros(f, data_off - (text_off + text_size));
    if (data_size) wbuf(f, obj->data.data, data_size);
    wzeros(f, rodata_off - (data_off + data_size));
    if (rodata_size) wbuf(f, obj->rodata.data, rodata_size);
    wzeros(f, rela_txt_off - (rodata_off + rodata_size));

    for (int i = 0; i < obj->text_reloc_count; i++) {
        ObjReloc *r = &obj->text_relocs[i];
        int es = r->sym_idx >= 0 ? sym_map[r->sym_idx] : 0;
        w64(f, r->offset);
        w64(f, ELF64_R_INFO((uint32_t)es, r->type));
        wi64(f, r->addend);
    }
    wzeros(f, rela_dat_off - (rela_txt_off + rela_txt_size));

    for (int i = 0; i < obj->data_reloc_count; i++) {
        ObjReloc *r = &obj->data_relocs[i];
        int es = r->sym_idx >= 0 ? sym_map[r->sym_idx] : 0;
        w64(f, r->offset);
        w64(f, ELF64_R_INFO((uint32_t)es, r->type));
        wi64(f, r->addend);
    }
    wzeros(f, rela_rod_off - (rela_dat_off + rela_dat_size));

    for (int i = 0; i < obj->rodata_reloc_count; i++) {
        ObjReloc *r = &obj->rodata_relocs[i];
        int es = r->sym_idx >= 0 ? sym_map[r->sym_idx] : 0;
        w64(f, r->offset);
        w64(f, ELF64_R_INFO((uint32_t)es, r->type));
        wi64(f, r->addend);
    }
    wzeros(f, symtab_off - (rela_rod_off + rela_rod_size));

    for (int i = 0; i < ea.len; i++)
        write_sym(f, ea.data[i].name, ea.data[i].info, ea.data[i].other,
                  ea.data[i].shndx, ea.data[i].value, ea.data[i].size);
    wzeros(f, strtab_off - (symtab_off + symtab_size));

    wbuf(f, symstrtab.data, strtab_size);
    wzeros(f, shstrtab_off - (strtab_off + strtab_size));

    wbuf(f, shstrtab.data, shstrtab_size);
    wzeros(f, shoff - (shstrtab_off + shstrtab_size));

    // Section headers
    write_shdr(f, 0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0);

    write_shdr(f, shn_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
               text_off, text_size, 0, 0, 16, 0);
    write_shdr(f, shn_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
               data_off, data_size, 0, 0, 8, 0);
    write_shdr(f, shn_bss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE,
               data_off + data_size, obj->bss_size, 0, 0, 8, 0);
    write_shdr(f, shn_rodata, SHT_PROGBITS, SHF_ALLOC,
               rodata_off, rodata_size, 0, 0, 1, 0);
    write_shdr(f, shn_note, SHT_PROGBITS, 0, note_off, 0, 0, 0, 1, 0);

    if (has_rela_text)
        write_shdr(f, shn_rela_txt, SHT_RELA, SHF_INFO_LINK,
                   rela_txt_off, rela_txt_size,
                   (uint32_t)sh_symtab_idx, 1 /* .text */, 8, 24);
    if (has_rela_data)
        write_shdr(f, shn_rela_dat, SHT_RELA, SHF_INFO_LINK,
                   rela_dat_off, rela_dat_size,
                   (uint32_t)sh_symtab_idx, 2 /* .data */, 8, 24);
    if (has_rela_rodata)
        write_shdr(f, shn_rela_rod, SHT_RELA, SHF_INFO_LINK,
                   rela_rod_off, rela_rod_size,
                   (uint32_t)sh_symtab_idx, 4 /* .rodata */, 8, 24);

    write_shdr(f, shn_symtab, SHT_SYMTAB, 0,
               symtab_off, symtab_size,
               (uint32_t)sh_strtab_idx, (uint32_t)first_global, 8, 24);
    write_shdr(f, shn_strtab, SHT_STRTAB, 0,
               strtab_off, strtab_size, 0, 0, 1, 0);
    write_shdr(f, shn_shstrtab, SHT_STRTAB, 0,
               shstrtab_off, shstrtab_size, 0, 0, 1, 0);

    fclose(f);
    free(sym_map);
    free(ea.data);
    free(symstrtab.data);
    free(shstrtab.data);
    return 0;
}
