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

#include <assert.h>
#include <errno.h>

#define STDCHAR char
#define TRUE true
#define FALSE false

// Include data headers without EXTERN_SCRIPTS for LOCAL definitions
#include "u8id_gc.h"
#include "scripts.h"
#include "mark.h"
#undef EXTERN_SCRIPTS
#include "unitr39.h"
#include "medial.h"

// Normalization table headers
#if !defined U8ID_NORM || U8ID_NORM == NFC || U8ID_NORM == NFD || U8ID_NORM == FCC || U8ID_NORM == FCD
#include "un8ifcan.h"
#endif
#if !defined U8ID_NORM || U8ID_NORM == NFKC || U8ID_NORM == NFKD
#include "un8ifcpt.h"
#endif
#if !defined U8ID_NORM || U8ID_NORM != FCD
#include "un8ifcmb.h"
#endif
#if !defined U8ID_NORM || U8ID_NORM == NFKC || U8ID_NORM == NFC || U8ID_NORM == FCC
#include "un8ifexc.h"
#include "un8ifcmp.h"
#endif
#include "hangul.h"

// Function prototypes (guarded re-includes safe)
#include "u8idscr.h"

// ===== Global state =====

unsigned s_u8id_options = U8ID_TR31_DEFAULT;
enum u8id_norm s_u8id_norm = U8ID_NORM_DEFAULT;
enum u8id_profile s_u8id_profile = U8ID_PROFILE_DEFAULT;
unsigned s_maxlen = 1024;

struct ctx_t ctx[U8ID_CTX_TRESH] = {0}; // pre-allocate 5 contexts
static u8id_ctx_t i_ctx = 0;
struct ctx_t *ctxp = NULL; // if more than 5 contexts

// ===== Simple getters =====

enum u8id_norm u8ident_norm(void) { return s_u8id_norm; }
enum u8id_profile u8ident_profile(void) { return s_u8id_profile; }
enum u8id_options u8ident_tr31(void) {
    return (enum u8id_options)(s_u8id_options & 127);
}
unsigned u8ident_options(void) { return s_u8id_options; }
unsigned u8ident_maxlength(void) { return s_maxlen; }

// ===== Context management =====

EXTERN u8id_ctx_t u8ident_new_ctx(void) {
    // thread-safety later
    u8id_ctx_t i = i_ctx + 1;
    i_ctx++;
    if (i == U8ID_CTX_TRESH) {
        ctxp = (struct ctx_t *)calloc(U8ID_CTX_TRESH, sizeof(struct ctx_t));
    } else if (i > U8ID_CTX_TRESH) {
        ctxp = (struct ctx_t *)realloc(ctxp, i * sizeof(struct ctx_t));
    } else {
        ctxp = &ctx[i];
    }
    memset(ctxp, 0, sizeof(struct ctx_t));
    return i_ctx;
}

/* Changes to the context previously generated with `u8ident_new_ctx`. */
EXTERN int u8ident_set_ctx(u8id_ctx_t i) {
    if (i <= i_ctx) {
        i_ctx = i;
        return 0;
    } else
        return -1;
}

/* Changes to the context previously generated with `u8ident_new_ctx`. */
LOCAL struct ctx_t *u8ident_ctx(void) {
    return (i_ctx < U8ID_CTX_TRESH) ? &ctx[i_ctx] : &ctxp[i_ctx];
}

// search in linear vector of scripts per ctx
LOCAL bool u8ident_has_script_ctx(const uint8_t scr, const struct ctx_t *c) {
    if (!c->count)
        return false;
    const uint8_t *u8p = (c->count > 8) ? c->u8p : c->scr8;
    for (int i = 0; i < c->count; i++) {
        if (scr == u8p[i])
            return true;
    }
    return false;
}

LOCAL bool u8ident_has_script(const uint8_t scr) {
    return u8ident_has_script_ctx(scr, u8ident_ctx());
}

LOCAL int u8ident_add_script_ctx(const uint8_t scr, struct ctx_t *c) {
    if (scr < 2 || scr >= FIRST_LIMITED_USE_SCRIPT)
        return -1;
    int i = c->count;
    if (unlikely(i == 8)) {
        uint8_t *p = malloc(16);
        memcpy(p, c->scr8, 8);
        c->u8p = p;
        c->u8p[i] = scr;
    } else if (unlikely(i > 8 && (i & 7) == 7)) {
        c->u8p = realloc(c->u8p, i + 8);
        c->u8p[i] = scr;
    } else {
        if (i > 8) {
            if (!c->u8p) {
                c->u8p = calloc(16, 1);
                memcpy(c->u8p, c->scr8, 8);
            }
            c->u8p[i] = scr;
        } else {
            c->scr8[i] = scr;
        }
    }
    if (scr == SC_Han)
        c->has_han = 1;
    else if (scr == SC_Bopomofo)
        c->is_chinese = 1;
    else if (scr == SC_Katakana || scr == SC_Hiragana)
        c->is_japanese = 1;
    else if (scr == SC_Hangul)
        c->is_korean = 1;
    else if (scr == SC_Hebrew || scr == SC_Arabic)
        c->is_rtl = 1;
    c->count++;
    return 0;
}

// ===== Search utilities =====

static inline bool linear_search(const uint32_t cp,
                                 const struct range_bool *sc_list,
                                 const int len) {
    struct range_bool *s = (struct range_bool *)sc_list;
    for (int i = 0; i < len; i++) {
        assert(s->from <= s->to);
        if ((cp - s->from) <= (s->to - s->from))
            return true;
        if (cp <= s->to) // s is sorted. not found
            return false;
        s++;
    }
    return false;
}

static inline struct sc *binary_search(const uint32_t cp, const char *list,
                                       const size_t len, const size_t size) {
    int n = (int)len;
    const char *p = list;
    struct sc *pos;
    while (n > 0) {
        pos = (struct sc *)(p + size * (n / 2));
        // hack: with unsigned wrapping max-cp is always higher, so false
        // was: (cp >= pos->from && cp <= pos->to)
        if ((cp - pos->from) <= (pos->to - pos->from))
            return pos;
        else if (cp < pos->from)
            n /= 2;
        else {
            p = (char *)pos + size;
            n -= (n / 2) + 1;
        }
    }
    return NULL;
}

// hybrid search: linear or binary
static inline uint8_t sc_search(const uint32_t cp, const struct sc *sc_list,
                                const size_t len) {
    if (cp < 255) { // 14 ranges a 9 byte (126 byte, i.e cache loads)
        struct sc *s = (struct sc *)sc_list;
        for (size_t i = 0; i < len; i++) {
            if ((cp - s->from) <= (s->to - s->from)) // faster in-between trick
                return s->scr;
            if (cp <= s->to) // s is sorted. not found
                return 255;
            s++;
        }
        return 255;
    } else {
        const struct sc *sc =
            (struct sc *)binary_search(cp, (char *)sc_list, len, sizeof(*sc_list));
        return sc ? sc->scr : 255;
    }
}

static inline bool range_bool_search(const uint32_t cp,
                                     const struct range_bool *list,
                                     const size_t len) {
    return binary_search(cp, (char *)list, len, sizeof(*list)) ? true : false;
}


// ===== Script and search functions =====

EXTERN uint8_t u8ident_get_script(const uint32_t cp) {
    // faster check, as we have no NON-xid's
    return sc_search(cp, nonxid_script_list, ARRAY_SIZE(nonxid_script_list));
}

