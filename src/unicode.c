// SPDX-License-Identifier: LGPL-2.1-or-later
/*
  Derived from slimcc by fuhsnn.
  Optimized by Reini Urban 2026
  Integrated libu8ident TR39 for secure identifier checking.
*/
#include "rcc.h"
// Must include u8id_private.h before u8ident.h to avoid EXTERN redefinition
#include "u8id_private.h"
#include "u8ident.h"
#include "u8idscr.h"

#define MASK(bits) (uint8_t)((1 << (bits)) - 1)
#define MASK2(bits, shift) (uint8_t)(MASK(bits) << (shift))

typedef struct {
    uint32_t first;
    uint32_t last;
} UTF32Range;

// clang-format off
// Math symbols allowed in C identifiers (universal character names)
// These augment the TR39 script-based ranges.
static UTF32Range math_start[] = {
  {0x2202, 0x2202},   {0x2207, 0x2207},   {0x221E, 0x221E},   {0x1D6C1, 0x1D6C1},
  {0x1D6DB, 0x1D6DB}, {0x1D6FB, 0x1D6FB}, {0x1D715, 0x1D715}, {0x1D735, 0x1D735},
  {0x1D74F, 0x1D74F}, {0x1D76F, 0x1D76F}, {0x1D789, 0x1D789}, {0x1D7A9, 0x1D7A9},
  {0x1D7C3, 0x1D7C3},
};

static UTF32Range math_cont[] = {
  {0xB2, 0xB3}, {0xB9, 0xB9}, {0x2070, 0x2070}, {0x2074, 0x207E}, {0x2080, 0x208E},
};
// clang-format on

int encode_utf8(char *buf, uint32_t c) {
    if (c <= 0x7F) {
        buf[0] = c;
        return 1;
    }

    if (c <= 0x7FF) {
        buf[0] = MASK2(2, 6) | (c >> 6);
        buf[1] = MASK2(1, 7) | (c & MASK(6));
        return 2;
    }

    if (c <= 0xFFFF) {
        buf[0] = MASK2(3, 5) | (c >> 12);
        buf[1] = MASK2(1, 7) | ((c >> 6) & MASK(6));
        buf[2] = MASK2(1, 7) | (c & MASK(6));
        return 3;
    }

    buf[0] = MASK2(4, 4) | (c >> 18);
    buf[1] = MASK2(1, 7) | ((c >> 12) & MASK(6));
    buf[2] = MASK2(1, 7) | ((c >> 6) & MASK(6));
    buf[3] = MASK2(1, 7) | (c & MASK(6));
    return 4;
}

// Read a UTF-8-encoded Unicode code point from a source file.
// We assume that source files are always in UTF-8.
uint32_t decode_utf8(char **new_pos, char *p) {
    if ((unsigned char)*p < 128) {
        *new_pos = p + 1;
        return *p;
    }

    char *start = p;
    int len = 0;
    uint32_t c = 0;

    if ((unsigned char)*p >= MASK2(4, 4)) {
        len = 4;
        c = *p & MASK(3);
    } else if ((unsigned char)*p >= MASK2(3, 5)) {
        len = 3;
        c = *p & MASK(4);
    } else if ((unsigned char)*p >= MASK2(2, 6)) {
        len = 2;
        c = *p & MASK(5);
    } else {
        error_at(start, "invalid UTF-8 sequence");
    }

    for (int i = 1; i < len; i++) {
        if ((unsigned char)p[i] >> 6 != MASK2(1, 1))
            error_at(start, "invalid UTF-8 sequence");
        c = (c << 6) | (p[i] & MASK(6));
    }

    *new_pos = p + len;
    return c;
}

static int compare_range(const void *key, const void *elem) {
    uint32_t c = *(const uint32_t *)key;
    const UTF32Range *r = elem;
    if (c < r->first)
        return -1;
    if (c > r->last)
        return 1;
    return 0;
}

static bool in_range(uint32_t c, UTF32Range *range, int len) {
    UTF32Range *result = bsearch(&c, range, len, sizeof(UTF32Range), compare_range);
    return result != NULL;
}

