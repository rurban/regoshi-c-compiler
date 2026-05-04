// SPDX-License-Identifier: LGPL-2.1-or-later
// Runtime support for Darwin (macOS): provides glibc extensions missing on macOS.

#include <stdlib.h>
#include <dlfcn.h>

/* on_exit - register cleanup function with exit code and argument.
   glibc extension, not available in Darwin libc. */
static struct {
    void (*fn)(int, void *);
    void *arg;
} on_exit_ent[32];
static int on_exit_n;
static int on_exit_code;

static void on_exit_run(void) {
    int i = on_exit_n;
    while (i > 0) {
        i--;
        on_exit_ent[i].fn(on_exit_code, on_exit_ent[i].arg);
    }
}
int on_exit(void (*fn)(int, void *), void *arg) {
    if (on_exit_n >= 32) return -1;
    on_exit_ent[on_exit_n].fn = fn;
    on_exit_ent[on_exit_n].arg = arg;
    if (on_exit_n == 0) atexit(on_exit_run);
    on_exit_n++;
    return 0;
}

/* Override exit() to capture the exit code before atexit handlers run,
   so on_exit callbacks receive the correct status. */
void exit(int code) {
    on_exit_code = code;
    // Call the real exit via dlsym(RTLD_NEXT)
    typedef void (*exit_fn_t)(int);
    static exit_fn_t real_exit;
    if (!real_exit) {
        real_exit = (exit_fn_t)(void *)dlsym(RTLD_NEXT, "exit");
    }
    if (real_exit)
        real_exit(code);
    _Exit(code);
}