/* Search for list of script indices */
LOCAL const struct scx *u8ident_get_scx(const uint32_t cp) {
    return (const struct scx *)binary_search(
        cp, (char *)scx_list, ARRAY_SIZE(scx_list), sizeof(*scx_list));
}
/* Search for TR39 XID entry, in start or cont lists */
LOCAL const struct sc_tr39 *u8ident_get_tr39(const uint32_t cp) {
    const struct sc_tr39 *sc = (const struct sc_tr39 *)binary_search(
        cp, (char *)tr39_start_list, ARRAY_SIZE(tr39_start_list),
        sizeof(*tr39_start_list));
    if (sc)
        return sc;
    else
        return (const struct sc_tr39 *)binary_search(cp, (char *)tr39_cont_list,
                                                     ARRAY_SIZE(tr39_cont_list),
                                                     sizeof(*tr39_cont_list));
}
LOCAL bool u8ident_is_MARK(uint32_t cp) {
    return range_bool_search(cp, mark_list, ARRAY_SIZE(mark_list));
}
LOCAL bool u8ident_is_MEDIAL(uint32_t cp) {
    return range_bool_search(cp, medial_list, ARRAY_SIZE(medial_list));
}
LOCAL bool u8ident_is_bidi(const uint32_t cp) {
    return linear_search(cp, bidi_list, ARRAY_SIZE(bidi_list));
}
LOCAL bool isTR39_start(const uint32_t cp) {
    return binary_search(cp, (char *)tr39_start_list, ARRAY_SIZE(tr39_start_list),
                         sizeof(*tr39_start_list))
        ? true
        : false;
}
LOCAL bool isTR39_cont(const uint32_t cp) {
    return binary_search(cp, (char *)tr39_cont_list, ARRAY_SIZE(tr39_cont_list),
                         sizeof(*tr39_cont_list))
        ? true
        : false;
}

LOCAL enum u8id_gc u8ident_get_gc(const uint32_t cp) {
    const struct gc *gc = (const struct gc *)binary_search(
        cp, (char *)gc_list, ARRAY_SIZE(gc_list), sizeof(*gc_list));
    if (gc)
        return gc->gc;
    else
        return GC_INVALID;
}
LOCAL const char *u8ident_gc_name(const enum u8id_gc gc) {
    if (gc >= GC_INVALID)
        return NULL;
    assert(gc < GC_INVALID);
    return u8id_gc_names[gc];
}

// bitmask of u8id_idtypes
LOCAL uint16_t u8ident_get_idtypes(const uint32_t cp) {
    const struct range_short *id = (struct range_short *)binary_search(
        cp, (char *)idtype_list, ARRAY_SIZE(idtype_list), sizeof(*idtype_list));
    return id ? id->types : 0;
}

static inline int compar32(const void *a, const void *b) {
    const uint32_t ai = *(const uint32_t *)a;
    const uint32_t bi = *(const uint32_t *)b;
    return ai < bi ? -1 : ai == bi ? 0
                                   : 1;
}

LOCAL bool u8ident_is_greek_latin_confus(const uint32_t cp) {
    return bsearch(&cp, greek_confus_list, ARRAY_SIZE(greek_confus_list), 4,
                   compar32) != NULL
        ? true
        : false;
}

EXTERN const char *u8ident_script_name(const int scr) {
    if (scr < 0 || scr > LAST_SCRIPT)
        return NULL;
    assert(scr >= 0 && scr <= LAST_SCRIPT);
    return all_scripts[scr];
}

EXTERN uint32_t u8ident_failed_char(const u8id_ctx_t i) {
    if (i <= i_ctx) {
        const struct ctx_t *c = (i_ctx < U8ID_CTX_TRESH) ? &ctx[i] : &ctxp[i];
        return c->last_cp;
    } else {
        return 0;
    }
}
/* returns the constant script name, which failed in the last check. */
EXTERN const char *u8ident_failed_script_name(const u8id_ctx_t i) {
    if (i <= i_ctx) {
        const struct ctx_t *c = (i_ctx < U8ID_CTX_TRESH) ? &ctx[i] : &ctxp[i];
        const uint32_t cp = c->last_cp;
        if (cp > 0)
            return u8ident_script_name(u8ident_get_script(cp));
    }
    return NULL;
}
EXTERN int u8ident_add_script(uint8_t scr) {
    return u8ident_add_script_ctx(scr, u8ident_ctx());
}
EXTERN int u8ident_free_ctx(u8id_ctx_t i) {
    if (i_ctx < U8ID_CTX_TRESH)
        ctxp = &ctx[0];
    if (i <= i_ctx) {
        if (ctxp[i].count > 8)
            free(ctxp[i].u8p);
        memset(&ctxp[i], 0, sizeof(u8id_ctx_t));
        if (i > 0)
            i_ctx = i - 1; // switch to the previous context
        else
            i_ctx = 0; // deleting 0 will lead to a reset
        return 0;
    } else
        return -1;
}

/* End this library, cleaning up all internal structures. */
EXTERN void u8ident_free(void) {
    for (u8id_ctx_t i = 0; i <= i_ctx; i++) {
        u8ident_free_ctx(i);
    }
    if (i_ctx >= U8ID_CTX_TRESH) {
        free(ctxp);
    }
}
EXTERN const char *u8ident_existing_scripts(const u8id_ctx_t i) {
    if (unlikely(i > i_ctx))
        return NULL;
    const struct ctx_t *c = (i_ctx < U8ID_CTX_TRESH) ? &ctx[i] : &ctxp[i];
    const uint8_t *u8p = (c->count > 8) ? c->u8p : c->scr8;
    size_t len = c->count * 12;
    char *res = malloc(len);
    *res = 0;
    for (int j = 0; j < c->count; j++) {
        const char *str = u8ident_script_name(u8p[j]);
        if (!str) {
            free(res);
            return NULL;
        }
        const size_t l = strlen(str);
        if (*res) {
            if (l + 3 > len) {
                len = l + 3;
                res = realloc(res, len);
            }
            strcat(res, ", ");
        } else { // first name
            if (l + 1 > len) {
                len = l + 1;
                res = realloc(res, len);
            }
        }
        strcat(res, str);
    }
    return res;
}
LOCAL bool u8ident_maybe_normalized(const uint32_t cp) {
    if (range_bool_search(cp, NFC_N_list, ARRAY_SIZE(NFC_N_list)))
        return true;
    return range_bool_search(cp, NFC_M_list, ARRAY_SIZE(NFC_M_list));
}


// ===== Core identifier checking API =====

LOCAL const char *u8ident_errstr(int errcode) {
    static const char *const _str[] = {
        "ERR_CONFUS", // -6
        "ERR_COMBINE", // -5
        "ERR_ENCODING", // -4
        "ERR_SCRIPTS", //-3
        "ERR_SCRIPT", //-2
        "ERR_XID", // -1
        "EOK", // 0
        "EOK_NORM", // 1
        "EOK_WARN_CONFUS", // 2
        "EOK_NORM_WARN_CONFUS", // 3
    };
    assert(errcode >= -6 && errcode <= 3);
    return _str[errcode + 6];
}

static struct func_tr31_s tr31_funcs[] = {
    {isTR39_start, isTR39_cont},
};