bool is32_ident1(uint32_t c) {
    return isTR39_start(c) ||
        in_range(c, math_start, sizeof(math_start) / sizeof(UTF32Range));
}

bool is32_ident2(uint32_t c) {
    return is32_ident1(c) ||
        isTR39_cont(c) ||
        in_range(c, math_cont, sizeof(math_cont) / sizeof(UTF32Range));
}

static bool u8ident_initialized = false;

static void ensure_u8ident_init(void) {
    if (!u8ident_initialized) {
        u8ident_init(U8ID_PROFILE_TR39_4, U8ID_NFC, 0);
        u8ident_initialized = true;
    }
}

void u8ident_allow_script(const char *name) {
    ensure_u8ident_init();
    if (!strcmp(name, "reset")) {
        u8ident_free_ctx(0);
        u8ident_new_ctx();
        return;
    }
    // Map common script names to IDs (from scripts.h ordering)
    // 0=Common, 1=Inherited, 2=Latin, 3=Arabic, 4=Armenian, 5=Bengali, 6=Bopomofo,
    // 7=Cyrillic, 8=Devanagari, 9=Ethiopic, 10=Georgian, 11=Greek, 12=Gujarati,
    // 13=Gurmukhi, 14=Hangul, 15=Han, 16=Hebrew, 17=Hiragana, 18=Katakana,
    // 19=Kannada, 20=Khmer, 21=Lao, 22=Malayalam, 23=Myanmar, 24=Oriya,
    // 25=Sinhala, 26=Tamil, 27=Telugu, 28=Thaana, 29=Thai, 30=Tibetan
    static const char *scripts[] = {
        "Latin",
        "Arabic",
        "Armenian",
        "Bengali",
        "Bopomofo",
        "Cyrillic",
        "Devanagari",
        "Ethiopic",
        "Georgian",
        "Greek",
        "Gujarati",
        "Gurmukhi",
        "Hangul",
        "Han",
        "Hebrew",
        "Hiragana",
        "Katakana",
        "Kannada",
        "Khmer",
        "Lao",
        "Malayalam",
        "Myanmar",
        "Oriya",
        "Sinhala",
        "Tamil",
        "Telugu",
        "Thaana",
        "Thai",
        "Tibetan",
    };
    static const uint8_t ids[] = {
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
        17,
        18,
        19,
        20,
        21,
        22,
        23,
        24,
        25,
        26,
        27,
        28,
        29,
        30,
    };
    for (int i = 0; i < (int)(sizeof(scripts) / sizeof(scripts[0])); i++) {
        if (!strcmp(name, scripts[i])) {
            u8ident_add_script(ids[i]);
            return;
        }
    }
}

const char *u8ident_check_ident(const char *name, int len) {
    ensure_u8ident_init();
    enum u8id_errors ret = u8ident_check_buf(name, len, NULL);
    switch (ret) {
    case U8ID_ERR_SCRIPT:
    case U8ID_ERR_SCRIPTS: {
        static char msg[128];
        const char *sc = u8ident_failed_script_name(0);
        uint32_t cp = u8ident_failed_char(0);
        if (sc)
            snprintf(msg, sizeof(msg), "disallowed script %s (U+%04X) in identifier",
                     sc, cp);
        else
            snprintf(msg, sizeof(msg), "identifier mixes scripts in a confusable way");
        return msg;
    }
    case U8ID_ERR_COMBINE: return "identifier has invalid combining mark sequence";
    case U8ID_ERR_CONFUS: return "identifier contains confusable characters";
    case U8ID_EOK_WARN_CONFUS:
    case U8ID_EOK_NORM_WARN_CONFUS:
        return "identifier may contain confusable characters";
    default: return NULL;
    }
}

int utf8_len(char *str) {
    int count = 0;
    unsigned char *p = (unsigned char *)str;
    while (*p) {
        // Count only lead bytes (not continuation bytes 0x80-0xBF)
        if ((*p & 0xC0) != 0x80)
            count++;
        p++;
    }
    return count;
}
