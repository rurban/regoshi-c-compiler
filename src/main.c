// SPDX-License-Identifier: LGPL-2.1-or-later
// Derived from chibicc by Rui Ueyama.
#include "rcc.h"
#include "asm.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#define _getpid getpid
#endif
#include <sys/stat.h>
#include <time.h>

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}
#ifndef GCC
#define GCC "gcc"
#endif
#ifndef LD
#define LD "ld"
#endif

void add_define(char *def);
void add_undef(char *name);
void dump_ast(Program *prog);

typedef struct OutPath OutPath;
struct OutPath {
    OutPath *next;
    char *path;
};
static OutPath *out_paths;

OutPath *reverse(OutPath *head) {
    OutPath *prev = NULL;
    OutPath *curr = head;

    while (curr) {
        OutPath *next = curr->next;
        curr->next = prev;
        prev = curr;
        curr = next;
    }
    return prev;
}

// Returns the contents of a given file.
static char *read_file(char *path) {
    bool is_stdin = strcmp(path, "-") == 0;
    FILE *fp = is_stdin ? stdin : fopen(path, "r");
    if (!fp)
        error("cannot open %s: %m", path);

    int filemax = 10 * 1024 * 1024;
    char *buf = arena_alloc(filemax);
    size_t size = fread(buf, 1, filemax - 2, fp);
    if (!feof(fp)) {
        error("%s: file too large", path);
    }

    if (size == 0 || buf[size - 1] != '\n') {
        buf[size++] = '\n';
    }
    if (!is_stdin)
        fclose(fp);
    buf[size] = '\0';
    return buf;
}

#ifndef MACHINE
#define MACHINE "unknown"
#endif

void help(void) {
    printf("rcc %s %s - Copyright 2026 Hosokawa-t and Reini Urban\n", VERSION, MACHINE);
    printf("Licensed under the GNU Lesser General Public License v2.1 or later\n");
    printf("rcc [options...] [-o outfile] [-c] infile(s)...\n");
    printf("Options:\n"
           "-I path             add include path\n"
           "-Dname[=val]        define a macro\n"
           "-Uname              undefine a macro\n"
           "-E                  preprocessor-only\n"
           "-S                  assemble-only\n"
           "-c                  compile-only\n"
           "-o file             set output filename\n"
           "-O0                 disable peephole optimizer\n"
           "-O1                 enable peephole + CTFE optimizations\n"
           "-g                  emit DWARF line-number debug info\n"
           "-W                  enable more compiler warnings\n"
           "-Lpath              add linker path\n"
           "-lname              add lib\n"
           "-pthread            link with pthreads library\n"
           "-shared             create shared library\n"
           "-static             link statically\n"
           "-Wl,<opt>           pass option to linker\n"
           "-mms-bitfields      use MSVC bitfield layout by default\n"
           "-mno-ms-bitfields   use GCC bitfield layout by default\n"
           "-pie|-fPIE|-fpie    generate position-independent executable\n"
           "-fPIC|-fpic         generate position-independent code\n"
           "-time               print timing for each compilation substep\n"
           "-###                dry-run (print commands, don't execute)\n"
           "-dM                 dump all macro definitions (use with -E)\n"
           "-fdump-ast          dump AST for debugging\n"
           "-print-search-dirs  print install, include and library paths\n"
           "--help\n"
           "--version\n");
}

bool opt_O0 = false;
bool opt_O1 = false;
bool opt_W = false;
bool opt_dryrun = false;
bool opt_dM = false;
bool opt_fdump_ast = false;
bool opt_g = false;
bool opt_pie = false;
bool opt_pic = false;
bool opt_time = false;
bool opt_ms_bitfields =
#ifdef _WIN32
    true;
#else
    false;
#endif
;

bool sse42_available = false;