EXTERN int u8ident_init(enum u8id_profile profile, enum u8id_norm norm,
                        unsigned options) {
    u8ident_free(); // clear and reset the ctx
    if (options > 1023)
        return -1;
    if (profile < U8ID_PROFILE_1 || profile > U8ID_PROFILE_TR39_4)
        return -1;
    if (norm > U8ID_FCC)
        return -1;
        // only one is allowed, else fail
#if U8ID_NORM == NFD
    if (norm != U8ID_NFD)
        return -1;
#endif
#if U8ID_NORM == NFC
    if (norm != U8ID_NFC)
        return -1;
#endif
#if U8ID_NORM == NFKC
    if (norm != U8ID_NFKC)
        return -1;
#endif
#if U8ID_NORM == NFKD
    if (norm != U8ID_NFKD)
        return -1;
#endif
#if U8ID_NORM == FCD
    if (norm != U8ID_FCD)
        return -1;
#endif
#if U8ID_NORM == FCC
    if (norm != U8ID_FCC)
        return -1;
#endif
    s_u8id_norm = norm;

    s_u8id_profile = U8ID_PROFILE_TR39_4;

    s_u8id_options = (options & ~U8ID_TR31_MASK) | U8ID_TR31_DEFAULT;

    return 0;
}
EXTERN void u8ident_set_maxlength(unsigned maxlen) {
    if (maxlen > 1)
        s_maxlen = maxlen;
}
bool in_SCX(const enum u8id_sc scr, const char *scx) {
    unsigned char *x = (unsigned char *)scx;
    while (*x) {
        if (*x == (unsigned char)scr)
            return true;
        x++;
    }
    return false;
}

