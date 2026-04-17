#include "rcc.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#define _getpid getpid
#endif

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
    buf[size] = '\0';
    return buf;
}

int main(int argc, char **argv) {
    char *out_path = "a.exe";
    char *in_path = NULL;
    bool opt_S = false;
    bool opt_c = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-S")) {
            opt_S = true;
        } else if (!strcmp(argv[i], "-c")) {
            opt_c = true;
        } else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) {
                fprintf(stderr, "error: missing argument for -o\n");
                return 1;
            }
            out_path = argv[i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "rcc: warning: ignored unknown option %s\n", argv[i]);
        } else {
            in_path = argv[i];
        }
    }

    if (!in_path) {
        fprintf(stderr, "rcc: fatal error: no input files\n");
        return 1;
    }

    char *asm_path = opt_S ? out_path : format("rcc_tmp_%d.s", _getpid());

    // Tokenize and Parse
    char *contents = read_file(in_path);
    Token *tok = tokenize(in_path, contents);
    Program *prog = parse(tok);
    
    // Type system / Semantic checks
    for (Function *fn = prog->funcs; fn; fn = fn->next) {
        for (Node *n = fn->body; n; n = n->next) {
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

    // Assemble / Link if not just compiling to assembly
    if (!opt_S) {
        char cmd[1024];
        if (opt_c) {
            snprintf(cmd, sizeof(cmd), "gcc -c %s -o %s", asm_path, out_path);
        } else {
            snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s", asm_path, out_path);
        }
        
        int status = system(cmd);
        if (status != 0) {
            fprintf(stderr, "rcc: error: backend gcc failed with code %d\n", status);
            remove(asm_path);
            return status;
        }
        remove(asm_path);
    }

    return 0;
}