int main(int argc, char **argv) {
#ifdef __x86_64__
    // SSE4.2 runtime detection (x86_64 only)
    sse42_available = __builtin_cpu_supports("sse4.2");
#elif defined(__aarch64__)
    // ARM64 host — no SSE4.2, native ARM64 target is implicit
#elif !defined(ARCH_ARM64)
    fprintf(stderr, "rcc: unsupported host architecture\n");
    return 1;
#endif

    init_builtins();
    char *out_path =
#ifdef _WIN32
        "a.exe"
#else
        "a.out"
#endif
        ;
    char *in_path = NULL;
    bool first_input = true;
    bool opt_S = false;
    bool opt_c = false;
    bool opt_E = false;
    bool opt_o = false;
    char libs[512] =
#ifdef _WIN32
        " -lm"
#else
        ""
#endif
        ;
    int libs_len = strlen(libs);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            help();
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            printf("rcc %s %s\n", VERSION, MACHINE);
            return 0;
        }
        if (!strcmp(argv[i], "-print-search-dirs")) {
            print_search_dirs(GCC);
            return 0;
        }
        if (!strcmp(argv[i], "-S")) {
            opt_S = true;
        } else if (!strcmp(argv[i], "-c")) {
            opt_c = true;
        } else if (!strcmp(argv[i], "-E")) {
            opt_E = true;
        } else if (!strcmp(argv[i], "-O0")) {
            opt_O0 = true;
        } else if (!strcmp(argv[i], "-O1")) {
            opt_O1 = true;
        } else if (!strcmp(argv[i], "-W")) {
            opt_W = true;
        } else if (!strcmp(argv[i], "-###")) {
            opt_dryrun = true;
        } else if (!strcmp(argv[i], "-dM")) {
            opt_dM = true;
        } else if (!strcmp(argv[i], "-fdump-ast")) {
            opt_fdump_ast = true;
        } else if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "-g1") ||
                   !strcmp(argv[i], "-g2") || !strcmp(argv[i], "-g3")) {
            opt_g = true;
        } else if (!strcmp(argv[i], "-g0")) {
            opt_g = false;
        } else if (!strcmp(argv[i], "-mms-bitfields")) {
            opt_ms_bitfields = true;
        } else if (!strcmp(argv[i], "-mno-ms-bitfields")) {
            opt_ms_bitfields = false;
        } else if (!strcmp(argv[i], "-pie") || !strcmp(argv[i], "-fPIE") ||
                   !strcmp(argv[i], "-fpie")) {
            opt_pie = true;
        } else if (!strcmp(argv[i], "-fPIC") || !strcmp(argv[i], "-fpic")) {
            opt_pic = true;
        } else if (!strcmp(argv[i], "-time")) {
            opt_time = true;
        } else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) {
                fprintf(stderr, "error: missing argument for -o\n");
                return 1;
            }
            out_path = argv[i];
            opt_o = true;
        } else if (!strcmp(argv[i], "-pthread")) {
            add_define("_REENTRANT");
            int n = snprintf(libs + libs_len, sizeof(libs) - libs_len, " %s", argv[i]);
            if (n > 0 && libs_len + n < (int)sizeof(libs))
                libs_len += n;
        } else if (!strncmp(argv[i], "-l", 2) || !strncmp(argv[i], "-L", 2) ||
                   !strcmp(argv[i], "-shared") || !strcmp(argv[i], "-static") ||
                   !strncmp(argv[i], "-Wl,", 4)) {
            int n = snprintf(libs + libs_len, sizeof(libs) - libs_len, " %s", argv[i]);
            if (n > 0 && libs_len + n < (int)sizeof(libs))
                libs_len += n;
        } else if (!strncmp(argv[i], "-D", 2)) {
            char *def = argv[i] + 2;
            if (*def == '\0') {
                if (++i >= argc) {
                    fprintf(stderr, "error: missing argument for -D\n");
                    return 1;
                }
                def = argv[i];
            }
            add_define(def);
        } else if (!strncmp(argv[i], "-U", 2)) {
            char *name = argv[i] + 2;
            if (*name == '\0') {
                if (++i >= argc) {
                    fprintf(stderr, "error: missing argument for -U\n");
                    return 1;
                }
                name = argv[i];
            }
            add_undef(name);
        } else if (!strncmp(argv[i], "-I", 2)) {
            char *path = argv[i] + 2;
            if (*path == '\0') {
                if (++i >= argc) {
                    fprintf(stderr, "error: missing argument for -I\n");
                    return 1;
                }
                path = argv[i];
            }
            add_include_path(path);
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "rcc: warning: ignored unknown option %s\n", argv[i]);
        } else {
            in_path = argv[i];

            char *asm_path = opt_S
                ? opt_o ? out_path : format("%s.s", path_basename(in_path))
                : format("rcc_tmp_%d_%d_%s.s", _getpid(), i, path_basename(in_path));

            // Tokenize and Parse
            char *contents = read_file(in_path);

            // Always preprocess - opt_E just outputs preprocessed result
            uint64_t t0 = opt_time ? now_us() : 0;
            char *preprocessed = preprocess(in_path, contents);
            if (opt_time)
                fprintf(stderr, "  preprocess  %s: %6lu us\n", in_path,
                        now_us() - t0);

            if (opt_E) {
                printf("%s", preprocessed);
                continue;
            }

            t0 = opt_time ? now_us() : 0;
            Token *tok = tokenize(in_path, preprocessed);
            if (opt_time)
                fprintf(stderr, "  lex         %s: %6lu us\n", in_path,
                        now_us() - t0);

            t0 = opt_time ? now_us() : 0;
            Program *prog = parse(tok);
            prog->in_path = in_path;
            if (opt_time)
                fprintf(stderr, "  parse       %s: %6lu us\n", in_path,
                        now_us() - t0);

            if (opt_fdump_ast)
                dump_ast(prog);

            // Type system / Semantic checks
            t0 = opt_time ? now_us() : 0;
            for (TLItem *item = prog->items; item; item = item->next) {
                if (item->kind != TL_FUNC)
                    continue;
                for (Node *n = item->fn->body; n; n = n->next) {
                    add_type(n);
                }
            }
            if (opt_time)
                fprintf(stderr, "  typecheck   %s: %6lu us\n", in_path,
                        now_us() - t0);

            // CTFE runs only with -O1; peephole skipped with -O0.
            if (opt_O1) {
                t0 = opt_time ? now_us() : 0;
                optimize(prog);
                if (opt_time)
                    fprintf(stderr, "  opt(CTFE)   %s: %6lu us\n", in_path,
                            now_us() - t0);
            }

            if (!opt_dryrun) {
                // Redirect stdout to our assembly file (append for multi-file)
                if (!freopen(asm_path, first_input ? "w" : "a", stdout)) {
                    fprintf(stderr, "rcc: error: cannot open output file %s\n", asm_path);
                    return 1;
                }
                first_input = false;
                // Code generation (prints assembly to stdout, which is now asm_path)
                time_peep_us = 0;
                t0 = opt_time ? now_us() : 0;
                codegen(prog);
                if (opt_time) {
                    uint64_t cg_total = now_us() - t0;
                    fprintf(stderr, "  codegen     %s: %6lu us\n", in_path,
                            cg_total - time_peep_us);
                    if (!opt_O0)
                        fprintf(stderr, "  peephole    %s: %6lu us\n", in_path,
                                time_peep_us);
                }
                fflush(stdout);
                // Restore stdout to console if we want to print further, but we are done.
                fclose(stdout);
            } else {
                first_input = false;
            }

            if (!opt_S && !opt_dryrun) {
                OutPath *p = arena_alloc(sizeof(OutPath));
                p->path = asm_path;
                p->next = out_paths;
                out_paths = p;
            }
        }
    }

    if (!in_path) {
        fprintf(stderr, "rcc: fatal error: no input files\n");
        return 1;
    }

    // Assemble / Link if not just compiling to assembly or preprocessing
    if (!opt_S && !opt_E) {
        if (opt_dryrun) {
            // Print what we would do
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "<built-in-assembler> -o %s", out_path);
            printf("%s\n", cmd);
            return 0;
        }

        out_paths = reverse(out_paths);

        if (opt_c) {
            // -c: assemble each .s file to the single output .o using built-in assembler
            // For multiple inputs we'd need per-file .o names, but rcc currently
            // concatenates all to one .s, so just assemble that one file.
            uint64_t t0 = opt_time ? now_us() : 0;
            int status = 0;
            for (OutPath *p = out_paths; p; p = p->next) {
                if (assemble_file(p->path, out_path) != 0) {
                    fprintf(stderr, "rcc: error: assembly failed for %s\n", p->path);
                    status = 1;
                }
                remove(p->path);
            }
            if (opt_time)
                fprintf(stderr, "  assemble    %s: %6lu us\n", out_path,
                        (unsigned long)(now_us() - t0));
            return status;
        }

        // Linking: assemble each .s to a temp .o, then call the linker
        char cmd[4096];
        int status = 0;

        // Use GCC as the linker frontend with -fuse-ld= so it handles crt files,
        // sysroot, and -Wl,... passthrough correctly across all targets.
        // LD is probed at build time (mold if available, else ld).