bool nsm_check(const uint32_t base_cp, const uint32_t cp) {
    if (cp == 0x307 && (base_cp == 'i' || base_cp == 0x131 // dotless i
                        || base_cp == 0x237 // dotless j
                        || base_cp == 0x25F // dotless j with stroke
                        || base_cp == 0x284 // dotless j with stroke and hook
                        || base_cp == 0x1DA1 // dotless j with stroke
                        || base_cp == 0x10798 // dotless i
                        || base_cp == 0x1D6A4 // dotless j
                        || base_cp == 0x1D645)) // dotless j
        return false;
    // Todo: check the 10 different STROKE Mn's: SHORT BAR OVERLAY, LONG BAR
    // OVERLAY, LIGHT CENTRALIZATION STROKE BELOW, STRONG CENTRALIZATION STROKE
    // BELOW, ...

    for (unsigned i = 0; i < ARRAY_SIZE(nsm_letters); i++) {
        const struct nsm_ws *l = &nsm_letters[i];
        if (l->nsm > cp)
            break;
        if (l->nsm != cp)
            continue;
        if (wcschr(l->letters, (wchar_t)base_cp))
            return false;
    }
    return true;
}
EXTERN enum u8id_errors u8ident_check_buf(const char *buf, const int bufsz,
                                          char **outnorm) {
    int ret = U8ID_EOK;
    char *s = (char *)buf;
    const char *e = (char *)&buf[bufsz];
    bool need_normalize = false;
    struct ctx_t *ctx = u8ident_ctx();
    enum u8id_sc scr;
    enum u8id_sc basesc = SC_Unknown;
    const unsigned xid_mask = s_u8id_options & U8ID_TR31_MASK;
    // default to XID (0)
    const enum xid_e xid = TR39;
    char *scx = NULL;
    assert(xid >= 0 && xid <= LAST_XID_E);
    uint32_t prev_cp = 0, base_cp = 0;
    int seq_mn = 0;
    uint32_t cp = dec_utf8(&s);

    // hardcoded TR31 funcs via static functions (inlinable)
    if (unlikely(!isTR39_start(cp))) {
        ctx->last_cp = cp;
        return U8ID_ERR_XID;
    }
    bool has_latin = u8ident_has_script_ctx(SC_Latin, ctx);

    do {

        // profile 6 shortcuts: skip all script checks.
        // when we need TR31 checks.
        // advance to normalize checks
#if defined U8ID_PROFILE && (U8ID_PROFILE == 6 || U8ID_PROFILE == C11_6) && \
    defined(DISABLE_CHECK_XID)
        need_normalize = true;
        // if (scr != SC_Common && scr != SC_Inherited)
        //   basesc = scr;
        goto norm;
#elif defined U8ID_PROFILE && U8ID_PROFILE != 6 && U8ID_PROFILE != C11_6
#else
        if (s_u8id_profile == U8ID_PROFILE_6 ||
            s_u8id_profile == U8ID_PROFILE_C11_6) {
            need_normalize = true;
            if (!((s_u8id_options & U8ID_TR31_MASK) == U8ID_TR31_ALLOWED))
                goto norm;
            else {
                scr = (enum u8id_sc)u8ident_get_script(cp);
                goto ok;
            }
        }
#endif

        scr = (enum u8id_sc)u8ident_get_script(cp);
        // disallow Limited Use scripts
        if (unlikely(scr >= FIRST_LIMITED_USE_SCRIPT &&
                     s_u8id_profile != U8ID_PROFILE_6 &&
                     s_u8id_profile != U8ID_PROFILE_C11_6)) {
            ctx->last_cp = cp;
            return U8ID_ERR_SCRIPT;
        }
        // disallow bidi formatting
        if (unlikely(!ctx->is_rtl && u8ident_is_bidi(cp))) {
            ctx->last_cp = cp;
            return U8ID_ERR_SCRIPT;
        }
        if (!need_normalize) {
            need_normalize = u8ident_maybe_normalized(cp);
        }

        bool is_new = false;
        // check scx on Common or Inherited.
        // TODO Keep list of possible scripts and reduce them.
        if (scr == SC_Common || scr == SC_Inherited) {
            // Almost everybody may mix with latin
            const struct scx *this_scx = u8ident_get_scx(cp);
            if (this_scx) {
                scx = (char *)this_scx->scx;
                const enum u8id_gc gc = (const enum u8id_gc)this_scx->gc;
                int n = 0;
                if (ctx->count && (s_u8id_profile < 5 || s_u8id_profile == TR39_4)) {
                    // Special-case for runs: only after japanese.
                    // This is the only context dependent Lm case.
                    // All others are Combining Marks.
                    if (!ctx->is_japanese &&
                        ((cp >= 0x30FC && cp <= 0x30FE) || cp == 0xFF70)) {
                        ctx->last_cp = cp;
                        return U8ID_ERR_SCRIPTS;
                    }
                    if (!has_latin) { // 6 cases for Hira Kana
                        if (strEQc(scx, "\x11\x12") && !ctx->is_japanese) {
                            ctx->last_cp = cp;
                            return U8ID_ERR_SCRIPTS;
                        }
                        // any cfk, also 6 cases for Bopo Hang Hani Hira Kana
                        if (strEQc(scx, "\x06\x0e\x0f\x11\x12") && !ctx->is_japanese &&
                            !ctx->has_han && !ctx->is_korean) {
                            ctx->last_cp = cp;
                            return U8ID_ERR_SCRIPTS;
                        }
                    }
                }
                // We have 2 Mc cases, and 30 Mn in SCX. No Me. More of them are in SC
                // though.
                if (gc == GC_Mn || gc == GC_Mc) {
                    if (!ctx->count || basesc == SC_Unknown) {
                        // Disallow combiners without any base char (which does have a
                        // script) This catches only a mark as very first char. We check the
                        // base char for runs at ok:
                        ctx->last_cp = cp;
                        return U8ID_ERR_COMBINE;
                    } else if (!in_SCX(basesc, this_scx->scx)) {
                        // Check combiners against basesc
                        ctx->last_cp = cp;
                        return U8ID_ERR_COMBINE;
                    } else if (cp == prev_cp) {
                        // TR39#5.4 "Forbid sequences of the same nonspacing mark"
                        ctx->last_cp = cp;
                        return U8ID_ERR_COMBINE;
                    } else if (gc == GC_Mn && ++seq_mn > 4) {
                        // TR39#5.4 "Forbid sequences of more than 4 nonspacing marks (gc=Mn
                        // or gc=Me)"
                        ctx->last_cp = cp;
                        return U8ID_ERR_COMBINE;
                    } else if (!nsm_check(base_cp, cp)) {
                        // TR39#5.5 "Forbid sequences of base character + nonspacing mark
                        // that look the same as or confusingly similar to the base
                        // character alone"
                        ctx->last_cp = cp;
                        return U8ID_ERR_COMBINE;
                    }
                } else { // not Mn|Mc
                    seq_mn = 0;
                }
                char *x = scx;
                while (*x) {
                    bool has = u8ident_has_script_ctx(*x, ctx);
                    n += has ? 1 : 0;
                    x++;
                }
                /* We have SCX and none of the SCX occured yet.
           So we have a new one.
           We dont know which yet, but we can set is_new. */
                if (!n) {
                    is_new = true;
                    // scx = (char *)this_scx->scx; // for errors
                }
            }
        } else {
            base_cp = cp;
        }

#if defined U8ID_PROFILE && U8ID_PROFILE == 5
        goto ok;
#elif defined U8ID_PROFILE && U8ID_PROFILE != 5
#else
        if (s_u8id_profile == U8ID_PROFILE_5)
            goto ok;
#endif

        // ignore Latin. This is compatible with everything
        if (likely(scr == SC_Latin)) {
            if (!u8ident_has_script_ctx(scr, ctx)) {
                has_latin = true;
                u8ident_add_script_ctx(scr, ctx);
            }
            basesc = scr;
            goto next;
        }

        // if not already have it, add it. EXCLUDED_SCRIPT must already exist
        if (!is_new && !(scr == SC_Common || scr == SC_Inherited))
            is_new = !u8ident_has_script_ctx(scr, ctx);
        if (is_new) {
            // if Limited Use it must have been already manually added
            if (unlikely(scr >= FIRST_LIMITED_USE_SCRIPT &&
                         (s_u8id_profile < U8ID_PROFILE_5 ||
                          s_u8id_profile == U8ID_PROFILE_TR39_4))) {
                ctx->last_cp = cp;
                return U8ID_ERR_SCRIPT;
            }
            // allowed is only one, unless it is an allowed combination
            if (ctx->count) {
#if !defined U8ID_PROFILE || U8ID_PROFILE != 2
                if (s_u8id_profile == U8ID_PROFILE_2)
#endif
                { // single script only
                    ctx->last_cp = cp;
                    return U8ID_ERR_SCRIPTS;
                }
                // check allowed CJK combinations
                if (scr == SC_Bopomofo) {
                    if (unlikely(!ctx->has_han && !has_latin)) {
                        ctx->last_cp = cp;
                        return U8ID_ERR_SCRIPTS;
                    } else
                        goto ok;
                } else if (scr == SC_Han) {
                    if (unlikely(!(ctx->is_chinese || ctx->is_japanese ||
                                   ctx->is_korean || has_latin))) {
                        ctx->last_cp = cp;
                        return U8ID_ERR_SCRIPTS;
                    } else
                        goto ok;
                } else if (scr == SC_Katakana || scr == SC_Hiragana) {
                    if (unlikely(!(ctx->is_japanese || ctx->has_han || has_latin))) {
                        ctx->last_cp = cp;
                        return U8ID_ERR_SCRIPTS;
                    } else
                        goto ok;
                } else if (scr == SC_Common || scr == SC_Inherited) {
                    // may we now collapse it?
                    goto ok;
                }
                // and disallow all other combinations
#if !defined U8ID_PROFILE || U8ID_PROFILE == 3
                else if (s_u8id_profile == U8ID_PROFILE_3) {
                    ctx->last_cp = cp;
                    return U8ID_ERR_SCRIPTS;
                }
#endif
#if !defined U8ID_PROFILE || U8ID_PROFILE == TR39_4
                else if (s_u8id_profile == U8ID_PROFILE_TR39_4) {
                    if (ctx->count >= 2 || scr == SC_Cyrillic) { // not more than 2
                        ctx->last_cp = cp;
                        return U8ID_ERR_SCRIPTS;
                    }
                    // some Greek may mix with Latin
                    if (scr == SC_Greek && has_latin) {
                        assert(s_u8id_profile == U8ID_PROFILE_TR39_4);
                        // only not confusables
                        if (u8ident_is_greek_latin_confus(cp)) {
                            ctx->last_cp = cp;
                            return U8ID_ERR_CONFUS;
                        }
                        goto ok;
                    }
                }
#endif
                // PROFILE_4: allow adding any Recommended to Latin,
                // but not Greek nor Cyrillic.
                // the only remaining profile
#if !defined U8ID_PROFILE || U8ID_PROFILE == 4
                else if (!has_latin || scr == SC_Greek || scr == SC_Cyrillic) {
                    assert(s_u8id_profile == U8ID_PROFILE_4);
                    ctx->last_cp = cp;
                    return U8ID_ERR_SCRIPTS;
                } else {
                    assert(s_u8id_profile == U8ID_PROFILE_4);
                    // but not Latin with more than 2
                    if (ctx->count >= 2) {
                        ctx->last_cp = cp;
                        return U8ID_ERR_SCRIPTS;
                    }
                }
#endif
            }
        ok:
            basesc = scr;
            if (!u8ident_has_script_ctx(scr, ctx))
                u8ident_add_script_ctx(scr, ctx);
            // not is new, but still a possible greek confusable
        } else if (s_u8id_profile == U8ID_PROFILE_TR39_4 && scr == SC_Greek &&
                   has_latin && u8ident_is_greek_latin_confus(cp)) {
            ctx->last_cp = cp;
            return U8ID_ERR_CONFUS;
        } else if (scr != SC_Common && scr != SC_Inherited) {
            basesc = scr;
            base_cp = cp;
        } else {
            // Check illegal runs.
            // is_MARK(cp) is too slow, and we need the full GC for all cases
#if !defined U8ID_PROFILE
            if (s_u8id_profile < 5 || s_u8id_profile == U8ID_PROFILE_TR39_4)
#elif (U8ID_PROFILE < 5 || U8ID_PROFILE == TR39_4)
            if (1)
#else
            if (0)
#endif
            {
                const enum u8id_gc gc = u8ident_get_gc(cp);
                if (gc == GC_Mn || gc == GC_Me) {
                    if (cp == prev_cp) {
                        // TR39#5.4 "Forbid sequences of the same nonspacing mark"
                        ctx->last_cp = cp;
                        return U8ID_ERR_COMBINE;
                    } else if (++seq_mn > 4) {
                        // TR39#5.4 "Forbid sequences of more than 4 nonspacing marks (gc=Mn
                        // or gc=Me)"
                        ctx->last_cp = cp;
                        return U8ID_ERR_COMBINE;
                    } else if (!nsm_check(base_cp, cp)) {
                        // TR39#5.5 "Forbid sequences of base character + nonspacing mark
                        // that look the same as or confusingly similar to the base
                        // character alone"
                        ctx->last_cp = cp;
                        return U8ID_ERR_COMBINE;
                    }
                }
                // Allow Sm as first
                if (basesc == SC_Unknown &&
                    (gc == GC_Mn || gc == GC_Me || gc == GC_Mc)) {
                    // Disallow combiners without any base char (which do have a script)
                    ctx->last_cp = cp;
                    return U8ID_ERR_COMBINE;
                }
            }
        }

    next:
        prev_cp = cp;
        cp = dec_utf8(&s);
        if (likely(s <= e && cp != 0)) {
            // hardcode cont also? not yet
            if (unlikely(!isTR39_cont(cp) && !isTR39_start(cp))) {
                ctx->last_cp = cp;
                return U8ID_ERR_XID;
            }
            if (s == e && u8ident_is_MEDIAL(cp)) {
                ctx->last_cp = cp;
                return U8ID_ERR_XID;
            }
        }
    } while (s <= e);

#if !defined U8ID_PROFILE || U8ID_PROFILE == 6 || U8ID_PROFILE == C11_6
norm:
#endif
    if (need_normalize) {
        char *norm = u8ident_normalize((char *)buf, bufsz);
        if (!norm || strcmp(norm, buf)) {
            ctx->last_cp = 0;
            ret = U8ID_EOK_NORM | ret;
        }
        if (outnorm)
            *outnorm = norm;
        else
            free(norm);
    }
    return ret;
}
EXTERN enum u8id_errors u8ident_check(const uint8_t *string, char **outnorm) {
    return u8ident_check_buf((char *)string, strlen((char *)string), outnorm);
}

