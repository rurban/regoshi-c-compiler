#include "rcc.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#define _getpid getpid
#endif
#ifndef GCC
#define GCC "gcc"
#endif

void add_define(char *def);
void add_undef(char *name);

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
    FILE *fp = fopen(path, "r");
    if (!fp)
        error("cannot open %s: %m", path);

    int filemax = 10 * 1024 * 1024;
    char *buf = arena_alloc(filemax);
    int size = fread(buf, 1, filemax - 2, fp);
    if (!feof(fp)) {
        error("%s: file too large", path);
    }

    if (size == 0 || buf[size - 1] != '\n') {
        buf[size++] = '\n';
    }
    fclose(fp);
    buf[size] = '\0';
    return buf;
}

void help(void) {
    printf("rcc v1.2-dev - Copyright 2026 Hamagoto-Y and Reini Urban\n");
    printf("rcc [options...] [-o outfile] [-c] infile(s)...\n");
    printf("Options:\n"
           "-I path       add include path\n"
           "-Lpath        add linker path\n"
           "-lname        add lib\n"
           "-E            preprocessor-only\n"
           "-S            assemble-only\n"
           "-c            compile-only\n"
           "-o file       set output filename\n"
           "-O0           skip peephole optimizer\n"
           "-Dname[=val]  define a macro value\n"
           "-Uname        undefine a macro value\n");
}

bool opt_O0 = false;

int main(int argc, char **argv) {
#ifndef __x86_64__
    fprintf(stderr, "rcc: unsupported target: only x86_64 is supported\n");
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
    bool opt_S = false;
    bool opt_c = false;
    bool opt_E = false;
    bool opt_o = false;
    char libs[512] =
#ifdef _WIN32
        ""
#else
        " -lm"
#endif
        ;
    int libs_len = strlen(libs);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            help();
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
        } else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) {
                fprintf(stderr, "error: missing argument for -o\n");
                return 1;
            }
            out_path = argv[i];
            opt_o = true;
        } else if (!strncmp(argv[i], "-l", 2) || !strncmp(argv[i], "-L", 2)) {
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
            char *preprocessed = preprocess(in_path, contents);

            if (opt_E) {
                printf("%s", preprocessed);
                continue;
            }

            Token *tok = tokenize(in_path, preprocessed);
            Program *prog = parse(tok);

            // Type system / Semantic checks
            for (TLItem *item = prog->items; item; item = item->next) {
                if (item->kind != TL_FUNC)
                    continue;
                for (Node *n = item->fn->body; n; n = n->next) {
                    add_type(n);
                }
            }

            // CTFE is still incomplete and has caused miscompiles/crashes on parts
            // of the TCC suite, so keep the compile path conservative for now.

            // Redirect stdout to our assembly file
            if (!freopen(asm_path, "w", stdout)) {
                fprintf(stderr, "rcc: error: cannot open output file %s\n", asm_path);
                return 1;
            }
            // Code generation (prints assembly to stdout, which is now asm_path)
            codegen(prog);
            fflush(stdout);
            // Restore stdout to console if we want to print further, but we are done.
            fclose(stdout);

            if (!opt_S) {
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
        char cmd[1024];
        if (opt_c) {
            snprintf(cmd, sizeof(cmd), GCC " -c -o %s", out_path);
        } else {
#if defined(__APPLE__)
            snprintf(cmd, sizeof(cmd), GCC " -Wl,-e,main -o %s%s", out_path, libs);
#else
            snprintf(cmd, sizeof(cmd), GCC " -no-pie -o %s%s", out_path, libs);
#endif
        }
        out_paths = reverse(out_paths);
        for (OutPath *p = out_paths; p; p = p->next) {
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd));
            strncat(cmd, p->path, sizeof(cmd) - strlen(cmd));
        }
        int status = system(cmd);
        if (status != 0) {
            fprintf(stderr, "rcc: error: backend %s failed with code %d\n", cmd, status);
        }
        for (OutPath *p = out_paths; p; p = p->next) {
            remove(p->path);
        }
        if (status != 0) {
            return status;
        }
    }
    return 0;
}