#ifdef __APPLE__
        snprintf(cmd, sizeof(cmd), GCC " -o %s -arch arm64"
                                       " -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
                                       " -Wl,-undefined,dynamic_lookup",
                 out_path);
#else
        if (opt_pie)
            snprintf(cmd, sizeof(cmd), GCC " -fuse-ld=" LD " -pie -o %s", out_path);
        else if (opt_pic)
            snprintf(cmd, sizeof(cmd), GCC " -fuse-ld=" LD " -o %s", out_path);
        else
            snprintf(cmd, sizeof(cmd), GCC " -fuse-ld=" LD " -no-pie -o %s", out_path);
#endif

        // Assemble each .s file to a temp .o and add to linker command
        OutPath *obj_paths = NULL;
        uint64_t t_asm = opt_time ? now_us() : 0;
        for (OutPath *p = out_paths; p; p = p->next) {
            char obj_tmp[256];
            snprintf(obj_tmp, sizeof(obj_tmp), "%s.tmp.o", p->path);
            if (assemble_file(p->path, obj_tmp) != 0) {
                fprintf(stderr, "rcc: error: assembly failed for %s\n", p->path);
                status = 1;
                break;
            }
            OutPath *op = arena_alloc(sizeof(OutPath));
            op->path = format("%s", obj_tmp);
            op->next = obj_paths;
            obj_paths = op;
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, obj_tmp, sizeof(cmd) - strlen(cmd) - 1);
        }
        if (opt_time)
            fprintf(stderr, "  assemble    %s: %6lu us\n", out_path,
                    (unsigned long)(now_us() - t_asm));