// ===== UTF-8 normalization =====

#if !defined U8ID_NORM || (U8ID_NORM != FCD)
char tmp_stack[128];
#endif

#define _UNICODE_MAX 0x10ffff

// UTF-8 helpers

/* from https://rosettacode.org/wiki/UTF-8_encode_and_decode#C
   taken from the safeclib
 */
typedef struct {
    uint8_t mask; /* char data will be bitwise AND with this */
    uint8_t lead; /* start bytes of current char in utf-8 encoded character */
    uint32_t beg; /* beginning of codepoint range */
    uint32_t end; /* end of codepoint range */
    int bits_stored; /* number of bits from the codepoint that fits in char */
} _utf_t;

static const _utf_t *utf[] = {
    // clang-format off
  /*             mask                 lead                beg      end    bits */
  [0] = &(_utf_t){0x3f/*0b00111111*/, 0x80/*0b10000000*/, 0,       0,        6},
  [1] = &(_utf_t){0x7f/*0b01111111*/, 0x00/*0b00000000*/, 0000,    0177,     7},
  [2] = &(_utf_t){0x1f/*0b00011111*/, 0xc0/*0b11000000*/, 0200,    03777,    5},
  [3] = &(_utf_t){0x0f/*0b00001111*/, 0xe0/*0b11100000*/, 04000,   0177777,  4},
  [4] = &(_utf_t){0x07/*0b00000111*/, 0xf0/*0b11110000*/, 0200000, 04177777, 3},
  &(_utf_t){0},
    // clang-format on
};

static int utf8_byte_len(const unsigned char ch) {
    int len = 0;
    for (_utf_t **u = (_utf_t **)utf; *u; ++u) {
        if ((ch & ~(*u)->mask) == (*u)->lead) {
            break;
        }
        ++len;
    }
#if 0 /* error handled in caller */
    if (len > 4) { /* Malformed leading byte */
        // "illegal UTF-8 character" EILSEQ
    }
#endif
    return len;
}

static int cp_len(const uint32_t cp) {
    int len = 0;
    for (_utf_t **u = (_utf_t **)utf; *u; ++u) {
        if ((cp >= (*u)->beg) && (cp <= (*u)->end)) {
            break;
        }
        ++len;
    }
#if 0 /* error handled in caller */
    if (len > 4) { /* Malformed leading byte */
        // "illegal UTF-8 character" EILSEQ
    }
#endif
    return len;
}

/* convert utf8 to unicode codepoint (to_cp) */
LOCAL uint32_t dec_utf8(char **strp) {
    const unsigned char *str = (const unsigned char *)*strp;
    int bytes = utf8_byte_len(*str);
    int shift;
    uint32_t cp;

    if (bytes > 4) {
        errno = EILSEQ;
        return 0;
    }
    shift = utf[0]->bits_stored * (bytes - 1);
    assert(shift >= 0);
    cp = (*str++ & utf[bytes]->mask) << shift;
    for (int i = 1; i < bytes; ++i, ++str) {
        shift -= utf[0]->bits_stored;
        assert(shift >= 0);
        cp |= (*str & utf[0]->mask) << shift;
    }
    *strp = (char *)str;
    return cp;
}

/* convert unicode codepoint to utf8 (to_utf8) */
LOCAL char *enc_utf8(char *dest, size_t *lenp, const uint32_t cp) {
    if (cp > _UNICODE_MAX) {
        errno = EILSEQ;
        *lenp = 0;
        return NULL;
    }
    const int bytes = cp_len(cp);

    if (bytes > 4) {
        errno = EILSEQ;
        *lenp = 0;
        return NULL;
    } else {
        int shift = utf[0]->bits_stored * (bytes - 1);
        assert(shift >= 0);
        dest[0] = (cp >> shift & utf[bytes]->mask) | utf[bytes]->lead;
        shift -= utf[0]->bits_stored;
        for (int i = 1; i < bytes; ++i) {
            assert(shift >= 0);
            dest[i] = (cp >> shift & utf[0]->mask) | utf[0]->lead;
            shift -= utf[0]->bits_stored;
        }
        *lenp = bytes;
        dest[bytes] = '\0';
        return dest;
    }
}

/* size of array for combining characters */
/* enough as an initial value? */
#define CC_SEQ_SIZE 10
#define CC_SEQ_STEP 5

#define ERR_ILSEQ -3
#define ERR_NOSPACE -2
#define ERR_INVAL -1
#define EOK 0

#if !defined U8ID_NORM || U8ID_NORM == NFKC || U8ID_NORM == NFKD

static inline int _bsearch_exc(const void *ptr1, const void *ptr2) {
    UN8IF_compat_exc_t *e1 = (UN8IF_compat_exc_t *)ptr1;
    UN8IF_compat_exc_t *e2 = (UN8IF_compat_exc_t *)ptr2;
    return e1->cp > e2->cp ? 1 : e1->cp == e2->cp ? 0
                                                  : -1;
}
#elif !defined U8ID_NORM || U8ID_NORM == NFC || U8ID_NORM == NFD || \
    U8ID_NORM == FCC || U8ID_NORM == FCD

static inline int _bsearch_exc(const void *ptr1, const void *ptr2) {
    UN8IF_canon_exc_t *e1 = (UN8IF_canon_exc_t *)ptr1;
    UN8IF_canon_exc_t *e2 = (UN8IF_canon_exc_t *)ptr2;
    return e1->cp > e2->cp ? 1 : e1->cp == e2->cp ? 0
                                                  : -1;
}
#endif

#if !defined U8ID_NORM || U8ID_NORM == NFC || U8ID_NORM == NFD || \
    U8ID_NORM == FCC || U8ID_NORM == FCD
/* Note that we can generate two versions of the tables.  The old format as
 * used in Unicode::Normalize, and the new 3x smaller NORMALIZE_IND_TBL cperl
 * variant, as used here and in cperl core since 5.27.2.
 * Return values:
 *   errors < 1 (see the ERR_* definitions)
 *   0: ok, passthru (not in table)
 *   >0: len of the returned utf-8 sequence
 */
static int _decomp_canonical_s(char *dest, size_t dmax, uint32_t cp) {
    /* the new format generated with cperl Unicode-Normalize/mkheader -uni -ind
   * -std
   */
    const UN8IF_canon_PLANE_T **plane, *row;
    if (unlikely(dmax < 8)) {
        *dest = 0;
        return ERR_NOSPACE;
    }
    plane = UN8IF_canon[cp >> 16];
    if (!plane) { /* Only the first 3 of 16 are filled */
        return EOK;
    }
    row = plane[(cp >> 8) & 0xff];
    if (row) { /* the first row is pretty filled, the rest very sparse */
        const UN8IF_canon_PLANE_T vi = row[cp & 0xff];
        if (!vi)
            return EOK;
#if UN8IF_canon_exc_size > 0
        /* overlong: search in extra list */
        else if (unlikely(vi == (uint16_t)-1)) {
            UN8IF_canon_exc_t *e;
            assert(UN8IF_canon_exc_size);
            e = (UN8IF_canon_exc_t *)bsearch(
                &cp, &UN8IF_canon_exc, UN8IF_canon_exc_size,
                sizeof(UN8IF_canon_exc[0]), _bsearch_exc);
            if (e) {
                size_t l = strlen(e->v);
                if (l + 1 > dmax) {
                    *dest = 0;
                    return ERR_NOSPACE;
                }
                memcpy(dest, e->v, l + 1); /* incl \0 */
                return (int)l;
            }
            return EOK;
        }
#endif
        else {
            /* value => length-index and offset */
            const int l = UN8IF_canon_LEN(vi);
            const int i = UN8IF_canon_IDX(vi);
            const char *tbl = (const char *)UN8IF_canon_tbl[l - 1];
            const int len = l;
#if defined(__DEBUG)
            printf("U+%04X vi=0x%x (>>12, &fff) => TBL(%d)|%d\n", cp, vi, l, i);
#endif
            assert(l > 0 && l <= UN8IF_canon_MAXLEN);
#if 0
            /* 13.0: tbl sizes: (917,763,227,36) */
            /* l: 1-4 */
            assert((l == 1 && i < 917) || (l == 2 && i < 763) ||
                   (l == 3 && i < 227) || (l == 4 && i < 36) || 0);
            assert(dmax > 4);
#endif
            memcpy(dest, &tbl[i * len], len); /* 33% perf */
            dest[len] = '\0';
            return len;
        }
    } else {
        return EOK;
    }
}
#endif // NFC, NFD, FCC, FCD

