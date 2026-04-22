#include "rcc.h"
#include <stdarg.h>

//
// Arena Allocator
//
typedef struct Chunk Chunk;
struct Chunk {
    Chunk *next;
    size_t size;
    size_t used;
    char data[];
};

static Chunk *current_chunk;

void *arena_alloc(size_t size) {
    // Align to 8 bytes for performance and struct padding
    size = (size + 7) & ~7;

    if (!current_chunk || current_chunk->used + size > current_chunk->size) {
        size_t chunk_size = 1024 * 1024; // 1MB chunks
        if (size > chunk_size) {
            chunk_size = size;
        }
        Chunk *chunk = calloc(1, sizeof(Chunk) + chunk_size);
        chunk->next = current_chunk;
        chunk->size = chunk_size;
        current_chunk = chunk;
    }

    void *ptr = current_chunk->data + current_chunk->used;
    current_chunk->used += size;
    return ptr;
}

char *format(char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char *s = arena_alloc(len + 1);
    strcpy(s, buf);
    return s;
}

//
// String Interning (Hash Map)
//

// FNV-1a Hash function
static uint32_t hash_str(char *s, int len) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)s[i];
        hash *= 16777619;
    }
    return hash;
}

typedef struct InternedStr InternedStr;
struct InternedStr {
    InternedStr *next;
    char *str;
    int len;
};

#define HASH_SIZE 4096
static InternedStr *strings[HASH_SIZE];

char *str_intern(char *start, int len) {
    uint32_t h = hash_str(start, len) % HASH_SIZE;

    for (InternedStr *s = strings[h]; s; s = s->next) {
        if (s->len == len && !memcmp(s->str, start, len))
            return s->str;
    }

    InternedStr *s = arena_alloc(sizeof(InternedStr));
    s->str = arena_alloc(len + 1);
    memcpy(s->str, start, len);
    s->str[len] = '\0';
    s->len = len;

    s->next = strings[h];
    strings[h] = s;
    return s->str;
}