#if defined(_WIN32) || defined(__MINGW32__)
        {
            struct stat libst;
#ifdef RCC_INCDIR
            const char *rcc_lib = RCC_INCDIR "/../lib/mingw.obj";
            if (stat("lib/mingw.obj", &libst) != 0 && stat(rcc_lib, &libst) == 0)
                snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " %s", rcc_lib);
            else
#endif
                if (stat("lib/mingw.obj", &libst) == 0)
                snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " lib/mingw.obj");
        }
#endif
#ifdef __APPLE__
        {
            struct stat libst;
            if (stat("lib/darwin.o", &libst) == 0)
                snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " lib/darwin.o");
#ifdef RCC_INCDIR
            else {
                const char *rcc_darwin = RCC_INCDIR "/../lib/darwin.o";
                if (stat(rcc_darwin, &libst) == 0)
                    snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " %s", rcc_darwin);
            }
#endif
        }
#endif

        if (libs_len)
            strncat(cmd, libs, sizeof(cmd) - strlen(cmd) - 1);

        if (!status) {
            uint64_t t_link = opt_time ? now_us() : 0;
            status = system(cmd);
            if (opt_time)
                fprintf(stderr, "  link        %s: %6lu us\n", out_path,
                        (unsigned long)(now_us() - t_link));
            if (status != 0)
                fprintf(stderr, "rcc: error: linker %s failed with code %d\n", cmd, status);
        }

        // Cleanup temp files
        for (OutPath *p = out_paths; p; p = p->next)
            remove(p->path);
        for (OutPath *p = obj_paths; p; p = p->next)
            remove(p->path);

        return status ? 1 : 0;
    }
    return 0;
}