#if !defined U8ID_NORM || U8ID_NORM == NFKC || U8ID_NORM == NFKD
static int _decomp_compat_s(char *dest, size_t dmax, uint32_t cp) {
    /* the new format generated with cperl Unicode-Normalize/mkheader -uni -ind
   * -std
   */
    const UN8IF_compat_PLANE_T **plane, *row;
    plane = UN8IF_compat[cp >> 16];
    if (!plane) { /* Only the first 3 of 16 are filled */
        return EOK;
    }
    row = plane[(cp >> 8) & 0xff];
    if (row) { /* the first row is pretty filled, the rest very sparse */
        const UN8IF_compat_PLANE_T vi = row[cp & 0xff];
        if (!vi)
            return EOK;
#if UN8IF_compat_exc_size > 0
        else if (unlikely(vi ==
                          (uint16_t)-1)) { /* overlong: search in extra list */
            UN8IF_compat_exc_t *e;
            assert(UN8IF_compat_exc_size);
            e = (UN8IF_compat_exc_t *)bsearch(
                &cp, &UN8IF_compat_exc, UN8IF_compat_exc_size,
                sizeof(UN8IF_compat_exc[0]), _bsearch_exc);
            if (e) {
                size_t l = strlen(e->v);
                if (l + 1 > dmax) {
                    *dest = 0;
                    return ERR_NOSPACE;
                }
                memcpy(dest, e->v, l + 1); /* incl \0 */
                return (int)l;
            }
            return EOK;
#endif
        } else {
            /* value => length and index */
            const int l = UN8IF_compat_LEN(vi);
            const int i = UN8IF_compat_IDX(vi);
            const char *tbl = (const char *)UN8IF_compat_tbl[l - 1];
            if (unlikely(dmax < (size_t)l)) {
                *dest = 0;
                return ERR_NOSPACE;
            }
            memcpy(dest, &tbl[i * l], l);
            dest[l] = L'\0';
            return l;
        }
    } else {
        return EOK;
    }
}
#endif // NFKC or NFKD

static int _decomp_hangul_s(char *dest, size_t dmax, uint32_t cp) {
    uint32_t sindex = cp - Hangul_SBase;
    uint32_t lindex = sindex / Hangul_NCount;
    uint32_t vindex = (sindex % Hangul_NCount) / Hangul_TCount;
    uint32_t tindex = sindex % Hangul_TCount;
    size_t dlen;

    if (unlikely(dmax < 4)) {
        return ERR_NOSPACE;
    }

    // encode to UTF-8
    enc_utf8(dest, &dlen, lindex + Hangul_LBase);
    enc_utf8(&dest[1], &dlen, vindex + Hangul_VBase);
    if (tindex) {
        enc_utf8(&dest[2], &dlen, tindex + Hangul_TBase);
        return 3;
    }
    return 2;
}

/* codepoint canonical or compatible decomposition.
   dmax should be > 4,
   19 with the single arabic outlier U+FDFA for compat accepted
*/
static int _decomp_s(char *restrict dest, size_t dmax, const uint32_t cp,
                     const bool iscompat) {
    assert(dmax > 0);
    /* The costly is_HANGUL_cp_high(cp) checks also all composing chars.
     Hangul_IsS only for the valid start points. Which we can do here. */
    if (Hangul_IsS(cp)) {
        return _decomp_hangul_s(dest, dmax, cp);
    } else {
#if defined U8ID_NORM && (U8ID_NORM == NFC || U8ID_NORM == NFD || U8ID_NORM == FCC || U8ID_NORM == FCD)
        (void)iscompat;
        assert(!iscompat);
        return _decomp_canonical_s(dest, dmax, cp);
#elif defined U8ID_NORM && (U8ID_NORM == NFKC || U8ID_NORM == NFKD)
        (void)iscompat;
        assert(iscompat);
        return _decomp_compat_s(dest, dmax, cp);
#else
        return iscompat ? _decomp_compat_s(dest, dmax, cp)
                        : _decomp_canonical_s(dest, dmax, cp);
#endif
    }
}

/**
 * @def u8id_decompose_s(dest,dmax,src,lenp,iscompat)
 * @brief
 *    Converts the UTF-8 string to the NFD or NFKD normalization,
 *    as defined in the latest Unicode standard. The conversion
 *    stops at the first null or after dmax characters.
 *
 * @details
 *    Composed characters are checked for the left-hand-size of the
 *    Decomposition_Mapping Unicode property, which means the codepoint will
 *    be normalized if the sequence is composed.
 *    This is equivalent to all 1963 combining mark characters, plus some
 *    remaining 869 non-mark and non-hangul normalizables.  Hangul has some
 *    special normalization logic.
 */
int u8id_decompose_s(char *restrict dest, long dmax, char *restrict src,
                     size_t *restrict lenp, const bool iscompat) {
    size_t orig_dmax;
    const char *overlap_bumper;
    uint32_t cp;
    int c;

    if (lenp)
        *lenp = 0;
    if (unlikely(dest == NULL)) {
        return ERR_INVAL;
    }
    if (unlikely(src == NULL || dest == NULL || dmax == 0 || dmax < 2 ||
                 (unsigned)dmax > u8ident_maxlength())) {
        *dest = 0;
        return ERR_INVAL;
    }
    if (unlikely(dest == src)) {
        return ERR_INVAL;
    }
    if (unlikely(iscompat && dmax < 19)) {
        *dest = 0;
        return ERR_INVAL;
    }

    /* hold base of dest in case src was not copied */
    orig_dmax = (size_t)dmax;

    if (dest < src) {
        overlap_bumper = src;

        while (dmax > 0 && *src != 0) {
            const char *p = src;
            cp = dec_utf8((char **)&src);
            if (!cp)
                goto done;
            if (unlikely(dest == overlap_bumper)) {
                return ERR_INVAL;
            }
            if (unlikely(_UNICODE_MAX < cp)) {
                return ERR_ILSEQ;
            }

            c = _decomp_s(dest, dmax, cp, iscompat);
            if (c > 0) {
                dest += c;
                dmax -= c;
            } else if (c == 0) {
                if (cp < 128) {
                    *dest++ = cp;
                    dmax--;
                } else {
                    long len = src - p;
                    if (len > dmax) {
                        *dest = 0;
                        return ERR_NOSPACE;
                    }
                    memcpy(dest, p, len);
                    dest += len;
                    dmax -= len;
                }
            } else {
                return c;
            }
        }
    } else {
        overlap_bumper = dest;

        while (dmax > 0 && *src != 0) {
            const char *p = src;
            cp = dec_utf8((char **)&src);
            if (!cp)
                goto done;
            if (unlikely(src == overlap_bumper)) {
                return ERR_INVAL;
            }
            if (unlikely(_UNICODE_MAX < cp)) {
                return ERR_ILSEQ;
            }

            c = _decomp_s(dest, dmax, cp, iscompat);
            if (c > 0) {
                dest += c;
                dmax -= c;
            } else if (c == 0) {
                if (cp < 128) {
                    *dest++ = cp;
                    dmax--;
                } else {
                    long len = src - p;
                    if (len > dmax) {
                        *dest = 0;
                        return ERR_NOSPACE;
                    }
                    memcpy(dest, p, len);
                    dest += len;
                    dmax -= len;
                }
            } else {
                return c;
            }
        }
    }

done:
    if (lenp)
        *lenp = (size_t)((long)orig_dmax - dmax);
    if (dmax > 0) {
        *dest = 0;
        return 0;
    } else {
        return ERR_NOSPACE;
    }
}

