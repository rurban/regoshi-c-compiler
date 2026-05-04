// Global struct with nested substruct containing function pointers
// This is the exact pattern that breaks SQLite
#include <stdio.h>

typedef unsigned char u8;

typedef struct {
    void *(*fn1)(int);
    void (*fn2)(void *);
    void *(*fn3)(void *, int);
    int (*fn4)(void *);
    int (*fn5)(int);
    int (*fn6)(void *);
    void (*fn7)(void *);
    void *data;
} Methods;

typedef struct {
    int (*init)(void);
    int (*end)(void);
    void *(*alloc)(int);
    void (*free)(void *);
    void (*enter)(void *);
    int (*try_)(void *);
    void (*leave)(void *);
    int (*held)(void *);
    int (*notheld)(void *);
} MutexMethods;

struct Config {
    int flag1;
    u8 a, b, c, d, e, f, g;
    int mxLen;
    int never;
    int szLA;
    int nLA;
    int nSpill;
    Methods m;
    MutexMethods mx;
    void *pHeap;
    int nHeap;
};

static int dummy_init(void) { return 0; }
static int dummy_end(void) { return 0; }

struct Config cfg = {
    1, // flag1
    1,
    0,
    0,
    1,
    0,
    1,
    0, // a-g
    0x7ffffffe, // mxLen
    0, // never
    1200, // szLA
    40, // nLA
    65536, // nSpill
    {0, 0, 0, 0, 0, 0, 0, 0}, // m (all null)
    {dummy_init, dummy_end, 0, 0, 0, 0, 0, 0, 0}, // mx
    0, // pHeap
    0, // nHeap
};

int main(void) {
    printf("mxLen=%d (expect %d)\n", cfg.mxLen, 0x7ffffffe);
    printf("szLA=%d nLA=%d nSpill=%d\n", cfg.szLA, cfg.nLA, cfg.nSpill);
    printf("mx.init is %s\n", cfg.mx.init ? "set" : "null");
    int r = cfg.mx.init();
    printf("mx.init()=%d\n", r);

    // Check bytes around mxLen
    unsigned char *p = (unsigned char *)&cfg;
    int off = (unsigned char *)&cfg.mxLen - p;
    printf("mxLen at offset %d: %02x %02x %02x %02x\n",
           off, p[off], p[off + 1], p[off + 2], p[off + 3]);
    return 0;
}