#if !defined U8ID_NORM || U8ID_NORM != FCD

/* canonical ordering of combining characters (c.c.). */
typedef struct {
    uint8_t cc; /* combining class */
    uint32_t cp; /* codepoint */
    size_t pos; /* position */
} UN8IF_cc;

/* rc = u8id_reorder_s(tmp, len+1, dest); */
static inline int _compare_cc(const void *a, const void *b) {
    int ret_cc;
    ret_cc = ((UN8IF_cc *)a)->cc - ((UN8IF_cc *)b)->cc;
    if (ret_cc)
        return ret_cc;

    return (((UN8IF_cc *)a)->pos > ((UN8IF_cc *)b)->pos) -
        (((UN8IF_cc *)a)->pos < ((UN8IF_cc *)b)->pos);
}

static inline uint8_t _combin_class(uint32_t cp) {
    const STDCHAR **plane, *row;
    plane = UN8IF_combin[cp >> 16];
    if (!plane)
        return 0;
    row = plane[(cp >> 8) & 0xff];
    if (row)
        return row[cp & 0xff];
    else
        return 0;
}

/**
 * @def u8id_reorder_s(dest,dmax,src,len)
 *    Reorder all decomposed sequences in a UTF-8 string to NFD,
 *    as defined in the latest Unicode standard. The conversion
 *    stops at the first null or after dmax characters.
 */
static int u8id_reorder_s(unsigned char *restrict dest, long dmax,
                          const char *restrict src, const size_t len) {
    UN8IF_cc seq_ary[CC_SEQ_SIZE];
    UN8IF_cc *seq_ptr = (UN8IF_cc *)seq_ary; /* start with stack */
    UN8IF_cc *seq_ext = NULL; /* heap when needed */
    size_t seq_max = CC_SEQ_SIZE;
    size_t cc_pos = 0;
    char *p = (char *)src;
    const char *e = p + len;
    unsigned char *orig_dest = dest;
    size_t orig_dmax = dmax;

    while (p < e) {
        uint8_t cur_cc;
        uint32_t cp = dec_utf8(&p);
        size_t dlen;
        cur_cc = _combin_class(cp);
        if (cur_cc != 0) {
            if (seq_max < cc_pos + 1) { /* extend if need */
                seq_max = cc_pos + CC_SEQ_STEP; /* new size */
                if (CC_SEQ_SIZE == cc_pos) { /* seq_ary full */
                    seq_ext = (UN8IF_cc *)malloc(seq_max * sizeof(UN8IF_cc));
                    memcpy(seq_ext, seq_ary, cc_pos * sizeof(UN8IF_cc));
                } else {
                    seq_ext = (UN8IF_cc *)realloc(seq_ext, seq_max * sizeof(UN8IF_cc));
                }
                seq_ptr = seq_ext; /* use seq_ext from now */
            }

            seq_ptr[cc_pos].cc = cur_cc;
            seq_ptr[cc_pos].cp = cp;
            seq_ptr[cc_pos].pos = cc_pos;
            ++cc_pos;

            if (p < e)
                continue;
        }

        /* output */
        if (cc_pos) {
            if (unlikely(dmax - cc_pos <= 0)) {
                return ERR_NOSPACE;
            }

            if (cc_pos > 1) /* reorder if there are two Combining Classes */
                qsort((void *)seq_ptr, cc_pos, sizeof(UN8IF_cc), _compare_cc);

            for (size_t i = 0; i < cc_pos; i++) {
                enc_utf8((char *)dest, &dlen, seq_ptr[i].cp);
                dest += dlen;
                dmax -= dlen;
            }
            cc_pos = 0;
        }

        if (cur_cc == 0) {
            enc_utf8((char *)dest, &dlen, cp);
            dest += dlen;
            dmax -= dlen;
        }
        if (unlikely(dmax <= 0)) {
            memset(orig_dest, 0, orig_dmax);
            return ERR_NOSPACE;
        }
    }
    if (seq_ext)
        free(seq_ext);
    *dest = 0;
    // memset(dest, 0, dmax); // clear the slack?
    return 0;
}
#endif // != FCD

#if !defined U8ID_NORM || \
    !(U8ID_NORM == NFD || U8ID_NORM == NFKD || U8ID_NORM == FCD)
// #if !defined U8ID_NORM || U8ID_NORM == NFC || U8ID_NORM == FCC

static uint32_t _composite_cp(uint32_t cp, uint32_t cp2) {
    const UN8IF_complist_s ***plane, **row, *cell;

    if (unlikely(!cp2)) {
        return EOK;
    }
    if (unlikely((_UNICODE_MAX < cp) || (_UNICODE_MAX < cp2))) {
        return ERR_ILSEQ;
    }

    if (Hangul_IsL(cp) && Hangul_IsV(cp2)) {
        uint32_t lindex = cp - Hangul_LBase;
        uint32_t vindex = cp2 - Hangul_VBase;
        return (Hangul_SBase + (lindex * Hangul_VCount + vindex) * Hangul_TCount);
    }
    if (Hangul_IsLV(cp) && Hangul_IsT(cp2)) {
        uint32_t tindex = cp2 - Hangul_TBase;
        return (cp + tindex);
    }
    plane = UN8IF_compos[cp >> 16];
    if (!plane) { /* only 3 of 16 are defined */
        return 0;
    }
    row = plane[(cp >> 8) & 0xff];
    if (!row) { /* the zero plane is pretty filled, the others sparse */
        return 0;
    }
    cell = row[cp & 0xff];
    if (!cell) {
        return 0;
    }
    /* no indirection here, but search in the composition lists */
    /* only 16 lists 011099-01d1bc need uint32, the rest can be short, uint16 */
    /* TODO: above which length is bsearch faster?
     But then we'd need to store the lengths also */
    if (likely(cp < UN8IF_COMPLIST_FIRST_LONG)) {
        UN8IF_complist_s *i;
        for (i = (UN8IF_complist_s *)cell; i->nextchar; i++) {
            if ((uint16_t)cp2 == i->nextchar) {
                return (uint32_t)(i->composite);
            } else if ((uint16_t)cp2 < i->nextchar) { /* nextchar is sorted */
                break;
            }
        }
    } else {
        UN8IF_complist *i;
        // GCC_DIAG_IGNORE(-Wcast-align)
        for (i = (UN8IF_complist *)cell; i->nextchar; i++) {
            // GCC_DIAG_RESTORE
            if (cp2 == i->nextchar) {
                return i->composite;
            } else if (cp2 < i->nextchar) { /* nextchar is sorted */
                break;
            }
        }
    }
    return 0;
}

/**
 * @def u8id_compose_s(dest,dmax,src,lenp,iscontig)
 *    Combine all decomposed sequences in a wide string to NFC,
 *    as defined in the latest Unicode standard. The conversion
 *    stops at the first null or after dmax characters. */
/* combine decomposed sequences to NFC. */
/* iscontig = false; composeContiguous? FCC if true */
static int u8id_compose_s(char *restrict dest, long dmax,
                          const char *restrict src, size_t *restrict lenp,
                          const bool iscontig) {
    char *p = (char *)src;
    const char *e = p + *lenp;
    uint32_t cpS = 0; /* starter code point */
    bool valid_cpS = false; /* if false, cpS isn't initialized yet */
    uint8_t pre_cc = 0;

    uint32_t seq_ary[CC_SEQ_SIZE];
    uint32_t *seq_ptr = (uint32_t *)seq_ary; /* either stack or heap */
    uint32_t *seq_ext = NULL; /* heap */
    size_t seq_max = CC_SEQ_SIZE;
    size_t cc_pos = 0;
    // char *orig_dest = dest;
    const long orig_dmax = dmax;

    if (unlikely((unsigned)dmax > u8ident_maxlength())) {
        *lenp = 0;
        return ERR_INVAL;
    }

    while (p < e) {
        uint8_t cur_cc;
        uint32_t cp = dec_utf8(&p);
        size_t dlen;
        cur_cc = _combin_class(cp);

        if (!valid_cpS) {
            if (cur_cc == 0) {
                cpS = cp; /* found the first Starter */
                valid_cpS = true;
                if (p < e)
                    continue;
            } else {
                enc_utf8(dest, &dlen, cp);
                dest += dlen;
                dmax -= dlen;
                if (unlikely(dmax <= 0)) {
                    return ERR_NOSPACE;
                }
                continue;
            }
        } else {
            bool composed;

            /* blocked */
            if ((iscontig && cc_pos) || /* discontiguous combination (FCC) */
                (cur_cc != 0 && pre_cc == cur_cc) || /* blocked by same CC */
                (pre_cc > cur_cc)) /* blocked by higher CC: revised D2 */
                composed = false;

            /* not blocked:
           iscontig && cc_pos == 0      -- contiguous combination
           cur_cc == 0 && pre_cc == 0     -- starter + starter
           cur_cc != 0 && pre_cc < cur_cc  -- lower CC */
            else {
                /* try composition */
                uint32_t cpComp = _composite_cp(cpS, cp);
                if (cpComp && !isExclusion(cpComp)) {
                    cpS = cpComp;
                    composed = true;
                    /* pre_cc should not be changed to cur_cc */
                    /* e.g. 1E14 = 0045 0304 0300 where CC(0304) == CC(0300) */
                    if (p < e)
                        continue;
                } else
                    composed = false;
            }

            if (!composed) {
                pre_cc = cur_cc;
                if (cur_cc != 0 || !(p < e)) {
                    if (seq_max < cc_pos + 1) { /* extend if need */
                        seq_max = cc_pos + CC_SEQ_STEP; /* new size */
                        if (CC_SEQ_SIZE == cc_pos) { /* seq_ary full */
                            seq_ext = (uint32_t *)malloc(seq_max * sizeof(uint32_t));
                            memcpy(seq_ext, seq_ary, cc_pos * sizeof(uint32_t));
                        } else {
                            seq_ext =
                                (uint32_t *)realloc(seq_ext, seq_max * sizeof(uint32_t));
                        }
                        seq_ptr = seq_ext; /* use seq_ext from now */
                    }
                    seq_ptr[cc_pos] = cp;
                    ++cc_pos;
                }
                if (cur_cc != 0 && p < e)
                    continue;
            }
        }

        /* output */
        enc_utf8(dest, &dlen, cpS); /* starter (composed or not) */
        dest += dlen;
        dmax -= dlen;
        if (unlikely(dmax <= 0)) {
            return ERR_NOSPACE;
        }

        if (cc_pos == 1) {
            enc_utf8(dest, &dlen, *seq_ptr);
            dest += dlen;
            dmax -= dlen;
            cc_pos = 0;
        } else if (cc_pos > 1) {
            memcpy(dest, seq_ptr, cc_pos);
            dest += cc_pos;
            dmax -= cc_pos;
            cc_pos = 0;
        }

        cpS = cp;
    }
    if (seq_ext)
        free(seq_ext);

    // memset(dest, 0, dmax); // clear the slack?
    *lenp = orig_dmax - dmax;
    return 0;
}
#endif // !(NFD, NFKD, FCD)

/* Returns a freshly allocated normalized string, in the option defined at
 * `u8ident_init`. */
/* TODO: more stack allocations for dest throughout */
// clang-format off
GCC_DIAG_IGNORE(-Wreturn-local-addr)
// clang-format on
EXTERN char *u8ident_normalize(const char *src, int srcsz) {
#if !defined U8ID_NORM || U8ID_NORM != FCD
    char *tmp_ptr;
    char *tmp = NULL;
    size_t tmp_size;
#endif
    const enum u8id_norm mode = u8ident_norm();
    const bool iscompat = (mode == U8ID_NFKC || mode == U8ID_NFKD);

    size_t dmax = srcsz;
    char *dest = NULL;
    size_t destlen;
    int err;
    if (iscompat && dmax < 19)
        dmax = 10;

    do {
        dmax *= 2;
        dest = realloc(dest, dmax);
        memset(dest, 0, dmax); // not really needed.
        err = u8id_decompose_s(dest, dmax, (char *)src, &destlen, iscompat);
    } while (err == ERR_NOSPACE);
    if (err) {
        free(dest);
        return NULL;
    }
#if !defined U8ID_NORM || (U8ID_NORM != FCD)
    if (mode == U8ID_FCD)
#else
    if (1)
#endif
        return dest;

#if !defined U8ID_NORM || (U8ID_NORM != FCD)
    /* temp. scratch space, on stack or heap */
    if (destlen + 2 < 128) {
        tmp_ptr = tmp_stack;
        tmp_size = 128;
    } else {
        tmp_size = destlen + 2;
        tmp_ptr = tmp = (char *)malloc(tmp_size);
    }
    // now reorder for some canonalization (if required)
    err = u8id_reorder_s((unsigned char *)tmp_ptr, tmp_size, dest, destlen);
    while (err == ERR_NOSPACE) {
        tmp_size *= 2;
        if (tmp)
            tmp_ptr = tmp = (char *)realloc(tmp_ptr, tmp_size);
        else
            tmp_ptr = tmp = (char *)malloc(tmp_size);
        memset(tmp_ptr, 0, tmp_size); // not really needed
        err = u8id_reorder_s((unsigned char *)tmp_ptr, tmp_size, dest, destlen);
    }

    // if decomposed
#if !defined U8ID_NORM || !(U8ID_NORM == NFD || U8ID_NORM == NFKD)
    if (mode == U8ID_NFD || mode == U8ID_NFKD)
#else
    if (1)
#endif // NFD or NFKD
    {
        free(dest);
        if (tmp) // on heap
            return (char *)tmp_ptr;
        else { // cannot return our stack value
            tmp_ptr = (char *)malloc(strlen(tmp_stack) + 1);
            strcpy(tmp_ptr, tmp_stack);
            return (char *)tmp_ptr;
        }
    }

#if !defined U8ID_NORM || !(U8ID_NORM == NFD || U8ID_NORM == NFKD)
    // now compose to a shorter sequence
    err = u8id_compose_s(dest, dmax, tmp_ptr, &destlen, mode == U8ID_FCC);
    if (tmp)
        free(tmp);
    if (err) {
        free(dest);
        return NULL;
    }
#endif // !(NFD or NFKD)
#endif // !FCD
    return dest;
}


// ===== Compiler-facing Unicode API =====

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
