// SPDX-License-Identifier: LGPL-2.1-or-later
// Unicode identifier checking via libu8ident TR39
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>

#if __GNUC__ >= 3
#ifndef likely
#define likely(expr) __builtin_expect((long)((expr) != 0), 1)
#define unlikely(expr) __builtin_expect((long)((expr) != 0), 0)
#endif
#else
#ifndef likely
#define likely(expr) (expr)
#define unlikely(expr) (expr)
#endif
#endif

#define STDCHAR char
#define TRUE true
#define FALSE false
#define ARRAY_SIZE(x) sizeof(x) / sizeof(*x)
#define strEQc(s1, s2) !strcmp((s1), s2 "")

#define U8ID_UNICODE_MAJOR 17
#define U8ID_UNICODE_MINOR 0

#define U8ID_CTX_TRESH 5
#define U8ID_SCR_TRESH 8
struct ctx_t {
    uint8_t count;
    uint8_t has_han : 1;
    uint8_t is_japanese : 1;
    uint8_t is_chinese : 1;
    uint8_t is_korean : 1;
    uint8_t is_rtl : 1; // Hebrew or Arabic
    uint32_t last_cp; // only set on errors
    union {
        uint64_t scr64; // room for 8 scripts
        uint8_t scr8[U8ID_SCR_TRESH];
        // we need more than 8 only with insecure
        // profiles, or when we manually add extra scripts.
        uint8_t *u8p; // or if count > 8
    };
};

// clang-format off
#if (defined(__GNUC__) && ((__GNUC__ * 100) + __GNUC_MINOR__) >= 480)
#  define GCC_DIAG_PRAGMA(x) _Pragma (#x)
#  define GCC_DIAG_IGNORE(x)                                                  \
    GCC_DIAG_PRAGMA (GCC diagnostic ignored #x)
#else
#  define GCC_DIAG_IGNORE(w)
#endif

// from u8idnorm.c
static uint32_t dec_utf8(char **strp);
static char *enc_utf8(char *dest, size_t *lenp, const uint32_t cp);
typedef unsigned u8id_ctx_t;

/* Initialize the library for a tr39 profile, normalization */
static int u8ident_init();

/* maxlength of an identifier. Default: 1024. Beware that such longs
   identifiers, are not really identifiable anymore, and keep them under 80 or
   even less. Some filesystems do allow now 32K identifiers, which is a glaring
   security hole, waiting to be exploited */
/* Generates a new identifier document/context/directory, which
   initializes a new list of seen scripts. Contexts are optional, by
   default all checks are done in the same context 0. With compilers
   and interpreters a context is a source file, with filesystems a directory,
   with usernames you may choose if you need to support different languages at
   once.
   I cannot think of any such usage, so better avoid contexts with usernames to
   avoid mixups. */
static u8id_ctx_t u8ident_new_ctx(void);

/* Optionally adds a script to the context, if it's known or declared
   beforehand. Such as `use utf8 "Greek";` in cperl.

   All http://www.unicode.org/reports/tr31/#Table_Recommended_Scripts
   need not to be declared beforehand.

   Common Inherited Arabic Armenian Bengali Bopomofo Cyrillic
   Devanagari Ethiopic Georgian Greek Gujarati Gurmukhi Hangul Han Hebrew
   Hiragana Katakana Kannada Khmer Lao Latin Malayalam Myanmar Oriya
   Sinhala Tamil Telugu Thaana Thai Tibetan

   All http://www.unicode.org/reports/tr31/#Table_Limited_Use_Scripts need
   to be or are disallowed by profile:

   Adlam Balinese Bamum Batak Canadian_Aboriginal Chakma Cham Cherokee
   Hanifi_Rohingya Javanese Kayah_Li Lepcha Limbu Lisu Mandaic
   Meetei_Mayek Miao New_Tai_Lue Newa Nko Nyiakeng_Puachue_Hmong Ol_Chiki
   Osage Saurashtra Sundanese Syloti_Nagri Syriac Tai_Le Tai_Tham
   Tai_Viet Tifinagh Vai Wancho Yi Unknown

 */
static int u8ident_add_script(uint8_t script);

/* Deletes the context generated with `u8ident_new_ctx`. This is
   optional, all remaining contexts are deleted by `u8ident_free` */
static int u8ident_free_ctx(u8id_ctx_t ctx);

/* End this library, cleaning up all internal structures. */
static void u8ident_free(void);

/* Returns a freshly allocated normalized string, in the option defined at
   `u8ident_init`. Defaults to U8ID_NFC. */
static char *u8ident_normalize(const char *buf, int len);

enum u8id_errors {
    U8ID_EOK = 0, /* valid without need to normalize */
    U8ID_EOK_NORM = 1, /* valid with need to normalize */
    U8ID_EOK_WARN_CONFUS = 2, /* warn about confusable */
    U8ID_EOK_NORM_WARN_CONFUS =
        3, /* warn about confusable and need to normalize */
    U8ID_ERR_XID = -1, /* invalid xid, disallowed via IdentifierStatus.txt */
    U8ID_ERR_SCRIPT = -2, /* invalid script */
    U8ID_ERR_SCRIPTS = -3, /* invalid mixed scripts */
    U8ID_ERR_ENCODING = -4, /* invalid encoding */
    U8ID_ERR_COMBINE = -5, /* invalid combination of codepoints */
    U8ID_ERR_CONFUS = -6, /* invalid because confusable */
};

/* Two variants to check if this identifier is valid. With u8ident_check_buf
   buf does not need to be zero-terminated.

   Return values (enum u8id_errors):
    * 0   - valid without need to normalize.
    * 1   - valid with need to normalize.
    * 2   - warn about confusable
    * 3   - warn about confusable and need to normalize
    * -1  - invalid xid, disallowed via IdentifierStatus.txt
    * -2  - invalid script
    * -3  - invalid mixed scripts
    * -4  - invalid encoding
    * -5  - invalid combination of codepoints
    * -6  - invalid because confusable
    outnorm is set to a fresh normalized string if valid.

  Note that in the check we explicitly allow the Latin confusables: 0 1 I `
  i.e. U+30, U+31, U+49, U+60
*/
enum u8id_errors u8ident_check_buf(const char *buf, const int len,
                                   char **outnorm);

/* returns the failing codepoint, which failed in the last check. */
static uint32_t u8ident_failed_char(const u8id_ctx_t ctx);
/* returns the constant script name, which failed in the last check. */
static const char *u8ident_failed_script_name(const u8id_ctx_t ctx);

// More Function prototypes
static bool u8ident_has_script_ctx(const uint8_t scr, const struct ctx_t *ctx);
static int u8ident_add_script_ctx(const uint8_t scr, struct ctx_t *ctx);
static struct ctx_t *u8ident_ctx(void);
// member or bidi formatting characters for reordering attacks.
// Only valid with RTL scripts, such as Hebrew and Arabic.
static bool u8ident_is_bidi(const uint32_t cp);
// Greek letters confusable with Latin
static bool u8ident_is_greek_latin_confus(const uint32_t cp);
static const char *u8ident_script_name(const int scr);
static bool u8ident_maybe_normalized(const uint32_t cp);

enum u8id_sc {
// clang-format off
#define FIRST_RECOMMENDED_SCRIPT 0
  SC_Common     = 0,
  SC_Inherited  = 1,
  SC_Latin      = 2,
  SC_Arabic     = 3,
  SC_Armenian   = 4,
  SC_Bengali    = 5,
  SC_Bopomofo   = 6,
  SC_Cyrillic   = 7,
  SC_Devanagari = 8,
  SC_Ethiopic   = 9,
  SC_Georgian   = 10,
  SC_Greek      = 11,
  SC_Gujarati   = 12,
  SC_Gurmukhi   = 13,
  SC_Hangul     = 14,
  SC_Han        = 15,
  SC_Hebrew     = 16,
  SC_Hiragana   = 17,
  SC_Katakana   = 18,
  SC_Kannada    = 19,
  SC_Khmer      = 20,
  SC_Lao        = 21,
  SC_Malayalam  = 22,
  SC_Myanmar    = 23,
  SC_Oriya      = 24,
  SC_Sinhala    = 25,
  SC_Tamil      = 26,
  SC_Telugu     = 27,
  SC_Thaana     = 28,
  SC_Thai       = 29,
  SC_Tibetan    = 30,
#define FIRST_EXCLUDED_SCRIPT 31
  SC_Ahom       = 31,
  SC_Anatolian_Hieroglyphs = 32,
  SC_Avestan    = 33,
  SC_Bassa_Vah  = 34,
  SC_Beria_Erfe = 35,
  SC_Bhaiksuki  = 36,
  SC_Brahmi     = 37,
  SC_Braille    = 38,
  SC_Buginese   = 39,
  SC_Buhid      = 40,
  SC_Carian     = 41,
  SC_Caucasian_Albanian = 42,
  SC_Chorasmian = 43,
  SC_Coptic     = 44,
  SC_Cuneiform  = 45,
  SC_Cypriot    = 46,
  SC_Cypro_Minoan = 47,
  SC_Deseret    = 48,
  SC_Dives_Akuru = 49,
  SC_Dogra      = 50,
  SC_Duployan   = 51,
  SC_Egyptian_Hieroglyphs = 52,
  SC_Elbasan    = 53,
  SC_Elymaic    = 54,
  SC_Garay      = 55,
  SC_Glagolitic = 56,
  SC_Gothic     = 57,
  SC_Grantha    = 58,
  SC_Gunjala_Gondi = 59,
  SC_Gurung_Khema = 60,
  SC_Hanunoo    = 61,
  SC_Hatran     = 62,
  SC_Imperial_Aramaic = 63,
  SC_Inscriptional_Pahlavi = 64,
  SC_Inscriptional_Parthian = 65,
  SC_Kaithi     = 66,
  SC_Kawi       = 67,
  SC_Kharoshthi = 68,
  SC_Khitan_Small_Script = 69,
  SC_Khojki     = 70,
  SC_Khudawadi  = 71,
  SC_Kirat_Rai  = 72,
  SC_Linear_A   = 73,
  SC_Linear_B   = 74,
  SC_Lycian     = 75,
  SC_Lydian     = 76,
  SC_Mahajani   = 77,
  SC_Makasar    = 78,
  SC_Manichaean = 79,
  SC_Marchen    = 80,
  SC_Masaram_Gondi = 81,
  SC_Medefaidrin = 82,
  SC_Mende_Kikakui = 83,
  SC_Meroitic_Cursive = 84,
  SC_Meroitic_Hieroglyphs = 85,
  SC_Modi       = 86,
  SC_Mongolian  = 87,
  SC_Mro        = 88,
  SC_Multani    = 89,
  SC_Nabataean  = 90,
  SC_Nag_Mundari = 91,
  SC_Nandinagari = 92,
  SC_Nushu      = 93,
  SC_Ogham      = 94,
  SC_Ol_Onal    = 95,
  SC_Old_Hungarian = 96,
  SC_Old_Italic = 97,
  SC_Old_North_Arabian = 98,
  SC_Old_Permic = 99,
  SC_Old_Persian = 100,
  SC_Old_Sogdian = 101,
  SC_Old_South_Arabian = 102,
  SC_Old_Turkic = 103,
  SC_Old_Uyghur = 104,
  SC_Osmanya    = 105,
  SC_Pahawh_Hmong = 106,
  SC_Palmyrene  = 107,
  SC_Pau_Cin_Hau = 108,
  SC_Phags_Pa   = 109,
  SC_Phoenician = 110,
  SC_Psalter_Pahlavi = 111,
  SC_Rejang     = 112,
  SC_Runic      = 113,
  SC_Samaritan  = 114,
  SC_Sharada    = 115,
  SC_Shavian    = 116,
  SC_Siddham    = 117,
  SC_Sidetic    = 118,
  SC_SignWriting = 119,
  SC_Sogdian    = 120,
  SC_Sora_Sompeng = 121,
  SC_Soyombo    = 122,
  SC_Sunuwar    = 123,
  SC_Tagalog    = 124,
  SC_Tagbanwa   = 125,
  SC_Tai_Yo     = 126,
  SC_Takri      = 127,
  SC_Tangsa     = 128,
  SC_Tangut     = 129,
  SC_Tirhuta    = 130,
  SC_Todhri     = 131,
  SC_Tolong_Siki = 132,
  SC_Toto       = 133,
  SC_Tulu_Tigalari = 134,
  SC_Ugaritic   = 135,
  SC_Vithkuqi   = 136,
  SC_Warang_Citi = 137,
  SC_Yezidi     = 138,
  SC_Zanabazar_Square = 139,
#define FIRST_LIMITED_USE_SCRIPT 140
  SC_Adlam      = 140,
  SC_Balinese   = 141,
  SC_Bamum      = 142,
  SC_Batak      = 143,
  SC_Canadian_Aboriginal = 144,
  SC_Chakma     = 145,
  SC_Cham       = 146,
  SC_Cherokee   = 147,
  SC_Hanifi_Rohingya = 148,
  SC_Javanese   = 149,
  SC_Kayah_Li   = 150,
  SC_Lepcha     = 151,
  SC_Limbu      = 152,
  SC_Lisu       = 153,
  SC_Mandaic    = 154,
  SC_Meetei_Mayek = 155,
  SC_Miao       = 156,
  SC_New_Tai_Lue = 157,
  SC_Newa       = 158,
  SC_Nko        = 159,
  SC_Nyiakeng_Puachue_Hmong = 160,
  SC_Ol_Chiki   = 161,
  SC_Osage      = 162,
  SC_Saurashtra = 163,
  SC_Sundanese  = 164,
  SC_Syloti_Nagri = 165,
  SC_Syriac     = 166,
  SC_Tai_Le     = 167,
  SC_Tai_Tham   = 168,
  SC_Tai_Viet   = 169,
  SC_Tifinagh   = 170,
  SC_Vai        = 171,
  SC_Wancho     = 172,
  SC_Yi         = 173,
  SC_Unknown    = 174,
#define LAST_SCRIPT 174
    // clang-format on
};

struct range_bool {
    uint32_t from;
    uint32_t to;
};

struct range_short {
    uint32_t from;
    uint32_t to;
    uint16_t types;
};

struct nsm_ws {
    uint32_t nsm;
    wchar_t *letters;
};

/* All Combining Marks, sorted */
static const struct range_bool mark_list[] = {
    // clang-format off
    { 0x300, 0x36F },
    { 0x483, 0x489 },
    { 0x591, 0x5BD },
    { 0x5BF, 0x5BF },
    { 0x5C1, 0x5C2 },
    { 0x5C4, 0x5C5 },
    { 0x5C7, 0x5C7 },
    { 0x610, 0x61A },
    { 0x64B, 0x65F },
    { 0x670, 0x670 },
    { 0x6D6, 0x6DC },
    { 0x6DF, 0x6E4 },
    { 0x6E7, 0x6E8 },
    { 0x6EA, 0x6ED },
    { 0x711, 0x711 },
    { 0x730, 0x74A },
    { 0x7A6, 0x7B0 },
    { 0x7EB, 0x7F3 },
    { 0x7FD, 0x7FD },
    { 0x816, 0x819 },
    { 0x81B, 0x823 },
    { 0x825, 0x827 },
    { 0x829, 0x82D },
    { 0x859, 0x85B },
    { 0x897, 0x89F },
    { 0x8CA, 0x8E1 },
    { 0x8E3, 0x903 },
    { 0x93A, 0x93C },
    { 0x93E, 0x94F },
    { 0x951, 0x957 },
    { 0x962, 0x963 },
    { 0x981, 0x983 },
    { 0x9BC, 0x9BC },
    { 0x9BE, 0x9C4 },
    { 0x9C7, 0x9C8 },
    { 0x9CB, 0x9CD },
    { 0x9D7, 0x9D7 },
    { 0x9E2, 0x9E3 },
    { 0x9FE, 0x9FE },
    { 0xA01, 0xA03 },
    { 0xA3C, 0xA3C },
    { 0xA3E, 0xA42 },
    { 0xA47, 0xA48 },
    { 0xA4B, 0xA4D },
    { 0xA51, 0xA51 },
    { 0xA70, 0xA71 },
    { 0xA75, 0xA75 },
    { 0xA81, 0xA83 },
    { 0xABC, 0xABC },
    { 0xABE, 0xAC5 },
    { 0xAC7, 0xAC9 },
    { 0xACB, 0xACD },
    { 0xAE2, 0xAE3 },
    { 0xAFA, 0xAFF },
    { 0xB01, 0xB03 },
    { 0xB3C, 0xB3C },
    { 0xB3E, 0xB44 },
    { 0xB47, 0xB48 },
    { 0xB4B, 0xB4D },
    { 0xB55, 0xB57 },
    { 0xB62, 0xB63 },
    { 0xB82, 0xB82 },
    { 0xBBE, 0xBC2 },
    { 0xBC6, 0xBC8 },
    { 0xBCA, 0xBCD },
    { 0xBD7, 0xBD7 },
    { 0xC00, 0xC04 },
    { 0xC3C, 0xC3C },
    { 0xC3E, 0xC44 },
    { 0xC46, 0xC48 },
    { 0xC4A, 0xC4D },
    { 0xC55, 0xC56 },
    { 0xC62, 0xC63 },
    { 0xC81, 0xC83 },
    { 0xCBC, 0xCBC },
    { 0xCBE, 0xCC4 },
    { 0xCC6, 0xCC8 },
    { 0xCCA, 0xCCD },
    { 0xCD5, 0xCD6 },
    { 0xCE2, 0xCE3 },
    { 0xCF3, 0xCF3 },
    { 0xD00, 0xD03 },
    { 0xD3B, 0xD3C },
    { 0xD3E, 0xD44 },
    { 0xD46, 0xD48 },
    { 0xD4A, 0xD4D },
    { 0xD57, 0xD57 },
    { 0xD62, 0xD63 },
    { 0xD81, 0xD83 },
    { 0xDCA, 0xDCA },
    { 0xDCF, 0xDD4 },
    { 0xDD6, 0xDD6 },
    { 0xDD8, 0xDDF },
    { 0xDF2, 0xDF3 },
    { 0xE31, 0xE31 },
    { 0xE34, 0xE3A },
    { 0xE47, 0xE4E },
    { 0xEB1, 0xEB1 },
    { 0xEB4, 0xEBC },
    { 0xEC8, 0xECE },
    { 0xF18, 0xF19 },
    { 0xF35, 0xF35 },
    { 0xF37, 0xF37 },
    { 0xF39, 0xF39 },
    { 0xF3E, 0xF3F },
    { 0xF71, 0xF84 },
    { 0xF86, 0xF87 },
    { 0xF8D, 0xF97 },
    { 0xF99, 0xFBC },
    { 0xFC6, 0xFC6 },
    { 0x102B, 0x103E },
    { 0x1056, 0x1059 },
    { 0x105E, 0x1060 },
    { 0x1062, 0x1064 },
    { 0x1067, 0x106D },
    { 0x1071, 0x1074 },
    { 0x1082, 0x108D },
    { 0x108F, 0x108F },
    { 0x109A, 0x109D },
    { 0x135D, 0x135F },
    { 0x1712, 0x1715 },
    { 0x1732, 0x1734 },
    { 0x1752, 0x1753 },
    { 0x1772, 0x1773 },
    { 0x17B4, 0x17D3 },
    { 0x17DD, 0x17DD },
    { 0x180B, 0x180D },
    { 0x180F, 0x180F },
    { 0x1885, 0x1886 },
    { 0x18A9, 0x18A9 },
    { 0x1920, 0x192B },
    { 0x1930, 0x193B },
    { 0x1A17, 0x1A1B },
    { 0x1A55, 0x1A5E },
    { 0x1A60, 0x1A7C },
    { 0x1A7F, 0x1A7F },
    { 0x1AB0, 0x1ADD },
    { 0x1AE0, 0x1AEB },
    { 0x1B00, 0x1B04 },
    { 0x1B34, 0x1B44 },
    { 0x1B6B, 0x1B73 },
    { 0x1B80, 0x1B82 },
    { 0x1BA1, 0x1BAD },
    { 0x1BE6, 0x1BF3 },
    { 0x1C24, 0x1C37 },
    { 0x1CD0, 0x1CD2 },
    { 0x1CD4, 0x1CE8 },
    { 0x1CED, 0x1CED },
    { 0x1CF4, 0x1CF4 },
    { 0x1CF7, 0x1CF9 },
    { 0x1DC0, 0x1DFF },
    { 0x20D0, 0x20F0 },
    { 0x2CEF, 0x2CF1 },
    { 0x2D7F, 0x2D7F },
    { 0x2DE0, 0x2DFF },
    { 0x302A, 0x302F },
    { 0x3099, 0x309A },
    { 0xA66F, 0xA672 },
    { 0xA674, 0xA67D },
    { 0xA69E, 0xA69F },
    { 0xA6F0, 0xA6F1 },
    { 0xA802, 0xA802 },
    { 0xA806, 0xA806 },
    { 0xA80B, 0xA80B },
    { 0xA823, 0xA827 },
    { 0xA82C, 0xA82C },
    { 0xA880, 0xA881 },
    { 0xA8B4, 0xA8C5 },
    { 0xA8E0, 0xA8F1 },
    { 0xA8FF, 0xA8FF },
    { 0xA926, 0xA92D },
    { 0xA947, 0xA953 },
    { 0xA980, 0xA983 },
    { 0xA9B3, 0xA9C0 },
    { 0xA9E5, 0xA9E5 },
    { 0xAA29, 0xAA36 },
    { 0xAA43, 0xAA43 },
    { 0xAA4C, 0xAA4D },
    { 0xAA7B, 0xAA7D },
    { 0xAAB0, 0xAAB0 },
    { 0xAAB2, 0xAAB4 },
    { 0xAAB7, 0xAAB8 },
    { 0xAABE, 0xAABF },
    { 0xAAC1, 0xAAC1 },
    { 0xAAEB, 0xAAEF },
    { 0xAAF5, 0xAAF6 },
    { 0xABE3, 0xABEA },
    { 0xABEC, 0xABED },
    { 0xFB1E, 0xFB1E },
    { 0xFE00, 0xFE0F },
    { 0xFE20, 0xFE2F },
    { 0x101FD, 0x101FD },
    { 0x102E0, 0x102E0 },
    { 0x10376, 0x1037A },
    { 0x10A01, 0x10A03 },
    { 0x10A05, 0x10A06 },
    { 0x10A0C, 0x10A0F },
    { 0x10A38, 0x10A3A },
    { 0x10A3F, 0x10A3F },
    { 0x10AE5, 0x10AE6 },
    { 0x10D24, 0x10D27 },
    { 0x10D69, 0x10D6D },
    { 0x10EAB, 0x10EAC },
    { 0x10EFA, 0x10EFF },
    { 0x10F46, 0x10F50 },
    { 0x10F82, 0x10F85 },
    { 0x11000, 0x11002 },
    { 0x11038, 0x11046 },
    { 0x11070, 0x11070 },
    { 0x11073, 0x11074 },
    { 0x1107F, 0x11082 },
    { 0x110B0, 0x110BA },
    { 0x110C2, 0x110C2 },
    { 0x11100, 0x11102 },
    { 0x11127, 0x11134 },
    { 0x11145, 0x11146 },
    { 0x11173, 0x11173 },
    { 0x11180, 0x11182 },
    { 0x111B3, 0x111C0 },
    { 0x111C9, 0x111CC },
    { 0x111CE, 0x111CF },
    { 0x1122C, 0x11237 },
    { 0x1123E, 0x1123E },
    { 0x11241, 0x11241 },
    { 0x112DF, 0x112EA },
    { 0x11300, 0x11303 },
    { 0x1133B, 0x1133C },
    { 0x1133E, 0x11344 },
    { 0x11347, 0x11348 },
    { 0x1134B, 0x1134D },
    { 0x11357, 0x11357 },
    { 0x11362, 0x11363 },
    { 0x11366, 0x1136C },
    { 0x11370, 0x11374 },
    { 0x113B8, 0x113C0 },
    { 0x113C2, 0x113C2 },
    { 0x113C5, 0x113C5 },
    { 0x113C7, 0x113CA },
    { 0x113CC, 0x113D0 },
    { 0x113D2, 0x113D2 },
    { 0x113E1, 0x113E2 },
    { 0x11435, 0x11446 },
    { 0x1145E, 0x1145E },
    { 0x114B0, 0x114C3 },
    { 0x115AF, 0x115B5 },
    { 0x115B8, 0x115C0 },
    { 0x115DC, 0x115DD },
    { 0x11630, 0x11640 },
    { 0x116AB, 0x116B7 },
    { 0x1171D, 0x1172B },
    { 0x1182C, 0x1183A },
    { 0x11930, 0x11935 },
    { 0x11937, 0x11938 },
    { 0x1193B, 0x1193E },
    { 0x11940, 0x11940 },
    { 0x11942, 0x11943 },
    { 0x119D1, 0x119D7 },
    { 0x119DA, 0x119E0 },
    { 0x119E4, 0x119E4 },
    { 0x11A01, 0x11A0A },
    { 0x11A33, 0x11A39 },
    { 0x11A3B, 0x11A3E },
    { 0x11A47, 0x11A47 },
    { 0x11A51, 0x11A5B },
    { 0x11A8A, 0x11A99 },
    { 0x11B60, 0x11B67 },
    { 0x11C2F, 0x11C36 },
    { 0x11C38, 0x11C3F },
    { 0x11C92, 0x11CA7 },
    { 0x11CA9, 0x11CB6 },
    { 0x11D31, 0x11D36 },
    { 0x11D3A, 0x11D3A },
    { 0x11D3C, 0x11D3D },
    { 0x11D3F, 0x11D45 },
    { 0x11D47, 0x11D47 },
    { 0x11D8A, 0x11D8E },
    { 0x11D90, 0x11D91 },
    { 0x11D93, 0x11D97 },
    { 0x11EF3, 0x11EF6 },
    { 0x11F00, 0x11F01 },
    { 0x11F03, 0x11F03 },
    { 0x11F34, 0x11F3A },
    { 0x11F3E, 0x11F42 },
    { 0x11F5A, 0x11F5A },
    { 0x13440, 0x13440 },
    { 0x13447, 0x13455 },
    { 0x1611E, 0x1612F },
    { 0x16AF0, 0x16AF4 },
    { 0x16B30, 0x16B36 },
    { 0x16F4F, 0x16F4F },
    { 0x16F51, 0x16F87 },
    { 0x16F8F, 0x16F92 },
    { 0x16FE4, 0x16FE4 },
    { 0x16FF0, 0x16FF1 },
    { 0x1BC9D, 0x1BC9E },
    { 0x1CF00, 0x1CF2D },
    { 0x1CF30, 0x1CF46 },
    { 0x1D165, 0x1D169 },
    { 0x1D16D, 0x1D172 },
    { 0x1D17B, 0x1D182 },
    { 0x1D185, 0x1D18B },
    { 0x1D1AA, 0x1D1AD },
    { 0x1D242, 0x1D244 },
    { 0x1DA00, 0x1DA36 },
    { 0x1DA3B, 0x1DA6C },
    { 0x1DA75, 0x1DA75 },
    { 0x1DA84, 0x1DA84 },
    { 0x1DA9B, 0x1DA9F },
    { 0x1DAA1, 0x1DAAF },
    { 0x1E000, 0x1E006 },
    { 0x1E008, 0x1E018 },
    { 0x1E01B, 0x1E021 },
    { 0x1E023, 0x1E024 },
    { 0x1E026, 0x1E02A },
    { 0x1E08F, 0x1E08F },
    { 0x1E130, 0x1E136 },
    { 0x1E2AE, 0x1E2AE },
    { 0x1E2EC, 0x1E2EF },
    { 0x1E4EC, 0x1E4EF },
    { 0x1E5EE, 0x1E5EF },
    { 0x1E6E3, 0x1E6E3 },
    { 0x1E6E6, 0x1E6E6 },
    { 0x1E6EE, 0x1E6EF },
    { 0x1E6F5, 0x1E6F5 },
    { 0x1E8D0, 0x1E8D6 },
    { 0x1E944, 0x1E94A },
    { 0xE0100, 0xE01EF },
    // clang-format on
};

/* All letters with non-spacing combining marks, sorted.
   The first entry is the NSM, if letters exist.
 */
static const struct nsm_ws nsm_letters[] = {
    // clang-format off
    { 0x0300,  /* NSM: GRAVE 300 */
      L"\u00C0\u00C8\u00CC\u00D2\u00D9\u00E0\u00E8\u00EC\u00F2\u00F9\u01DB\u01DC\u01F8\u01F9\u0400\u040D\u0450\u045D\u1E14\u1E15\u1E50\u1E51\u1E80\u1E81\u1EA6\u1EA7\u1EB0\u1EB1\u1EC0\u1EC1\u1ED2\u1ED3\u1EDC\u1EDD\u1EEA\u1EEB\u1EF2\u1EF3\u1F02\u1F03\u1F0A\u1F0B\u1F12\u1F13\u1F1A\u1F1B\u1F22\u1F23\u1F2A\u1F2B\u1F32\u1F33\u1F3A\u1F3B\u1F42\u1F43\u1F4A\u1F4B\u1F52\u1F53\u1F5B\u1F62\u1F63\u1F6A\u1F6B\u1F70\u1F72\u1F74\u1F76\u1F78\u1F7A\u1F7C\u1FBA\u1FC8\u1FCA\u1FD2\u1FDA\u1FE2\u1FEA\u1FF8\u1FFA" },
      /* ÀÈÌÒÙàèìòùǛǜǸǹЀЍѐѝḔḕṐṑẀẁẦầẰằỀềỒồỜờỪừỲỳἂἃἊἋἒἓἚἛἢἣἪἫἲἳἺἻὂὃὊὋὒὓὛὢὣὪὫὰὲὴὶὸὺὼᾺῈῊῒῚῢῪῸῺ */
    { 0x0301,  /* NSM: ACUTE 301 */
      L"\u00C1\u00C9\u00CD\u00D3\u00DA\u00DD\u00E1\u00E9\u00ED\u00F3\u00FA\u00FD\u0106\u0107\u0139\u013A\u0143\u0144\u0154\u0155\u015A\u015B\u0179\u017A\u01D7\u01D8\u01F4\u01F5\u01FA\u01FB\u01FC\u01FD\u01FE\u01FF\u0386\u0388\u0389\u038A\u038C\u038E\u038F\u0390\u03AC\u03AD\u03AE\u03AF\u03B0\u03CC\u03CD\u03CE\u03D3\u0403\u040C\u0453\u045C\u1E08\u1E09\u1E16\u1E17\u1E2E\u1E2F\u1E30\u1E31\u1E3E\u1E3F\u1E4C\u1E4D\u1E52\u1E53\u1E54\u1E55\u1E78\u1E79\u1E82\u1E83\u1EA4\u1EA5\u1EAE\u1EAF\u1EBE\u1EBF\u1ED0\u1ED1\u1EDA\u1EDB\u1EE8\u1EE9\u1F04\u1F05\u1F0C\u1F0D\u1F14\u1F15\u1F1C\u1F1D\u1F24\u1F25\u1F2C\u1F2D\u1F34\u1F35\u1F3C\u1F3D\u1F44\u1F45\u1F4C\u1F4D\u1F54\u1F55\u1F5D\u1F64\u1F65\u1F6C\u1F6D" },
      /* ÁÉÍÓÚÝáéíóúýĆćĹĺŃńŔŕŚśŹźǗǘǴǵǺǻǼǽǾǿΆΈΉΊΌΎΏΐάέήίΰόύώϓЃЌѓќḈḉḖḗḮḯḰḱḾḿṌṍṒṓṔṕṸṹẂẃẤấẮắẾếỐốỚớỨứἄἅἌἍἔἕἜἝἤἥἬἭἴἵἼἽὄὅὌὍὔὕὝὤὥὬὭ */
    { 0x0302,  /* NSM: CIRCUMFLEX 302 */
      L"\u00C2\u00CA\u00CE\u00D4\u00DB\u00E2\u00EA\u00EE\u00F4\u00FB\u0108\u0109\u011C\u011D\u0124\u0125\u0134\u0135\u015C\u015D\u0174\u0175\u0176\u0177\u1E90\u1E91\u1EAC\u1EAD\u1EC6\u1EC7\u1ED8\u1ED9" },
      /* ÂÊÎÔÛâêîôûĈĉĜĝĤĥĴĵŜŝŴŵŶŷẐẑẬậỆệỘộ */
    { 0x0303,  /* NSM: TILDE 303 */
      L"\u00C3\u00D1\u00D5\u00E3\u00F1\u00F5\u0128\u0129\u0168\u0169\u1E7C\u1E7D\u1EAA\u1EAB\u1EB4\u1EB5\u1EBC\u1EBD\u1EC4\u1EC5\u1ED6\u1ED7\u1EE0\u1EE1\u1EEE\u1EEF\u1EF8\u1EF9" },
      /* ÃÑÕãñõĨĩŨũṼṽẪẫẴẵẼẽỄễỖỗỠỡỮữỸỹ */
    { 0x0304,  /* NSM: MACRON 304 */
      L"\u0100\u0101\u0112\u0113\u012A\u012B\u014C\u014D\u016A\u016B\u01D5\u01D6\u01DE\u01DF\u01E0\u01E1\u01E2\u01E3\u01EC\u01ED\u022A\u022B\u022C\u022D\u0230\u0231\u0232\u0233\u04E2\u04E3\u04EE\u04EF\u1E20\u1E21\u1E38\u1E39\u1E5C\u1E5D\u1FB1\u1FB9\u1FD1\u1FD9\u1FE1\u1FE9" },
      /* ĀāĒēĪīŌōŪūǕǖǞǟǠǡǢǣǬǭȪȫȬȭȰȱȲȳӢӣӮӯḠḡḸḹṜṝᾱᾹῑῙῡῩ */
    { 0x0306,  /* NSM: BREVE 306 */
      L"\u0102\u0103\u0114\u0115\u011E\u011F\u012C\u012D\u014E\u014F\u016C\u016D\u040E\u0419\u0439\u045E\u04C1\u04C2\u04D0\u04D1\u04D6\u04D7\u1E1C\u1E1D\u1EB6\u1EB7\u1FB0\u1FB8\u1FD0\u1FD8\u1FE0\u1FE8" },
      /* ĂăĔĕĞğĬĭŎŏŬŭЎЙйўӁӂӐӑӖӗḜḝẶặᾰᾸῐῘῠῨ */
    { 0x0307,  /* NSM: DOT ABOVE 307 */
      L"\u010A\u010B\u0116\u0117\u0120\u0121\u0130\u017B\u017C\u0226\u0227\u022E\u022F\u06A7\u06AC\u06B6\u06BF\u06CF\u0762\u0765\u087A\u1DA1\u1E02\u1E03\u1E0A\u1E0B\u1E1E\u1E1F\u1E22\u1E23\u1E40\u1E41\u1E44\u1E45\u1E56\u1E57\u1E58\u1E59\u1E60\u1E61\u1E64\u1E65\u1E66\u1E67\u1E68\u1E69\u1E6A\u1E6B\u1E86\u1E87\u1E8A\u1E8B\u1E8E\u1E8F\u1E9B\u312E\U000105C9\U000105E4\U00010798\U00010EB0" },
      /* ĊċĖėĠġİŻżȦȧȮȯڧڬڶڿۏݢݥࡺᶡḂḃḊḋḞḟḢḣṀṁṄṅṖṗṘṙṠṡṤṥṦṧṨṩṪṫẆẇẊẋẎẏẛㄮ𐗉𐗤𐞘𐺰 */
    { 0x0308,  /* NSM: DIAERESIS 308 */
      L"\u00C4\u00CB\u00CF\u00D6\u00DC\u00E4\u00EB\u00EF\u00F6\u00FC\u00FF\u0178\u03AA\u03AB\u03CA\u03CB\u03D4\u0401\u0407\u0451\u0457\u04D2\u04D3\u04DA\u04DB\u04DC\u04DD\u04DE\u04DF\u04E4\u04E5\u04E6\u04E7\u04EA\u04EB\u04EC\u04ED\u04F0\u04F1\u04F4\u04F5\u04F8\u04F9\u1DF2\u1DF3\u1DF4\u1E26\u1E27\u1E4E\u1E4F\u1E7A\u1E7B\u1E84\u1E85\u1E8C\u1E8D\u1E97" },
      /* ÄËÏÖÜäëïöüÿŸΪΫϊϋϔЁЇёїӒӓӚӛӜӝӞӟӤӥӦӧӪӫӬӭӰӱӴӵӸӹᷲᷳᷴḦḧṎṏṺṻẄẅẌẍẗ */
    { 0x0309,  /* NSM: HOOK ABOVE 309 */
      L"\u1EA2\u1EA3\u1EA8\u1EA9\u1EB2\u1EB3\u1EBA\u1EBB\u1EC2\u1EC3\u1EC8\u1EC9\u1ECE\u1ECF\u1ED4\u1ED5\u1EDE\u1EDF\u1EE6\u1EE7\u1EEC\u1EED\u1EF6\u1EF7" },
      /* ẢảẨẩẲẳẺẻỂểỈỉỎỏỔổỞởỦủỬửỶỷ */
    { 0x030A,  /* NSM: RING ABOVE 30A */
      L"\u00C5\u00E5\u016E\u016F\u088F\u1E98\u1E99" },
      /* ÅåŮů࢏ẘẙ */
    { 0x030B,  /* NSM: DOUBLE ACUTE 30B */
      L"\u0150\u0151\u0170\u0171\u04F2\u04F3" },
      /* ŐőŰűӲӳ */
    { 0x030C,  /* NSM: HACEK 30C */
      L"\u010C\u010D\u010E\u010F\u011A\u011B\u013D\u013E\u0147\u0148\u0158\u0159\u0160\u0161\u0164\u0165\u017D\u017E\u01CD\u01CE\u01CF\u01D0\u01D1\u01D2\u01D3\u01D4\u01D9\u01DA\u01E6\u01E7\u01E8\u01E9\u01EE\u01EF\u01F0\u021E\u021F" },
      /* ČčĎďĚěĽľŇňŘřŠšŤťŽžǍǎǏǐǑǒǓǔǙǚǦǧǨǩǮǯǰȞȟ */
    { 0x030F,  /* NSM: DOUBLE GRAVE 30F */
      L"\u0200\u0201\u0204\u0205\u0208\u0209\u020C\u020D\u0210\u0211\u0214\u0215\u0476\u0477" },
      /* ȀȁȄȅȈȉȌȍȐȑȔȕѶѷ */
    { 0x0311,  /* NSM: INVERTED BREVE 311 */
      L"\u0202\u0203\u0206\u0207\u020A\u020B\u020E\u020F\u0212\u0213\u0216\u0217" },
      /* ȂȃȆȇȊȋȎȏȒȓȖȗ */
    { 0x0313,  /* NSM: COMMA ABOVE 313 */
      L"\u1F00\u1F08\u1F10\u1F18\u1F20\u1F28\u1F30\u1F38\u1F40\u1F48\u1F50\u1F60\u1F68\u1FE4" },
      /* ἀἈἐἘἠἨἰἸὀὈὐὠὨῤ */
    { 0x0314,  /* NSM: REVERSED COMMA ABOVE 314 */
      L"\u1F01\u1F09\u1F11\u1F19\u1F21\u1F29\u1F31\u1F39\u1F41\u1F49\u1F51\u1F59\u1F61\u1F69\u1FE5\u1FEC" },
      /* ἁἉἑἙἡἩἱἹὁὉὑὙὡὩῥῬ */
    { 0x031B,  /* NSM: HORN 31B */
      L"\u01A0\u01A1\u01AF\u01B0" },
      /* ƠơƯư */
    { 0x0323,  /* NSM: DOT BELOW 323 */
      L"\u068A\u0694\u06A3\u06B9\u06FA\u06FB\u06FC\u0766\u088B\u08A5\u08B4\u1E04\u1E05\u1E0C\u1E0D\u1E24\u1E25\u1E32\u1E33\u1E36\u1E37\u1E42\u1E43\u1E46\u1E47\u1E5A\u1E5B\u1E62\u1E63\u1E6C\u1E6D\u1E7E\u1E7F\u1E88\u1E89\u1E92\u1E93\u1EA0\u1EA1\u1EB8\u1EB9\u1ECA\u1ECB\u1ECC\u1ECD\u1EE2\u1EE3\u1EE4\u1EE5\u1EF0\u1EF1\u1EF4\u1EF5\U0001BC26" },
      /* ڊڔڣڹۺۻۼݦࢋࢥࢴḄḅḌḍḤḥḲḳḶḷṂṃṆṇṚṛṢṣṬṭṾṿẈẉẒẓẠạẸẹỊịỌọỢợỤụỰựỴỵ𛰦 */
    { 0x0324,  /* NSM: DOUBLE DOT BELOW 324 */
      L"\u1E72\u1E73" },
      /* Ṳṳ */
    { 0x0325,  /* NSM: RING BELOW 325 */
      L"\u1E00\u1E01" },
      /* Ḁḁ */
    { 0x0326,  /* NSM: COMMA BELOW 326 */
      L"\u0218\u0219\u021A\u021B" },
      /* ȘșȚț */
    { 0x0327,  /* NSM: CEDILLA 327 */
      L"\u00C7\u00E7\u0122\u0123\u0136\u0137\u013B\u013C\u0145\u0146\u0156\u0157\u015E\u015F\u0162\u0163\u0228\u0229\u1E10\u1E11\u1E28\u1E29" },
      /* ÇçĢģĶķĻļŅņŖŗŞşŢţȨȩḐḑḨḩ */
    { 0x0328,  /* NSM: OGONEK 328 */
      L"\u0104\u0105\u0118\u0119\u012E\u012F\u0172\u0173\u01EA\u01EB" },
      /* ĄąĘęĮįŲųǪǫ */
    { 0x032D,  /* NSM: CIRCUMFLEX BELOW 32D */
      L"\u1E12\u1E13\u1E18\u1E19\u1E3C\u1E3D\u1E4A\u1E4B\u1E70\u1E71\u1E76\u1E77" },
      /* ḒḓḘḙḼḽṊṋṰṱṶṷ */
    { 0x032E,  /* NSM: BREVE BELOW 32E */
      L"\u1E2A\u1E2B" },
      /* Ḫḫ */
    { 0x0330,  /* NSM: TILDE BELOW 330 */
      L"\u1E1A\u1E1B\u1E2C\u1E2D\u1E74\u1E75" },
      /* ḚḛḬḭṴṵ */
    { 0x0331,  /* NSM: MACRON BELOW 331 */
      L"\u1E06\u1E07\u1E0E\u1E0F\u1E34\u1E35\u1E3A\u1E3B\u1E48\u1E49\u1E5E\u1E5F\u1E6E\u1E6F\u1E94\u1E95\u1E96" },
      /* ḆḇḎḏḴḵḺḻṈṉṞṟṮṯẔẕẖ */
    { 0x20DB,  /* NSM: THREE DOTS ABOVE 20DB */
      L"\u063F\u0685\u069E\u069F\u06A0\u06A8\u06B4\u06B7\u06BD\u0763\u08A7\u08C3\u08C4\u08C5" },
      /* ؿڅڞڟڠڨڴڷڽݣࢧࣃࣄࣅ */
    { 0x20DC,  /* NSM: FOUR DOTS ABOVE 20DC */
      L"\u0690\u0699\u075C" },
      /* ڐڙݜ */
    { 0x3099,  /* NSM: KATAKANA-HIRAGANA VOICED SOUND MARK 3099 */
      L"\u304C\u304E\u3050\u3052\u3054\u3056\u3058\u305A\u305C\u305E\u3060\u3062\u3065\u3067\u3069\u3070\u3073\u3076\u3079\u307C\u3094\u309E\u30AC\u30AE\u30B0\u30B2\u30B4\u30B6\u30B8\u30BA\u30BC\u30BE\u30C0\u30C2\u30C5\u30C7\u30C9\u30D0\u30D3\u30D6\u30D9\u30DC\u30F4\u30F7\u30F8\u30F9\u30FA\u30FE\uFF9E" },
      /* がぎぐげござじずぜぞだぢづでどばびぶべぼゔゞガギグゲゴザジズゼゾダヂヅデドバビブベボヴヷヸヹヺヾﾞ */
    { 0x309A,  /* NSM: KATAKANA-HIRAGANA SEMI-VOICED SOUND MARK 309A */
      L"\u3071\u3074\u3077\u307A\u307D\u30D1\u30D4\u30D7\u30DA\u30DD\uFF9F" },
      /* ぱぴぷぺぽパピプペポﾟ */
    // clang-format on
};

enum u8id_gc {
    GC_Cc,
    GC_Cf,
    GC_Co,
    GC_Cs,
    GC_Ll,
    GC_Lm,
    GC_Lo,
    GC_Lt,
    GC_Lu,
    GC_Mc,
    GC_Me,
    GC_Mn,
    GC_Nd,
    GC_Nl,
    GC_No,
    GC_Pc,
    GC_Pd,
    GC_Pe,
    GC_Pf,
    GC_Pi,
    GC_Po,
    GC_Ps,
    GC_Sc,
    GC_Sk,
    GC_Sm,
    GC_So,
    GC_Zl,
    GC_Zp,
    GC_Zs,
    GC_L, // is L&, all letters. (for unitr39.h only)
    GC_V, // is varying, eg L or S. (for unitr39.h only),
    GC_INVALID,
};

struct sc_tr39 {
    uint32_t from;
    uint32_t to;
    enum u8id_sc sc;
    enum u8id_gc gc;
    // maxsize: Beng Deva Dogr Gong Gonm Gran Gujr Guru Knda Limb
    //          Mahj Mlym Nand Orya Sind Sinh Sylo Takr Taml Telu Tirh
    const char *scx;
};

// Filtering allowed scripts, XID_Start, Skipped Ids, !MEDIAL and NFC.
// Ranges split on GC and SCX changes
static const struct sc_tr39 tr39_start_list[] = {
#ifdef ALLOW_DOLLAR
    {'$', '$', SC_Latin, GC_Sc, NULL},
#endif
    {'A', 'Z', SC_Latin, GC_Lu, NULL},
    {'_', '_', SC_Latin, GC_Pc, NULL},
    {'a', 'z', SC_Latin, GC_Ll, NULL},
    {0xC0, 0xD6, SC_Latin, GC_Lu, NULL}, //  À..Ö
    {0xD8, 0xF6, SC_Latin, GC_L, NULL}, //  Ø..ö
    {0xF8, 0x113, SC_Latin, GC_L, NULL}, //  ø..ē
    {0x116, 0x12B, SC_Latin, GC_L, NULL}, //  Ė..ī
    {0x12E, 0x131, SC_Latin, GC_L, NULL}, //  Į..ı
    {0x134, 0x137, SC_Latin, GC_L, NULL}, //  Ĵ..ķ
    {0x139, 0x13E, SC_Latin, GC_L, NULL}, //  Ĺ..ľ
    {0x141, 0x148, SC_Latin, GC_L, NULL}, //  Ł..ň
    {0x14A, 0x14D, SC_Latin, GC_L, NULL}, //  Ŋ..ō
    {0x150, 0x155, SC_Latin, GC_L, NULL}, //  Ő..ŕ
    {0x158, 0x161, SC_Latin, GC_L, NULL}, //  Ř..š
    {0x164, 0x17E, SC_Latin, GC_L, NULL}, //  Ť..ž
    {0x180, 0x181, SC_Latin, GC_L, NULL}, //  ƀ..Ɓ
    {0x186, 0x186, SC_Latin, GC_Lu, NULL}, //  Ɔ
    {0x189, 0x18A, SC_Latin, GC_Lu, NULL}, //  Ɖ..Ɗ
    {0x18E, 0x192, SC_Latin, GC_L, NULL}, //  Ǝ..ƒ
    {0x194, 0x194, SC_Latin, GC_Lu, NULL}, //  Ɣ
    {0x196, 0x199, SC_Latin, GC_L, NULL}, //  Ɩ..ƙ
    {0x19D, 0x19D, SC_Latin, GC_Lu, NULL}, //  Ɲ
    {0x1A0, 0x1A1, SC_Latin, GC_L, NULL}, //  Ơ..ơ
    {0x1AF, 0x1B0, SC_Latin, GC_L, NULL}, //  Ư..ư
    {0x1B2, 0x1B4, SC_Latin, GC_L, NULL}, //  Ʋ..ƴ
    {0x1B7, 0x1B7, SC_Latin, GC_Lu, NULL}, //  Ʒ
    {0x1CD, 0x1D4, SC_Latin, GC_L, NULL}, //  Ǎ..ǔ
    {0x1DD, 0x1DD, SC_Latin, GC_Ll, NULL}, //  ǝ
    {0x1E6, 0x1E9, SC_Latin, GC_L, NULL}, //  Ǧ..ǩ
    {0x1EE, 0x1EF, SC_Latin, GC_L, NULL}, //  Ǯ..ǯ
    {0x1F8, 0x1F9, SC_Latin, GC_L, NULL}, //  Ǹ..ǹ
    {0x200, 0x21B, SC_Latin, GC_L, NULL}, //  Ȁ..ț
    {0x234, 0x236, SC_Latin, GC_Ll, NULL}, //  ȴ..ȶ
    {0x244, 0x244, SC_Latin, GC_Lu, NULL}, //  Ʉ
    {0x24C, 0x24D, SC_Latin, GC_L, NULL}, //  Ɍ..ɍ
    {0x250, 0x276, SC_Latin, GC_Ll, NULL}, //  ɐ..ɶ
    {0x278, 0x27B, SC_Latin, GC_Ll, NULL}, //  ɸ..ɻ
    {0x27D, 0x29D, SC_Latin, GC_L, NULL}, //  ɽ..ʝ
    {0x29F, 0x2AF, SC_Latin, GC_Ll, NULL}, //  ʟ..ʯ
    // SPLIT on SCX (prev to U+2C1)
    {0x2B9, 0x2BB, SC_Common, GC_Lm, NULL}, //  ʹ..ʻ
    // SPLIT on SCX (prev to U+2C1)
    {0x2BC, 0x2BC, SC_Common, GC_Lm, "\x05\x07\x08\x02\x99\x1d\x85"}, //Bengali,Cyrillic,Devanagari,Latin,Lisu,Thai,Toto //  ʼ
    {0x2BD, 0x2C1, SC_Common, GC_Lm, NULL}, //  ʽ..ˁ
    // SPLIT on SCX (prev to U+2D1)
    {0x2C6, 0x2C6, SC_Common, GC_Lm, NULL}, //  ˆ
    // SPLIT on SCX (prev to U+2D1)
    {0x2C7, 0x2C7, SC_Common, GC_Lm, "\x06\x02"}, //Bopomofo,Latin //  ˇ
    // SPLIT on SCX (prev to U+2D1)
    {0x2C8, 0x2C8, SC_Common, GC_Lm, NULL}, //  ˈ
    // SPLIT on SCX (prev to U+2D1)
    {0x2C9, 0x2CB, SC_Common, GC_Lm, "\x06\x02"}, //Bopomofo,Latin //  ˉ..ˋ
    // SPLIT on SCX (prev to U+2D1)
    {0x2CC, 0x2CC, SC_Common, GC_Lm, NULL}, //  ˌ
    // SPLIT on SCX (prev to U+2D1)
    {0x2CD, 0x2CD, SC_Common, GC_Lm, "\x02\x99"}, //Latin,Lisu //  ˍ
    {0x2CE, 0x2D1, SC_Common, GC_Lm, NULL}, //  ˎ..ˑ
    {0x2EC, 0x2EC, SC_Common, GC_Lm, NULL}, //  ˬ
    {0x2EE, 0x2EE, SC_Common, GC_Lm, NULL}, //  ˮ
    {0x386, 0x386, SC_Greek, GC_Lu, NULL}, //  Ά
    {0x388, 0x38A, SC_Greek, GC_Lu, NULL}, //  Έ..Ί
    {0x38C, 0x38C, SC_Greek, GC_Lu, NULL}, //  Ό
    {0x38E, 0x3A1, SC_Greek, GC_L, NULL}, //  Ύ..Ρ
    {0x3A3, 0x3CF, SC_Greek, GC_L, NULL}, //  Σ..Ϗ
    {0x3D7, 0x3D7, SC_Greek, GC_Ll, NULL}, //  ϗ
    {0x401, 0x40C, SC_Cyrillic, GC_Lu, NULL}, //  Ё..Ќ
    {0x40E, 0x44F, SC_Cyrillic, GC_L, NULL}, //  Ў..я
    {0x451, 0x45C, SC_Cyrillic, GC_Ll, NULL}, //  ё..ќ
    {0x45E, 0x45F, SC_Cyrillic, GC_Ll, NULL}, //  ў..џ
    {0x490, 0x49B, SC_Cyrillic, GC_L, NULL}, //  Ґ..қ
    {0x49E, 0x4A5, SC_Cyrillic, GC_L, NULL}, //  Ҟ..ҥ
    {0x4A8, 0x4B7, SC_Cyrillic, GC_L, NULL}, //  Ҩ..ҷ
    {0x4BA, 0x4C0, SC_Cyrillic, GC_L, NULL}, //  Һ..Ӏ
    {0x4CF, 0x4D9, SC_Cyrillic, GC_L, NULL}, //  ӏ..ә
    {0x4DC, 0x4E9, SC_Cyrillic, GC_L, NULL}, //  Ӝ..ө
    {0x4EE, 0x4F5, SC_Cyrillic, GC_L, NULL}, //  Ӯ..ӵ
    {0x4F8, 0x4F9, SC_Cyrillic, GC_L, NULL}, //  Ӹ..ӹ
    {0x524, 0x525, SC_Cyrillic, GC_L, NULL}, //  Ԥ..ԥ
    {0x531, 0x556, SC_Armenian, GC_Lu, NULL}, //  Ա..Ֆ
    {0x559, 0x559, SC_Armenian, GC_Lm, NULL}, //  ՙ
    {0x560, 0x586, SC_Armenian, GC_Ll, NULL}, //  ՠ..ֆ
    {0x588, 0x588, SC_Armenian, GC_Ll, NULL}, //  ֈ
    {0x5D0, 0x5EA, SC_Hebrew, GC_Lo, NULL}, //  א..ת
    {0x620, 0x63A, SC_Arabic, GC_Lo, NULL}, //  ؠ..غ
    {0x63D, 0x63D, SC_Arabic, GC_Lo, NULL}, //  ؽ
    {0x641, 0x64A, SC_Arabic, GC_Lo, NULL}, //  ف..ي
    {0x671, 0x672, SC_Arabic, GC_Lo, NULL}, //  ٱ..ٲ
    {0x674, 0x674, SC_Arabic, GC_Lo, NULL}, //  ٴ
    {0x679, 0x68F, SC_Arabic, GC_Lo, NULL}, //  ٹ..ڏ
    {0x691, 0x69A, SC_Arabic, GC_Lo, NULL}, //  ڑ..ښ
    {0x69F, 0x6A0, SC_Arabic, GC_Lo, NULL}, //  ڟ..ڠ
    {0x6A2, 0x6A2, SC_Arabic, GC_Lo, NULL}, //  ڢ
    {0x6A4, 0x6AB, SC_Arabic, GC_Lo, NULL}, //  ڤ..ګ
    {0x6AD, 0x6B1, SC_Arabic, GC_Lo, NULL}, //  ڭ..ڱ
    {0x6B3, 0x6B3, SC_Arabic, GC_Lo, NULL}, //  ڳ
    {0x6B5, 0x6B7, SC_Arabic, GC_Lo, NULL}, //  ڵ..ڷ
    {0x6BA, 0x6BE, SC_Arabic, GC_Lo, NULL}, //  ں..ھ
    {0x6C0, 0x6D3, SC_Arabic, GC_Lo, NULL}, //  ۀ..ۓ
    {0x6D5, 0x6D5, SC_Arabic, GC_Lo, NULL}, //  ە
    {0x6E5, 0x6E6, SC_Arabic, GC_Lm, NULL}, //  ۥ..ۦ
    {0x6EE, 0x6EF, SC_Arabic, GC_Lo, NULL}, //  ۮ..ۯ
    {0x6FF, 0x6FF, SC_Arabic, GC_Lo, NULL}, //  ۿ
    {0x751, 0x752, SC_Arabic, GC_Lo, NULL}, //  ݑ..ݒ
    {0x756, 0x756, SC_Arabic, GC_Lo, NULL}, //  ݖ
    {0x760, 0x760, SC_Arabic, GC_Lo, NULL}, //  ݠ
    {0x762, 0x763, SC_Arabic, GC_Lo, NULL}, //  ݢ..ݣ
    {0x766, 0x768, SC_Arabic, GC_Lo, NULL}, //  ݦ..ݨ
    {0x76A, 0x76A, SC_Arabic, GC_Lo, NULL}, //  ݪ
    {0x76E, 0x771, SC_Arabic, GC_Lo, NULL}, //  ݮ..ݱ
    {0x780, 0x7A5, SC_Thaana, GC_Lo, NULL}, //  ހ..ޥ
    {0x7B1, 0x7B1, SC_Thaana, GC_Lo, NULL}, //  ޱ
    {0x870, 0x887, SC_Arabic, GC_Lo, NULL}, //  ࡰ..ࢇ
    {0x88F, 0x88F, SC_Arabic, GC_Lo, NULL}, //  ࢏
    {0x8A0, 0x8A0, SC_Arabic, GC_Lo, NULL}, //  ࢠ
    {0x8A2, 0x8A9, SC_Arabic, GC_Lo, NULL}, //  ࢢ..ࢩ
    {0x8BB, 0x8C2, SC_Arabic, GC_Lo, NULL}, //  ࢻ..ࣂ
    {0x8C7, 0x8C7, SC_Arabic, GC_Lo, NULL}, //  ࣇ
    {0x8C9, 0x8C9, SC_Arabic, GC_Lm, NULL}, //  ࣉ
    {0x905, 0x90B, SC_Devanagari, GC_Lo, NULL}, //  अ..ऋ
    {0x90D, 0x928, SC_Devanagari, GC_Lo, NULL}, //  ऍ..न
    {0x92A, 0x933, SC_Devanagari, GC_Lo, NULL}, //  प..ळ
    {0x935, 0x939, SC_Devanagari, GC_Lo, NULL}, //  व..ह
    {0x950, 0x950, SC_Devanagari, GC_Lo, NULL}, //  ॐ
    {0x972, 0x977, SC_Devanagari, GC_Lo, NULL}, //  ॲ..ॷ
    {0x97B, 0x97F, SC_Devanagari, GC_Lo, NULL}, //  ॻ..ॿ
    {0x985, 0x98B, SC_Bengali, GC_Lo, NULL}, //  অ..ঋ
    {0x98F, 0x990, SC_Bengali, GC_Lo, NULL}, //  এ..ঐ
    {0x993, 0x9A8, SC_Bengali, GC_Lo, NULL}, //  ও..ন
    {0x9AA, 0x9B0, SC_Bengali, GC_Lo, NULL}, //  প..র
    {0x9B2, 0x9B2, SC_Bengali, GC_Lo, NULL}, //  ল
    {0x9B6, 0x9B9, SC_Bengali, GC_Lo, NULL}, //  শ..হ
    {0x9CE, 0x9CE, SC_Bengali, GC_Lo, NULL}, //  ৎ
    {0x9F0, 0x9F1, SC_Bengali, GC_Lo, NULL}, //  ৰ..ৱ
    {0xA05, 0xA0A, SC_Gurmukhi, GC_Lo, NULL}, //  ਅ..ਊ
    {0xA0F, 0xA10, SC_Gurmukhi, GC_Lo, NULL}, //  ਏ..ਐ
    {0xA13, 0xA28, SC_Gurmukhi, GC_Lo, NULL}, //  ਓ..ਨ
    {0xA2A, 0xA30, SC_Gurmukhi, GC_Lo, NULL}, //  ਪ..ਰ
    {0xA32, 0xA32, SC_Gurmukhi, GC_Lo, NULL}, //  ਲ
    {0xA35, 0xA35, SC_Gurmukhi, GC_Lo, NULL}, //  ਵ
    {0xA38, 0xA39, SC_Gurmukhi, GC_Lo, NULL}, //  ਸ..ਹ
    {0xA5C, 0xA5C, SC_Gurmukhi, GC_Lo, NULL}, //  ੜ
    {0xA74, 0xA74, SC_Gurmukhi, GC_Lo, NULL}, //  ੴ
    {0xA85, 0xA8D, SC_Gujarati, GC_Lo, NULL}, //  અ..ઍ
    {0xA8F, 0xA91, SC_Gujarati, GC_Lo, NULL}, //  એ..ઑ
    {0xA93, 0xAA8, SC_Gujarati, GC_Lo, NULL}, //  ઓ..ન
    {0xAAA, 0xAB0, SC_Gujarati, GC_Lo, NULL}, //  પ..ર
    {0xAB2, 0xAB3, SC_Gujarati, GC_Lo, NULL}, //  લ..ળ
    {0xAB5, 0xAB9, SC_Gujarati, GC_Lo, NULL}, //  વ..હ
    {0xAD0, 0xAD0, SC_Gujarati, GC_Lo, NULL}, //  ૐ
    {0xB05, 0xB0B, SC_Oriya, GC_Lo, NULL}, //  ଅ..ଋ
    {0xB0F, 0xB10, SC_Oriya, GC_Lo, NULL}, //  ଏ..ଐ
    {0xB13, 0xB28, SC_Oriya, GC_Lo, NULL}, //  ଓ..ନ
    {0xB2A, 0xB30, SC_Oriya, GC_Lo, NULL}, //  ପ..ର
    {0xB32, 0xB33, SC_Oriya, GC_Lo, NULL}, //  ଲ..ଳ
    {0xB36, 0xB39, SC_Oriya, GC_Lo, NULL}, //  ଶ..ହ
    {0xB5F, 0xB5F, SC_Oriya, GC_Lo, NULL}, //  ୟ
    {0xB71, 0xB71, SC_Oriya, GC_Lo, NULL}, //  ୱ
    {0xB83, 0xB83, SC_Tamil, GC_Lo, NULL}, //  ஃ
    {0xB85, 0xB8A, SC_Tamil, GC_Lo, NULL}, //  அ..ஊ
    {0xB8E, 0xB90, SC_Tamil, GC_Lo, NULL}, //  எ..ஐ
    {0xB92, 0xB95, SC_Tamil, GC_Lo, NULL}, //  ஒ..க
    {0xB99, 0xB9A, SC_Tamil, GC_Lo, NULL}, //  ங..ச
    {0xB9C, 0xB9C, SC_Tamil, GC_Lo, NULL}, //  ஜ
    {0xB9E, 0xB9F, SC_Tamil, GC_Lo, NULL}, //  ஞ..ட
    {0xBA3, 0xBA4, SC_Tamil, GC_Lo, NULL}, //  ண..த
    {0xBA8, 0xBAA, SC_Tamil, GC_Lo, NULL}, //  ந..ப
    {0xBAE, 0xBB9, SC_Tamil, GC_Lo, NULL}, //  ம..ஹ
    {0xBD0, 0xBD0, SC_Tamil, GC_Lo, NULL}, //  ௐ
    {0xC05, 0xC0B, SC_Telugu, GC_Lo, NULL}, //  అ..ఋ
    {0xC0E, 0xC10, SC_Telugu, GC_Lo, NULL}, //  ఎ..ఐ
    {0xC12, 0xC28, SC_Telugu, GC_Lo, NULL}, //  ఒ..న
    {0xC2A, 0xC30, SC_Telugu, GC_Lo, NULL}, //  ప..ర
    {0xC32, 0xC33, SC_Telugu, GC_Lo, NULL}, //  ల..ళ
    {0xC35, 0xC39, SC_Telugu, GC_Lo, NULL}, //  వ..హ
    {0xC85, 0xC8B, SC_Kannada, GC_Lo, NULL}, //  ಅ..ಋ
    {0xC8E, 0xC90, SC_Kannada, GC_Lo, NULL}, //  ಎ..ಐ
    {0xC92, 0xCA8, SC_Kannada, GC_Lo, NULL}, //  ಒ..ನ
    {0xCAA, 0xCB0, SC_Kannada, GC_Lo, NULL}, //  ಪ..ರ
    {0xCB2, 0xCB3, SC_Kannada, GC_Lo, NULL}, //  ಲ..ಳ
    {0xCB5, 0xCB9, SC_Kannada, GC_Lo, NULL}, //  ವ..ಹ
    {0xD05, 0xD0B, SC_Malayalam, GC_Lo, NULL}, //  അ..ഋ
    {0xD0E, 0xD10, SC_Malayalam, GC_Lo, NULL}, //  എ..ഐ
    {0xD12, 0xD28, SC_Malayalam, GC_Lo, NULL}, //  ഒ..ന
    {0xD2A, 0xD39, SC_Malayalam, GC_Lo, NULL}, //  പ..ഹ
    {0xD7A, 0xD7F, SC_Malayalam, GC_Lo, NULL}, //  ൺ..ൿ
    {0xD85, 0xD8D, SC_Sinhala, GC_Lo, NULL}, //  අ..ඍ
    {0xD91, 0xD96, SC_Sinhala, GC_Lo, NULL}, //  එ..ඖ
    {0xD9A, 0xD9D, SC_Sinhala, GC_Lo, NULL}, //  ක..ඝ
    {0xD9F, 0xDB1, SC_Sinhala, GC_Lo, NULL}, //  ඟ..න
    {0xDB3, 0xDBB, SC_Sinhala, GC_Lo, NULL}, //  ඳ..ර
    {0xDBD, 0xDBD, SC_Sinhala, GC_Lo, NULL}, //  ල
    {0xDC0, 0xDC6, SC_Sinhala, GC_Lo, NULL}, //  ව..ෆ
    {0xE01, 0xE30, SC_Thai, GC_Lo, NULL}, //  ก..ะ
    {0xE32, 0xE32, SC_Thai, GC_Lo, NULL}, //  า
    {0xE40, 0xE46, SC_Thai, GC_L, NULL}, //  เ..ๆ
    {0xE81, 0xE82, SC_Lao, GC_Lo, NULL}, //  ກ..ຂ
    {0xE84, 0xE84, SC_Lao, GC_Lo, NULL}, //  ຄ
    {0xE87, 0xE88, SC_Lao, GC_Lo, NULL}, //  ງ..ຈ
    {0xE8A, 0xE8A, SC_Lao, GC_Lo, NULL}, //  ຊ
    {0xE8D, 0xE8D, SC_Lao, GC_Lo, NULL}, //  ຍ
    {0xE94, 0xE97, SC_Lao, GC_Lo, NULL}, //  ດ..ທ
    {0xE99, 0xE9F, SC_Lao, GC_Lo, NULL}, //  ນ..ຟ
    {0xEA1, 0xEA3, SC_Lao, GC_Lo, NULL}, //  ມ..ຣ
    {0xEA5, 0xEA5, SC_Lao, GC_Lo, NULL}, //  ລ
    {0xEA7, 0xEA7, SC_Lao, GC_Lo, NULL}, //  ວ
    {0xEAA, 0xEAB, SC_Lao, GC_Lo, NULL}, //  ສ..ຫ
    {0xEAD, 0xEB0, SC_Lao, GC_Lo, NULL}, //  ອ..ະ
    {0xEB2, 0xEB2, SC_Lao, GC_Lo, NULL}, //  າ
    {0xEBD, 0xEBD, SC_Lao, GC_Lo, NULL}, //  ຽ
    {0xEC0, 0xEC4, SC_Lao, GC_Lo, NULL}, //  ເ..ໄ
    {0xEC6, 0xEC6, SC_Lao, GC_Lm, NULL}, //  ໆ
    {0xF00, 0xF00, SC_Tibetan, GC_Lo, NULL}, //  ༀ
    {0xF40, 0xF42, SC_Tibetan, GC_Lo, NULL}, //  ཀ..ག
    {0xF44, 0xF47, SC_Tibetan, GC_Lo, NULL}, //  ང..ཇ
    {0xF49, 0xF4C, SC_Tibetan, GC_Lo, NULL}, //  ཉ..ཌ
    {0xF4E, 0xF51, SC_Tibetan, GC_Lo, NULL}, //  ཎ..ད
    {0xF53, 0xF56, SC_Tibetan, GC_Lo, NULL}, //  ན..བ
    {0xF58, 0xF5B, SC_Tibetan, GC_Lo, NULL}, //  མ..ཛ
    {0xF5D, 0xF68, SC_Tibetan, GC_Lo, NULL}, //  ཝ..ཨ
    {0x1000, 0x102A, SC_Myanmar, GC_Lo, NULL}, //  က..ဪ
    {0x103F, 0x103F, SC_Myanmar, GC_Lo, NULL}, //  ဿ
    {0x105A, 0x105D, SC_Myanmar, GC_Lo, NULL}, //  ၚ..ၝ
    {0x1061, 0x1061, SC_Myanmar, GC_Lo, NULL}, //  ၡ
    {0x1075, 0x1081, SC_Myanmar, GC_Lo, NULL}, //  ၵ..ႁ
    {0x10C7, 0x10C7, SC_Georgian, GC_Lu, NULL}, //  Ⴧ
    {0x10CD, 0x10CD, SC_Georgian, GC_Lu, NULL}, //  Ⴭ
    {0x10D0, 0x10F0, SC_Georgian, GC_Ll, NULL}, //  ა..ჰ
    {0x1200, 0x1206, SC_Ethiopic, GC_Lo, NULL}, //  ሀ..ሆ
    {0x1208, 0x1248, SC_Ethiopic, GC_Lo, NULL}, //  ለ..ቈ
    {0x124A, 0x124D, SC_Ethiopic, GC_Lo, NULL}, //  ቊ..ቍ
    {0x1250, 0x1256, SC_Ethiopic, GC_Lo, NULL}, //  ቐ..ቖ
    {0x1258, 0x1258, SC_Ethiopic, GC_Lo, NULL}, //  ቘ
    {0x125A, 0x125D, SC_Ethiopic, GC_Lo, NULL}, //  ቚ..ቝ
    {0x1260, 0x1286, SC_Ethiopic, GC_Lo, NULL}, //  በ..ኆ
    {0x1288, 0x1288, SC_Ethiopic, GC_Lo, NULL}, //  ኈ
    {0x128A, 0x128D, SC_Ethiopic, GC_Lo, NULL}, //  ኊ..ኍ
    {0x1290, 0x12AE, SC_Ethiopic, GC_Lo, NULL}, //  ነ..ኮ
    {0x12B0, 0x12B0, SC_Ethiopic, GC_Lo, NULL}, //  ኰ
    {0x12B2, 0x12B5, SC_Ethiopic, GC_Lo, NULL}, //  ኲ..ኵ
    {0x12B8, 0x12BE, SC_Ethiopic, GC_Lo, NULL}, //  ኸ..ኾ
    {0x12C0, 0x12C0, SC_Ethiopic, GC_Lo, NULL}, //  ዀ
    {0x12C2, 0x12C5, SC_Ethiopic, GC_Lo, NULL}, //  ዂ..ዅ
    {0x12C8, 0x12D6, SC_Ethiopic, GC_Lo, NULL}, //  ወ..ዖ
    {0x12D8, 0x12F7, SC_Ethiopic, GC_Lo, NULL}, //  ዘ..ዷ
    {0x1300, 0x130E, SC_Ethiopic, GC_Lo, NULL}, //  ጀ..ጎ
    {0x1310, 0x1310, SC_Ethiopic, GC_Lo, NULL}, //  ጐ
    {0x1312, 0x1315, SC_Ethiopic, GC_Lo, NULL}, //  ጒ..ጕ
    {0x1318, 0x131E, SC_Ethiopic, GC_Lo, NULL}, //  ጘ..ጞ
    {0x1320, 0x1346, SC_Ethiopic, GC_Lo, NULL}, //  ጠ..ፆ
    {0x1348, 0x1359, SC_Ethiopic, GC_Lo, NULL}, //  ፈ..ፙ
    {0x1780, 0x179C, SC_Khmer, GC_Lo, NULL}, //  ក..វ
    {0x179F, 0x17A2, SC_Khmer, GC_Lo, NULL}, //  ស..អ
    {0x17A5, 0x17A7, SC_Khmer, GC_Lo, NULL}, //  ឥ..ឧ
    {0x17AA, 0x17B3, SC_Khmer, GC_Lo, NULL}, //  ឪ..ឳ
    {0x1C90, 0x1CBA, SC_Georgian, GC_Lu, NULL}, //  Ა..Ჺ
    {0x1CBD, 0x1CBF, SC_Georgian, GC_Lu, NULL}, //  Ჽ..Ჿ
    {0x1D00, 0x1D25, SC_Latin, GC_Ll, NULL}, //  ᴀ..ᴥ
    {0x1D27, 0x1D2A, SC_Greek, GC_Ll, NULL}, //  ᴧ..ᴪ
    {0x1D2F, 0x1D2F, SC_Latin, GC_Lm, NULL}, //  ᴯ
    {0x1D3B, 0x1D3B, SC_Latin, GC_Lm, NULL}, //  ᴻ
    {0x1D4E, 0x1D4E, SC_Latin, GC_Lm, NULL}, //  ᵎ
    {0x1D6B, 0x1D77, SC_Latin, GC_Ll, NULL}, //  ᵫ..ᵷ
    {0x1D79, 0x1D9A, SC_Latin, GC_Ll, NULL}, //  ᵹ..ᶚ
    {0x1E00, 0x1E01, SC_Latin, GC_L, NULL}, //  Ḁ..ḁ
    {0x1E0C, 0x1E0D, SC_Latin, GC_L, NULL}, //  Ḍ..ḍ
    {0x1E12, 0x1E13, SC_Latin, GC_L, NULL}, //  Ḓ..ḓ
    {0x1E18, 0x1E1B, SC_Latin, GC_L, NULL}, //  Ḙ..ḛ
    {0x1E20, 0x1E21, SC_Latin, GC_L, NULL}, //  Ḡ..ḡ
    {0x1E24, 0x1E25, SC_Latin, GC_L, NULL}, //  Ḥ..ḥ
    {0x1E2A, 0x1E2D, SC_Latin, GC_L, NULL}, //  Ḫ..ḭ
    {0x1E36, 0x1E37, SC_Latin, GC_L, NULL}, //  Ḷ..ḷ
    {0x1E3C, 0x1E3F, SC_Latin, GC_L, NULL}, //  Ḽ..ḿ
    {0x1E42, 0x1E4B, SC_Latin, GC_L, NULL}, //  Ṃ..ṋ
    {0x1E5A, 0x1E5B, SC_Latin, GC_L, NULL}, //  Ṛ..ṛ
    {0x1E62, 0x1E63, SC_Latin, GC_L, NULL}, //  Ṣ..ṣ
    {0x1E6C, 0x1E6D, SC_Latin, GC_L, NULL}, //  Ṭ..ṭ
    {0x1E70, 0x1E77, SC_Latin, GC_L, NULL}, //  Ṱ..ṷ
    {0x1E8C, 0x1E8D, SC_Latin, GC_L, NULL}, //  Ẍ..ẍ
    {0x1E92, 0x1E93, SC_Latin, GC_L, NULL}, //  Ẓ..ẓ
    {0x1E9C, 0x1EFF, SC_Latin, GC_L, NULL}, //  ẜ..ỿ
    {0x1FA0, 0x1FAF, SC_Greek, GC_L, NULL}, //  ᾠ..ᾯ
    {0x1FB2, 0x1FB4, SC_Greek, GC_Ll, NULL}, //  ᾲ..ᾴ
    {0x1FEC, 0x1FEC, SC_Greek, GC_Lu, NULL}, //  Ῥ
    {0x2118, 0x2118, SC_Common, GC_Sm, NULL}, //  ℘
    {0x212E, 0x212E, SC_Common, GC_So, NULL}, //  ℮
    {0x2C60, 0x2C67, SC_Latin, GC_L, NULL}, //  Ⱡ..Ⱨ
    {0x2C77, 0x2C7B, SC_Latin, GC_Ll, NULL}, //  ⱷ..ⱻ
    {0x2D27, 0x2D27, SC_Georgian, GC_Ll, NULL}, //  ⴧ
    {0x2D2D, 0x2D2D, SC_Georgian, GC_Ll, NULL}, //  ⴭ
    {0x3005, 0x3005, SC_Han, GC_Lm, NULL}, //  々
    {0x3007, 0x3007, SC_Han, GC_Nl, NULL}, //  〇
    {0x3021, 0x3029, SC_Han, GC_Nl, NULL}, //  〡..〩
    {0x3031, 0x3035, SC_Common, GC_Lm, "\x11\x12"}, //Hiragana,Katakana //  〱..〵
    {0x303B, 0x303B, SC_Han, GC_Lm, NULL}, //  〻
    {0x3041, 0x3096, SC_Hiragana, GC_Lo, NULL}, //  ぁ..ゖ
    {0x309D, 0x309E, SC_Hiragana, GC_Lm, NULL}, //  ゝ..ゞ
    {0x30A1, 0x30FA, SC_Katakana, GC_Lo, NULL}, //  ァ..ヺ
    {0x30FC, 0x30FC, SC_Common, GC_Lm, "\x11\x12"}, //Hiragana,Katakana //  ー
    {0x30FE, 0x30FE, SC_Katakana, GC_Lm, NULL}, //  ヾ
    {0x3447, 0x3447, SC_Han, GC_Lo, NULL}, //  㑇
    {0x3473, 0x3473, SC_Han, GC_Lo, NULL}, //  㑳
    {0x34E4, 0x34E4, SC_Han, GC_Lo, NULL}, //  㓤
    {0x3577, 0x3577, SC_Han, GC_Lo, NULL}, //  㕷
    {0x359E, 0x359E, SC_Han, GC_Lo, NULL}, //  㖞
    {0x35A1, 0x35A1, SC_Han, GC_Lo, NULL}, //  㖡
    {0x35AD, 0x35AD, SC_Han, GC_Lo, NULL}, //  㖭
    {0x35BF, 0x35BF, SC_Han, GC_Lo, NULL}, //  㖿
    {0x35CE, 0x35CE, SC_Han, GC_Lo, NULL}, //  㗎
    {0x35F3, 0x35F3, SC_Han, GC_Lo, NULL}, //  㗳
    {0x35FE, 0x35FE, SC_Han, GC_Lo, NULL}, //  㗾
    {0x360E, 0x360E, SC_Han, GC_Lo, NULL}, //  㘎
    {0x361A, 0x361A, SC_Han, GC_Lo, NULL}, //  㘚
    {0x3918, 0x3918, SC_Han, GC_Lo, NULL}, //  㤘
    {0x3960, 0x3960, SC_Han, GC_Lo, NULL}, //  㥠
    {0x396E, 0x396E, SC_Han, GC_Lo, NULL}, //  㥮
    {0x39CF, 0x39D0, SC_Han, GC_Lo, NULL}, //  㧏..㧐
    {0x39DB, 0x39DB, SC_Han, GC_Lo, NULL}, //  㧛
    {0x39DF, 0x39DF, SC_Han, GC_Lo, NULL}, //  㧟
    {0x39F8, 0x39F8, SC_Han, GC_Lo, NULL}, //  㧸
    {0x39FE, 0x39FE, SC_Han, GC_Lo, NULL}, //  㧾
    {0x3A18, 0x3A18, SC_Han, GC_Lo, NULL}, //  㨘
    {0x3A52, 0x3A52, SC_Han, GC_Lo, NULL}, //  㩒
    {0x3A5C, 0x3A5C, SC_Han, GC_Lo, NULL}, //  㩜
    {0x3A67, 0x3A67, SC_Han, GC_Lo, NULL}, //  㩧
    {0x3A73, 0x3A73, SC_Han, GC_Lo, NULL}, //  㩳
    {0x3B39, 0x3B39, SC_Han, GC_Lo, NULL}, //  㬹
    {0x3B4E, 0x3B4E, SC_Han, GC_Lo, NULL}, //  㭎
    {0x3BA3, 0x3BA3, SC_Han, GC_Lo, NULL}, //  㮣
    {0x3C6E, 0x3C6E, SC_Han, GC_Lo, NULL}, //  㱮
    {0x3CE0, 0x3CE0, SC_Han, GC_Lo, NULL}, //  㳠
    {0x3DE7, 0x3DE7, SC_Han, GC_Lo, NULL}, //  㷧
    {0x3DEB, 0x3DEB, SC_Han, GC_Lo, NULL}, //  㷫
    {0x3E74, 0x3E74, SC_Han, GC_Lo, NULL}, //  㹴
    {0x3ED0, 0x3ED0, SC_Han, GC_Lo, NULL}, //  㻐
    {0x4056, 0x4056, SC_Han, GC_Lo, NULL}, //  䁖
    {0x4065, 0x4065, SC_Han, GC_Lo, NULL}, //  䁥
    {0x406A, 0x406A, SC_Han, GC_Lo, NULL}, //  䁪
    {0x40BB, 0x40BB, SC_Han, GC_Lo, NULL}, //  䂻
    {0x40DF, 0x40DF, SC_Han, GC_Lo, NULL}, //  䃟
    {0x4137, 0x4137, SC_Han, GC_Lo, NULL}, //  䄷
    {0x415F, 0x415F, SC_Han, GC_Lo, NULL}, //  䅟
    {0x4337, 0x4337, SC_Han, GC_Lo, NULL}, //  䌷
    {0x43AC, 0x43AC, SC_Han, GC_Lo, NULL}, //  䎬
    {0x43B1, 0x43B1, SC_Han, GC_Lo, NULL}, //  䎱
    {0x43D3, 0x43D3, SC_Han, GC_Lo, NULL}, //  䏓
    {0x43DD, 0x43DD, SC_Han, GC_Lo, NULL}, //  䏝
    {0x4443, 0x4443, SC_Han, GC_Lo, NULL}, //  䑃
    {0x44D6, 0x44D6, SC_Han, GC_Lo, NULL}, //  䓖
    {0x44EA, 0x44EA, SC_Han, GC_Lo, NULL}, //  䓪
    {0x4606, 0x4606, SC_Han, GC_Lo, NULL}, //  䘆
    {0x464C, 0x464C, SC_Han, GC_Lo, NULL}, //  䙌
    {0x4661, 0x4661, SC_Han, GC_Lo, NULL}, //  䙡
    {0x4723, 0x4723, SC_Han, GC_Lo, NULL}, //  䜣
    {0x4729, 0x4729, SC_Han, GC_Lo, NULL}, //  䜩
    {0x477C, 0x477C, SC_Han, GC_Lo, NULL}, //  䝼
    {0x478D, 0x478D, SC_Han, GC_Lo, NULL}, //  䞍
    {0x47F4, 0x47F4, SC_Han, GC_Lo, NULL}, //  䟴
    {0x4882, 0x4882, SC_Han, GC_Lo, NULL}, //  䢂
    {0x4947, 0x4947, SC_Han, GC_Lo, NULL}, //  䥇
    {0x497A, 0x497A, SC_Han, GC_Lo, NULL}, //  䥺
    {0x497D, 0x497D, SC_Han, GC_Lo, NULL}, //  䥽
    {0x4982, 0x4983, SC_Han, GC_Lo, NULL}, //  䦂..䦃
    {0x4985, 0x4986, SC_Han, GC_Lo, NULL}, //  䦅..䦆
    {0x499B, 0x499B, SC_Han, GC_Lo, NULL}, //  䦛
    {0x499F, 0x499F, SC_Han, GC_Lo, NULL}, //  䦟
    {0x49B6, 0x49B7, SC_Han, GC_Lo, NULL}, //  䦶..䦷
    {0x4A12, 0x4A12, SC_Han, GC_Lo, NULL}, //  䨒
    {0x4AB8, 0x4AB8, SC_Han, GC_Lo, NULL}, //  䪸
    {0x4C77, 0x4C77, SC_Han, GC_Lo, NULL}, //  䱷
    {0x4C7D, 0x4C7D, SC_Han, GC_Lo, NULL}, //  䱽
    {0x4C81, 0x4C81, SC_Han, GC_Lo, NULL}, //  䲁
    {0x4C85, 0x4C85, SC_Han, GC_Lo, NULL}, //  䲅
    {0x4C9D, 0x4CA3, SC_Han, GC_Lo, NULL}, //  䲝..䲣
    {0x4D13, 0x4D19, SC_Han, GC_Lo, NULL}, //  䴓..䴙
    {0x4DAE, 0x4DAE, SC_Han, GC_Lo, NULL}, //  䶮
    {0x4E00, 0x4E11, SC_Han, GC_Lo, NULL}, //  一..丑
    {0x4E13, 0x4E28, SC_Han, GC_Lo, NULL}, //  专..丨
    {0x4E2A, 0x4E67, SC_Han, GC_Lo, NULL}, //  个..乧
    {0x4E69, 0x4E78, SC_Han, GC_Lo, NULL}, //  乩..乸
    {0x4E7A, 0x4E95, SC_Han, GC_Lo, NULL}, //  乺..井
    {0x4E97, 0x4EA2, SC_Han, GC_Lo, NULL}, //  亗..亢
    {0x4EA4, 0x4EBB, SC_Han, GC_Lo, NULL}, //  交..亻
    {0x4EBD, 0x4ECB, SC_Han, GC_Lo, NULL}, //  亽..介
    {0x4ECD, 0x4EE6, SC_Han, GC_Lo, NULL}, //  仍..仦
    {0x4EE8, 0x4EF7, SC_Han, GC_Lo, NULL}, //  仨..价
    {0x4EFB, 0x4EFB, SC_Han, GC_Lo, NULL}, //  任
    {0x4EFD, 0x4EFD, SC_Han, GC_Lo, NULL}, //  份
    {0x4EFF, 0x4F06, SC_Han, GC_Lo, NULL}, //  仿..伆
    {0x4F08, 0x4F15, SC_Han, GC_Lo, NULL}, //  伈..伕
    {0x4F17, 0x4F27, SC_Han, GC_Lo, NULL}, //  众..伧
    {0x4F29, 0x4F30, SC_Han, GC_Lo, NULL}, //  伩..估
    {0x4F32, 0x4F34, SC_Han, GC_Lo, NULL}, //  伲..伴
    {0x4F36, 0x4F36, SC_Han, GC_Lo, NULL}, //  伶
    {0x4F38, 0x4F3F, SC_Han, GC_Lo, NULL}, //  伸..伿
    {0x4F41, 0x4F43, SC_Han, GC_Lo, NULL}, //  佁..佃
    {0x4F45, 0x4F70, SC_Han, GC_Lo, NULL}, //  佅..佰
    {0x4F72, 0x4F8B, SC_Han, GC_Lo, NULL}, //  佲..例
    {0x4F8D, 0x4F8D, SC_Han, GC_Lo, NULL}, //  侍
    {0x4F8F, 0x4FA1, SC_Han, GC_Lo, NULL}, //  侏..価
    {0x4FA3, 0x4FBC, SC_Han, GC_Lo, NULL}, //  侣..侼
    {0x4FBE, 0x4FC5, SC_Han, GC_Lo, NULL}, //  侾..俅
    {0x4FC7, 0x4FC7, SC_Han, GC_Lo, NULL}, //  俇
    {0x4FC9, 0x4FCB, SC_Han, GC_Lo, NULL}, //  俉..俋
    {0x4FCD, 0x4FE1, SC_Han, GC_Lo, NULL}, //  俍..信
    {0x4FE3, 0x4FFB, SC_Han, GC_Lo, NULL}, //  俣..俻
    {0x4FFE, 0x500F, SC_Han, GC_Lo, NULL}, //  俾..倏
    {0x5011, 0x5033, SC_Han, GC_Lo, NULL}, //  們..倳
    {0x5035, 0x5037, SC_Han, GC_Lo, NULL}, //  倵..倷
    {0x5039, 0x503C, SC_Han, GC_Lo, NULL}, //  倹..值
    {0x503E, 0x5041, SC_Han, GC_Lo, NULL}, //  倾..偁
    {0x5043, 0x5051, SC_Han, GC_Lo, NULL}, //  偃..偑
    {0x5053, 0x5057, SC_Han, GC_Lo, NULL}, //  偓..偗
    {0x5059, 0x507B, SC_Han, GC_Lo, NULL}, //  偙..偻
    {0x507D, 0x5080, SC_Han, GC_Lo, NULL}, //  偽..傀
    {0x5082, 0x5092, SC_Han, GC_Lo, NULL}, //  傂..傒
    {0x5094, 0x5096, SC_Han, GC_Lo, NULL}, //  傔..傖
    {0x5098, 0x509E, SC_Han, GC_Lo, NULL}, //  傘..傞
    {0x50A2, 0x50B8, SC_Han, GC_Lo, NULL}, //  傢..傸
    {0x50BA, 0x50C2, SC_Han, GC_Lo, NULL}, //  傺..僂
    {0x50C4, 0x50D7, SC_Han, GC_Lo, NULL}, //  僄..僗
    {0x50D9, 0x50DE, SC_Han, GC_Lo, NULL}, //  僙..僞
    {0x50E0, 0x50E0, SC_Han, GC_Lo, NULL}, //  僠
    {0x50E3, 0x50EA, SC_Han, GC_Lo, NULL}, //  僣..僪
    {0x50EC, 0x50F3, SC_Han, GC_Lo, NULL}, //  僬..僳
    {0x50F5, 0x50F6, SC_Han, GC_Lo, NULL}, //  僵..僶
    {0x50F8, 0x511A, SC_Han, GC_Lo, NULL}, //  僸..儚
    {0x511C, 0x5127, SC_Han, GC_Lo, NULL}, //  儜..儧
    {0x5129, 0x512A, SC_Han, GC_Lo, NULL}, //  儩..優
    {0x512C, 0x5141, SC_Han, GC_Lo, NULL}, //  儬..允
    {0x5143, 0x5149, SC_Han, GC_Lo, NULL}, //  元..光
    {0x514B, 0x514E, SC_Han, GC_Lo, NULL}, //  克..兎
    {0x5150, 0x5152, SC_Han, GC_Lo, NULL}, //  児..兒
    {0x5154, 0x5157, SC_Han, GC_Lo, NULL}, //  兔..兗
    {0x5159, 0x515F, SC_Han, GC_Lo, NULL}, //  兙..兟
    {0x5161, 0x5163, SC_Han, GC_Lo, NULL}, //  兡..兣
    {0x5165, 0x5171, SC_Han, GC_Lo, NULL}, //  入..共
    {0x5173, 0x517D, SC_Han, GC_Lo, NULL}, //  关..兽
    {0x517F, 0x5182, SC_Han, GC_Lo, NULL}, //  兿..冂
    {0x5185, 0x518D, SC_Han, GC_Lo, NULL}, //  内..再
    {0x518F, 0x51A0, SC_Han, GC_Lo, NULL}, //  冏..冠
    {0x51A2, 0x51A2, SC_Han, GC_Lo, NULL}, //  冢
    {0x51A4, 0x51AC, SC_Han, GC_Lo, NULL}, //  冤..冬
    {0x51AE, 0x51B7, SC_Han, GC_Lo, NULL}, //  冮..冷
    {0x51B9, 0x51B9, SC_Han, GC_Lo, NULL}, //  冹
    {0x51BB, 0x51C1, SC_Han, GC_Lo, NULL}, //  冻..凁
    {0x51C3, 0x51D1, SC_Han, GC_Lo, NULL}, //  凃..凑
    {0x51D4, 0x51DE, SC_Han, GC_Lo, NULL}, //  凔..凞
    {0x51E0, 0x51EB, SC_Han, GC_Lo, NULL}, //  几..凫
    {0x51ED, 0x51ED, SC_Han, GC_Lo, NULL}, //  凭
    {0x51EF, 0x51F1, SC_Han, GC_Lo, NULL}, //  凯..凱
    {0x51F3, 0x5252, SC_Han, GC_Lo, NULL}, //  凳..剒
    {0x5254, 0x5265, SC_Han, GC_Lo, NULL}, //  剔..剥
    {0x5267, 0x5278, SC_Han, GC_Lo, NULL}, //  剧..剸
    {0x527A, 0x5284, SC_Han, GC_Lo, NULL}, //  剺..劄
    {0x5286, 0x528D, SC_Han, GC_Lo, NULL}, //  劆..劍
    {0x528F, 0x52C3, SC_Han, GC_Lo, NULL}, //  劏..勃
    {0x52C5, 0x52C7, SC_Han, GC_Lo, NULL}, //  勅..勇
    {0x52C9, 0x52CB, SC_Han, GC_Lo, NULL}, //  勉..勋
    {0x52CD, 0x52CD, SC_Han, GC_Lo, NULL}, //  勍
    {0x52CF, 0x52D0, SC_Han, GC_Lo, NULL}, //  勏..勐
    {0x52D2, 0x52D3, SC_Han, GC_Lo, NULL}, //  勒..勓
    {0x52D5, 0x52E0, SC_Han, GC_Lo, NULL}, //  動..勠
    {0x52E2, 0x52E4, SC_Han, GC_Lo, NULL}, //  勢..勤
    {0x52E6, 0x52ED, SC_Han, GC_Lo, NULL}, //  勦..勭
    {0x52EF, 0x5302, SC_Han, GC_Lo, NULL}, //  勯..匂
    {0x5305, 0x5317, SC_Han, GC_Lo, NULL}, //  包..北
    {0x5319, 0x531A, SC_Han, GC_Lo, NULL}, //  匙..匚
    {0x531C, 0x531D, SC_Han, GC_Lo, NULL}, //  匜..匝
    {0x531F, 0x5326, SC_Han, GC_Lo, NULL}, //  匟..匦
    {0x5328, 0x5328, SC_Han, GC_Lo, NULL}, //  匨
    {0x532A, 0x5331, SC_Han, GC_Lo, NULL}, //  匪..匱
    {0x5333, 0x5334, SC_Han, GC_Lo, NULL}, //  匳..匴
    {0x5337, 0x5341, SC_Han, GC_Lo, NULL}, //  匷..十
    {0x5343, 0x535A, SC_Han, GC_Lo, NULL}, //  千..博
    {0x535C, 0x535C, SC_Han, GC_Lo, NULL}, //  卜
    {0x535E, 0x5369, SC_Han, GC_Lo, NULL}, //  卞..卩
    {0x536B, 0x536C, SC_Han, GC_Lo, NULL}, //  卫..卬
    {0x536E, 0x537F, SC_Han, GC_Lo, NULL}, //  卮..卿
    {0x5381, 0x53A0, SC_Han, GC_Lo, NULL}, //  厁..厠
    {0x53A2, 0x53A9, SC_Han, GC_Lo, NULL}, //  厢..厩
    {0x53AC, 0x53AE, SC_Han, GC_Lo, NULL}, //  厬..厮
    {0x53B0, 0x53B9, SC_Han, GC_Lo, NULL}, //  厰..厹
    {0x53BB, 0x53C4, SC_Han, GC_Lo, NULL}, //  去..叄
    {0x53C6, 0x53CE, SC_Han, GC_Lo, NULL}, //  叆..収
    {0x53D0, 0x53DC, SC_Han, GC_Lo, NULL}, //  叐..叜
    {0x53DF, 0x53E6, SC_Han, GC_Lo, NULL}, //  叟..另
    {0x53E8, 0x53FE, SC_Han, GC_Lo, NULL}, //  叨..叾
    {0x5401, 0x5419, SC_Han, GC_Lo, NULL}, //  吁..吙
    {0x541B, 0x5421, SC_Han, GC_Lo, NULL}, //  君..吡
    {0x5423, 0x544B, SC_Han, GC_Lo, NULL}, //  吣..呋
    {0x544D, 0x545C, SC_Han, GC_Lo, NULL}, //  呍..呜
    {0x545E, 0x5468, SC_Han, GC_Lo, NULL}, //  呞..周
    {0x546A, 0x5489, SC_Han, GC_Lo, NULL}, //  呪..咉
    {0x548B, 0x54B4, SC_Han, GC_Lo, NULL}, //  咋..咴
    {0x54B6, 0x54F5, SC_Han, GC_Lo, NULL}, //  咶..哵
    {0x54F7, 0x5514, SC_Han, GC_Lo, NULL}, //  哷..唔
    {0x5516, 0x5517, SC_Han, GC_Lo, NULL}, //  唖..唗
    {0x551A, 0x5546, SC_Han, GC_Lo, NULL}, //  唚..商
    {0x5548, 0x555F, SC_Han, GC_Lo, NULL}, //  啈..啟
    {0x5561, 0x5579, SC_Han, GC_Lo, NULL}, //  啡..啹
    {0x557B, 0x55DF, SC_Han, GC_Lo, NULL}, //  啻..嗟
    {0x55E1, 0x55F7, SC_Han, GC_Lo, NULL}, //  嗡..嗷
    {0x55F9, 0x5609, SC_Han, GC_Lo, NULL}, //  嗹..嘉
    {0x560C, 0x561F, SC_Han, GC_Lo, NULL}, //  嘌..嘟
    {0x5621, 0x562A, SC_Han, GC_Lo, NULL}, //  嘡..嘪
    {0x562C, 0x5636, SC_Han, GC_Lo, NULL}, //  嘬..嘶
    {0x5638, 0x563B, SC_Han, GC_Lo, NULL}, //  嘸..嘻
    {0x563D, 0x5643, SC_Han, GC_Lo, NULL}, //  嘽..噃
    {0x5645, 0x564A, SC_Han, GC_Lo, NULL}, //  噅..噊
    {0x564C, 0x5650, SC_Han, GC_Lo, NULL}, //  噌..噐
    {0x5652, 0x5655, SC_Han, GC_Lo, NULL}, //  噒..噕
    {0x5657, 0x565E, SC_Han, GC_Lo, NULL}, //  噗..噞
    {0x5660, 0x5660, SC_Han, GC_Lo, NULL}, //  噠
    {0x5662, 0x5674, SC_Han, GC_Lo, NULL}, //  噢..噴
    {0x5676, 0x567C, SC_Han, GC_Lo, NULL}, //  噶..噼
    {0x567E, 0x5687, SC_Han, GC_Lo, NULL}, //  噾..嚇
    {0x5689, 0x568A, SC_Han, GC_Lo, NULL}, //  嚉..嚊
    {0x568C, 0x5695, SC_Han, GC_Lo, NULL}, //  嚌..嚕
    {0x5697, 0x569D, SC_Han, GC_Lo, NULL}, //  嚗..嚝
    {0x569F, 0x56B9, SC_Han, GC_Lo, NULL}, //  嚟..嚹
    {0x56BB, 0x56CE, SC_Han, GC_Lo, NULL}, //  嚻..囎
    {0x56D0, 0x56D8, SC_Han, GC_Lo, NULL}, //  囐..囘
    {0x56DA, 0x56E5, SC_Han, GC_Lo, NULL}, //  囚..囥
    {0x56E7, 0x56F5, SC_Han, GC_Lo, NULL}, //  囧..囵
    {0x56F7, 0x56F7, SC_Han, GC_Lo, NULL}, //  囷
    {0x56F9, 0x56FA, SC_Han, GC_Lo, NULL}, //  囹..固
    {0x56FD, 0x5704, SC_Han, GC_Lo, NULL}, //  国..圄
    {0x5706, 0x5710, SC_Han, GC_Lo, NULL}, //  圆..圐
    {0x5712, 0x5716, SC_Han, GC_Lo, NULL}, //  園..圖
    {0x5718, 0x5720, SC_Han, GC_Lo, NULL}, //  團..圠
    {0x5722, 0x5723, SC_Han, GC_Lo, NULL}, //  圢..圣
    {0x5725, 0x573C, SC_Han, GC_Lo, NULL}, //  圥..圼
    {0x573E, 0x5742, SC_Han, GC_Lo, NULL}, //  圾..坂
    {0x5744, 0x5747, SC_Han, GC_Lo, NULL}, //  坄..均
    {0x5749, 0x5754, SC_Han, GC_Lo, NULL}, //  坉..坔
    {0x5757, 0x5757, SC_Han, GC_Lo, NULL}, //  块
    {0x5759, 0x5762, SC_Han, GC_Lo, NULL}, //  坙..坢
    {0x5764, 0x5777, SC_Han, GC_Lo, NULL}, //  坤..坷
    {0x5779, 0x5780, SC_Han, GC_Lo, NULL}, //  坹..垀
    {0x5782, 0x5786, SC_Han, GC_Lo, NULL}, //  垂..垆
    {0x5788, 0x5795, SC_Han, GC_Lo, NULL}, //  垈..垕
    {0x5797, 0x57A7, SC_Han, GC_Lo, NULL}, //  垗..垧
    {0x57A9, 0x57C9, SC_Han, GC_Lo, NULL}, //  垩..埉
    {0x57CB, 0x57D0, SC_Han, GC_Lo, NULL}, //  埋..埐
    {0x57D2, 0x57DA, SC_Han, GC_Lo, NULL}, //  埒..埚
    {0x57DC, 0x5816, SC_Han, GC_Lo, NULL}, //  埜..堖
    {0x5819, 0x584F, SC_Han, GC_Lo, NULL}, //  堙..塏
    {0x5851, 0x5855, SC_Han, GC_Lo, NULL}, //  塑..塕
    {0x5857, 0x585F, SC_Han, GC_Lo, NULL}, //  塗..塟
    {0x5861, 0x5865, SC_Han, GC_Lo, NULL}, //  塡..塥
    {0x5868, 0x5876, SC_Han, GC_Lo, NULL}, //  塨..塶
    {0x5878, 0x5894, SC_Han, GC_Lo, NULL}, //  塸..墔
    {0x5896, 0x58A9, SC_Han, GC_Lo, NULL}, //  墖..墩
    {0x58AB, 0x58B5, SC_Han, GC_Lo, NULL}, //  墫..墵
    {0x58B7, 0x58BF, SC_Han, GC_Lo, NULL}, //  墷..墿
    {0x58C1, 0x58C2, SC_Han, GC_Lo, NULL}, //  壁..壂
    {0x58C5, 0x58CC, SC_Han, GC_Lo, NULL}, //  壅..壌
    {0x58CE, 0x58CF, SC_Han, GC_Lo, NULL}, //  壎..壏
    {0x58D1, 0x58E0, SC_Han, GC_Lo, NULL}, //  壑..壠
    {0x58E2, 0x58E5, SC_Han, GC_Lo, NULL}, //  壢..壥
    {0x58E7, 0x58F4, SC_Han, GC_Lo, NULL}, //  壧..壴
    {0x58F6, 0x5900, SC_Han, GC_Lo, NULL}, //  壶..夀
    {0x5902, 0x5904, SC_Han, GC_Lo, NULL}, //  夂..处
    {0x5906, 0x5907, SC_Han, GC_Lo, NULL}, //  夆..备
    {0x5909, 0x5910, SC_Han, GC_Lo, NULL}, //  変..夐
    {0x5912, 0x5912, SC_Han, GC_Lo, NULL}, //  夒
    {0x5914, 0x5922, SC_Han, GC_Lo, NULL}, //  夔..夢
    {0x5924, 0x5932, SC_Han, GC_Lo, NULL}, //  夤..夲
    {0x5934, 0x5935, SC_Han, GC_Lo, NULL}, //  头..夵
    {0x5937, 0x5958, SC_Han, GC_Lo, NULL}, //  夷..奘
    {0x595A, 0x595A, SC_Han, GC_Lo, NULL}, //  奚
    {0x595C, 0x59B6, SC_Han, GC_Lo, NULL}, //  奜..妶
    {0x59B8, 0x59E6, SC_Han, GC_Lo, NULL}, //  妸..姦
    {0x59E8, 0x5A23, SC_Han, GC_Lo, NULL}, //  姨..娣
    {0x5A25, 0x5A25, SC_Han, GC_Lo, NULL}, //  娥
    {0x5A27, 0x5A2B, SC_Han, GC_Lo, NULL}, //  娧..娫
    {0x5A2D, 0x5A2F, SC_Han, GC_Lo, NULL}, //  娭..娯
    {0x5A31, 0x5A53, SC_Han, GC_Lo, NULL}, //  娱..婓
    {0x5A55, 0x5A58, SC_Han, GC_Lo, NULL}, //  婕..婘
    {0x5A5A, 0x5A6E, SC_Han, GC_Lo, NULL}, //  婚..婮
    {0x5A70, 0x5A70, SC_Han, GC_Lo, NULL}, //  婰
    {0x5A72, 0x5A86, SC_Han, GC_Lo, NULL}, //  婲..媆
    {0x5A88, 0x5A8C, SC_Han, GC_Lo, NULL}, //  媈..媌
    {0x5A8E, 0x5AAA, SC_Han, GC_Lo, NULL}, //  媎..媪
    {0x5AAC, 0x5AD2, SC_Han, GC_Lo, NULL}, //  媬..嫒
    {0x5AD4, 0x5AEE, SC_Han, GC_Lo, NULL}, //  嫔..嫮
    {0x5AF1, 0x5B09, SC_Han, GC_Lo, NULL}, //  嫱..嬉
    {0x5B0B, 0x5B0C, SC_Han, GC_Lo, NULL}, //  嬋..嬌
    {0x5B0E, 0x5B38, SC_Han, GC_Lo, NULL}, //  嬎..嬸
    {0x5B3A, 0x5B45, SC_Han, GC_Lo, NULL}, //  嬺..孅
    {0x5B47, 0x5B4E, SC_Han, GC_Lo, NULL}, //  孇..孎
    {0x5B50, 0x5B51, SC_Han, GC_Lo, NULL}, //  子..孑
    {0x5B53, 0x5B5F, SC_Han, GC_Lo, NULL}, //  孓..孟
    {0x5B62, 0x5B6E, SC_Han, GC_Lo, NULL}, //  孢..孮
    {0x5B70, 0x5B78, SC_Han, GC_Lo, NULL}, //  孰..學
    {0x5B7A, 0x5B7D, SC_Han, GC_Lo, NULL}, //  孺..孽
    {0x5B7F, 0x5B85, SC_Han, GC_Lo, NULL}, //  孿..宅
    {0x5B87, 0x5B8F, SC_Han, GC_Lo, NULL}, //  宇..宏
    {0x5B91, 0x5BA8, SC_Han, GC_Lo, NULL}, //  宑..宨
    {0x5BAA, 0x5BB1, SC_Han, GC_Lo, NULL}, //  宪..宱
    {0x5BB3, 0x5BB6, SC_Han, GC_Lo, NULL}, //  害..家
    {0x5BB8, 0x5BBB, SC_Han, GC_Lo, NULL}, //  宸..宻
    {0x5BBD, 0x5BC7, SC_Han, GC_Lo, NULL}, //  宽..寇
    {0x5BC9, 0x5BD9, SC_Han, GC_Lo, NULL}, //  寉..寙
    {0x5BDB, 0x5BFF, SC_Han, GC_Lo, NULL}, //  寛..寿
    {0x5C01, 0x5C1A, SC_Han, GC_Lo, NULL}, //  封..尚
    {0x5C1C, 0x5C22, SC_Han, GC_Lo, NULL}, //  尜..尢
    {0x5C24, 0x5C25, SC_Han, GC_Lo, NULL}, //  尤..尥
    {0x5C27, 0x5C28, SC_Han, GC_Lo, NULL}, //  尧..尨
    {0x5C2A, 0x5C35, SC_Han, GC_Lo, NULL}, //  尪..尵
    {0x5C37, 0x5C59, SC_Han, GC_Lo, NULL}, //  尷..屙
    {0x5C5B, 0x5C84, SC_Han, GC_Lo, NULL}, //  屛..岄
    {0x5C86, 0x5CB3, SC_Han, GC_Lo, NULL}, //  岆..岳
    {0x5CB5, 0x5CB8, SC_Han, GC_Lo, NULL}, //  岵..岸
    {0x5CBA, 0x5CD4, SC_Han, GC_Lo, NULL}, //  岺..峔
    {0x5CD6, 0x5CDC, SC_Han, GC_Lo, NULL}, //  峖..峜
    {0x5CDE, 0x5CF4, SC_Han, GC_Lo, NULL}, //  峞..峴
    {0x5CF6, 0x5D2A, SC_Han, GC_Lo, NULL}, //  島..崪
    {0x5D2C, 0x5D2E, SC_Han, GC_Lo, NULL}, //  崬..崮
    {0x5D30, 0x5D3A, SC_Han, GC_Lo, NULL}, //  崰..崺
    {0x5D3C, 0x5D52, SC_Han, GC_Lo, NULL}, //  崼..嵒
    {0x5D54, 0x5D56, SC_Han, GC_Lo, NULL}, //  嵔..嵖
    {0x5D58, 0x5D5F, SC_Han, GC_Lo, NULL}, //  嵘..嵟
    {0x5D61, 0x5D82, SC_Han, GC_Lo, NULL}, //  嵡..嶂
    {0x5D84, 0x5D95, SC_Han, GC_Lo, NULL}, //  嶄..嶕
    {0x5D97, 0x5DA2, SC_Han, GC_Lo, NULL}, //  嶗..嶢
    {0x5DA5, 0x5DAA, SC_Han, GC_Lo, NULL}, //  嶥..嶪
    {0x5DAC, 0x5DB2, SC_Han, GC_Lo, NULL}, //  嶬..嶲
    {0x5DB4, 0x5DB8, SC_Han, GC_Lo, NULL}, //  嶴..嶸
    {0x5DBA, 0x5DC3, SC_Han, GC_Lo, NULL}, //  嶺..巃
    {0x5DC5, 0x5DD6, SC_Han, GC_Lo, NULL}, //  巅..巖
    {0x5DD8, 0x5DD9, SC_Han, GC_Lo, NULL}, //  巘..巙
    {0x5DDB, 0x5DDB, SC_Han, GC_Lo, NULL}, //  巛
    {0x5DDD, 0x5DF5, SC_Han, GC_Lo, NULL}, //  川..巵
    {0x5DF7, 0x5E11, SC_Han, GC_Lo, NULL}, //  巷..帑
    {0x5E13, 0x5E47, SC_Han, GC_Lo, NULL}, //  帓..幇
    {0x5E49, 0x5E50, SC_Han, GC_Lo, NULL}, //  幉..幐
    {0x5E52, 0x5E91, SC_Han, GC_Lo, NULL}, //  幒..庑
    {0x5E93, 0x5EB9, SC_Han, GC_Lo, NULL}, //  库..庹
    {0x5EBB, 0x5EBF, SC_Han, GC_Lo, NULL}, //  庻..庿
    {0x5EC1, 0x5EEA, SC_Han, GC_Lo, NULL}, //  廁..廪
    {0x5EEC, 0x5EF8, SC_Han, GC_Lo, NULL}, //  廬..廸
    {0x5EFA, 0x5F0D, SC_Han, GC_Lo, NULL}, //  建..弍
    {0x5F0F, 0x5F3A, SC_Han, GC_Lo, NULL}, //  式..强
    {0x5F3C, 0x5F3C, SC_Han, GC_Lo, NULL}, //  弼
    {0x5F3E, 0x5F8E, SC_Han, GC_Lo, NULL}, //  弾..徎
    {0x5F90, 0x5F99, SC_Han, GC_Lo, NULL}, //  徐..徙
    {0x5F9B, 0x5FA2, SC_Han, GC_Lo, NULL}, //  徛..徢
    {0x5FA5, 0x5FAF, SC_Han, GC_Lo, NULL}, //  徥..徯
    {0x5FB1, 0x5FC1, SC_Han, GC_Lo, NULL}, //  徱..忁
    {0x5FC3, 0x5FCD, SC_Han, GC_Lo, NULL}, //  心..忍
    {0x5FCF, 0x5FDA, SC_Han, GC_Lo, NULL}, //  忏..忚
    {0x5FDC, 0x5FE1, SC_Han, GC_Lo, NULL}, //  応..忡
    {0x5FE3, 0x5FEB, SC_Han, GC_Lo, NULL}, //  忣..快
    {0x5FED, 0x5FFB, SC_Han, GC_Lo, NULL}, //  忭..忻
    {0x5FFD, 0x6022, SC_Han, GC_Lo, NULL}, //  忽..怢
    {0x6024, 0x6055, SC_Han, GC_Lo, NULL}, //  怤..恕
    {0x6057, 0x6060, SC_Han, GC_Lo, NULL}, //  恗..恠
    {0x6062, 0x6070, SC_Han, GC_Lo, NULL}, //  恢..恰
    {0x6072, 0x6073, SC_Han, GC_Lo, NULL}, //  恲..恳
    {0x6075, 0x6090, SC_Han, GC_Lo, NULL}, //  恵..悐
    {0x6092, 0x6092, SC_Han, GC_Lo, NULL}, //  悒
    {0x6094, 0x60A4, SC_Han, GC_Lo, NULL}, //  悔..悤
    {0x60A6, 0x60D1, SC_Han, GC_Lo, NULL}, //  悦..惑
    {0x60D3, 0x60D5, SC_Han, GC_Lo, NULL}, //  惓..惕
    {0x60D7, 0x60DD, SC_Han, GC_Lo, NULL}, //  惗..惝
    {0x60DF, 0x60E4, SC_Han, GC_Lo, NULL}, //  惟..惤
    {0x60E6, 0x60FC, SC_Han, GC_Lo, NULL}, //  惦..惼
    {0x60FE, 0x6101, SC_Han, GC_Lo, NULL}, //  惾..愁
    {0x6103, 0x6106, SC_Han, GC_Lo, NULL}, //  愃..愆
    {0x6108, 0x6110, SC_Han, GC_Lo, NULL}, //  愈..愐
    {0x6112, 0x611D, SC_Han, GC_Lo, NULL}, //  愒..愝
    {0x611F, 0x6130, SC_Han, GC_Lo, NULL}, //  感..愰
    {0x6132, 0x6132, SC_Han, GC_Lo, NULL}, //  愲
    {0x6134, 0x6134, SC_Han, GC_Lo, NULL}, //  愴
    {0x6136, 0x6137, SC_Han, GC_Lo, NULL}, //  愶..愷
    {0x613A, 0x615F, SC_Han, GC_Lo, NULL}, //  愺..慟
    {0x6161, 0x617A, SC_Han, GC_Lo, NULL}, //  慡..慺
    {0x617C, 0x617E, SC_Han, GC_Lo, NULL}, //  慼..慾
    {0x6180, 0x6185, SC_Han, GC_Lo, NULL}, //  憀..憅
    {0x6187, 0x6196, SC_Han, GC_Lo, NULL}, //  憇..憖
    {0x6198, 0x619B, SC_Han, GC_Lo, NULL}, //  憘..憛
    {0x619D, 0x61B8, SC_Han, GC_Lo, NULL}, //  憝..憸
    {0x61BA, 0x61BA, SC_Han, GC_Lo, NULL}, //  憺
    {0x61BC, 0x61D2, SC_Han, GC_Lo, NULL}, //  憼..懒
    {0x61D4, 0x61D4, SC_Han, GC_Lo, NULL}, //  懔
    {0x61D6, 0x61EB, SC_Han, GC_Lo, NULL}, //  懖..懫
    {0x61ED, 0x61EE, SC_Han, GC_Lo, NULL}, //  懭..懮
    {0x61F0, 0x6204, SC_Han, GC_Lo, NULL}, //  懰..戄
    {0x6206, 0x6234, SC_Han, GC_Lo, NULL}, //  戆..戴
    {0x6236, 0x6238, SC_Han, GC_Lo, NULL}, //  戶..戸
    {0x623A, 0x6256, SC_Han, GC_Lo, NULL}, //  戺..扖
    {0x6258, 0x628C, SC_Han, GC_Lo, NULL}, //  托..抌
    {0x628E, 0x629C, SC_Han, GC_Lo, NULL}, //  抎..抜
    {0x629E, 0x62DD, SC_Han, GC_Lo, NULL}, //  択..拝
    {0x62DF, 0x62E9, SC_Han, GC_Lo, NULL}, //  拟..择
    {0x62EB, 0x6309, SC_Han, GC_Lo, NULL}, //  拫..按
    {0x630B, 0x6316, SC_Han, GC_Lo, NULL}, //  挋..挖
    {0x6318, 0x6330, SC_Han, GC_Lo, NULL}, //  挘..挰
    {0x6332, 0x6336, SC_Han, GC_Lo, NULL}, //  挲..挶
    {0x6338, 0x635A, SC_Han, GC_Lo, NULL}, //  挸..捚
    {0x635C, 0x638A, SC_Han, GC_Lo, NULL}, //  捜..掊
    {0x638C, 0x6392, SC_Han, GC_Lo, NULL}, //  掌..排
    {0x6394, 0x63D0, SC_Han, GC_Lo, NULL}, //  掔..提
    {0x63D2, 0x643A, SC_Han, GC_Lo, NULL}, //  插..携
    {0x643D, 0x6448, SC_Han, GC_Lo, NULL}, //  搽..摈
    {0x644A, 0x6459, SC_Han, GC_Lo, NULL}, //  摊..摙
    {0x645B, 0x647D, SC_Han, GC_Lo, NULL}, //  摛..摽
    {0x647F, 0x6485, SC_Han, GC_Lo, NULL}, //  摿..撅
    {0x6487, 0x64A0, SC_Han, GC_Lo, NULL}, //  撇..撠
    {0x64A2, 0x64AE, SC_Han, GC_Lo, NULL}, //  撢..撮
    {0x64B0, 0x64B5, SC_Han, GC_Lo, NULL}, //  撰..撵
    {0x64B7, 0x64C7, SC_Han, GC_Lo, NULL}, //  撷..擇
    {0x64C9, 0x64D4, SC_Han, GC_Lo, NULL}, //  擉..擔
    {0x64D6, 0x64ED, SC_Han, GC_Lo, NULL}, //  擖..擭
    {0x64EF, 0x64F4, SC_Han, GC_Lo, NULL}, //  擯..擴
    {0x64F6, 0x64F8, SC_Han, GC_Lo, NULL}, //  擶..擸
    {0x64FA, 0x6501, SC_Han, GC_Lo, NULL}, //  擺..攁
    {0x6503, 0x6509, SC_Han, GC_Lo, NULL}, //  攃..攉
    {0x650B, 0x651E, SC_Han, GC_Lo, NULL}, //  攋..攞
    {0x6520, 0x6527, SC_Han, GC_Lo, NULL}, //  攠..攧
    {0x6529, 0x653F, SC_Han, GC_Lo, NULL}, //  攩..政
    {0x6541, 0x6541, SC_Han, GC_Lo, NULL}, //  敁
    {0x6543, 0x6559, SC_Han, GC_Lo, NULL}, //  敃..教
    {0x655B, 0x655E, SC_Han, GC_Lo, NULL}, //  敛..敞
    {0x6560, 0x657C, SC_Han, GC_Lo, NULL}, //  敠..敼
    {0x657E, 0x6589, SC_Han, GC_Lo, NULL}, //  敾..斉
    {0x658B, 0x6599, SC_Han, GC_Lo, NULL}, //  斋..料
    {0x659B, 0x65B4, SC_Han, GC_Lo, NULL}, //  斛..斴
    {0x65B6, 0x65BD, SC_Han, GC_Lo, NULL}, //  斶..施
    {0x65BF, 0x65C7, SC_Han, GC_Lo, NULL}, //  斿..旇
    {0x65CA, 0x65D0, SC_Han, GC_Lo, NULL}, //  旊..旐
    {0x65D2, 0x65D7, SC_Han, GC_Lo, NULL}, //  旒..旗
    {0x65D9, 0x65DB, SC_Han, GC_Lo, NULL}, //  旙..旛
    {0x65DD, 0x65E3, SC_Han, GC_Lo, NULL}, //  旝..旣
    {0x65E5, 0x65E9, SC_Han, GC_Lo, NULL}, //  日..早
    {0x65EB, 0x65F8, SC_Han, GC_Lo, NULL}, //  旫..旸
    {0x65FA, 0x65FD, SC_Han, GC_Lo, NULL}, //  旺..旽
    {0x65FF, 0x6616, SC_Han, GC_Lo, NULL}, //  旿..昖
    {0x6618, 0x662B, SC_Han, GC_Lo, NULL}, //  昘..昫
    {0x662D, 0x6636, SC_Han, GC_Lo, NULL}, //  昭..昶
    {0x6639, 0x6647, SC_Han, GC_Lo, NULL}, //  昹..晇
    {0x6649, 0x664C, SC_Han, GC_Lo, NULL}, //  晉..晌
    {0x664E, 0x665F, SC_Han, GC_Lo, NULL}, //  晎..晟
    {0x6661, 0x6662, SC_Han, GC_Lo, NULL}, //  晡..晢
    {0x6664, 0x6691, SC_Han, GC_Lo, NULL}, //  晤..暑
    {0x6693, 0x669B, SC_Han, GC_Lo, NULL}, //  暓..暛
    {0x669D, 0x669D, SC_Han, GC_Lo, NULL}, //  暝
    {0x669F, 0x66AB, SC_Han, GC_Lo, NULL}, //  暟..暫
    {0x66AE, 0x66CF, SC_Han, GC_Lo, NULL}, //  暮..曏
    {0x66D1, 0x66D2, SC_Han, GC_Lo, NULL}, //  曑..曒
    {0x66D4, 0x66D6, SC_Han, GC_Lo, NULL}, //  曔..曖
    {0x66D8, 0x66DE, SC_Han, GC_Lo, NULL}, //  曘..曞
    {0x66E0, 0x66EE, SC_Han, GC_Lo, NULL}, //  曠..曮
    {0x66F0, 0x6701, SC_Han, GC_Lo, NULL}, //  曰..朁
    {0x6703, 0x6706, SC_Han, GC_Lo, NULL}, //  會..朆
    {0x6708, 0x6718, SC_Han, GC_Lo, NULL}, //  月..朘
    {0x671A, 0x6723, SC_Han, GC_Lo, NULL}, //  朚..朣
    {0x6725, 0x6728, SC_Han, GC_Lo, NULL}, //  朥..木
    {0x672A, 0x6766, SC_Han, GC_Lo, NULL}, //  未..杦
    {0x6768, 0x6787, SC_Han, GC_Lo, NULL}, //  杨..枇
    {0x6789, 0x6795, SC_Han, GC_Lo, NULL}, //  枉..枕
    {0x6797, 0x67BC, SC_Han, GC_Lo, NULL}, //  林..枼
    {0x67BE, 0x67BE, SC_Han, GC_Lo, NULL}, //  枾
    {0x67C0, 0x67D4, SC_Han, GC_Lo, NULL}, //  柀..柔
    {0x67D6, 0x67D6, SC_Han, GC_Lo, NULL}, //  柖
    {0x67D8, 0x67F8, SC_Han, GC_Lo, NULL}, //  柘..柸
    {0x67FA, 0x6800, SC_Han, GC_Lo, NULL}, //  柺..栀
    {0x6802, 0x6814, SC_Han, GC_Lo, NULL}, //  栂..栔
    {0x6816, 0x6826, SC_Han, GC_Lo, NULL}, //  栖..栦
    {0x6828, 0x682F, SC_Han, GC_Lo, NULL}, //  栨..栯
    {0x6831, 0x6857, SC_Han, GC_Lo, NULL}, //  栱..桗
    {0x6859, 0x6859, SC_Han, GC_Lo, NULL}, //  桙
    {0x685B, 0x685D, SC_Han, GC_Lo, NULL}, //  桛..桝
    {0x685F, 0x6879, SC_Han, GC_Lo, NULL}, //  桟..桹
    {0x687B, 0x6894, SC_Han, GC_Lo, NULL}, //  桻..梔
    {0x6896, 0x6898, SC_Han, GC_Lo, NULL}, //  梖..梘
    {0x689A, 0x68A4, SC_Han, GC_Lo, NULL}, //  梚..梤
    {0x68A6, 0x68B7, SC_Han, GC_Lo, NULL}, //  梦..梷
    {0x68B9, 0x68C2, SC_Han, GC_Lo, NULL}, //  梹..棂
    {0x68C4, 0x68D8, SC_Han, GC_Lo, NULL}, //  棄..棘
    {0x68DA, 0x68E1, SC_Han, GC_Lo, NULL}, //  棚..棡
    {0x68E3, 0x68E4, SC_Han, GC_Lo, NULL}, //  棣..棤
    {0x68E6, 0x6908, SC_Han, GC_Lo, NULL}, //  棦..椈
    {0x690A, 0x693D, SC_Han, GC_Lo, NULL}, //  椊..椽
    {0x693F, 0x694C, SC_Han, GC_Lo, NULL}, //  椿..楌
    {0x694E, 0x699E, SC_Han, GC_Lo, NULL}, //  楎..榞
    {0x69A0, 0x69A1, SC_Han, GC_Lo, NULL}, //  榠..榡
    {0x69A3, 0x69BF, SC_Han, GC_Lo, NULL}, //  榣..榿
    {0x69C1, 0x69D0, SC_Han, GC_Lo, NULL}, //  槁..槐
    {0x69D3, 0x69D4, SC_Han, GC_Lo, NULL}, //  槓..槔
    {0x69D8, 0x6A02, SC_Han, GC_Lo, NULL}, //  様..樂
    {0x6A04, 0x6A1B, SC_Han, GC_Lo, NULL}, //  樄..樛
    {0x6A1D, 0x6A23, SC_Han, GC_Lo, NULL}, //  樝..樣
    {0x6A25, 0x6A36, SC_Han, GC_Lo, NULL}, //  樥..樶
    {0x6A38, 0x6A49, SC_Han, GC_Lo, NULL}, //  樸..橉
    {0x6A4B, 0x6A5B, SC_Han, GC_Lo, NULL}, //  橋..橛
    {0x6A5D, 0x6A6D, SC_Han, GC_Lo, NULL}, //  橝..橭
    {0x6A6F, 0x6A6F, SC_Han, GC_Lo, NULL}, //  橯
    {0x6A71, 0x6A85, SC_Han, GC_Lo, NULL}, //  橱..檅
    {0x6A87, 0x6A89, SC_Han, GC_Lo, NULL}, //  檇..檉
    {0x6A8B, 0x6A8E, SC_Han, GC_Lo, NULL}, //  檋..檎
    {0x6A90, 0x6A98, SC_Han, GC_Lo, NULL}, //  檐..檘
    {0x6A9A, 0x6A9C, SC_Han, GC_Lo, NULL}, //  檚..檜
    {0x6A9E, 0x6AB0, SC_Han, GC_Lo, NULL}, //  檞..檰
    {0x6AB2, 0x6ABD, SC_Han, GC_Lo, NULL}, //  檲..檽
    {0x6ABF, 0x6ABF, SC_Han, GC_Lo, NULL}, //  檿
    {0x6AC1, 0x6AC3, SC_Han, GC_Lo, NULL}, //  櫁..櫃
    {0x6AC5, 0x6AC8, SC_Han, GC_Lo, NULL}, //  櫅..櫈
    {0x6ACA, 0x6AD7, SC_Han, GC_Lo, NULL}, //  櫊..櫗
    {0x6AD9, 0x6AE8, SC_Han, GC_Lo, NULL}, //  櫙..櫨
    {0x6AEA, 0x6B0D, SC_Han, GC_Lo, NULL}, //  櫪..欍
    {0x6B0F, 0x6B1A, SC_Han, GC_Lo, NULL}, //  欏..欚
    {0x6B1C, 0x6B2D, SC_Han, GC_Lo, NULL}, //  欜..欭
    {0x6B2F, 0x6B34, SC_Han, GC_Lo, NULL}, //  欯..欴
    {0x6B36, 0x6B3F, SC_Han, GC_Lo, NULL}, //  欶..欿
    {0x6B41, 0x6B56, SC_Han, GC_Lo, NULL}, //  歁..歖
    {0x6B59, 0x6B5C, SC_Han, GC_Lo, NULL}, //  歙..歜
    {0x6B5E, 0x6B67, SC_Han, GC_Lo, NULL}, //  歞..歧
    {0x6B69, 0x6B6B, SC_Han, GC_Lo, NULL}, //  歩..歫
    {0x6B6D, 0x6B6D, SC_Han, GC_Lo, NULL}, //  歭
    {0x6B6F, 0x6B70, SC_Han, GC_Lo, NULL}, //  歯..歰
    {0x6B72, 0x6B74, SC_Han, GC_Lo, NULL}, //  歲..歴
    {0x6B76, 0x6B7C, SC_Han, GC_Lo, NULL}, //  歶..歼
    {0x6B7E, 0x6BB7, SC_Han, GC_Lo, NULL}, //  歾..殷
    {0x6BB9, 0x6BE8, SC_Han, GC_Lo, NULL}, //  殹..毨
    {0x6BEA, 0x6BF0, SC_Han, GC_Lo, NULL}, //  毪..毰
    {0x6BF2, 0x6BF3, SC_Han, GC_Lo, NULL}, //  毲..毳
    {0x6BF5, 0x6BF9, SC_Han, GC_Lo, NULL}, //  毵..毹
    {0x6BFB, 0x6C09, SC_Han, GC_Lo, NULL}, //  毻..氉
    {0x6C0B, 0x6C1B, SC_Han, GC_Lo, NULL}, //  氋..氛
    {0x6C1D, 0x6C2C, SC_Han, GC_Lo, NULL}, //  氝..氬
    {0x6C2E, 0x6C3B, SC_Han, GC_Lo, NULL}, //  氮..氻
    {0x6C3D, 0x6C44, SC_Han, GC_Lo, NULL}, //  氽..汄
    {0x6C46, 0x6C6B, SC_Han, GC_Lo, NULL}, //  汆..汫
    {0x6C6D, 0x6C6D, SC_Han, GC_Lo, NULL}, //  汭
    {0x6C6F, 0x6C9F, SC_Han, GC_Lo, NULL}, //  汯..沟
    {0x6CA1, 0x6CD7, SC_Han, GC_Lo, NULL}, //  没..泗
    {0x6CD9, 0x6CF3, SC_Han, GC_Lo, NULL}, //  泙..泳
    {0x6CF5, 0x6D01, SC_Han, GC_Lo, NULL}, //  泵..洁
    {0x6D03, 0x6D1B, SC_Han, GC_Lo, NULL}, //  洃..洛
    {0x6D1D, 0x6D23, SC_Han, GC_Lo, NULL}, //  洝..洣
    {0x6D25, 0x6D70, SC_Han, GC_Lo, NULL}, //  津..浰
    {0x6D72, 0x6D80, SC_Han, GC_Lo, NULL}, //  浲..涀
    {0x6D82, 0x6D95, SC_Han, GC_Lo, NULL}, //  涂..涕
    {0x6D97, 0x6DAF, SC_Han, GC_Lo, NULL}, //  涗..涯
    {0x6DB2, 0x6DB5, SC_Han, GC_Lo, NULL}, //  液..涵
    {0x6DB7, 0x6DFD, SC_Han, GC_Lo, NULL}, //  涷..淽
    {0x6E00, 0x6E00, SC_Han, GC_Lo, NULL}, //  渀
    {0x6E03, 0x6E05, SC_Han, GC_Lo, NULL}, //  渃..清
    {0x6E07, 0x6E11, SC_Han, GC_Lo, NULL}, //  渇..渑
    {0x6E13, 0x6E17, SC_Han, GC_Lo, NULL}, //  渓..渗
    {0x6E19, 0x6E29, SC_Han, GC_Lo, NULL}, //  渙..温
    {0x6E2B, 0x6E4B, SC_Han, GC_Lo, NULL}, //  渫..湋
    {0x6E4D, 0x6E6B, SC_Han, GC_Lo, NULL}, //  湍..湫
    {0x6E6D, 0x6E7A, SC_Han, GC_Lo, NULL}, //  湭..湺
    {0x6E7E, 0x6E8A, SC_Han, GC_Lo, NULL}, //  湾..溊
    {0x6E8C, 0x6E94, SC_Han, GC_Lo, NULL}, //  溌..溔
    {0x6E96, 0x6EDA, SC_Han, GC_Lo, NULL}, //  準..滚
    {0x6EDC, 0x6EE2, SC_Han, GC_Lo, NULL}, //  滜..滢
    {0x6EE4, 0x6F03, SC_Han, GC_Lo, NULL}, //  滤..漃
    {0x6F05, 0x6F0A, SC_Han, GC_Lo, NULL}, //  漅..漊
    {0x6F0C, 0x6F41, SC_Han, GC_Lo, NULL}, //  漌..潁
    {0x6F43, 0x6F47, SC_Han, GC_Lo, NULL}, //  潃..潇
    {0x6F49, 0x6F49, SC_Han, GC_Lo, NULL}, //  潉
    {0x6F4B, 0x6F78, SC_Han, GC_Lo, NULL}, //  潋..潸
    {0x6F7A, 0x6F97, SC_Han, GC_Lo, NULL}, //  潺..澗
    {0x6F99, 0x6F99, SC_Han, GC_Lo, NULL}, //  澙
    {0x6F9B, 0x6F9E, SC_Han, GC_Lo, NULL}, //  澛..澞
    {0x6FA0, 0x6FB6, SC_Han, GC_Lo, NULL}, //  澠..澶
    {0x6FB8, 0x6FC4, SC_Han, GC_Lo, NULL}, //  澸..濄
    {0x6FC6, 0x6FCF, SC_Han, GC_Lo, NULL}, //  濆..濏
    {0x6FD1, 0x6FD2, SC_Han, GC_Lo, NULL}, //  濑..濒
    {0x6FD4, 0x6FF4, SC_Han, GC_Lo, NULL}, //  濔..濴
    {0x6FF6, 0x6FFC, SC_Han, GC_Lo, NULL}, //  濶..濼
    {0x6FFE, 0x700F, SC_Han, GC_Lo, NULL}, //  濾..瀏
    {0x7011, 0x7012, SC_Han, GC_Lo, NULL}, //  瀑..瀒
    {0x7014, 0x7046, SC_Han, GC_Lo, NULL}, //  瀔..灆
    {0x7048, 0x704A, SC_Han, GC_Lo, NULL}, //  灈..灊
    {0x704C, 0x704D, SC_Han, GC_Lo, NULL}, //  灌..灍
    {0x704F, 0x7071, SC_Han, GC_Lo, NULL}, //  灏..灱
    {0x7074, 0x707A, SC_Han, GC_Lo, NULL}, //  灴..灺
    {0x707C, 0x7080, SC_Han, GC_Lo, NULL}, //  灼..炀
    {0x7082, 0x708C, SC_Han, GC_Lo, NULL}, //  炂..炌
    {0x708E, 0x7096, SC_Han, GC_Lo, NULL}, //  炎..炖
    {0x7098, 0x709A, SC_Han, GC_Lo, NULL}, //  炘..炚
    {0x709C, 0x70A9, SC_Han, GC_Lo, NULL}, //  炜..炩
    {0x70AB, 0x70B1, SC_Han, GC_Lo, NULL}, //  炫..炱
    {0x70B3, 0x70B5, SC_Han, GC_Lo, NULL}, //  炳..炵
    {0x70B7, 0x70D4, SC_Han, GC_Lo, NULL}, //  炷..烔
    {0x70D6, 0x70FD, SC_Han, GC_Lo, NULL}, //  烖..烽
    {0x70FF, 0x7107, SC_Han, GC_Lo, NULL}, //  烿..焇
    {0x7109, 0x7123, SC_Han, GC_Lo, NULL}, //  焉..焣
    {0x7125, 0x7132, SC_Han, GC_Lo, NULL}, //  焥..焲
    {0x7135, 0x7156, SC_Han, GC_Lo, NULL}, //  焵..煖
    {0x7158, 0x716A, SC_Han, GC_Lo, NULL}, //  煘..煪
    {0x716C, 0x716C, SC_Han, GC_Lo, NULL}, //  煬
    {0x716E, 0x718C, SC_Han, GC_Lo, NULL}, //  煮..熌
    {0x718E, 0x7195, SC_Han, GC_Lo, NULL}, //  熎..熕
    {0x7197, 0x71A5, SC_Han, GC_Lo, NULL}, //  熗..熥
    {0x71A7, 0x71AA, SC_Han, GC_Lo, NULL}, //  熧..熪
    {0x71AC, 0x71B5, SC_Han, GC_Lo, NULL}, //  熬..熵
    {0x71B7, 0x71CB, SC_Han, GC_Lo, NULL}, //  熷..燋
    {0x71CD, 0x71D2, SC_Han, GC_Lo, NULL}, //  燍..燒
    {0x71D4, 0x71F2, SC_Han, GC_Lo, NULL}, //  燔..燲
    {0x71F4, 0x71F9, SC_Han, GC_Lo, NULL}, //  燴..燹
    {0x71FB, 0x720A, SC_Han, GC_Lo, NULL}, //  燻..爊
    {0x720C, 0x7210, SC_Han, GC_Lo, NULL}, //  爌..爐
    {0x7212, 0x7214, SC_Han, GC_Lo, NULL}, //  爒..爔
    {0x7216, 0x7216, SC_Han, GC_Lo, NULL}, //  爖
    {0x7218, 0x721F, SC_Han, GC_Lo, NULL}, //  爘..爟
    {0x7221, 0x7223, SC_Han, GC_Lo, NULL}, //  爡..爣
    {0x7226, 0x722E, SC_Han, GC_Lo, NULL}, //  爦..爮
    {0x7230, 0x7233, SC_Han, GC_Lo, NULL}, //  爰..爳
    {0x7235, 0x7244, SC_Han, GC_Lo, NULL}, //  爵..牄
    {0x7246, 0x724D, SC_Han, GC_Lo, NULL}, //  牆..牍
    {0x724F, 0x724F, SC_Han, GC_Lo, NULL}, //  牏
    {0x7251, 0x7254, SC_Han, GC_Lo, NULL}, //  牑..牔
    {0x7256, 0x72AA, SC_Han, GC_Lo, NULL}, //  牖..犪
    {0x72AC, 0x72BD, SC_Han, GC_Lo, NULL}, //  犬..犽
    {0x72BF, 0x7301, SC_Han, GC_Lo, NULL}, //  犿..猁
    {0x7303, 0x730F, SC_Han, GC_Lo, NULL}, //  猃..猏
    {0x7311, 0x7327, SC_Han, GC_Lo, NULL}, //  猑..猧
    {0x7329, 0x7352, SC_Han, GC_Lo, NULL}, //  猩..獒
    {0x7354, 0x739B, SC_Han, GC_Lo, NULL}, //  獔..玛
    {0x739D, 0x73C0, SC_Han, GC_Lo, NULL}, //  玝..珀
    {0x73C2, 0x73F2, SC_Han, GC_Lo, NULL}, //  珂..珲
    {0x73F4, 0x73FA, SC_Han, GC_Lo, NULL}, //  珴..珺
    {0x73FC, 0x7417, SC_Han, GC_Lo, NULL}, //  珼..琗
    {0x7419, 0x7438, SC_Han, GC_Lo, NULL}, //  琙..琸
    {0x743A, 0x743D, SC_Han, GC_Lo, NULL}, //  琺..琽
    {0x743F, 0x7446, SC_Han, GC_Lo, NULL}, //  琿..瑆
    {0x7448, 0x7448, SC_Han, GC_Lo, NULL}, //  瑈
    {0x744A, 0x7457, SC_Han, GC_Lo, NULL}, //  瑊..瑗
    {0x7459, 0x747A, SC_Han, GC_Lo, NULL}, //  瑙..瑺
    {0x747C, 0x7483, SC_Han, GC_Lo, NULL}, //  瑼..璃
    {0x7485, 0x7495, SC_Han, GC_Lo, NULL}, //  璅..璕
    {0x7497, 0x749C, SC_Han, GC_Lo, NULL}, //  璗..璜
    {0x749E, 0x74C6, SC_Han, GC_Lo, NULL}, //  璞..瓆
    {0x74C8, 0x74C8, SC_Han, GC_Lo, NULL}, //  瓈
    {0x74CA, 0x74CB, SC_Han, GC_Lo, NULL}, //  瓊..瓋
    {0x74CD, 0x74EA, SC_Han, GC_Lo, NULL}, //  瓍..瓪
    {0x74EC, 0x751F, SC_Han, GC_Lo, NULL}, //  瓬..生
    {0x7521, 0x7540, SC_Han, GC_Lo, NULL}, //  甡..畀
    {0x7542, 0x7551, SC_Han, GC_Lo, NULL}, //  畂..畑
    {0x7553, 0x7554, SC_Han, GC_Lo, NULL}, //  畓..畔
    {0x7556, 0x755D, SC_Han, GC_Lo, NULL}, //  畖..畝
    {0x755F, 0x7560, SC_Han, GC_Lo, NULL}, //  畟..畠
    {0x7562, 0x7570, SC_Han, GC_Lo, NULL}, //  畢..異
    {0x7572, 0x757A, SC_Han, GC_Lo, NULL}, //  畲..畺
    {0x757C, 0x7584, SC_Han, GC_Lo, NULL}, //  畼..疄
    {0x7586, 0x75A8, SC_Han, GC_Lo, NULL}, //  疆..疨
    {0x75AA, 0x75B6, SC_Han, GC_Lo, NULL}, //  疪..疶
    {0x75B8, 0x75DB, SC_Han, GC_Lo, NULL}, //  疸..痛
    {0x75DD, 0x75ED, SC_Han, GC_Lo, NULL}, //  痝..痭
    {0x75EF, 0x762B, SC_Han, GC_Lo, NULL}, //  痯..瘫
    {0x762D, 0x7643, SC_Han, GC_Lo, NULL}, //  瘭..癃
    {0x7646, 0x7650, SC_Han, GC_Lo, NULL}, //  癆..癐
    {0x7652, 0x7654, SC_Han, GC_Lo, NULL}, //  癒..癔
    {0x7656, 0x7672, SC_Han, GC_Lo, NULL}, //  癖..癲
    {0x7674, 0x768C, SC_Han, GC_Lo, NULL}, //  癴..皌
    {0x768E, 0x76A0, SC_Han, GC_Lo, NULL}, //  皎..皠
    {0x76A3, 0x76A4, SC_Han, GC_Lo, NULL}, //  皣..皤
    {0x76A6, 0x76A7, SC_Han, GC_Lo, NULL}, //  皦..皧
    {0x76A9, 0x76B2, SC_Han, GC_Lo, NULL}, //  皩..皲
    {0x76B4, 0x76B5, SC_Han, GC_Lo, NULL}, //  皴..皵
    {0x76B7, 0x76C0, SC_Han, GC_Lo, NULL}, //  皷..盀
    {0x76C2, 0x76CA, SC_Han, GC_Lo, NULL}, //  盂..益
    {0x76CC, 0x76D8, SC_Han, GC_Lo, NULL}, //  盌..盘
    {0x76DA, 0x76EA, SC_Han, GC_Lo, NULL}, //  盚..盪
    {0x76EC, 0x76FF, SC_Han, GC_Lo, NULL}, //  盬..盿
    {0x7701, 0x7701, SC_Han, GC_Lo, NULL}, //  省
    {0x7703, 0x770D, SC_Han, GC_Lo, NULL}, //  眃..眍
    {0x770F, 0x7720, SC_Han, GC_Lo, NULL}, //  眏..眠
    {0x7722, 0x772A, SC_Han, GC_Lo, NULL}, //  眢..眪
    {0x772C, 0x773E, SC_Han, GC_Lo, NULL}, //  眬..眾
    {0x7740, 0x7741, SC_Han, GC_Lo, NULL}, //  着..睁
    {0x7743, 0x7763, SC_Han, GC_Lo, NULL}, //  睃..督
    {0x7765, 0x7795, SC_Han, GC_Lo, NULL}, //  睥..瞕
    {0x7797, 0x77A3, SC_Han, GC_Lo, NULL}, //  瞗..瞣
    {0x77A5, 0x77BD, SC_Han, GC_Lo, NULL}, //  瞥..瞽
    {0x77BF, 0x77C0, SC_Han, GC_Lo, NULL}, //  瞿..矀
    {0x77C2, 0x77D1, SC_Han, GC_Lo, NULL}, //  矂..矑
    {0x77D3, 0x77DC, SC_Han, GC_Lo, NULL}, //  矓..矜
    {0x77DE, 0x77E3, SC_Han, GC_Lo, NULL}, //  矞..矣
    {0x77E5, 0x77E5, SC_Han, GC_Lo, NULL}, //  知
    {0x77E7, 0x77F3, SC_Han, GC_Lo, NULL}, //  矧..石
    {0x77F6, 0x7823, SC_Han, GC_Lo, NULL}, //  矶..砣
    {0x7825, 0x7835, SC_Han, GC_Lo, NULL}, //  砥..砵
    {0x7837, 0x7841, SC_Han, GC_Lo, NULL}, //  砷..硁
    {0x7843, 0x7845, SC_Han, GC_Lo, NULL}, //  硃..硅
    {0x7847, 0x784A, SC_Han, GC_Lo, NULL}, //  硇..硊
    {0x784C, 0x7875, SC_Han, GC_Lo, NULL}, //  硌..硵
    {0x7877, 0x7887, SC_Han, GC_Lo, NULL}, //  硷..碇
    {0x7889, 0x78C1, SC_Han, GC_Lo, NULL}, //  碉..磁
    {0x78C3, 0x78C6, SC_Han, GC_Lo, NULL}, //  磃..磆
    {0x78C8, 0x78D1, SC_Han, GC_Lo, NULL}, //  磈..磑
    {0x78D3, 0x78EF, SC_Han, GC_Lo, NULL}, //  磓..磯
    {0x78F1, 0x78F7, SC_Han, GC_Lo, NULL}, //  磱..磷
    {0x78F9, 0x78FF, SC_Han, GC_Lo, NULL}, //  磹..磿
    {0x7901, 0x7907, SC_Han, GC_Lo, NULL}, //  礁..礇
    {0x7909, 0x790C, SC_Han, GC_Lo, NULL}, //  礉..礌
    {0x790E, 0x7914, SC_Han, GC_Lo, NULL}, //  礎..礔
    {0x7916, 0x791E, SC_Han, GC_Lo, NULL}, //  礖..礞
    {0x7921, 0x7931, SC_Han, GC_Lo, NULL}, //  礡..礱
    {0x7933, 0x7935, SC_Han, GC_Lo, NULL}, //  礳..礵
    {0x7937, 0x7958, SC_Han, GC_Lo, NULL}, //  礷..祘
    {0x795A, 0x796B, SC_Han, GC_Lo, NULL}, //  祚..祫
    {0x796D, 0x796D, SC_Han, GC_Lo, NULL}, //  祭
    {0x796F, 0x7974, SC_Han, GC_Lo, NULL}, //  祯..祴
    {0x7977, 0x7985, SC_Han, GC_Lo, NULL}, //  祷..禅
    {0x7988, 0x799D, SC_Han, GC_Lo, NULL}, //  禈..禝
    {0x799F, 0x79A8, SC_Han, GC_Lo, NULL}, //  禟..禨
    {0x79AA, 0x79BB, SC_Han, GC_Lo, NULL}, //  禪..离
    {0x79BD, 0x79C3, SC_Han, GC_Lo, NULL}, //  禽..秃
    {0x79C5, 0x79C6, SC_Han, GC_Lo, NULL}, //  秅..秆
    {0x79C8, 0x79CB, SC_Han, GC_Lo, NULL}, //  秈..秋
    {0x79CD, 0x79D3, SC_Han, GC_Lo, NULL}, //  种..秓
    {0x79D5, 0x79D6, SC_Han, GC_Lo, NULL}, //  秕..秖
    {0x79D8, 0x7A00, SC_Han, GC_Lo, NULL}, //  秘..稀
    {0x7A02, 0x7A06, SC_Han, GC_Lo, NULL}, //  稂..稆
    {0x7A08, 0x7A08, SC_Han, GC_Lo, NULL}, //  稈
    {0x7A0A, 0x7A2B, SC_Han, GC_Lo, NULL}, //  稊..稫
    {0x7A2D, 0x7A37, SC_Han, GC_Lo, NULL}, //  稭..稷
    {0x7A39, 0x7A39, SC_Han, GC_Lo, NULL}, //  稹
    {0x7A3B, 0x7A63, SC_Han, GC_Lo, NULL}, //  稻..穣
    {0x7A65, 0x7A69, SC_Han, GC_Lo, NULL}, //  穥..穩
    {0x7A6B, 0x7A6E, SC_Han, GC_Lo, NULL}, //  穫..穮
    {0x7A70, 0x7A81, SC_Han, GC_Lo, NULL}, //  穰..突
    {0x7A83, 0x7A99, SC_Han, GC_Lo, NULL}, //  窃..窙
    {0x7A9C, 0x7AB8, SC_Han, GC_Lo, NULL}, //  窜..窸
    {0x7ABA, 0x7ABA, SC_Han, GC_Lo, NULL}, //  窺
    {0x7ABE, 0x7AC1, SC_Han, GC_Lo, NULL}, //  窾..竁
    {0x7AC3, 0x7AC5, SC_Han, GC_Lo, NULL}, //  竃..竅
    {0x7AC7, 0x7AE8, SC_Han, GC_Lo, NULL}, //  竇..竨
    {0x7AEA, 0x7AF4, SC_Han, GC_Lo, NULL}, //  竪..竴
    {0x7AF6, 0x7AFB, SC_Han, GC_Lo, NULL}, //  競..竻
    {0x7AFD, 0x7B06, SC_Han, GC_Lo, NULL}, //  竽..笆
    {0x7B08, 0x7B1E, SC_Han, GC_Lo, NULL}, //  笈..笞
    {0x7B20, 0x7B26, SC_Han, GC_Lo, NULL}, //  笠..符
    {0x7B28, 0x7B28, SC_Han, GC_Lo, NULL}, //  笨
    {0x7B2A, 0x7B41, SC_Han, GC_Lo, NULL}, //  笪..筁
    {0x7B43, 0x7B52, SC_Han, GC_Lo, NULL}, //  筃..筒
    {0x7B54, 0x7BA2, SC_Han, GC_Lo, NULL}, //  答..箢
    {0x7BA4, 0x7BA4, SC_Han, GC_Lo, NULL}, //  箤
    {0x7BA6, 0x7BAF, SC_Han, GC_Lo, NULL}, //  箦..箯
    {0x7BB1, 0x7BB1, SC_Han, GC_Lo, NULL}, //  箱
    {0x7BB3, 0x7BF9, SC_Han, GC_Lo, NULL}, //  箳..篹
    {0x7BFB, 0x7C1A, SC_Han, GC_Lo, NULL}, //  篻..簚
    {0x7C1C, 0x7C2D, SC_Han, GC_Lo, NULL}, //  簜..簭
    {0x7C30, 0x7C51, SC_Han, GC_Lo, NULL}, //  簰..籑
    {0x7C53, 0x7C54, SC_Han, GC_Lo, NULL}, //  籓..籔
    {0x7C56, 0x7C5C, SC_Han, GC_Lo, NULL}, //  籖..籜
    {0x7C5E, 0x7C75, SC_Han, GC_Lo, NULL}, //  籞..籵
    {0x7C77, 0x7C86, SC_Han, GC_Lo, NULL}, //  籷..粆
    {0x7C88, 0x7C92, SC_Han, GC_Lo, NULL}, //  粈..粒
    {0x7C94, 0x7C99, SC_Han, GC_Lo, NULL}, //  粔..粙
    {0x7C9B, 0x7CAB, SC_Han, GC_Lo, NULL}, //  粛..粫
    {0x7CAD, 0x7CD2, SC_Han, GC_Lo, NULL}, //  粭..糒
    {0x7CD4, 0x7CD9, SC_Han, GC_Lo, NULL}, //  糔..糙
    {0x7CDC, 0x7CE0, SC_Han, GC_Lo, NULL}, //  糜..糠
    {0x7CE2, 0x7CE2, SC_Han, GC_Lo, NULL}, //  糢
    {0x7CE4, 0x7CE4, SC_Han, GC_Lo, NULL}, //  糤
    {0x7CE7, 0x7CFB, SC_Han, GC_Lo, NULL}, //  糧..系
    {0x7CFD, 0x7CFE, SC_Han, GC_Lo, NULL}, //  糽..糾
    {0x7D00, 0x7D22, SC_Han, GC_Lo, NULL}, //  紀..索
    {0x7D24, 0x7D29, SC_Han, GC_Lo, NULL}, //  紤..紩
    {0x7D2B, 0x7D2C, SC_Han, GC_Lo, NULL}, //  紫..紬
    {0x7D2E, 0x7D47, SC_Han, GC_Lo, NULL}, //  紮..絇
    {0x7D49, 0x7D4C, SC_Han, GC_Lo, NULL}, //  絉..経
    {0x7D4E, 0x7D59, SC_Han, GC_Lo, NULL}, //  絎..絙
    {0x7D5B, 0x7D63, SC_Han, GC_Lo, NULL}, //  絛..絣
    {0x7D65, 0x7D77, SC_Han, GC_Lo, NULL}, //  絥..絷
    {0x7D79, 0x7D81, SC_Han, GC_Lo, NULL}, //  絹..綁
    {0x7D83, 0x7D94, SC_Han, GC_Lo, NULL}, //  綃..綔
    {0x7D96, 0x7D97, SC_Han, GC_Lo, NULL}, //  綖..綗
    {0x7D99, 0x7DA3, SC_Han, GC_Lo, NULL}, //  継..綣
    {0x7DA5, 0x7DA7, SC_Han, GC_Lo, NULL}, //  綥..綧
    {0x7DA9, 0x7DCC, SC_Han, GC_Lo, NULL}, //  綩..緌
    {0x7DCE, 0x7DD2, SC_Han, GC_Lo, NULL}, //  緎..緒
    {0x7DD4, 0x7DE4, SC_Han, GC_Lo, NULL}, //  緔..緤
    {0x7DE6, 0x7DEA, SC_Han, GC_Lo, NULL}, //  緦..緪
    {0x7DEC, 0x7DFC, SC_Han, GC_Lo, NULL}, //  緬..緼
    {0x7E00, 0x7E17, SC_Han, GC_Lo, NULL}, //  縀..縗
    {0x7E19, 0x7E5A, SC_Han, GC_Lo, NULL}, //  縙..繚
    {0x7E5C, 0x7E63, SC_Han, GC_Lo, NULL}, //  繜..繣
    {0x7E65, 0x7E9C, SC_Han, GC_Lo, NULL}, //  繥..纜
    {0x7E9E, 0x7F3A, SC_Han, GC_Lo, NULL}, //  纞..缺
    {0x7F3D, 0x7F40, SC_Han, GC_Lo, NULL}, //  缽..罀
    {0x7F42, 0x7F45, SC_Han, GC_Lo, NULL}, //  罂..罅
    {0x7F47, 0x7F58, SC_Han, GC_Lo, NULL}, //  罇..罘
    {0x7F5A, 0x7F83, SC_Han, GC_Lo, NULL}, //  罚..羃
    {0x7F85, 0x7F8F, SC_Han, GC_Lo, NULL}, //  羅..羏
    {0x7F91, 0x7F96, SC_Han, GC_Lo, NULL}, //  羑..羖
    {0x7F98, 0x7F98, SC_Han, GC_Lo, NULL}, //  羘
    {0x7F9A, 0x7FB3, SC_Han, GC_Lo, NULL}, //  羚..羳
    {0x7FB5, 0x7FD5, SC_Han, GC_Lo, NULL}, //  羵..翕
    {0x7FD7, 0x7FDC, SC_Han, GC_Lo, NULL}, //  翗..翜
    {0x7FDE, 0x7FE3, SC_Han, GC_Lo, NULL}, //  翞..翣
    {0x7FE5, 0x8009, SC_Han, GC_Lo, NULL}, //  翥..耉
    {0x800B, 0x802E, SC_Han, GC_Lo, NULL}, //  耋..耮
    {0x8030, 0x803B, SC_Han, GC_Lo, NULL}, //  耰..耻
    {0x803D, 0x803F, SC_Han, GC_Lo, NULL}, //  耽..耿
    {0x8041, 0x8065, SC_Han, GC_Lo, NULL}, //  聁..聥
    {0x8067, 0x8087, SC_Han, GC_Lo, NULL}, //  聧..肇
    {0x8089, 0x808D, SC_Han, GC_Lo, NULL}, //  肉..肍
    {0x808F, 0x8093, SC_Han, GC_Lo, NULL}, //  肏..肓
    {0x8095, 0x80A5, SC_Han, GC_Lo, NULL}, //  肕..肥
    {0x80A9, 0x80B2, SC_Han, GC_Lo, NULL}, //  肩..育
    {0x80B4, 0x80B8, SC_Han, GC_Lo, NULL}, //  肴..肸
    {0x80BA, 0x80DE, SC_Han, GC_Lo, NULL}, //  肺..胞
    {0x80E0, 0x8102, SC_Han, GC_Lo, NULL}, //  胠..脂
    {0x8105, 0x8133, SC_Han, GC_Lo, NULL}, //  脅..脳
    {0x8136, 0x8183, SC_Han, GC_Lo, NULL}, //  脶..膃
    {0x8185, 0x818F, SC_Han, GC_Lo, NULL}, //  膅..膏
    {0x8191, 0x8195, SC_Han, GC_Lo, NULL}, //  膑..膕
    {0x8197, 0x81CA, SC_Han, GC_Lo, NULL}, //  膗..臊
    {0x81CC, 0x81E3, SC_Han, GC_Lo, NULL}, //  臌..臣
    {0x81E5, 0x81EE, SC_Han, GC_Lo, NULL}, //  臥..臮
    {0x81F1, 0x8212, SC_Han, GC_Lo, NULL}, //  臱..舒
    {0x8214, 0x8223, SC_Han, GC_Lo, NULL}, //  舔..舣
    {0x8225, 0x8240, SC_Han, GC_Lo, NULL}, //  舥..艀
    {0x8242, 0x8264, SC_Han, GC_Lo, NULL}, //  艂..艤
    {0x8266, 0x828B, SC_Han, GC_Lo, NULL}, //  艦..芋
    {0x828D, 0x82B1, SC_Han, GC_Lo, NULL}, //  芍..花
    {0x82B3, 0x82E1, SC_Han, GC_Lo, NULL}, //  芳..苡
    {0x82E3, 0x82FB, SC_Han, GC_Lo, NULL}, //  苣..苻
    {0x82FD, 0x8309, SC_Han, GC_Lo, NULL}, //  苽..茉
    {0x830B, 0x830F, SC_Han, GC_Lo, NULL}, //  茋..茏
    {0x8311, 0x832F, SC_Han, GC_Lo, NULL}, //  茑..茯
    {0x8331, 0x8354, SC_Han, GC_Lo, NULL}, //  茱..荔
    {0x8356, 0x83BD, SC_Han, GC_Lo, NULL}, //  荖..莽
    {0x83BF, 0x83E5, SC_Han, GC_Lo, NULL}, //  莿..菥
    {0x83E7, 0x83EC, SC_Han, GC_Lo, NULL}, //  菧..菬
    {0x83EE, 0x8413, SC_Han, GC_Lo, NULL}, //  菮..萓
    {0x8415, 0x8415, SC_Han, GC_Lo, NULL}, //  萕
    {0x8418, 0x841E, SC_Han, GC_Lo, NULL}, //  萘..萞
    {0x8420, 0x8457, SC_Han, GC_Lo, NULL}, //  萠..著
    {0x8459, 0x8482, SC_Han, GC_Lo, NULL}, //  葙..蒂
    {0x8484, 0x8494, SC_Han, GC_Lo, NULL}, //  蒄..蒔
    {0x8496, 0x84B6, SC_Han, GC_Lo, NULL}, //  蒖..蒶
    {0x84B8, 0x84C2, SC_Han, GC_Lo, NULL}, //  蒸..蓂
    {0x84C4, 0x84EC, SC_Han, GC_Lo, NULL}, //  蓄..蓬
    {0x84EE, 0x8504, SC_Han, GC_Lo, NULL}, //  蓮..蔄
    {0x8506, 0x850F, SC_Han, GC_Lo, NULL}, //  蔆..蔏
    {0x8511, 0x8531, SC_Han, GC_Lo, NULL}, //  蔑..蔱
    {0x8534, 0x854B, SC_Han, GC_Lo, NULL}, //  蔴..蕋
    {0x854D, 0x854F, SC_Han, GC_Lo, NULL}, //  蕍..蕏
    {0x8551, 0x857E, SC_Han, GC_Lo, NULL}, //  蕑..蕾
    {0x8580, 0x8592, SC_Han, GC_Lo, NULL}, //  薀..薒
    {0x8594, 0x85B1, SC_Han, GC_Lo, NULL}, //  薔..薱
    {0x85B3, 0x85BA, SC_Han, GC_Lo, NULL}, //  薳..薺
    {0x85BC, 0x85CB, SC_Han, GC_Lo, NULL}, //  薼..藋
    {0x85CD, 0x85ED, SC_Han, GC_Lo, NULL}, //  藍..藭
    {0x85EF, 0x85F2, SC_Han, GC_Lo, NULL}, //  藯..藲
    {0x85F4, 0x85FB, SC_Han, GC_Lo, NULL}, //  藴..藻
    {0x85FD, 0x8602, SC_Han, GC_Lo, NULL}, //  藽..蘂
    {0x8604, 0x860C, SC_Han, GC_Lo, NULL}, //  蘄..蘌
    {0x860F, 0x860F, SC_Han, GC_Lo, NULL}, //  蘏
    {0x8611, 0x8614, SC_Han, GC_Lo, NULL}, //  蘑..蘔
    {0x8616, 0x861C, SC_Han, GC_Lo, NULL}, //  蘖..蘜
    {0x861E, 0x8636, SC_Han, GC_Lo, NULL}, //  蘞..蘶
    {0x8638, 0x8656, SC_Han, GC_Lo, NULL}, //  蘸..虖
    {0x8658, 0x8674, SC_Han, GC_Lo, NULL}, //  虘..虴
    {0x8676, 0x8688, SC_Han, GC_Lo, NULL}, //  虶..蚈
    {0x868A, 0x8691, SC_Han, GC_Lo, NULL}, //  蚊..蚑
    {0x8693, 0x869F, SC_Han, GC_Lo, NULL}, //  蚓..蚟
    {0x86A1, 0x86A5, SC_Han, GC_Lo, NULL}, //  蚡..蚥
    {0x86A7, 0x86D4, SC_Han, GC_Lo, NULL}, //  蚧..蛔
    {0x86D6, 0x86DF, SC_Han, GC_Lo, NULL}, //  蛖..蛟
    {0x86E1, 0x86E6, SC_Han, GC_Lo, NULL}, //  蛡..蛦
    {0x86E8, 0x86FC, SC_Han, GC_Lo, NULL}, //  蛨..蛼
    {0x86FE, 0x871C, SC_Han, GC_Lo, NULL}, //  蛾..蜜
    {0x871E, 0x872E, SC_Han, GC_Lo, NULL}, //  蜞..蜮
    {0x8730, 0x873C, SC_Han, GC_Lo, NULL}, //  蜰..蜼
    {0x873E, 0x8744, SC_Han, GC_Lo, NULL}, //  蜾..蝄
    {0x8746, 0x8770, SC_Han, GC_Lo, NULL}, //  蝆..蝰
    {0x8772, 0x878D, SC_Han, GC_Lo, NULL}, //  蝲..融
    {0x878F, 0x8798, SC_Han, GC_Lo, NULL}, //  螏..螘
    {0x879A, 0x87D9, SC_Han, GC_Lo, NULL}, //  螚..蟙
    {0x87DB, 0x87EF, SC_Han, GC_Lo, NULL}, //  蟛..蟯
    {0x87F1, 0x8806, SC_Han, GC_Lo, NULL}, //  蟱..蠆
    {0x8808, 0x8811, SC_Han, GC_Lo, NULL}, //  蠈..蠑
    {0x8813, 0x882C, SC_Han, GC_Lo, NULL}, //  蠓..蠬
    {0x882E, 0x8839, SC_Han, GC_Lo, NULL}, //  蠮..蠹
    {0x883B, 0x8846, SC_Han, GC_Lo, NULL}, //  蠻..衆
    {0x8848, 0x8857, SC_Han, GC_Lo, NULL}, //  衈..街
    {0x8859, 0x885B, SC_Han, GC_Lo, NULL}, //  衙..衛
    {0x885D, 0x885E, SC_Han, GC_Lo, NULL}, //  衝..衞
    {0x8860, 0x8879, SC_Han, GC_Lo, NULL}, //  衠..衹
    {0x887B, 0x88E5, SC_Han, GC_Lo, NULL}, //  衻..裥
    {0x88E7, 0x88E8, SC_Han, GC_Lo, NULL}, //  裧..裨
    {0x88EA, 0x88EC, SC_Han, GC_Lo, NULL}, //  裪..裬
    {0x88EE, 0x8902, SC_Han, GC_Lo, NULL}, //  裮..褂
    {0x8904, 0x890E, SC_Han, GC_Lo, NULL}, //  褄..褎
    {0x8910, 0x8923, SC_Han, GC_Lo, NULL}, //  褐..褣
    {0x8925, 0x8964, SC_Han, GC_Lo, NULL}, //  褥..襤
    {0x8966, 0x8974, SC_Han, GC_Lo, NULL}, //  襦..襴
    {0x8976, 0x897C, SC_Han, GC_Lo, NULL}, //  襶..襼
    {0x897E, 0x898C, SC_Han, GC_Lo, NULL}, //  襾..覌
    {0x898E, 0x898F, SC_Han, GC_Lo, NULL}, //  覎..規
    {0x8991, 0x8993, SC_Han, GC_Lo, NULL}, //  覑..覓
    {0x8995, 0x8998, SC_Han, GC_Lo, NULL}, //  覕..覘
    {0x899A, 0x89AF, SC_Han, GC_Lo, NULL}, //  覚..覯
    {0x89B1, 0x89B3, SC_Han, GC_Lo, NULL}, //  覱..観
    {0x89B5, 0x89BA, SC_Han, GC_Lo, NULL}, //  覵..覺
    {0x89BD, 0x89ED, SC_Han, GC_Lo, NULL}, //  覽..觭
    {0x89EF, 0x89F4, SC_Han, GC_Lo, NULL}, //  觯..觴
    {0x89F6, 0x89F8, SC_Han, GC_Lo, NULL}, //  觶..觸
    {0x89FA, 0x89FC, SC_Han, GC_Lo, NULL}, //  觺..觼
    {0x89FE, 0x8A04, SC_Han, GC_Lo, NULL}, //  觾..訄
    {0x8A07, 0x8A13, SC_Han, GC_Lo, NULL}, //  訇..訓
    {0x8A15, 0x8A18, SC_Han, GC_Lo, NULL}, //  訕..記
    {0x8A1A, 0x8A1F, SC_Han, GC_Lo, NULL}, //  訚..訟
    {0x8A22, 0x8A2A, SC_Han, GC_Lo, NULL}, //  訢..訪
    {0x8A2C, 0x8A3C, SC_Han, GC_Lo, NULL}, //  訬..証
    {0x8A3E, 0x8A4A, SC_Han, GC_Lo, NULL}, //  訾..詊
    {0x8A4C, 0x8A63, SC_Han, GC_Lo, NULL}, //  詌..詣
    {0x8A65, 0x8A77, SC_Han, GC_Lo, NULL}, //  詥..詷
    {0x8A79, 0x8A7C, SC_Han, GC_Lo, NULL}, //  詹..詼
    {0x8A7E, 0x8A87, SC_Han, GC_Lo, NULL}, //  詾..誇
    {0x8A89, 0x8A9E, SC_Han, GC_Lo, NULL}, //  誉..語
    {0x8AA0, 0x8AAE, SC_Han, GC_Lo, NULL}, //  誠..誮
    {0x8AB0, 0x8AB6, SC_Han, GC_Lo, NULL}, //  誰..誶
    {0x8AB8, 0x8ACF, SC_Han, GC_Lo, NULL}, //  誸..諏
    {0x8AD1, 0x8AEB, SC_Han, GC_Lo, NULL}, //  諑..諫
    {0x8AED, 0x8B28, SC_Han, GC_Lo, NULL}, //  諭..謨
    {0x8B2A, 0x8B31, SC_Han, GC_Lo, NULL}, //  謪..謱
    {0x8B33, 0x8B37, SC_Han, GC_Lo, NULL}, //  謳..謷
    {0x8B39, 0x8B3E, SC_Han, GC_Lo, NULL}, //  謹..謾
    {0x8B40, 0x8B60, SC_Han, GC_Lo, NULL}, //  譀..譠
    {0x8B63, 0x8B68, SC_Han, GC_Lo, NULL}, //  譣..譨
    {0x8B6A, 0x8B74, SC_Han, GC_Lo, NULL}, //  譪..譴
    {0x8B76, 0x8B7B, SC_Han, GC_Lo, NULL}, //  譶..譻
    {0x8B7D, 0x8B80, SC_Han, GC_Lo, NULL}, //  譽..讀
    {0x8B82, 0x8B86, SC_Han, GC_Lo, NULL}, //  讂..讆
    {0x8B88, 0x8B8C, SC_Han, GC_Lo, NULL}, //  讈..讌
    {0x8B8E, 0x8B8E, SC_Han, GC_Lo, NULL}, //  讎
    {0x8B90, 0x8B9A, SC_Han, GC_Lo, NULL}, //  讐..讚
    {0x8B9C, 0x8C37, SC_Han, GC_Lo, NULL}, //  讜..谷
    {0x8C39, 0x8C3F, SC_Han, GC_Lo, NULL}, //  谹..谿
    {0x8C41, 0x8C43, SC_Han, GC_Lo, NULL}, //  豁..豃
    {0x8C45, 0x8C50, SC_Han, GC_Lo, NULL}, //  豅..豐
    {0x8C54, 0x8C57, SC_Han, GC_Lo, NULL}, //  豔..豗
    {0x8C59, 0x8C73, SC_Han, GC_Lo, NULL}, //  豙..豳
    {0x8C75, 0x8C7E, SC_Han, GC_Lo, NULL}, //  豵..豾
    {0x8C80, 0x8C82, SC_Han, GC_Lo, NULL}, //  貀..貂
    {0x8C84, 0x8C86, SC_Han, GC_Lo, NULL}, //  貄..貆
    {0x8C88, 0x8C8A, SC_Han, GC_Lo, NULL}, //  貈..貊
    {0x8C8C, 0x8C9A, SC_Han, GC_Lo, NULL}, //  貌..貚
    {0x8C9C, 0x8CA5, SC_Han, GC_Lo, NULL}, //  貜..貥
    {0x8CA7, 0x8CCA, SC_Han, GC_Lo, NULL}, //  貧..賊
    {0x8CCC, 0x8CD5, SC_Han, GC_Lo, NULL}, //  賌..賕
    {0x8CD7, 0x8CD7, SC_Han, GC_Lo, NULL}, //  賗
    {0x8CD9, 0x8CE8, SC_Han, GC_Lo, NULL}, //  賙..賨
    {0x8CEA, 0x8CF6, SC_Han, GC_Lo, NULL}, //  質..賶
    {0x8CF8, 0x8D00, SC_Han, GC_Lo, NULL}, //  賸..贀
    {0x8D02, 0x8D10, SC_Han, GC_Lo, NULL}, //  贂..贐
    {0x8D13, 0x8D7B, SC_Han, GC_Lo, NULL}, //  贓..赻
    {0x8D7D, 0x8DA5, SC_Han, GC_Lo, NULL}, //  赽..趥
    {0x8DA7, 0x8DBF, SC_Han, GC_Lo, NULL}, //  趧..趿
    {0x8DC1, 0x8DE4, SC_Han, GC_Lo, NULL}, //  跁..跤
    {0x8DE6, 0x8E00, SC_Han, GC_Lo, NULL}, //  跦..踀
    {0x8E02, 0x8E0A, SC_Han, GC_Lo, NULL}, //  踂..踊
    {0x8E0C, 0x8E31, SC_Han, GC_Lo, NULL}, //  踌..踱
    {0x8E33, 0x8E45, SC_Han, GC_Lo, NULL}, //  踳..蹅
    {0x8E47, 0x8E4E, SC_Han, GC_Lo, NULL}, //  蹇..蹎
    {0x8E50, 0x8E6D, SC_Han, GC_Lo, NULL}, //  蹐..蹭
    {0x8E6F, 0x8E74, SC_Han, GC_Lo, NULL}, //  蹯..蹴
    {0x8E76, 0x8E76, SC_Han, GC_Lo, NULL}, //  蹶
    {0x8E78, 0x8E78, SC_Han, GC_Lo, NULL}, //  蹸
    {0x8E7A, 0x8E9A, SC_Han, GC_Lo, NULL}, //  蹺..躚
    {0x8E9C, 0x8EA1, SC_Han, GC_Lo, NULL}, //  躜..躡
    {0x8EA3, 0x8EB2, SC_Han, GC_Lo, NULL}, //  躣..躲
    {0x8EB4, 0x8EB5, SC_Han, GC_Lo, NULL}, //  躴..躵
    {0x8EB8, 0x8EC0, SC_Han, GC_Lo, NULL}, //  躸..軀
    {0x8EC2, 0x8EC3, SC_Han, GC_Lo, NULL}, //  軂..軃
    {0x8EC5, 0x8ED8, SC_Han, GC_Lo, NULL}, //  軅..軘
    {0x8EDA, 0x8EEF, SC_Han, GC_Lo, NULL}, //  軚..軯
    {0x8EF1, 0x8F0E, SC_Han, GC_Lo, NULL}, //  軱..輎
    {0x8F10, 0x8F2C, SC_Han, GC_Lo, NULL}, //  輐..輬
    {0x8F2E, 0x8F39, SC_Han, GC_Lo, NULL}, //  輮..輹
    {0x8F3B, 0x8F40, SC_Han, GC_Lo, NULL}, //  輻..轀
    {0x8F42, 0x8F9C, SC_Han, GC_Lo, NULL}, //  轂..辜
    {0x8F9E, 0x8FA3, SC_Han, GC_Lo, NULL}, //  辞..辣
    {0x8FA5, 0x8FB2, SC_Han, GC_Lo, NULL}, //  辥..農
    {0x8FB4, 0x8FC2, SC_Han, GC_Lo, NULL}, //  辴..迂
    {0x8FC4, 0x8FC9, SC_Han, GC_Lo, NULL}, //  迄..迉
    {0x8FCB, 0x8FE6, SC_Han, GC_Lo, NULL}, //  迋..迦
    {0x8FE8, 0x9029, SC_Han, GC_Lo, NULL}, //  迨..逩
    {0x902B, 0x902B, SC_Han, GC_Lo, NULL}, //  逫
    {0x902D, 0x9036, SC_Han, GC_Lo, NULL}, //  逭..逶
    {0x9038, 0x903F, SC_Han, GC_Lo, NULL}, //  逸..逿
    {0x9041, 0x9045, SC_Han, GC_Lo, NULL}, //  遁..遅
    {0x9047, 0x90AA, SC_Han, GC_Lo, NULL}, //  遇..邪
    {0x90AC, 0x90CB, SC_Han, GC_Lo, NULL}, //  邬..郋
    {0x90CE, 0x90D1, SC_Han, GC_Lo, NULL}, //  郎..郑
    {0x90D3, 0x90F5, SC_Han, GC_Lo, NULL}, //  郓..郵
    {0x90F7, 0x9109, SC_Han, GC_Lo, NULL}, //  郷..鄉
    {0x910B, 0x913B, SC_Han, GC_Lo, NULL}, //  鄋..鄻
    {0x913E, 0x9158, SC_Han, GC_Lo, NULL}, //  鄾..酘
    {0x915A, 0x917A, SC_Han, GC_Lo, NULL}, //  酚..酺
    {0x917C, 0x9194, SC_Han, GC_Lo, NULL}, //  酼..醔
    {0x9196, 0x9197, SC_Han, GC_Lo, NULL}, //  醖..醗
    {0x9199, 0x91A8, SC_Han, GC_Lo, NULL}, //  醙..醨
    {0x91AA, 0x91BE, SC_Han, GC_Lo, NULL}, //  醪..醾
    {0x91C0, 0x91C3, SC_Han, GC_Lo, NULL}, //  釀..釃
    {0x91C5, 0x91DF, SC_Han, GC_Lo, NULL}, //  釅..釟
    {0x91E1, 0x91EE, SC_Han, GC_Lo, NULL}, //  釡..釮
    {0x91F0, 0x9212, SC_Han, GC_Lo, NULL}, //  釰..鈒
    {0x9214, 0x921E, SC_Han, GC_Lo, NULL}, //  鈔..鈞
    {0x9220, 0x9221, SC_Han, GC_Lo, NULL}, //  鈠..鈡
    {0x9223, 0x9242, SC_Han, GC_Lo, NULL}, //  鈣..鉂
    {0x9244, 0x9268, SC_Han, GC_Lo, NULL}, //  鉄..鉨
    {0x926B, 0x9280, SC_Han, GC_Lo, NULL}, //  鉫..銀
    {0x9282, 0x9283, SC_Han, GC_Lo, NULL}, //  銂..銃
    {0x9285, 0x929D, SC_Han, GC_Lo, NULL}, //  銅..銝
    {0x929F, 0x92BC, SC_Han, GC_Lo, NULL}, //  銟..銼
    {0x92BE, 0x92D3, SC_Han, GC_Lo, NULL}, //  銾..鋓
    {0x92D5, 0x92DA, SC_Han, GC_Lo, NULL}, //  鋕..鋚
    {0x92DC, 0x92E1, SC_Han, GC_Lo, NULL}, //  鋜..鋡
    {0x92E3, 0x931B, SC_Han, GC_Lo, NULL}, //  鋣..錛
    {0x931D, 0x932F, SC_Han, GC_Lo, NULL}, //  錝..錯
    {0x9332, 0x9361, SC_Han, GC_Lo, NULL}, //  録..鍡
    {0x9363, 0x9367, SC_Han, GC_Lo, NULL}, //  鍣..鍧
    {0x9369, 0x936A, SC_Han, GC_Lo, NULL}, //  鍩..鍪
    {0x936C, 0x936E, SC_Han, GC_Lo, NULL}, //  鍬..鍮
    {0x9370, 0x9372, SC_Han, GC_Lo, NULL}, //  鍰..鍲
    {0x9374, 0x9377, SC_Han, GC_Lo, NULL}, //  鍴..鍷
    {0x9379, 0x937E, SC_Han, GC_Lo, NULL}, //  鍹..鍾
    {0x9380, 0x9380, SC_Han, GC_Lo, NULL}, //  鎀
    {0x9382, 0x938A, SC_Han, GC_Lo, NULL}, //  鎂..鎊
    {0x938C, 0x939B, SC_Han, GC_Lo, NULL}, //  鎌..鎛
    {0x939D, 0x939F, SC_Han, GC_Lo, NULL}, //  鎝..鎟
    {0x93A1, 0x93AA, SC_Han, GC_Lo, NULL}, //  鎡..鎪
    {0x93AC, 0x93BA, SC_Han, GC_Lo, NULL}, //  鎬..鎺
    {0x93BC, 0x93DF, SC_Han, GC_Lo, NULL}, //  鎼..鏟
    {0x93E1, 0x93F2, SC_Han, GC_Lo, NULL}, //  鏡..鏲
    {0x93F4, 0x9401, SC_Han, GC_Lo, NULL}, //  鏴..鐁
    {0x9403, 0x9416, SC_Han, GC_Lo, NULL}, //  鐃..鐖
    {0x9418, 0x941B, SC_Han, GC_Lo, NULL}, //  鐘..鐛
    {0x941D, 0x941D, SC_Han, GC_Lo, NULL}, //  鐝
    {0x9420, 0x9423, SC_Han, GC_Lo, NULL}, //  鐠..鐣
    {0x9425, 0x9442, SC_Han, GC_Lo, NULL}, //  鐥..鑂
    {0x9444, 0x944D, SC_Han, GC_Lo, NULL}, //  鑄..鑍
    {0x944F, 0x946B, SC_Han, GC_Lo, NULL}, //  鑏..鑫
    {0x946D, 0x947A, SC_Han, GC_Lo, NULL}, //  鑭..鑺
    {0x947C, 0x9577, SC_Han, GC_Lo, NULL}, //  鑼..長
    {0x957A, 0x957D, SC_Han, GC_Lo, NULL}, //  镺..镽
    {0x957F, 0x9584, SC_Han, GC_Lo, NULL}, //  长..閄
    {0x9586, 0x9596, SC_Han, GC_Lo, NULL}, //  閆..閖
    {0x9598, 0x95B2, SC_Han, GC_Lo, NULL}, //  閘..閲
    {0x95B5, 0x95B7, SC_Han, GC_Lo, NULL}, //  閵..閷
    {0x95B9, 0x95C0, SC_Han, GC_Lo, NULL}, //  閹..闀
    {0x95C2, 0x95D8, SC_Han, GC_Lo, NULL}, //  闂..闘
    {0x95DA, 0x95DC, SC_Han, GC_Lo, NULL}, //  闚..關
    {0x95DE, 0x9624, SC_Han, GC_Lo, NULL}, //  闞..阤
    {0x9627, 0x9628, SC_Han, GC_Lo, NULL}, //  阧..阨
    {0x962A, 0x963D, SC_Han, GC_Lo, NULL}, //  阪..阽
    {0x963F, 0x9655, SC_Han, GC_Lo, NULL}, //  阿..陕
    {0x9658, 0x9678, SC_Han, GC_Lo, NULL}, //  陘..陸
    {0x967A, 0x967A, SC_Han, GC_Lo, NULL}, //  険
    {0x967C, 0x967E, SC_Han, GC_Lo, NULL}, //  陼..陾
    {0x9680, 0x9680, SC_Han, GC_Lo, NULL}, //  隀
    {0x9683, 0x968B, SC_Han, GC_Lo, NULL}, //  隃..隋
    {0x968D, 0x9695, SC_Han, GC_Lo, NULL}, //  隍..隕
    {0x9697, 0x9699, SC_Han, GC_Lo, NULL}, //  隗..隙
    {0x969B, 0x969C, SC_Han, GC_Lo, NULL}, //  際..障
    {0x969E, 0x969E, SC_Han, GC_Lo, NULL}, //  隞
    {0x96A0, 0x96AA, SC_Han, GC_Lo, NULL}, //  隠..險
    {0x96AC, 0x96AE, SC_Han, GC_Lo, NULL}, //  隬..隮
    {0x96B0, 0x96B4, SC_Han, GC_Lo, NULL}, //  隰..隴
    {0x96B6, 0x96E3, SC_Han, GC_Lo, NULL}, //  隶..難
    {0x96E5, 0x96E5, SC_Han, GC_Lo, NULL}, //  雥
    {0x96E8, 0x96FB, SC_Han, GC_Lo, NULL}, //  雨..電
    {0x96FD, 0x9713, SC_Han, GC_Lo, NULL}, //  雽..霓
    {0x9715, 0x9716, SC_Han, GC_Lo, NULL}, //  霕..霖
    {0x9718, 0x9719, SC_Han, GC_Lo, NULL}, //  霘..霙
    {0x971C, 0x9732, SC_Han, GC_Lo, NULL}, //  霜..露
    {0x9735, 0x9736, SC_Han, GC_Lo, NULL}, //  霵..霶
    {0x9738, 0x973F, SC_Han, GC_Lo, NULL}, //  霸..霿
    {0x9742, 0x974C, SC_Han, GC_Lo, NULL}, //  靂..靌
    {0x974E, 0x9756, SC_Han, GC_Lo, NULL}, //  靎..靖
    {0x9758, 0x9762, SC_Han, GC_Lo, NULL}, //  靘..面
    {0x9764, 0x9774, SC_Han, GC_Lo, NULL}, //  靤..靴
    {0x9776, 0x9786, SC_Han, GC_Lo, NULL}, //  靶..鞆
    {0x9788, 0x9788, SC_Han, GC_Lo, NULL}, //  鞈
    {0x978A, 0x979A, SC_Han, GC_Lo, NULL}, //  鞊..鞚
    {0x979C, 0x97A8, SC_Han, GC_Lo, NULL}, //  鞜..鞨
    {0x97AA, 0x97AF, SC_Han, GC_Lo, NULL}, //  鞪..鞯
    {0x97B2, 0x97B4, SC_Han, GC_Lo, NULL}, //  鞲..鞴
    {0x97B6, 0x97BD, SC_Han, GC_Lo, NULL}, //  鞶..鞽
    {0x97BF, 0x97BF, SC_Han, GC_Lo, NULL}, //  鞿
    {0x97C1, 0x97D1, SC_Han, GC_Lo, NULL}, //  韁..韑
    {0x97D3, 0x97FB, SC_Han, GC_Lo, NULL}, //  韓..韻
    {0x97FD, 0x981E, SC_Han, GC_Lo, NULL}, //  韽..頞
    {0x9820, 0x9824, SC_Han, GC_Lo, NULL}, //  頠..頤
    {0x9826, 0x9829, SC_Han, GC_Lo, NULL}, //  頦..頩
    {0x982B, 0x9832, SC_Han, GC_Lo, NULL}, //  頫..頲
    {0x9834, 0x9839, SC_Han, GC_Lo, NULL}, //  頴..頹
    {0x983B, 0x983D, SC_Han, GC_Lo, NULL}, //  頻..頽
    {0x983F, 0x9841, SC_Han, GC_Lo, NULL}, //  頿..顁
    {0x9843, 0x9846, SC_Han, GC_Lo, NULL}, //  顃..顆
    {0x9848, 0x9855, SC_Han, GC_Lo, NULL}, //  顈..顕
    {0x9857, 0x9865, SC_Han, GC_Lo, NULL}, //  顗..顥
    {0x9867, 0x9867, SC_Han, GC_Lo, NULL}, //  顧
    {0x9869, 0x98B6, SC_Han, GC_Lo, NULL}, //  顩..颶
    {0x98B8, 0x98C9, SC_Han, GC_Lo, NULL}, //  颸..飉
    {0x98CB, 0x98E3, SC_Han, GC_Lo, NULL}, //  飋..飣
    {0x98E5, 0x98EB, SC_Han, GC_Lo, NULL}, //  飥..飫
    {0x98ED, 0x98F0, SC_Han, GC_Lo, NULL}, //  飭..飰
    {0x98F2, 0x98F7, SC_Han, GC_Lo, NULL}, //  飲..飷
    {0x98F9, 0x98FA, SC_Han, GC_Lo, NULL}, //  飹..飺
    {0x98FC, 0x9918, SC_Han, GC_Lo, NULL}, //  飼..餘
    {0x991A, 0x993A, SC_Han, GC_Lo, NULL}, //  餚..餺
    {0x993C, 0x9943, SC_Han, GC_Lo, NULL}, //  餼..饃
    {0x9945, 0x9959, SC_Han, GC_Lo, NULL}, //  饅..饙
    {0x995B, 0x995C, SC_Han, GC_Lo, NULL}, //  饛..饜
    {0x995E, 0x99BE, SC_Han, GC_Lo, NULL}, //  饞..馾
    {0x99C0, 0x99DF, SC_Han, GC_Lo, NULL}, //  駀..駟
    {0x99E1, 0x99E5, SC_Han, GC_Lo, NULL}, //  駡..駥
    {0x99E7, 0x99EA, SC_Han, GC_Lo, NULL}, //  駧..駪
    {0x99EC, 0x99F4, SC_Han, GC_Lo, NULL}, //  駬..駴
    {0x99F6, 0x9A0F, SC_Han, GC_Lo, NULL}, //  駶..騏
    {0x9A11, 0x9A16, SC_Han, GC_Lo, NULL}, //  騑..騖
    {0x9A19, 0x9A3A, SC_Han, GC_Lo, NULL}, //  騙..騺
    {0x9A3C, 0x9A50, SC_Han, GC_Lo, NULL}, //  騼..驐
    {0x9A52, 0x9A57, SC_Han, GC_Lo, NULL}, //  驒..驗
    {0x9A59, 0x9A5C, SC_Han, GC_Lo, NULL}, //  驙..驜
    {0x9A5E, 0x9A62, SC_Han, GC_Lo, NULL}, //  驞..驢
    {0x9A64, 0x9AA8, SC_Han, GC_Lo, NULL}, //  驤..骨
    {0x9AAA, 0x9ABC, SC_Han, GC_Lo, NULL}, //  骪..骼
    {0x9ABE, 0x9AC7, SC_Han, GC_Lo, NULL}, //  骾..髇
    {0x9AC9, 0x9AD6, SC_Han, GC_Lo, NULL}, //  髉..髖
    {0x9AD8, 0x9ADF, SC_Han, GC_Lo, NULL}, //  高..髟
    {0x9AE1, 0x9AE3, SC_Han, GC_Lo, NULL}, //  髡..髣
    {0x9AE5, 0x9AE7, SC_Han, GC_Lo, NULL}, //  髥..髧
    {0x9AEA, 0x9AEF, SC_Han, GC_Lo, NULL}, //  髪..髯
    {0x9AF1, 0x9AFF, SC_Han, GC_Lo, NULL}, //  髱..髿
    {0x9B01, 0x9B01, SC_Han, GC_Lo, NULL}, //  鬁
    {0x9B03, 0x9B08, SC_Han, GC_Lo, NULL}, //  鬃..鬈
    {0x9B0A, 0x9B13, SC_Han, GC_Lo, NULL}, //  鬊..鬓
    {0x9B15, 0x9B1A, SC_Han, GC_Lo, NULL}, //  鬕..鬚
    {0x9B1C, 0x9B33, SC_Han, GC_Lo, NULL}, //  鬜..鬳
    {0x9B35, 0x9B3C, SC_Han, GC_Lo, NULL}, //  鬵..鬼
    {0x9B3E, 0x9B3F, SC_Han, GC_Lo, NULL}, //  鬾..鬿
    {0x9B41, 0x9B4F, SC_Han, GC_Lo, NULL}, //  魁..魏
    {0x9B51, 0x9B56, SC_Han, GC_Lo, NULL}, //  魑..魖
    {0x9B58, 0x9B61, SC_Han, GC_Lo, NULL}, //  魘..魡
    {0x9B63, 0x9B71, SC_Han, GC_Lo, NULL}, //  魣..魱
    {0x9B73, 0x9B88, SC_Han, GC_Lo, NULL}, //  魳..鮈
    {0x9B8A, 0x9B8B, SC_Han, GC_Lo, NULL}, //  鮊..鮋
    {0x9B8D, 0x9B98, SC_Han, GC_Lo, NULL}, //  鮍..鮘
    {0x9B9A, 0x9BC1, SC_Han, GC_Lo, NULL}, //  鮚..鯁
    {0x9BC3, 0x9BF5, SC_Han, GC_Lo, NULL}, //  鯃..鯵
    {0x9BF7, 0x9BFF, SC_Han, GC_Lo, NULL}, //  鯷..鯿
    {0x9C02, 0x9C02, SC_Han, GC_Lo, NULL}, //  鰂
    {0x9C04, 0x9C41, SC_Han, GC_Lo, NULL}, //  鰄..鱁
    {0x9C43, 0x9C4E, SC_Han, GC_Lo, NULL}, //  鱃..鱎
    {0x9C50, 0x9C50, SC_Han, GC_Lo, NULL}, //  鱐
    {0x9C52, 0x9C60, SC_Han, GC_Lo, NULL}, //  鱒..鱠
    {0x9C62, 0x9C63, SC_Han, GC_Lo, NULL}, //  鱢..鱣
    {0x9C65, 0x9C7A, SC_Han, GC_Lo, NULL}, //  鱥..鱺
    {0x9C7C, 0x9D0B, SC_Han, GC_Lo, NULL}, //  鱼..鴋
    {0x9D0E, 0x9D10, SC_Han, GC_Lo, NULL}, //  鴎..鴐
    {0x9D12, 0x9D26, SC_Han, GC_Lo, NULL}, //  鴒..鴦
    {0x9D28, 0x9D34, SC_Han, GC_Lo, NULL}, //  鴨..鴴
    {0x9D36, 0x9D3B, SC_Han, GC_Lo, NULL}, //  鴶..鴻
    {0x9D3D, 0x9D6C, SC_Han, GC_Lo, NULL}, //  鴽..鵬
    {0x9D6E, 0x9D94, SC_Han, GC_Lo, NULL}, //  鵮..鶔
    {0x9D96, 0x9DAD, SC_Han, GC_Lo, NULL}, //  鶖..鶭
    {0x9DAF, 0x9DBC, SC_Han, GC_Lo, NULL}, //  鶯..鶼
    {0x9DBE, 0x9DBF, SC_Han, GC_Lo, NULL}, //  鶾..鶿
    {0x9DC1, 0x9DE9, SC_Han, GC_Lo, NULL}, //  鷁..鷩
    {0x9DEB, 0x9DFB, SC_Han, GC_Lo, NULL}, //  鷫..鷻
    {0x9DFD, 0x9E0D, SC_Han, GC_Lo, NULL}, //  鷽..鸍
    {0x9E0F, 0x9E15, SC_Han, GC_Lo, NULL}, //  鸏..鸕
    {0x9E17, 0x9E1B, SC_Han, GC_Lo, NULL}, //  鸗..鸛
    {0x9E1D, 0x9E7A, SC_Han, GC_Lo, NULL}, //  鸝..鹺
    {0x9E7C, 0x9E8E, SC_Han, GC_Lo, NULL}, //  鹼..麎
    {0x9E91, 0x9E97, SC_Han, GC_Lo, NULL}, //  麑..麗
    {0x9E99, 0x9E9D, SC_Han, GC_Lo, NULL}, //  麙..麝
    {0x9E9F, 0x9EA1, SC_Han, GC_Lo, NULL}, //  麟..麡
    {0x9EA3, 0x9EAA, SC_Han, GC_Lo, NULL}, //  麣..麪
    {0x9EAD, 0x9EB0, SC_Han, GC_Lo, NULL}, //  麭..麰
    {0x9EB2, 0x9EEB, SC_Han, GC_Lo, NULL}, //  麲..黫
    {0x9EED, 0x9EF0, SC_Han, GC_Lo, NULL}, //  黭..黰
    {0x9EF2, 0x9F02, SC_Han, GC_Lo, NULL}, //  黲..鼂
    {0x9F04, 0x9F10, SC_Han, GC_Lo, NULL}, //  鼄..鼐
    {0x9F12, 0x9F13, SC_Han, GC_Lo, NULL}, //  鼒..鼓
    {0x9F15, 0x9F25, SC_Han, GC_Lo, NULL}, //  鼕..鼥
    {0x9F27, 0x9F44, SC_Han, GC_Lo, NULL}, //  鼧..齄
    {0x9F46, 0x9F52, SC_Han, GC_Lo, NULL}, //  齆..齒
    {0x9F54, 0x9F6C, SC_Han, GC_Lo, NULL}, //  齔..齬
    {0x9F6E, 0x9FA0, SC_Han, GC_Lo, NULL}, //  齮..龠
    {0x9FA2, 0x9FA2, SC_Han, GC_Lo, NULL}, //  龢
    {0x9FA4, 0x9FA5, SC_Han, GC_Lo, NULL}, //  龤..龥
    {0xA717, 0xA71F, SC_Common, GC_Lm, NULL}, //  ꜗ..ꜟ
    {0xA788, 0xA788, SC_Common, GC_Lm, NULL}, //  ꞈ
    {0xA78D, 0xA78E, SC_Latin, GC_L, NULL}, //  Ɥ..ꞎ
    {0xA7AA, 0xA7AA, SC_Latin, GC_Lu, NULL}, //  Ɦ
    {0xA7AE, 0xA7AF, SC_Latin, GC_L, NULL}, //  Ɪ..ꞯ
    {0xA7BA, 0xA7BF, SC_Latin, GC_L, NULL}, //  Ꞻ..ꞿ
    {0xA7C5, 0xA7C6, SC_Latin, GC_Lu, NULL}, //  Ʂ..Ᶎ
    {0xA7FA, 0xA7FA, SC_Latin, GC_Ll, NULL}, //  ꟺ
    {0xAB68, 0xAB68, SC_Latin, GC_Ll, NULL}, //  ꭨ
    {0x10EC5, 0x10EC6, SC_Arabic, GC_L, NULL}, //  𐻅..𐻆
    {0x16FF2, 0x16FF6, SC_Han, GC_V, NULL}, //  𖿲..𖿶
    {0x1DF00, 0x1DF1E, SC_Latin, GC_L, NULL}, //  𝼀..𝼞
    {0x1DF25, 0x1DF2A, SC_Latin, GC_Ll, NULL}, //  𝼥..𝼪
    {0x1E7E0, 0x1E7E6, SC_Ethiopic, GC_Lo, NULL}, //  𞟠..𞟦
    {0x1E7E8, 0x1E7EB, SC_Ethiopic, GC_Lo, NULL}, //  𞟨..𞟫
    {0x1E7ED, 0x1E7EE, SC_Ethiopic, GC_Lo, NULL}, //  𞟭..𞟮
    {0x1E7F0, 0x1E7FE, SC_Ethiopic, GC_Lo, NULL}, //  𞟰..𞟾
    {0x2070E, 0x2070E, SC_Han, GC_Lo, NULL}, //  𠜎
    {0x20731, 0x20731, SC_Han, GC_Lo, NULL}, //  𠜱
    {0x20779, 0x20779, SC_Han, GC_Lo, NULL}, //  𠝹
    {0x20C53, 0x20C53, SC_Han, GC_Lo, NULL}, //  𠱓
    {0x20C78, 0x20C78, SC_Han, GC_Lo, NULL}, //  𠱸
    {0x20C96, 0x20C96, SC_Han, GC_Lo, NULL}, //  𠲖
    {0x20CCF, 0x20CCF, SC_Han, GC_Lo, NULL}, //  𠳏
    {0x20CD5, 0x20CD5, SC_Han, GC_Lo, NULL}, //  𠳕
    {0x20D15, 0x20D15, SC_Han, GC_Lo, NULL}, //  𠴕
    {0x20D7C, 0x20D7C, SC_Han, GC_Lo, NULL}, //  𠵼
    {0x20D7F, 0x20D7F, SC_Han, GC_Lo, NULL}, //  𠵿
    {0x20E0E, 0x20E0F, SC_Han, GC_Lo, NULL}, //  𠸎..𠸏
    {0x20E77, 0x20E77, SC_Han, GC_Lo, NULL}, //  𠹷
    {0x20E9D, 0x20E9D, SC_Han, GC_Lo, NULL}, //  𠺝
    {0x20EA2, 0x20EA2, SC_Han, GC_Lo, NULL}, //  𠺢
    {0x20ED7, 0x20ED7, SC_Han, GC_Lo, NULL}, //  𠻗
    {0x20EF9, 0x20EFA, SC_Han, GC_Lo, NULL}, //  𠻹..𠻺
    {0x20F2D, 0x20F2E, SC_Han, GC_Lo, NULL}, //  𠼭..𠼮
    {0x20F4C, 0x20F4C, SC_Han, GC_Lo, NULL}, //  𠽌
    {0x20FB4, 0x20FB4, SC_Han, GC_Lo, NULL}, //  𠾴
    {0x20FBC, 0x20FBC, SC_Han, GC_Lo, NULL}, //  𠾼
    {0x20FEA, 0x20FEA, SC_Han, GC_Lo, NULL}, //  𠿪
    {0x2105C, 0x2105C, SC_Han, GC_Lo, NULL}, //  𡁜
    {0x2106F, 0x2106F, SC_Han, GC_Lo, NULL}, //  𡁯
    {0x21075, 0x21076, SC_Han, GC_Lo, NULL}, //  𡁵..𡁶
    {0x2107B, 0x2107B, SC_Han, GC_Lo, NULL}, //  𡁻
    {0x210C1, 0x210C1, SC_Han, GC_Lo, NULL}, //  𡃁
    {0x210C9, 0x210C9, SC_Han, GC_Lo, NULL}, //  𡃉
    {0x211D9, 0x211D9, SC_Han, GC_Lo, NULL}, //  𡇙
    {0x220C7, 0x220C7, SC_Han, GC_Lo, NULL}, //  𢃇
    {0x227B5, 0x227B5, SC_Han, GC_Lo, NULL}, //  𢞵
    {0x22AD5, 0x22AD5, SC_Han, GC_Lo, NULL}, //  𢫕
    {0x22B43, 0x22B43, SC_Han, GC_Lo, NULL}, //  𢭃
    {0x22BCA, 0x22BCA, SC_Han, GC_Lo, NULL}, //  𢯊
    {0x22C51, 0x22C51, SC_Han, GC_Lo, NULL}, //  𢱑
    {0x22C55, 0x22C55, SC_Han, GC_Lo, NULL}, //  𢱕
    {0x22CC2, 0x22CC2, SC_Han, GC_Lo, NULL}, //  𢳂
    {0x22D08, 0x22D08, SC_Han, GC_Lo, NULL}, //  𢴈
    {0x22D4C, 0x22D4C, SC_Han, GC_Lo, NULL}, //  𢵌
    {0x22D67, 0x22D67, SC_Han, GC_Lo, NULL}, //  𢵧
    {0x22EB3, 0x22EB3, SC_Han, GC_Lo, NULL}, //  𢺳
    {0x23CB7, 0x23CB7, SC_Han, GC_Lo, NULL}, //  𣲷
    {0x244D3, 0x244D3, SC_Han, GC_Lo, NULL}, //  𤓓
    {0x24DB8, 0x24DB8, SC_Han, GC_Lo, NULL}, //  𤶸
    {0x24DEA, 0x24DEA, SC_Han, GC_Lo, NULL}, //  𤷪
    {0x2512B, 0x2512B, SC_Han, GC_Lo, NULL}, //  𥄫
    {0x26258, 0x26258, SC_Han, GC_Lo, NULL}, //  𦉘
    {0x267CC, 0x267CC, SC_Han, GC_Lo, NULL}, //  𦟌
    {0x269F2, 0x269F2, SC_Han, GC_Lo, NULL}, //  𦧲
    {0x269FA, 0x269FA, SC_Han, GC_Lo, NULL}, //  𦧺
    {0x27A3E, 0x27A3E, SC_Han, GC_Lo, NULL}, //  𧨾
    {0x2815D, 0x2815D, SC_Han, GC_Lo, NULL}, //  𨅝
    {0x28207, 0x28207, SC_Han, GC_Lo, NULL}, //  𨈇
    {0x282E2, 0x282E2, SC_Han, GC_Lo, NULL}, //  𨋢
    {0x28CCA, 0x28CCA, SC_Han, GC_Lo, NULL}, //  𨳊
    {0x28CCD, 0x28CCD, SC_Han, GC_Lo, NULL}, //  𨳍
    {0x28CD2, 0x28CD2, SC_Han, GC_Lo, NULL}, //  𨳒
    {0x29D98, 0x29D98, SC_Han, GC_Lo, NULL}, //  𩶘
}; // 1279 ranges, 282 singles, 20907 codepoints

// Filtering allowed scripts, XID_Continue,!XID_Start, safe IDTypes, NFC
// MEDIAL from XID_Start and !MARK. Split on GC and SCX
static const struct sc_tr39 tr39_cont_list[] = {
    {0x30, 0x39, SC_Common, GC_Nd, NULL}, //  0..9
    {0x5F, 0x5F, SC_Common, GC_Pc, NULL}, //  _
    {0x660, 0x669, SC_Arabic, GC_Nd, "\x03\x1c\x8a"}, //Arabic,Thaana,Yezidi //  ٠..٩
    {0x6F0, 0x6F9, SC_Arabic, GC_Nd, NULL}, //  ۰..۹
    {0x966, 0x96F, SC_Devanagari, GC_Nd, "\x08\x32\x42\x4d"}, //Devanagari,Dogra,Kaithi,Mahajani //  ०..९
    {0x9E6, 0x9EF, SC_Bengali, GC_Nd, "\x05\x91\xa5"}, //Bengali,Chakma,Syloti_Nagri //  ০..৯
    {0xAE6, 0xAEF, SC_Gujarati, GC_Nd, "\x0c\x46"}, //Gujarati,Khojki //  ૦..૯
    {0xCE6, 0xCEF, SC_Kannada, GC_Nd, "\x13\x5c\x86"}, //Kannada,Nandinagari,Tulu_Tigalari //  ೦..೯
    {0xE50, 0xE59, SC_Thai, GC_Nd, NULL}, //  ๐..๙
    {0xED0, 0xED9, SC_Lao, GC_Nd, NULL}, //  ໐..໙
    {0xF20, 0xF29, SC_Tibetan, GC_Nd, NULL}, //  ༠..༩
    {0x1040, 0x1049, SC_Myanmar, GC_Nd, "\x91\x17\xa7"}, //Chakma,Myanmar,Tai_Le //  ၀..၉
    {0x17E0, 0x17E9, SC_Khmer, GC_Nd, NULL}, //  ០..៩
    {0x203F, 0x2040, SC_Common, GC_Pc, NULL}, //  ‿..⁀
    {0x30FB, 0x30FB, SC_Common, GC_Po, "\x06\x0e\x0f\x11\x12\xad"}, //Bopomofo,Hangul,Han,Hiragana,Katakana,Yi //  ・
}; // 13 ranges, 2 singles, 109 codepoints


//---------------------------------------------------

// Only excluded scripts, XID_Start, more IDTypes, NFC, !MEDIAL and !MARK
static const struct sc_tr39 tr39_excl_start_list[] = {
    {0x3E2, 0x3EF, SC_Coptic, GC_L, NULL}, //  (Excluded) Ϣ..ϯ
    {0x800, 0x815, SC_Samaritan, GC_Lo, NULL}, //  (Excluded) ࠀ..ࠕ
    {0x81A, 0x81A, SC_Samaritan, GC_Lm, NULL}, //  (Excluded) ࠚ
    {0x824, 0x824, SC_Samaritan, GC_Lm, NULL}, //  (Excluded) ࠤ
    {0x828, 0x828, SC_Samaritan, GC_Lm, NULL}, //  (Excluded) ࠨ
    {0x1681, 0x169A, SC_Ogham, GC_Lo, NULL}, //  (Excluded) ᚁ..ᚚ
    {0x16A0, 0x16EA, SC_Runic, GC_Lo, NULL}, //  (Excluded) ᚠ..ᛪ
    {0x16EE, 0x16F8, SC_Runic, GC_V, NULL}, //  (Excluded) ᛮ..ᛸ
    {0x1700, 0x1711, SC_Tagalog, GC_Lo, NULL}, //  (Excluded) ᜀ..ᜑ
    {0x171F, 0x171F, SC_Tagalog, GC_Lo, NULL}, //  (Excluded) ᜟ
    {0x1721, 0x1731, SC_Hanunoo, GC_Lo, NULL}, //  (Excluded) ᜡ..ᜱ
    {0x1740, 0x1751, SC_Buhid, GC_Lo, NULL}, //  (Excluded) ᝀ..ᝑ
    {0x1760, 0x176C, SC_Tagbanwa, GC_Lo, NULL}, //  (Excluded) ᝠ..ᝬ
    {0x176E, 0x1770, SC_Tagbanwa, GC_Lo, NULL}, //  (Excluded) ᝮ..ᝰ
    {0x1820, 0x1878, SC_Mongolian, GC_L, NULL}, //  (Excluded) ᠠ..ᡸ
    {0x1880, 0x1884, SC_Mongolian, GC_Lo, NULL}, //  (Excluded) ᢀ..ᢄ
    {0x1887, 0x18A8, SC_Mongolian, GC_Lo, NULL}, //  (Excluded) ᢇ..ᢨ
    {0x18AA, 0x18AA, SC_Mongolian, GC_Lo, NULL}, //  (Excluded) ᢪ
    {0x1A00, 0x1A16, SC_Buginese, GC_Lo, NULL}, //  (Excluded) ᨀ..ᨖ
    {0x2C00, 0x2C5F, SC_Glagolitic, GC_L, NULL}, //  (Excluded) Ⰰ..ⱟ
    {0x2C80, 0x2CE4, SC_Coptic, GC_L, NULL}, //  (Excluded) Ⲁ..ⳤ
    {0x2CEB, 0x2CEE, SC_Coptic, GC_L, NULL}, //  (Excluded) Ⳬ..ⳮ
    {0x2CF2, 0x2CF3, SC_Coptic, GC_L, NULL}, //  (Excluded) Ⳳ..ⳳ
    {0xA840, 0xA873, SC_Phags_Pa, GC_Lo, NULL}, //  (Excluded) ꡀ..ꡳ
    {0xA930, 0xA946, SC_Rejang, GC_Lo, NULL}, //  (Excluded) ꤰ..ꥆ
    {0x10000, 0x1000B, SC_Linear_B, GC_Lo, NULL}, //  (Excluded) 𐀀..𐀋
    {0x1000D, 0x10026, SC_Linear_B, GC_Lo, NULL}, //  (Excluded) 𐀍..𐀦
    {0x10028, 0x1003A, SC_Linear_B, GC_Lo, NULL}, //  (Excluded) 𐀨..𐀺
    {0x1003C, 0x1003D, SC_Linear_B, GC_Lo, NULL}, //  (Excluded) 𐀼..𐀽
    {0x1003F, 0x1004D, SC_Linear_B, GC_Lo, NULL}, //  (Excluded) 𐀿..𐁍
    {0x10050, 0x1005D, SC_Linear_B, GC_Lo, NULL}, //  (Excluded) 𐁐..𐁝
    {0x10080, 0x100FA, SC_Linear_B, GC_Lo, NULL}, //  (Excluded) 𐂀..𐃺
    {0x10280, 0x1029C, SC_Lycian, GC_Lo, NULL}, //  (Excluded) 𐊀..𐊜
    {0x102A0, 0x102D0, SC_Carian, GC_Lo, NULL}, //  (Excluded) 𐊠..𐋐
    {0x10300, 0x1031F, SC_Old_Italic, GC_Lo, NULL}, //  (Excluded) 𐌀..𐌟
    {0x1032D, 0x1032F, SC_Old_Italic, GC_Lo, NULL}, //  (Excluded) 𐌭..𐌯
    {0x10331, 0x1034A, SC_Gothic, GC_V, NULL}, //  (Excluded) 𐌱..𐍊
    {0x10350, 0x10375, SC_Old_Permic, GC_Lo, NULL}, //  (Excluded) 𐍐..𐍵
    {0x10380, 0x1039D, SC_Ugaritic, GC_Lo, NULL}, //  (Excluded) 𐎀..𐎝
    {0x103A0, 0x103C3, SC_Old_Persian, GC_Lo, NULL}, //  (Excluded) 𐎠..𐏃
    {0x103C8, 0x103CF, SC_Old_Persian, GC_Lo, NULL}, //  (Excluded) 𐏈..𐏏
    {0x103D1, 0x103D5, SC_Old_Persian, GC_Nl, NULL}, //  (Excluded) 𐏑..𐏕
    {0x10400, 0x1044F, SC_Deseret, GC_L, NULL}, //  (Excluded) 𐐀..𐑏
    {0x10451, 0x1047F, SC_Shavian, GC_Lo, NULL}, //  (Excluded) 𐑑..𐑿
    {0x10481, 0x1049D, SC_Osmanya, GC_Lo, NULL}, //  (Excluded) 𐒁..𐒝
    {0x10500, 0x10527, SC_Elbasan, GC_Lo, NULL}, //  (Excluded) 𐔀..𐔧
    {0x10530, 0x10563, SC_Caucasian_Albanian, GC_Lo, NULL}, //  (Excluded) 𐔰..𐕣
    {0x10570, 0x1057A, SC_Vithkuqi, GC_Lu, NULL}, //  (Excluded) 𐕰..𐕺
    {0x1057C, 0x1058A, SC_Vithkuqi, GC_Lu, NULL}, //  (Excluded) 𐕼..𐖊
    {0x1058C, 0x10592, SC_Vithkuqi, GC_Lu, NULL}, //  (Excluded) 𐖌..𐖒
    {0x10594, 0x10595, SC_Vithkuqi, GC_Lu, NULL}, //  (Excluded) 𐖔..𐖕
    {0x10597, 0x105A1, SC_Vithkuqi, GC_Ll, NULL}, //  (Excluded) 𐖗..𐖡
    {0x105A3, 0x105B1, SC_Vithkuqi, GC_Ll, NULL}, //  (Excluded) 𐖣..𐖱
    {0x105B3, 0x105B9, SC_Vithkuqi, GC_Ll, NULL}, //  (Excluded) 𐖳..𐖹
    {0x105BB, 0x105BC, SC_Vithkuqi, GC_Ll, NULL}, //  (Excluded) 𐖻..𐖼
    {0x105C0, 0x105F3, SC_Todhri, GC_Lo, NULL}, //  (Excluded) 𐗀..𐗳
    {0x10600, 0x10736, SC_Linear_A, GC_Lo, NULL}, //  (Excluded) 𐘀..𐜶
    {0x10740, 0x10755, SC_Linear_A, GC_Lo, NULL}, //  (Excluded) 𐝀..𐝕
    {0x10760, 0x10767, SC_Linear_A, GC_Lo, NULL}, //  (Excluded) 𐝠..𐝧
    {0x10800, 0x10805, SC_Cypriot, GC_Lo, NULL}, //  (Excluded) 𐠀..𐠅
    {0x10808, 0x10808, SC_Cypriot, GC_Lo, NULL}, //  (Excluded) 𐠈
    {0x1080A, 0x10835, SC_Cypriot, GC_Lo, NULL}, //  (Excluded) 𐠊..𐠵
    {0x10837, 0x10838, SC_Cypriot, GC_Lo, NULL}, //  (Excluded) 𐠷..𐠸
    {0x1083C, 0x1083C, SC_Cypriot, GC_Lo, NULL}, //  (Excluded) 𐠼
    {0x1083F, 0x1083F, SC_Cypriot, GC_Lo, NULL}, //  (Excluded) 𐠿
    {0x10841, 0x10855, SC_Imperial_Aramaic, GC_Lo, NULL}, //  (Excluded) 𐡁..𐡕
    {0x10860, 0x10876, SC_Palmyrene, GC_Lo, NULL}, //  (Excluded) 𐡠..𐡶
    {0x10880, 0x1089E, SC_Nabataean, GC_Lo, NULL}, //  (Excluded) 𐢀..𐢞
    {0x108E0, 0x108F2, SC_Hatran, GC_Lo, NULL}, //  (Excluded) 𐣠..𐣲
    {0x108F4, 0x108F5, SC_Hatran, GC_Lo, NULL}, //  (Excluded) 𐣴..𐣵
    {0x10900, 0x10915, SC_Phoenician, GC_Lo, NULL}, //  (Excluded) 𐤀..𐤕
    {0x10920, 0x10939, SC_Lydian, GC_Lo, NULL}, //  (Excluded) 𐤠..𐤹
    {0x10940, 0x10959, SC_Sidetic, GC_Lo, NULL}, //  (Excluded) 𐥀..𐥙
    {0x10980, 0x1099F, SC_Meroitic_Hieroglyphs, GC_Lo, NULL}, //  (Excluded) 𐦀..𐦟
    {0x109A1, 0x109B7, SC_Meroitic_Cursive, GC_Lo, NULL}, //  (Excluded) 𐦡..𐦷
    {0x109BE, 0x109BF, SC_Meroitic_Cursive, GC_Lo, NULL}, //  (Excluded) 𐦾..𐦿
    {0x10A00, 0x10A00, SC_Kharoshthi, GC_Lo, NULL}, //  (Excluded) 𐨀
    {0x10A10, 0x10A13, SC_Kharoshthi, GC_Lo, NULL}, //  (Excluded) 𐨐..𐨓
    {0x10A15, 0x10A17, SC_Kharoshthi, GC_Lo, NULL}, //  (Excluded) 𐨕..𐨗
    {0x10A19, 0x10A35, SC_Kharoshthi, GC_Lo, NULL}, //  (Excluded) 𐨙..𐨵
    {0x10A60, 0x10A7C, SC_Old_South_Arabian, GC_Lo, NULL}, //  (Excluded) 𐩠..𐩼
    {0x10A80, 0x10A9C, SC_Old_North_Arabian, GC_Lo, NULL}, //  (Excluded) 𐪀..𐪜
    {0x10AC0, 0x10AC7, SC_Manichaean, GC_Lo, NULL}, //  (Excluded) 𐫀..𐫇
    {0x10AC9, 0x10AE4, SC_Manichaean, GC_Lo, NULL}, //  (Excluded) 𐫉..𐫤
    {0x10B00, 0x10B35, SC_Avestan, GC_Lo, NULL}, //  (Excluded) 𐬀..𐬵
    {0x10B40, 0x10B55, SC_Inscriptional_Parthian, GC_Lo, NULL}, //  (Excluded) 𐭀..𐭕
    {0x10B60, 0x10B72, SC_Inscriptional_Pahlavi, GC_Lo, NULL}, //  (Excluded) 𐭠..𐭲
    {0x10B80, 0x10B91, SC_Psalter_Pahlavi, GC_Lo, NULL}, //  (Excluded) 𐮀..𐮑
    {0x10C00, 0x10C48, SC_Old_Turkic, GC_Lo, NULL}, //  (Excluded) 𐰀..𐱈
    {0x10C80, 0x10CB2, SC_Old_Hungarian, GC_Lu, NULL}, //  (Excluded) 𐲀..𐲲
    {0x10CC0, 0x10CF2, SC_Old_Hungarian, GC_Ll, NULL}, //  (Excluded) 𐳀..𐳲
    {0x10D4A, 0x10D65, SC_Garay, GC_L, NULL}, //  (Excluded) 𐵊..𐵥
    {0x10D6F, 0x10D85, SC_Garay, GC_L, NULL}, //  (Excluded) 𐵯..𐶅
    {0x10E80, 0x10EA9, SC_Yezidi, GC_Lo, NULL}, //  (Excluded) 𐺀..𐺩
    {0x10EB0, 0x10EB1, SC_Yezidi, GC_Lo, NULL}, //  (Excluded) 𐺰..𐺱
    {0x10F00, 0x10F1C, SC_Old_Sogdian, GC_Lo, NULL}, //  (Excluded) 𐼀..𐼜
    {0x10F27, 0x10F27, SC_Old_Sogdian, GC_Lo, NULL}, //  (Excluded) 𐼧
    {0x10F30, 0x10F45, SC_Sogdian, GC_Lo, NULL}, //  (Excluded) 𐼰..𐽅
    {0x10F70, 0x10F81, SC_Old_Uyghur, GC_Lo, NULL}, //  (Excluded) 𐽰..𐾁
    {0x10FB0, 0x10FC4, SC_Chorasmian, GC_Lo, NULL}, //  (Excluded) 𐾰..𐿄
    {0x10FE0, 0x10FF6, SC_Elymaic, GC_Lo, NULL}, //  (Excluded) 𐿠..𐿶
    {0x11003, 0x11037, SC_Brahmi, GC_Lo, NULL}, //  (Excluded) 𑀃..𑀷
    {0x11071, 0x11072, SC_Brahmi, GC_Lo, NULL}, //  (Excluded) 𑁱..𑁲
    {0x11075, 0x11075, SC_Brahmi, GC_Lo, NULL}, //  (Excluded) 𑁵
    {0x11083, 0x110AF, SC_Kaithi, GC_Lo, NULL}, //  (Excluded) 𑂃..𑂯
    {0x110D0, 0x110E8, SC_Sora_Sompeng, GC_Lo, NULL}, //  (Excluded) 𑃐..𑃨
    {0x11150, 0x11172, SC_Mahajani, GC_Lo, NULL}, //  (Excluded) 𑅐..𑅲
    {0x11176, 0x11176, SC_Mahajani, GC_Lo, NULL}, //  (Excluded) 𑅶
    {0x11183, 0x111B2, SC_Sharada, GC_Lo, NULL}, //  (Excluded) 𑆃..𑆲
    {0x111C1, 0x111C4, SC_Sharada, GC_Lo, NULL}, //  (Excluded) 𑇁..𑇄
    {0x111DA, 0x111DA, SC_Sharada, GC_Lo, NULL}, //  (Excluded) 𑇚
    {0x111DC, 0x111DC, SC_Sharada, GC_Lo, NULL}, //  (Excluded) 𑇜
    {0x11200, 0x11211, SC_Khojki, GC_Lo, NULL}, //  (Excluded) 𑈀..𑈑
    {0x11213, 0x1122B, SC_Khojki, GC_Lo, NULL}, //  (Excluded) 𑈓..𑈫
    {0x1123F, 0x11240, SC_Khojki, GC_Lo, NULL}, //  (Excluded) 𑈿..𑉀
    {0x11280, 0x11286, SC_Multani, GC_Lo, NULL}, //  (Excluded) 𑊀..𑊆
    {0x11288, 0x11288, SC_Multani, GC_Lo, NULL}, //  (Excluded) 𑊈
    {0x1128A, 0x1128D, SC_Multani, GC_Lo, NULL}, //  (Excluded) 𑊊..𑊍
    {0x1128F, 0x1129D, SC_Multani, GC_Lo, NULL}, //  (Excluded) 𑊏..𑊝
    {0x1129F, 0x112A8, SC_Multani, GC_Lo, NULL}, //  (Excluded) 𑊟..𑊨
    {0x112B0, 0x112DE, SC_Khudawadi, GC_Lo, NULL}, //  (Excluded) 𑊰..𑋞
    {0x11305, 0x1130C, SC_Grantha, GC_Lo, NULL}, //  (Excluded) 𑌅..𑌌
    {0x1130F, 0x11310, SC_Grantha, GC_Lo, NULL}, //  (Excluded) 𑌏..𑌐
    {0x11313, 0x11328, SC_Grantha, GC_Lo, NULL}, //  (Excluded) 𑌓..𑌨
    {0x1132A, 0x11330, SC_Grantha, GC_Lo, NULL}, //  (Excluded) 𑌪..𑌰
    {0x11332, 0x11333, SC_Grantha, GC_Lo, NULL}, //  (Excluded) 𑌲..𑌳
    {0x11335, 0x11339, SC_Grantha, GC_Lo, NULL}, //  (Excluded) 𑌵..𑌹
    {0x1133D, 0x1133D, SC_Grantha, GC_Lo, NULL}, //  (Excluded) 𑌽
    {0x11350, 0x11350, SC_Grantha, GC_Lo, NULL}, //  (Excluded) 𑍐
    {0x1135D, 0x11361, SC_Grantha, GC_Lo, NULL}, //  (Excluded) 𑍝..𑍡
    {0x11380, 0x11389, SC_Tulu_Tigalari, GC_Lo, NULL}, //  (Excluded) 𑎀..𑎉
    {0x1138B, 0x1138B, SC_Tulu_Tigalari, GC_Lo, NULL}, //  (Excluded) 𑎋
    {0x1138E, 0x1138E, SC_Tulu_Tigalari, GC_Lo, NULL}, //  (Excluded) 𑎎
    {0x11390, 0x113B5, SC_Tulu_Tigalari, GC_Lo, NULL}, //  (Excluded) 𑎐..𑎵
    {0x113B7, 0x113B7, SC_Tulu_Tigalari, GC_Lo, NULL}, //  (Excluded) 𑎷
    {0x113D1, 0x113D1, SC_Tulu_Tigalari, GC_Lo, NULL}, //  (Excluded) 𑏑
    {0x113D3, 0x113D3, SC_Tulu_Tigalari, GC_Lo, NULL}, //  (Excluded) 𑏓
    {0x11480, 0x114AF, SC_Tirhuta, GC_Lo, NULL}, //  (Excluded) 𑒀..𑒯
    {0x114C4, 0x114C5, SC_Tirhuta, GC_Lo, NULL}, //  (Excluded) 𑓄..𑓅
    {0x114C7, 0x114C7, SC_Tirhuta, GC_Lo, NULL}, //  (Excluded) 𑓇
    {0x11580, 0x115AE, SC_Siddham, GC_Lo, NULL}, //  (Excluded) 𑖀..𑖮
    {0x115D8, 0x115DB, SC_Siddham, GC_Lo, NULL}, //  (Excluded) 𑗘..𑗛
    {0x11600, 0x1162F, SC_Modi, GC_Lo, NULL}, //  (Excluded) 𑘀..𑘯
    {0x11644, 0x11644, SC_Modi, GC_Lo, NULL}, //  (Excluded) 𑙄
    {0x11680, 0x116AA, SC_Takri, GC_Lo, NULL}, //  (Excluded) 𑚀..𑚪
    {0x116B8, 0x116B8, SC_Takri, GC_Lo, NULL}, //  (Excluded) 𑚸
    {0x11700, 0x1171A, SC_Ahom, GC_Lo, NULL}, //  (Excluded) 𑜀..𑜚
    {0x11740, 0x11746, SC_Ahom, GC_Lo, NULL}, //  (Excluded) 𑝀..𑝆
    {0x11800, 0x1182B, SC_Dogra, GC_Lo, NULL}, //  (Excluded) 𑠀..𑠫
    {0x118A0, 0x118DF, SC_Warang_Citi, GC_L, NULL}, //  (Excluded) 𑢠..𑣟
    {0x118FF, 0x118FF, SC_Warang_Citi, GC_Lo, NULL}, //  (Excluded) 𑣿
    {0x11901, 0x11906, SC_Dives_Akuru, GC_Lo, NULL}, //  (Excluded) 𑤁..𑤆
    {0x11909, 0x11909, SC_Dives_Akuru, GC_Lo, NULL}, //  (Excluded) 𑤉
    {0x1190C, 0x11913, SC_Dives_Akuru, GC_Lo, NULL}, //  (Excluded) 𑤌..𑤓
    {0x11915, 0x11916, SC_Dives_Akuru, GC_Lo, NULL}, //  (Excluded) 𑤕..𑤖
    {0x11918, 0x1192F, SC_Dives_Akuru, GC_Lo, NULL}, //  (Excluded) 𑤘..𑤯
    {0x1193F, 0x1193F, SC_Dives_Akuru, GC_Lo, NULL}, //  (Excluded) 𑤿
    {0x11941, 0x11941, SC_Dives_Akuru, GC_Lo, NULL}, //  (Excluded) 𑥁
    {0x119A0, 0x119A7, SC_Nandinagari, GC_Lo, NULL}, //  (Excluded) 𑦠..𑦧
    {0x119AA, 0x119D0, SC_Nandinagari, GC_Lo, NULL}, //  (Excluded) 𑦪..𑧐
    {0x119E1, 0x119E1, SC_Nandinagari, GC_Lo, NULL}, //  (Excluded) 𑧡
    {0x119E3, 0x119E3, SC_Nandinagari, GC_Lo, NULL}, //  (Excluded) 𑧣
    {0x11A00, 0x11A00, SC_Zanabazar_Square, GC_Lo, NULL}, //  (Excluded) 𑨀
    {0x11A0B, 0x11A32, SC_Zanabazar_Square, GC_Lo, NULL}, //  (Excluded) 𑨋..𑨲
    {0x11A3A, 0x11A3A, SC_Zanabazar_Square, GC_Lo, NULL}, //  (Excluded) 𑨺
    {0x11A50, 0x11A50, SC_Soyombo, GC_Lo, NULL}, //  (Excluded) 𑩐
    {0x11A5C, 0x11A89, SC_Soyombo, GC_Lo, NULL}, //  (Excluded) 𑩜..𑪉
    {0x11A9D, 0x11A9D, SC_Soyombo, GC_Lo, NULL}, //  (Excluded) 𑪝
    {0x11AC0, 0x11AF8, SC_Pau_Cin_Hau, GC_Lo, NULL}, //  (Excluded) 𑫀..𑫸
    {0x11BC0, 0x11BE0, SC_Sunuwar, GC_Lo, NULL}, //  (Excluded) 𑯀..𑯠
    {0x11C00, 0x11C08, SC_Bhaiksuki, GC_Lo, NULL}, //  (Excluded) 𑰀..𑰈
    {0x11C0A, 0x11C2E, SC_Bhaiksuki, GC_Lo, NULL}, //  (Excluded) 𑰊..𑰮
    {0x11C40, 0x11C40, SC_Bhaiksuki, GC_Lo, NULL}, //  (Excluded) 𑱀
    {0x11C72, 0x11C8F, SC_Marchen, GC_Lo, NULL}, //  (Excluded) 𑱲..𑲏
    {0x11D00, 0x11D06, SC_Masaram_Gondi, GC_Lo, NULL}, //  (Excluded) 𑴀..𑴆
    {0x11D08, 0x11D09, SC_Masaram_Gondi, GC_Lo, NULL}, //  (Excluded) 𑴈..𑴉
    {0x11D0B, 0x11D30, SC_Masaram_Gondi, GC_Lo, NULL}, //  (Excluded) 𑴋..𑴰
    {0x11D46, 0x11D46, SC_Masaram_Gondi, GC_Lo, NULL}, //  (Excluded) 𑵆
    {0x11D60, 0x11D65, SC_Gunjala_Gondi, GC_Lo, NULL}, //  (Excluded) 𑵠..𑵥
    {0x11D67, 0x11D68, SC_Gunjala_Gondi, GC_Lo, NULL}, //  (Excluded) 𑵧..𑵨
    {0x11D6A, 0x11D89, SC_Gunjala_Gondi, GC_Lo, NULL}, //  (Excluded) 𑵪..𑶉
    {0x11D98, 0x11D98, SC_Gunjala_Gondi, GC_Lo, NULL}, //  (Excluded) 𑶘
    {0x11DB0, 0x11DDB, SC_Tolong_Siki, GC_L, NULL}, //  (Excluded) 𑶰..𑷛
    {0x11EE0, 0x11EF2, SC_Makasar, GC_Lo, NULL}, //  (Excluded) 𑻠..𑻲
    {0x11F02, 0x11F02, SC_Kawi, GC_Lo, NULL}, //  (Excluded) 𑼂
    {0x11F04, 0x11F10, SC_Kawi, GC_Lo, NULL}, //  (Excluded) 𑼄..𑼐
    {0x11F12, 0x11F33, SC_Kawi, GC_Lo, NULL}, //  (Excluded) 𑼒..𑼳
    {0x12000, 0x12399, SC_Cuneiform, GC_Lo, NULL}, //  (Excluded) 𒀀..𒎙
    {0x12400, 0x1246E, SC_Cuneiform, GC_Nl, NULL}, //  (Excluded) 𒐀..𒑮
    {0x12480, 0x12543, SC_Cuneiform, GC_Lo, NULL}, //  (Excluded) 𒒀..𒕃
    {0x12F90, 0x12FF0, SC_Cypro_Minoan, GC_Lo, NULL}, //  (Excluded) 𒾐..𒿰
    {0x13000, 0x1342F, SC_Egyptian_Hieroglyphs, GC_Lo, NULL}, //  (Excluded) 𓀀..𓐯
    {0x13441, 0x13446, SC_Egyptian_Hieroglyphs, GC_Lo, NULL}, //  (Excluded) 𓑁..𓑆
    {0x13460, 0x143FA, SC_Egyptian_Hieroglyphs, GC_Lo, NULL}, //  (Excluded) 𓑠..𔏺
    {0x14400, 0x14646, SC_Anatolian_Hieroglyphs, GC_Lo, NULL}, //  (Excluded) 𔐀..𔙆
    {0x16100, 0x1611D, SC_Gurung_Khema, GC_Lo, NULL}, //  (Excluded) 𖄀..𖄝
    {0x16A70, 0x16ABE, SC_Tangsa, GC_Lo, NULL}, //  (Excluded) 𖩰..𖪾
    {0x16AD0, 0x16AED, SC_Bassa_Vah, GC_Lo, NULL}, //  (Excluded) 𖫐..𖫭
    {0x16B00, 0x16B2F, SC_Pahawh_Hmong, GC_Lo, NULL}, //  (Excluded) 𖬀..𖬯
    {0x16B40, 0x16B43, SC_Pahawh_Hmong, GC_Lm, NULL}, //  (Excluded) 𖭀..𖭃
    {0x16B63, 0x16B77, SC_Pahawh_Hmong, GC_Lo, NULL}, //  (Excluded) 𖭣..𖭷
    {0x16B7D, 0x16B8F, SC_Pahawh_Hmong, GC_Lo, NULL}, //  (Excluded) 𖭽..𖮏
    {0x16D40, 0x16D6C, SC_Kirat_Rai, GC_L, NULL}, //  (Excluded) 𖵀..𖵬
    {0x16E40, 0x16E7F, SC_Medefaidrin, GC_L, NULL}, //  (Excluded) 𖹀..𖹿
    {0x16EA0, 0x16EB8, SC_Beria_Erfe, GC_Lu, NULL}, //  (Excluded) 𖺠..𖺸
    {0x16EBB, 0x16ED3, SC_Beria_Erfe, GC_Ll, NULL}, //  (Excluded) 𖺻..𖻓
    {0x16FE0, 0x16FE0, SC_Tangut, GC_Lm, NULL}, //  (Excluded) 𖿠
    {0x17000, 0x18AFF, SC_Tangut, GC_Lo, NULL}, //  (Excluded) 𗀀..𘫿
    {0x18B01, 0x18CD5, SC_Khitan_Small_Script, GC_Lo, NULL}, //  (Excluded) 𘬁..𘳕
    {0x18CFF, 0x18CFF, SC_Khitan_Small_Script, GC_Lo, NULL}, //  (Excluded) 𘳿
    {0x18D01, 0x18D1E, SC_Tangut, GC_Lo, NULL}, //  (Excluded) 𘴁..𘴞
    {0x18D80, 0x18DF2, SC_Tangut, GC_Lo, NULL}, //  (Excluded) 𘶀..𘷲
    {0x1B170, 0x1B2FB, SC_Nushu, GC_Lo, NULL}, //  (Excluded) 𛅰..𛋻
    {0x1BC00, 0x1BC6A, SC_Duployan, GC_Lo, NULL}, //  (Excluded) 𛰀..𛱪
    {0x1BC70, 0x1BC7C, SC_Duployan, GC_Lo, NULL}, //  (Excluded) 𛱰..𛱼
    {0x1BC80, 0x1BC88, SC_Duployan, GC_Lo, NULL}, //  (Excluded) 𛲀..𛲈
    {0x1BC90, 0x1BC99, SC_Duployan, GC_Lo, NULL}, //  (Excluded) 𛲐..𛲙
    {0x1E290, 0x1E2AD, SC_Toto, GC_Lo, NULL}, //  (Excluded) 𞊐..𞊭
    {0x1E4D0, 0x1E4EB, SC_Nag_Mundari, GC_L, NULL}, //  (Excluded) 𞓐..𞓫
    {0x1E5D0, 0x1E5ED, SC_Ol_Onal, GC_Lo, NULL}, //  (Excluded) 𞗐..𞗭
    {0x1E5F0, 0x1E5F0, SC_Ol_Onal, GC_Lo, NULL}, //  (Excluded) 𞗰
    {0x1E6C0, 0x1E6DE, SC_Tai_Yo, GC_Lo, NULL}, //  (Excluded) 𞛀..𞛞
    {0x1E6E0, 0x1E6E2, SC_Tai_Yo, GC_Lo, NULL}, //  (Excluded) 𞛠..𞛢
    {0x1E6E4, 0x1E6E5, SC_Tai_Yo, GC_Lo, NULL}, //  (Excluded) 𞛤..𞛥
    {0x1E6E7, 0x1E6ED, SC_Tai_Yo, GC_Lo, NULL}, //  (Excluded) 𞛧..𞛭
    {0x1E6F0, 0x1E6F4, SC_Tai_Yo, GC_Lo, NULL}, //  (Excluded) 𞛰..𞛴
    {0x1E6FE, 0x1E6FF, SC_Tai_Yo, GC_L, NULL}, //  (Excluded) 𞛾..𞛿
    {0x1E800, 0x1E8C4, SC_Mende_Kikakui, GC_Lo, NULL}, //  (Excluded) 𞠀..𞣄
}; // 186 ranges, 42 singles, 19620 codepoints

// Only excluded scripts, XID_Continue,!XID_Start, more IDTypes, NFC and !MARK
static const struct sc_tr39 tr39_excl_cont_list[] = {
    {0x1810, 0x1819, SC_Mongolian, GC_Nd, NULL}, //  (Excluded) ᠐..᠙
    {0x104A0, 0x104A9, SC_Osmanya, GC_Nd, NULL}, //  (Excluded) 𐒠..𐒩
    {0x10D40, 0x10D49, SC_Garay, GC_Nd, NULL}, //  (Excluded) 𐵀..𐵉
    {0x11066, 0x1106F, SC_Brahmi, GC_Nd, NULL}, //  (Excluded) 𑁦..𑁯
    {0x110F0, 0x110F9, SC_Sora_Sompeng, GC_Nd, NULL}, //  (Excluded) 𑃰..𑃹
    {0x111D0, 0x111D9, SC_Sharada, GC_Nd, NULL}, //  (Excluded) 𑇐..𑇙
    {0x112F0, 0x112F9, SC_Khudawadi, GC_Nd, NULL}, //  (Excluded) 𑋰..𑋹
    {0x114D0, 0x114D9, SC_Tirhuta, GC_Nd, NULL}, //  (Excluded) 𑓐..𑓙
    {0x11650, 0x11659, SC_Modi, GC_Nd, NULL}, //  (Excluded) 𑙐..𑙙
    {0x116C0, 0x116C9, SC_Takri, GC_Nd, NULL}, //  (Excluded) 𑛀..𑛉
    {0x11730, 0x11739, SC_Ahom, GC_Nd, NULL}, //  (Excluded) 𑜰..𑜹
    {0x118E0, 0x118E9, SC_Warang_Citi, GC_Nd, NULL}, //  (Excluded) 𑣠..𑣩
    {0x11950, 0x11959, SC_Dives_Akuru, GC_Nd, NULL}, //  (Excluded) 𑥐..𑥙
    {0x11BF0, 0x11BF9, SC_Sunuwar, GC_Nd, NULL}, //  (Excluded) 𑯰..𑯹
    {0x11C50, 0x11C59, SC_Bhaiksuki, GC_Nd, NULL}, //  (Excluded) 𑱐..𑱙
    {0x11D50, 0x11D59, SC_Masaram_Gondi, GC_Nd, NULL}, //  (Excluded) 𑵐..𑵙
    {0x11DA0, 0x11DA9, SC_Gunjala_Gondi, GC_Nd, NULL}, //  (Excluded) 𑶠..𑶩
    {0x11DE0, 0x11DE9, SC_Tolong_Siki, GC_Nd, NULL}, //  (Excluded) 𑷠..𑷩
    {0x11F50, 0x11F59, SC_Kawi, GC_Nd, NULL}, //  (Excluded) 𑽐..𑽙
    {0x16130, 0x16139, SC_Gurung_Khema, GC_Nd, NULL}, //  (Excluded) 𖄰..𖄹
    {0x16AC0, 0x16AC9, SC_Tangsa, GC_Nd, NULL}, //  (Excluded) 𖫀..𖫉
    {0x16B50, 0x16B59, SC_Pahawh_Hmong, GC_Nd, NULL}, //  (Excluded) 𖭐..𖭙
    {0x16D70, 0x16D79, SC_Kirat_Rai, GC_Nd, NULL}, //  (Excluded) 𖵰..𖵹
    {0x1E4F0, 0x1E4F9, SC_Nag_Mundari, GC_Nd, NULL}, //  (Excluded) 𞓰..𞓹
    {0x1E5F1, 0x1E5FA, SC_Ol_Onal, GC_Nd, NULL}, //  (Excluded) 𞗱..𞗺
}; // 25 ranges, 0 singles, 225 codepoints

// tr39_start/cont + MEDIAL
static const struct range_bool tr39_medial_list[] = {
    {0x30FB, 0x30FB}, // Common ・
}; // 0 ranges, 1 singles, 0 codepoints

// NFC Normalization tables:

/* Canonical Decomposition */
/* Using now indirect tables with indices into the unique values */
/* even sparing the final \0 */

#undef NORMALIZE_IND_TBL
#define NORMALIZE_IND_TBL
/* tbl sizes: (3,8,1173,302,162,233,,67,2,,,13) */
/* l: 1-12 */
/* max size: 1173 0x495 */
#define UN8IF_canon_MAXLEN  5
#define TBL(i)               ((i-1) << 11)
/* value = (const char*)&UN8IF_canon_tbl[LEN-1][IDX] */
#define UN8IF_canon_LEN(v)  (((v) >> 11) + 1)
#define UN8IF_canon_IDX(v)  ((v) & 0x7ff)
#define UN8IF_canon_PLANE_T uint16_t

/* the values */
static const char UN8IF_canon_tbl_1[3][1] = {
    /*   0 */ {';'}, {'K'}, {'`'}};

static const char UN8IF_canon_tbl_2[8][2] = {
    /*   0 */ {'\xc2', '\xb4'}, {'\xc2', '\xb7'}, {'\xca', '\xb9'}, {'\xcc', '\x80'}, {'\xcc', '\x81'}, {'\xcc', '\x93'}, {'\xce', '\xa9'}, {'\xce', '\xb9'}};

static const char UN8IF_canon_tbl_3[1173][3] = {
    /*   0 */ {'<', '\xcc', '\xb8'}, {'=', '\xcc', '\xb8'}, {'>', '\xcc', '\xb8'}, {'A', '\xcc', '\x80'}, {'A', '\xcc', '\x81'}, {'A', '\xcc', '\x82'}, {'A', '\xcc', '\x83'}, {'A', '\xcc', '\x84'},
    /*   8 */ {'A', '\xcc', '\x86'},
    {'A', '\xcc', '\x87'},
    {'A', '\xcc', '\x88'},
    {'A', '\xcc', '\x89'},
    {'A', '\xcc', '\x8a'},
    {'A', '\xcc', '\x8c'},
    {'A', '\xcc', '\x8f'},
    {'A', '\xcc', '\x91'},
    /*  16 */ {'A', '\xcc', '\xa3'},
    {'A', '\xcc', '\xa5'},
    {'A', '\xcc', '\xa8'},
    {'B', '\xcc', '\x87'},
    {'B', '\xcc', '\xa3'},
    {'B', '\xcc', '\xb1'},
    {'C', '\xcc', '\x81'},
    {'C', '\xcc', '\x82'},
    /*  24 */ {'C', '\xcc', '\x87'},
    {'C', '\xcc', '\x8c'},
    {'C', '\xcc', '\xa7'},
    {'D', '\xcc', '\x87'},
    {'D', '\xcc', '\x8c'},
    {'D', '\xcc', '\xa3'},
    {'D', '\xcc', '\xa7'},
    {'D', '\xcc', '\xad'},
    /*  32 */ {'D', '\xcc', '\xb1'},
    {'E', '\xcc', '\x80'},
    {'E', '\xcc', '\x81'},
    {'E', '\xcc', '\x82'},
    {'E', '\xcc', '\x83'},
    {'E', '\xcc', '\x84'},
    {'E', '\xcc', '\x86'},
    {'E', '\xcc', '\x87'},
    /*  40 */ {'E', '\xcc', '\x88'},
    {'E', '\xcc', '\x89'},
    {'E', '\xcc', '\x8c'},
    {'E', '\xcc', '\x8f'},
    {'E', '\xcc', '\x91'},
    {'E', '\xcc', '\xa3'},
    {'E', '\xcc', '\xa7'},
    {'E', '\xcc', '\xa8'},
    /*  48 */ {'E', '\xcc', '\xad'},
    {'E', '\xcc', '\xb0'},
    {'F', '\xcc', '\x87'},
    {'G', '\xcc', '\x81'},
    {'G', '\xcc', '\x82'},
    {'G', '\xcc', '\x84'},
    {'G', '\xcc', '\x86'},
    {'G', '\xcc', '\x87'},
    /*  56 */ {'G', '\xcc', '\x8c'},
    {'G', '\xcc', '\xa7'},
    {'H', '\xcc', '\x82'},
    {'H', '\xcc', '\x87'},
    {'H', '\xcc', '\x88'},
    {'H', '\xcc', '\x8c'},
    {'H', '\xcc', '\xa3'},
    {'H', '\xcc', '\xa7'},
    /*  64 */ {'H', '\xcc', '\xae'},
    {'I', '\xcc', '\x80'},
    {'I', '\xcc', '\x81'},
    {'I', '\xcc', '\x82'},
    {'I', '\xcc', '\x83'},
    {'I', '\xcc', '\x84'},
    {'I', '\xcc', '\x86'},
    {'I', '\xcc', '\x87'},
    /*  72 */ {'I', '\xcc', '\x88'},
    {'I', '\xcc', '\x89'},
    {'I', '\xcc', '\x8c'},
    {'I', '\xcc', '\x8f'},
    {'I', '\xcc', '\x91'},
    {'I', '\xcc', '\xa3'},
    {'I', '\xcc', '\xa8'},
    {'I', '\xcc', '\xb0'},
    /*  80 */ {'J', '\xcc', '\x82'},
    {'K', '\xcc', '\x81'},
    {'K', '\xcc', '\x8c'},
    {'K', '\xcc', '\xa3'},
    {'K', '\xcc', '\xa7'},
    {'K', '\xcc', '\xb1'},
    {'L', '\xcc', '\x81'},
    {'L', '\xcc', '\x8c'},
    /*  88 */ {'L', '\xcc', '\xa3'},
    {'L', '\xcc', '\xa7'},
    {'L', '\xcc', '\xad'},
    {'L', '\xcc', '\xb1'},
    {'M', '\xcc', '\x81'},
    {'M', '\xcc', '\x87'},
    {'M', '\xcc', '\xa3'},
    {'N', '\xcc', '\x80'},
    /*  96 */ {'N', '\xcc', '\x81'},
    {'N', '\xcc', '\x83'},
    {'N', '\xcc', '\x87'},
    {'N', '\xcc', '\x8c'},
    {'N', '\xcc', '\xa3'},
    {'N', '\xcc', '\xa7'},
    {'N', '\xcc', '\xad'},
    {'N', '\xcc', '\xb1'},
    /* 104 */ {'O', '\xcc', '\x80'},
    {'O', '\xcc', '\x81'},
    {'O', '\xcc', '\x82'},
    {'O', '\xcc', '\x83'},
    {'O', '\xcc', '\x84'},
    {'O', '\xcc', '\x86'},
    {'O', '\xcc', '\x87'},
    {'O', '\xcc', '\x88'},
    /* 112 */ {'O', '\xcc', '\x89'},
    {'O', '\xcc', '\x8b'},
    {'O', '\xcc', '\x8c'},
    {'O', '\xcc', '\x8f'},
    {'O', '\xcc', '\x91'},
    {'O', '\xcc', '\x9b'},
    {'O', '\xcc', '\xa3'},
    {'O', '\xcc', '\xa8'},
    /* 120 */ {'P', '\xcc', '\x81'},
    {'P', '\xcc', '\x87'},
    {'R', '\xcc', '\x81'},
    {'R', '\xcc', '\x87'},
    {'R', '\xcc', '\x8c'},
    {'R', '\xcc', '\x8f'},
    {'R', '\xcc', '\x91'},
    {'R', '\xcc', '\xa3'},
    /* 128 */ {'R', '\xcc', '\xa7'},
    {'R', '\xcc', '\xb1'},
    {'S', '\xcc', '\x81'},
    {'S', '\xcc', '\x82'},
    {'S', '\xcc', '\x87'},
    {'S', '\xcc', '\x8c'},
    {'S', '\xcc', '\xa3'},
    {'S', '\xcc', '\xa6'},
    /* 136 */ {'S', '\xcc', '\xa7'},
    {'T', '\xcc', '\x87'},
    {'T', '\xcc', '\x8c'},
    {'T', '\xcc', '\xa3'},
    {'T', '\xcc', '\xa6'},
    {'T', '\xcc', '\xa7'},
    {'T', '\xcc', '\xad'},
    {'T', '\xcc', '\xb1'},
    /* 144 */ {'U', '\xcc', '\x80'},
    {'U', '\xcc', '\x81'},
    {'U', '\xcc', '\x82'},
    {'U', '\xcc', '\x83'},
    {'U', '\xcc', '\x84'},
    {'U', '\xcc', '\x86'},
    {'U', '\xcc', '\x88'},
    {'U', '\xcc', '\x89'},
    /* 152 */ {'U', '\xcc', '\x8a'},
    {'U', '\xcc', '\x8b'},
    {'U', '\xcc', '\x8c'},
    {'U', '\xcc', '\x8f'},
    {'U', '\xcc', '\x91'},
    {'U', '\xcc', '\x9b'},
    {'U', '\xcc', '\xa3'},
    {'U', '\xcc', '\xa4'},
    /* 160 */ {'U', '\xcc', '\xa8'},
    {'U', '\xcc', '\xad'},
    {'U', '\xcc', '\xb0'},
    {'V', '\xcc', '\x83'},
    {'V', '\xcc', '\xa3'},
    {'W', '\xcc', '\x80'},
    {'W', '\xcc', '\x81'},
    {'W', '\xcc', '\x82'},
    /* 168 */ {'W', '\xcc', '\x87'},
    {'W', '\xcc', '\x88'},
    {'W', '\xcc', '\xa3'},
    {'X', '\xcc', '\x87'},
    {'X', '\xcc', '\x88'},
    {'Y', '\xcc', '\x80'},
    {'Y', '\xcc', '\x81'},
    {'Y', '\xcc', '\x82'},
    /* 176 */ {'Y', '\xcc', '\x83'},
    {'Y', '\xcc', '\x84'},
    {'Y', '\xcc', '\x87'},
    {'Y', '\xcc', '\x88'},
    {'Y', '\xcc', '\x89'},
    {'Y', '\xcc', '\xa3'},
    {'Z', '\xcc', '\x81'},
    {'Z', '\xcc', '\x82'},
    /* 184 */ {'Z', '\xcc', '\x87'},
    {'Z', '\xcc', '\x8c'},
    {'Z', '\xcc', '\xa3'},
    {'Z', '\xcc', '\xb1'},
    {'\xe2', '\x80', '\x82'},
    {'\xe2', '\x80', '\x83'},
    {'\xe3', '\x80', '\x88'},
    {'\xe3', '\x80', '\x89'},
    /* 192 */ {'\xe3', '\x92', '\x9e'},
    {'\xe3', '\x92', '\xb9'},
    {'\xe3', '\x92', '\xbb'},
    {'\xe3', '\x93', '\x9f'},
    {'\xe3', '\x94', '\x95'},
    {'\xe3', '\x9b', '\xae'},
    {'\xe3', '\x9b', '\xbc'},
    {'\xe3', '\x9e', '\x81'},
    /* 200 */ {'\xe3', '\xa0', '\xaf'},
    {'\xe3', '\xa1', '\xa2'},
    {'\xe3', '\xa1', '\xbc'},
    {'\xe3', '\xa3', '\x87'},
    {'\xe3', '\xa3', '\xa3'},
    {'\xe3', '\xa4', '\x9c'},
    {'\xe3', '\xa4', '\xba'},
    {'\xe3', '\xa8', '\xae'},
    /* 208 */ {'\xe3', '\xa9', '\xac'},
    {'\xe3', '\xab', '\xa4'},
    {'\xe3', '\xac', '\x88'},
    {'\xe3', '\xac', '\x99'},
    {'\xe3', '\xad', '\x89'},
    {'\xe3', '\xae', '\x9d'},
    {'\xe3', '\xb0', '\x98'},
    {'\xe3', '\xb1', '\x8e'},
    /* 216 */ {'\xe3', '\xb4', '\xb3'},
    {'\xe3', '\xb6', '\x96'},
    {'\xe3', '\xba', '\xac'},
    {'\xe3', '\xba', '\xb8'},
    {'\xe3', '\xbc', '\x9b'},
    {'\xe3', '\xbf', '\xbc'},
    {'\xe4', '\x80', '\x88'},
    {'\xe4', '\x80', '\x98'},
    /* 224 */ {'\xe4', '\x80', '\xb9'},
    {'\xe4', '\x81', '\x86'},
    {'\xe4', '\x82', '\x96'},
    {'\xe4', '\x83', '\xa3'},
    {'\xe4', '\x84', '\xaf'},
    {'\xe4', '\x88', '\x82'},
    {'\xe4', '\x88', '\xa7'},
    {'\xe4', '\x8a', '\xa0'},
    /* 232 */ {'\xe4', '\x8c', '\x81'},
    {'\xe4', '\x8c', '\xb4'},
    {'\xe4', '\x8d', '\x99'},
    {'\xe4', '\x8f', '\x95'},
    {'\xe4', '\x8f', '\x99'},
    {'\xe4', '\x90', '\x8b'},
    {'\xe4', '\x91', '\xab'},
    {'\xe4', '\x94', '\xab'},
    /* 240 */ {'\xe4', '\x95', '\x9d'},
    {'\xe4', '\x95', '\xa1'},
    {'\xe4', '\x95', '\xab'},
    {'\xe4', '\x97', '\x97'},
    {'\xe4', '\x97', '\xb9'},
    {'\xe4', '\x98', '\xb5'},
    {'\xe4', '\x9a', '\xbe'},
    {'\xe4', '\x9b', '\x87'},
    /* 248 */ {'\xe4', '\xa6', '\x95'},
    {'\xe4', '\xa7', '\xa6'},
    {'\xe4', '\xa9', '\xae'},
    {'\xe4', '\xa9', '\xb6'},
    {'\xe4', '\xaa', '\xb2'},
    {'\xe4', '\xac', '\xb3'},
    {'\xe4', '\xaf', '\x8e'},
    {'\xe4', '\xb3', '\x8e'},
    /* 256 */ {'\xe4', '\xb3', '\xad'},
    {'\xe4', '\xb3', '\xb8'},
    {'\xe4', '\xb5', '\x96'},
    {'\xe4', '\xb8', '\x8d'},
    {'\xe4', '\xb8', '\xa6'},
    {'\xe4', '\xb8', '\xb2'},
    {'\xe4', '\xb8', '\xb8'},
    {'\xe4', '\xb8', '\xb9'},
    /* 264 */ {'\xe4', '\xb8', '\xbd'},
    {'\xe4', '\xb9', '\x81'},
    {'\xe4', '\xba', '\x82'},
    {'\xe4', '\xba', '\x86'},
    {'\xe4', '\xba', '\xae'},
    {'\xe4', '\xbb', '\x80'},
    {'\xe4', '\xbb', '\x8c'},
    {'\xe4', '\xbb', '\xa4'},
    /* 272 */ {'\xe4', '\xbd', '\xa0'},
    {'\xe4', '\xbe', '\x80'},
    {'\xe4', '\xbe', '\x86'},
    {'\xe4', '\xbe', '\x8b'},
    {'\xe4', '\xbe', '\xae'},
    {'\xe4', '\xbe', '\xbb'},
    {'\xe4', '\xbe', '\xbf'},
    {'\xe5', '\x80', '\x82'},
    /* 280 */ {'\xe5', '\x80', '\xab'},
    {'\xe5', '\x81', '\xba'},
    {'\xe5', '\x82', '\x99'},
    {'\xe5', '\x83', '\x8f'},
    {'\xe5', '\x83', '\x9a'},
    {'\xe5', '\x83', '\xa7'},
    {'\xe5', '\x85', '\x80'},
    {'\xe5', '\x85', '\x85'},
    /* 288 */ {'\xe5', '\x85', '\x8d'},
    {'\xe5', '\x85', '\x94'},
    {'\xe5', '\x85', '\xa4'},
    {'\xe5', '\x85', '\xa7'},
    {'\xe5', '\x85', '\xa8'},
    {'\xe5', '\x85', '\xa9'},
    {'\xe5', '\x85', '\xad'},
    {'\xe5', '\x85', '\xb7'},
    /* 296 */ {'\xe5', '\x86', '\x80'},
    {'\xe5', '\x86', '\x8d'},
    {'\xe5', '\x86', '\x92'},
    {'\xe5', '\x86', '\x95'},
    {'\xe5', '\x86', '\x97'},
    {'\xe5', '\x86', '\xa4'},
    {'\xe5', '\x86', '\xac'},
    {'\xe5', '\x86', '\xb5'},
    /* 304 */ {'\xe5', '\x86', '\xb7'},
    {'\xe5', '\x87', '\x89'},
    {'\xe5', '\x87', '\x8c'},
    {'\xe5', '\x87', '\x9c'},
    {'\xe5', '\x87', '\x9e'},
    {'\xe5', '\x87', '\xb5'},
    {'\xe5', '\x88', '\x83'},
    {'\xe5', '\x88', '\x87'},
    /* 312 */ {'\xe5', '\x88', '\x97'},
    {'\xe5', '\x88', '\xa9'},
    {'\xe5', '\x88', '\xba'},
    {'\xe5', '\x88', '\xbb'},
    {'\xe5', '\x89', '\x86'},
    {'\xe5', '\x89', '\xb2'},
    {'\xe5', '\x89', '\xb7'},
    {'\xe5', '\x8a', '\x89'},
    /* 320 */ {'\xe5', '\x8a', '\x9b'},
    {'\xe5', '\x8a', '\xa3'},
    {'\xe5', '\x8a', '\xb3'},
    {'\xe5', '\x8b', '\x87'},
    {'\xe5', '\x8b', '\x89'},
    {'\xe5', '\x8b', '\x92'},
    {'\xe5', '\x8b', '\x9e'},
    {'\xe5', '\x8b', '\xa4'},
    /* 328 */ {'\xe5', '\x8b', '\xb5'},
    {'\xe5', '\x8b', '\xba'},
    {'\xe5', '\x8c', '\x85'},
    {'\xe5', '\x8c', '\x86'},
    {'\xe5', '\x8c', '\x97'},
    {'\xe5', '\x8c', '\xbf'},
    {'\xe5', '\x8d', '\x89'},
    {'\xe5', '\x8d', '\x91'},
    /* 336 */ {'\xe5', '\x8d', '\x9a'},
    {'\xe5', '\x8d', '\xb3'},
    {'\xe5', '\x8d', '\xb5'},
    {'\xe5', '\x8d', '\xbd'},
    {'\xe5', '\x8d', '\xbf'},
    {'\xe5', '\x8f', '\x83'},
    {'\xe5', '\x8f', '\x8a'},
    {'\xe5', '\x8f', '\x9f'},
    /* 344 */ {'\xe5', '\x8f', '\xa5'},
    {'\xe5', '\x8f', '\xab'},
    {'\xe5', '\x8f', '\xb1'},
    {'\xe5', '\x90', '\x86'},
    {'\xe5', '\x90', '\x8f'},
    {'\xe5', '\x90', '\x9d'},
    {'\xe5', '\x90', '\xb8'},
    {'\xe5', '\x91', '\x82'},
    /* 352 */ {'\xe5', '\x91', '\x88'},
    {'\xe5', '\x91', '\xa8'},
    {'\xe5', '\x92', '\x9e'},
    {'\xe5', '\x92', '\xa2'},
    {'\xe5', '\x92', '\xbd'},
    {'\xe5', '\x93', '\xb6'},
    {'\xe5', '\x94', '\x90'},
    {'\xe5', '\x95', '\x93'},
    /* 360 */ {'\xe5', '\x95', '\x95'},
    {'\xe5', '\x95', '\xa3'},
    {'\xe5', '\x96', '\x84'},
    {'\xe5', '\x96', '\x87'},
    {'\xe5', '\x96', '\x99'},
    {'\xe5', '\x96', '\x9d'},
    {'\xe5', '\x96', '\xab'},
    {'\xe5', '\x96', '\xb3'},
    /* 368 */ {'\xe5', '\x97', '\x80'},
    {'\xe5', '\x97', '\x82'},
    {'\xe5', '\x97', '\xa2'},
    {'\xe5', '\x98', '\x86'},
    {'\xe5', '\x99', '\x91'},
    {'\xe5', '\x99', '\xa8'},
    {'\xe5', '\x99', '\xb4'},
    {'\xe5', '\x9b', '\xb9'},
    /* 376 */ {'\xe5', '\x9c', '\x96'},
    {'\xe5', '\x9c', '\x97'},
    {'\xe5', '\x9e', '\x8b'},
    {'\xe5', '\x9f', '\x8e'},
    {'\xe5', '\x9f', '\xb4'},
    {'\xe5', '\xa0', '\x8d'},
    {'\xe5', '\xa0', '\xb1'},
    {'\xe5', '\xa0', '\xb2'},
    /* 384 */ {'\xe5', '\xa1', '\x80'},
    {'\xe5', '\xa1', '\x9a'},
    {'\xe5', '\xa1', '\x9e'},
    {'\xe5', '\xa2', '\xa8'},
    {'\xe5', '\xa2', '\xac'},
    {'\xe5', '\xa2', '\xb3'},
    {'\xe5', '\xa3', '\x98'},
    {'\xe5', '\xa3', '\x9f'},
    /* 392 */ {'\xe5', '\xa3', '\xae'},
    {'\xe5', '\xa3', '\xb2'},
    {'\xe5', '\xa3', '\xb7'},
    {'\xe5', '\xa4', '\x86'},
    {'\xe5', '\xa4', '\x9a'},
    {'\xe5', '\xa4', '\xa2'},
    {'\xe5', '\xa5', '\x84'},
    {'\xe5', '\xa5', '\x88'},
    /* 400 */ {'\xe5', '\xa5', '\x91'},
    {'\xe5', '\xa5', '\x94'},
    {'\xe5', '\xa5', '\xa2'},
    {'\xe5', '\xa5', '\xb3'},
    {'\xe5', '\xa7', '\x98'},
    {'\xe5', '\xa7', '\xac'},
    {'\xe5', '\xa8', '\x9b'},
    {'\xe5', '\xa8', '\xa7'},
    /* 408 */ {'\xe5', '\xa9', '\xa2'},
    {'\xe5', '\xa9', '\xa6'},
    {'\xe5', '\xaa', '\xb5'},
    {'\xe5', '\xac', '\x88'},
    {'\xe5', '\xac', '\xa8'},
    {'\xe5', '\xac', '\xbe'},
    {'\xe5', '\xae', '\x85'},
    {'\xe5', '\xaf', '\x83'},
    /* 416 */ {'\xe5', '\xaf', '\x98'},
    {'\xe5', '\xaf', '\xa7'},
    {'\xe5', '\xaf', '\xae'},
    {'\xe5', '\xaf', '\xb3'},
    {'\xe5', '\xaf', '\xbf'},
    {'\xe5', '\xb0', '\x86'},
    {'\xe5', '\xb0', '\xa2'},
    {'\xe5', '\xb0', '\xbf'},
    /* 424 */ {'\xe5', '\xb1', '\xa0'},
    {'\xe5', '\xb1', '\xa2'},
    {'\xe5', '\xb1', '\xa4'},
    {'\xe5', '\xb1', '\xa5'},
    {'\xe5', '\xb1', '\xae'},
    {'\xe5', '\xb2', '\x8d'},
    {'\xe5', '\xb3', '\x80'},
    {'\xe5', '\xb4', '\x99'},
    /* 432 */ {'\xe5', '\xb5', '\x83'},
    {'\xe5', '\xb5', '\x90'},
    {'\xe5', '\xb5', '\xab'},
    {'\xe5', '\xb5', '\xae'},
    {'\xe5', '\xb5', '\xbc'},
    {'\xe5', '\xb6', '\xb2'},
    {'\xe5', '\xb6', '\xba'},
    {'\xe5', '\xb7', '\xa1'},
    /* 440 */ {'\xe5', '\xb7', '\xa2'},
    {'\xe5', '\xb7', '\xbd'},
    {'\xe5', '\xb8', '\xa8'},
    {'\xe5', '\xb8', '\xbd'},
    {'\xe5', '\xb9', '\xa9'},
    {'\xe5', '\xb9', '\xb4'},
    {'\xe5', '\xba', '\xa6'},
    {'\xe5', '\xba', '\xb0'},
    /* 448 */ {'\xe5', '\xba', '\xb3'},
    {'\xe5', '\xba', '\xb6'},
    {'\xe5', '\xbb', '\x89'},
    {'\xe5', '\xbb', '\x8a'},
    {'\xe5', '\xbb', '\x92'},
    {'\xe5', '\xbb', '\x93'},
    {'\xe5', '\xbb', '\x99'},
    {'\xe5', '\xbb', '\xac'},
    /* 456 */ {'\xe5', '\xbb', '\xbe'},
    {'\xe5', '\xbc', '\x84'},
    {'\xe5', '\xbc', '\xa2'},
    {'\xe5', '\xbd', '\x93'},
    {'\xe5', '\xbd', '\xa2'},
    {'\xe5', '\xbd', '\xa9'},
    {'\xe5', '\xbd', '\xab'},
    {'\xe5', '\xbe', '\x8b'},
    /* 464 */ {'\xe5', '\xbe', '\x9a'},
    {'\xe5', '\xbe', '\xa9'},
    {'\xe5', '\xbe', '\xad'},
    {'\xe5', '\xbf', '\x8d'},
    {'\xe5', '\xbf', '\x97'},
    {'\xe5', '\xbf', '\xb5'},
    {'\xe5', '\xbf', '\xb9'},
    {'\xe6', '\x80', '\x92'},
    /* 472 */ {'\xe6', '\x80', '\x9c'},
    {'\xe6', '\x81', '\xb5'},
    {'\xe6', '\x82', '\x81'},
    {'\xe6', '\x82', '\x94'},
    {'\xe6', '\x83', '\x87'},
    {'\xe6', '\x83', '\x98'},
    {'\xe6', '\x83', '\xa1'},
    {'\xe6', '\x84', '\x88'},
    /* 480 */ {'\xe6', '\x85', '\x84'},
    {'\xe6', '\x85', '\x88'},
    {'\xe6', '\x85', '\x8c'},
    {'\xe6', '\x85', '\x8e'},
    {'\xe6', '\x85', '\xa0'},
    {'\xe6', '\x85', '\xa8'},
    {'\xe6', '\x85', '\xba'},
    {'\xe6', '\x86', '\x8e'},
    /* 488 */ {'\xe6', '\x86', '\x90'},
    {'\xe6', '\x86', '\xa4'},
    {'\xe6', '\x86', '\xaf'},
    {'\xe6', '\x86', '\xb2'},
    {'\xe6', '\x87', '\x9e'},
    {'\xe6', '\x87', '\xb2'},
    {'\xe6', '\x87', '\xb6'},
    {'\xe6', '\x88', '\x80'},
    /* 496 */ {'\xe6', '\x88', '\x90'},
    {'\xe6', '\x88', '\x9b'},
    {'\xe6', '\x88', '\xae'},
    {'\xe6', '\x88', '\xb4'},
    {'\xe6', '\x89', '\x9d'},
    {'\xe6', '\x8a', '\xb1'},
    {'\xe6', '\x8b', '\x89'},
    {'\xe6', '\x8b', '\x8f'},
    /* 504 */ {'\xe6', '\x8b', '\x93'},
    {'\xe6', '\x8b', '\x94'},
    {'\xe6', '\x8b', '\xbc'},
    {'\xe6', '\x8b', '\xbe'},
    {'\xe6', '\x8c', '\xbd'},
    {'\xe6', '\x8d', '\x90'},
    {'\xe6', '\x8d', '\xa8'},
    {'\xe6', '\x8d', '\xbb'},
    /* 512 */ {'\xe6', '\x8e', '\x83'},
    {'\xe6', '\x8e', '\xa0'},
    {'\xe6', '\x8e', '\xa9'},
    {'\xe6', '\x8f', '\x84'},
    {'\xe6', '\x8f', '\x85'},
    {'\xe6', '\x8f', '\xa4'},
    {'\xe6', '\x90', '\x9c'},
    {'\xe6', '\x90', '\xa2'},
    /* 520 */ {'\xe6', '\x91', '\x92'},
    {'\xe6', '\x91', '\xa9'},
    {'\xe6', '\x91', '\xb7'},
    {'\xe6', '\x91', '\xbe'},
    {'\xe6', '\x92', '\x9a'},
    {'\xe6', '\x92', '\x9d'},
    {'\xe6', '\x93', '\x84'},
    {'\xe6', '\x95', '\x8f'},
    /* 528 */ {'\xe6', '\x95', '\x96'},
    {'\xe6', '\x95', '\xac'},
    {'\xe6', '\x95', '\xb8'},
    {'\xe6', '\x96', '\x99'},
    {'\xe6', '\x97', '\x85'},
    {'\xe6', '\x97', '\xa2'},
    {'\xe6', '\x97', '\xa3'},
    {'\xe6', '\x98', '\x93'},
    /* 536 */ {'\xe6', '\x99', '\x89'},
    {'\xe6', '\x99', '\xb4'},
    {'\xe6', '\x9a', '\x88'},
    {'\xe6', '\x9a', '\x91'},
    {'\xe6', '\x9a', '\x9c'},
    {'\xe6', '\x9a', '\xb4'},
    {'\xe6', '\x9b', '\x86'},
    {'\xe6', '\x9b', '\xb4'},
    /* 544 */ {'\xe6', '\x9b', '\xb8'},
    {'\xe6', '\x9c', '\x80'},
    {'\xe6', '\x9c', '\x97'},
    {'\xe6', '\x9c', '\x9b'},
    {'\xe6', '\x9c', '\xa1'},
    {'\xe6', '\x9d', '\x8e'},
    {'\xe6', '\x9d', '\x93'},
    {'\xe6', '\x9d', '\x96'},
    /* 552 */ {'\xe6', '\x9d', '\x9e'},
    {'\xe6', '\x9d', '\xbb'},
    {'\xe6', '\x9e', '\x85'},
    {'\xe6', '\x9e', '\x97'},
    {'\xe6', '\x9f', '\xb3'},
    {'\xe6', '\x9f', '\xba'},
    {'\xe6', '\xa0', '\x97'},
    {'\xe6', '\xa0', '\x9f'},
    /* 560 */ {'\xe6', '\xa1', '\x92'},
    {'\xe6', '\xa2', '\x81'},
    {'\xe6', '\xa2', '\x85'},
    {'\xe6', '\xa2', '\x8e'},
    {'\xe6', '\xa2', '\xa8'},
    {'\xe6', '\xa4', '\x94'},
    {'\xe6', '\xa5', '\x82'},
    {'\xe6', '\xa6', '\xa3'},
    /* 568 */ {'\xe6', '\xa7', '\xaa'},
    {'\xe6', '\xa8', '\x82'},
    {'\xe6', '\xa8', '\x93'},
    {'\xe6', '\xaa', '\xa8'},
    {'\xe6', '\xab', '\x93'},
    {'\xe6', '\xab', '\x9b'},
    {'\xe6', '\xac', '\x84'},
    {'\xe6', '\xac', '\xa1'},
    /* 576 */ {'\xe6', '\xad', '\x94'},
    {'\xe6', '\xad', '\xb2'},
    {'\xe6', '\xad', '\xb7'},
    {'\xe6', '\xad', '\xb9'},
    {'\xe6', '\xae', '\x9f'},
    {'\xe6', '\xae', '\xae'},
    {'\xe6', '\xae', '\xba'},
    {'\xe6', '\xae', '\xbb'},
    /* 584 */ {'\xe6', '\xb1', '\x8e'},
    {'\xe6', '\xb1', '\xa7'},
    {'\xe6', '\xb2', '\x88'},
    {'\xe6', '\xb2', '\xbf'},
    {'\xe6', '\xb3', '\x8c'},
    {'\xe6', '\xb3', '\x8d'},
    {'\xe6', '\xb3', '\xa5'},
    {'\xe6', '\xb4', '\x96'},
    /* 592 */ {'\xe6', '\xb4', '\x9b'},
    {'\xe6', '\xb4', '\x9e'},
    {'\xe6', '\xb4', '\xb4'},
    {'\xe6', '\xb4', '\xbe'},
    {'\xe6', '\xb5', '\x81'},
    {'\xe6', '\xb5', '\xa9'},
    {'\xe6', '\xb5', '\xaa'},
    {'\xe6', '\xb5', '\xb7'},
    /* 600 */ {'\xe6', '\xb5', '\xb8'},
    {'\xe6', '\xb6', '\x85'},
    {'\xe6', '\xb7', '\x8b'},
    {'\xe6', '\xb7', '\x9a'},
    {'\xe6', '\xb7', '\xaa'},
    {'\xe6', '\xb7', '\xb9'},
    {'\xe6', '\xb8', '\x9a'},
    {'\xe6', '\xb8', '\xaf'},
    /* 608 */ {'\xe6', '\xb9', '\xae'},
    {'\xe6', '\xba', '\x9c'},
    {'\xe6', '\xba', '\xba'},
    {'\xe6', '\xbb', '\x87'},
    {'\xe6', '\xbb', '\x8b'},
    {'\xe6', '\xbb', '\x91'},
    {'\xe6', '\xbb', '\x9b'},
    {'\xe6', '\xbc', '\x8f'},
    /* 616 */ {'\xe6', '\xbc', '\xa2'},
    {'\xe6', '\xbc', '\xa3'},
    {'\xe6', '\xbd', '\xae'},
    {'\xe6', '\xbf', '\x86'},
    {'\xe6', '\xbf', '\xab'},
    {'\xe6', '\xbf', '\xbe'},
    {'\xe7', '\x80', '\x9b'},
    {'\xe7', '\x80', '\x9e'},
    /* 624 */ {'\xe7', '\x80', '\xb9'},
    {'\xe7', '\x81', '\x8a'},
    {'\xe7', '\x81', '\xb0'},
    {'\xe7', '\x81', '\xb7'},
    {'\xe7', '\x81', '\xbd'},
    {'\xe7', '\x82', '\x99'},
    {'\xe7', '\x82', '\xad'},
    {'\xe7', '\x83', '\x88'},
    /* 632 */ {'\xe7', '\x83', '\x99'},
    {'\xe7', '\x85', '\x85'},
    {'\xe7', '\x85', '\x89'},
    {'\xe7', '\x85', '\xae'},
    {'\xe7', '\x86', '\x9c'},
    {'\xe7', '\x87', '\x8e'},
    {'\xe7', '\x87', '\x90'},
    {'\xe7', '\x88', '\x90'},
    /* 640 */ {'\xe7', '\x88', '\x9b'},
    {'\xe7', '\x88', '\xa8'},
    {'\xe7', '\x88', '\xab'},
    {'\xe7', '\x88', '\xb5'},
    {'\xe7', '\x89', '\x90'},
    {'\xe7', '\x89', '\xa2'},
    {'\xe7', '\x8a', '\x80'},
    {'\xe7', '\x8a', '\x95'},
    /* 648 */ {'\xe7', '\x8a', '\xaf'},
    {'\xe7', '\x8b', '\x80'},
    {'\xe7', '\x8b', '\xbc'},
    {'\xe7', '\x8c', '\xaa'},
    {'\xe7', '\x8d', '\xb5'},
    {'\xe7', '\x8d', '\xba'},
    {'\xe7', '\x8e', '\x87'},
    {'\xe7', '\x8e', '\x8b'},
    /* 656 */ {'\xe7', '\x8e', '\xa5'},
    {'\xe7', '\x8e', '\xb2'},
    {'\xe7', '\x8f', '\x9e'},
    {'\xe7', '\x90', '\x86'},
    {'\xe7', '\x90', '\x89'},
    {'\xe7', '\x90', '\xa2'},
    {'\xe7', '\x91', '\x87'},
    {'\xe7', '\x91', '\x9c'},
    /* 664 */ {'\xe7', '\x91', '\xa9'},
    {'\xe7', '\x91', '\xb1'},
    {'\xe7', '\x92', '\x85'},
    {'\xe7', '\x92', '\x89'},
    {'\xe7', '\x92', '\x98'},
    {'\xe7', '\x93', '\x8a'},
    {'\xe7', '\x94', '\x86'},
    {'\xe7', '\x94', '\xa4'},
    /* 672 */ {'\xe7', '\x94', '\xbb'},
    {'\xe7', '\x94', '\xbe'},
    {'\xe7', '\x95', '\x99'},
    {'\xe7', '\x95', '\xa5'},
    {'\xe7', '\x95', '\xb0'},
    {'\xe7', '\x97', '\xa2'},
    {'\xe7', '\x98', '\x90'},
    {'\xe7', '\x98', '\x9d'},
    /* 680 */ {'\xe7', '\x98', '\x9f'},
    {'\xe7', '\x99', '\x82'},
    {'\xe7', '\x99', '\xa9'},
    {'\xe7', '\x9b', '\x8a'},
    {'\xe7', '\x9b', '\x9b'},
    {'\xe7', '\x9b', '\xa7'},
    {'\xe7', '\x9b', '\xb4'},
    {'\xe7', '\x9c', '\x81'},
    /* 688 */ {'\xe7', '\x9c', '\x9e'},
    {'\xe7', '\x9c', '\x9f'},
    {'\xe7', '\x9d', '\x80'},
    {'\xe7', '\x9d', '\x8a'},
    {'\xe7', '\x9e', '\x8b'},
    {'\xe7', '\x9e', '\xa7'},
    {'\xe7', '\xa1', '\x8e'},
    {'\xe7', '\xa1', '\xab'},
    /* 696 */ {'\xe7', '\xa2', '\x8c'},
    {'\xe7', '\xa2', '\x91'},
    {'\xe7', '\xa3', '\x8a'},
    {'\xe7', '\xa3', '\x8c'},
    {'\xe7', '\xa3', '\xbb'},
    {'\xe7', '\xa4', '\xaa'},
    {'\xe7', '\xa4', '\xbc'},
    {'\xe7', '\xa4', '\xbe'},
    /* 704 */ {'\xe7', '\xa5', '\x88'},
    {'\xe7', '\xa5', '\x89'},
    {'\xe7', '\xa5', '\x90'},
    {'\xe7', '\xa5', '\x96'},
    {'\xe7', '\xa5', '\x9d'},
    {'\xe7', '\xa5', '\x9e'},
    {'\xe7', '\xa5', '\xa5'},
    {'\xe7', '\xa5', '\xbf'},
    /* 712 */ {'\xe7', '\xa6', '\x8d'},
    {'\xe7', '\xa6', '\x8e'},
    {'\xe7', '\xa6', '\x8f'},
    {'\xe7', '\xa6', '\xae'},
    {'\xe7', '\xa7', '\x8a'},
    {'\xe7', '\xa7', '\xab'},
    {'\xe7', '\xa8', '\x9c'},
    {'\xe7', '\xa9', '\x80'},
    /* 720 */ {'\xe7', '\xa9', '\x8a'},
    {'\xe7', '\xa9', '\x8f'},
    {'\xe7', '\xaa', '\x81'},
    {'\xe7', '\xaa', '\xb1'},
    {'\xe7', '\xab', '\x8b'},
    {'\xe7', '\xab', '\xae'},
    {'\xe7', '\xac', '\xa0'},
    {'\xe7', '\xaf', '\x80'},
    /* 728 */ {'\xe7', '\xaf', '\x86'},
    {'\xe7', '\xaf', '\x89'},
    {'\xe7', '\xb0', '\xbe'},
    {'\xe7', '\xb1', '\xa0'},
    {'\xe7', '\xb1', '\xbb'},
    {'\xe7', '\xb2', '\x92'},
    {'\xe7', '\xb2', '\xbe'},
    {'\xe7', '\xb3', '\x92'},
    /* 736 */ {'\xe7', '\xb3', '\x96'},
    {'\xe7', '\xb3', '\xa3'},
    {'\xe7', '\xb3', '\xa7'},
    {'\xe7', '\xb3', '\xa8'},
    {'\xe7', '\xb4', '\x80'},
    {'\xe7', '\xb4', '\x90'},
    {'\xe7', '\xb4', '\xa2'},
    {'\xe7', '\xb4', '\xaf'},
    /* 744 */ {'\xe7', '\xb5', '\x9b'},
    {'\xe7', '\xb5', '\xa3'},
    {'\xe7', '\xb6', '\xa0'},
    {'\xe7', '\xb6', '\xbe'},
    {'\xe7', '\xb7', '\x87'},
    {'\xe7', '\xb7', '\xb4'},
    {'\xe7', '\xb8', '\x82'},
    {'\xe7', '\xb8', '\x89'},
    /* 752 */ {'\xe7', '\xb8', '\xb7'},
    {'\xe7', '\xb9', '\x81'},
    {'\xe7', '\xb9', '\x85'},
    {'\xe7', '\xbc', '\xbe'},
    {'\xe7', '\xbd', '\xb2'},
    {'\xe7', '\xbd', '\xb9'},
    {'\xe7', '\xbd', '\xba'},
    {'\xe7', '\xbe', '\x85'},
    /* 760 */ {'\xe7', '\xbe', '\x95'},
    {'\xe7', '\xbe', '\x9a'},
    {'\xe7', '\xbe', '\xbd'},
    {'\xe7', '\xbf', '\xba'},
    {'\xe8', '\x80', '\x81'},
    {'\xe8', '\x80', '\x85'},
    {'\xe8', '\x81', '\x86'},
    {'\xe8', '\x81', '\xa0'},
    /* 768 */ {'\xe8', '\x81', '\xaf'},
    {'\xe8', '\x81', '\xb0'},
    {'\xe8', '\x81', '\xbe'},
    {'\xe8', '\x82', '\x8b'},
    {'\xe8', '\x82', '\xad'},
    {'\xe8', '\x82', '\xb2'},
    {'\xe8', '\x84', '\x83'},
    {'\xe8', '\x84', '\xbe'},
    /* 776 */ {'\xe8', '\x87', '\x98'},
    {'\xe8', '\x87', '\xa8'},
    {'\xe8', '\x87', '\xad'},
    {'\xe8', '\x88', '\x81'},
    {'\xe8', '\x88', '\x84'},
    {'\xe8', '\x88', '\x98'},
    {'\xe8', '\x89', '\xaf'},
    {'\xe8', '\x89', '\xb9'},
    /* 784 */ {'\xe8', '\x8a', '\x8b'},
    {'\xe8', '\x8a', '\x91'},
    {'\xe8', '\x8a', '\x9d'},
    {'\xe8', '\x8a', '\xb1'},
    {'\xe8', '\x8a', '\xb3'},
    {'\xe8', '\x8a', '\xbd'},
    {'\xe8', '\x8b', '\xa5'},
    {'\xe8', '\x8b', '\xa6'},
    /* 792 */ {'\xe8', '\x8c', '\x9d'},
    {'\xe8', '\x8c', '\xa3'},
    {'\xe8', '\x8c', '\xb6'},
    {'\xe8', '\x8d', '\x92'},
    {'\xe8', '\x8d', '\x93'},
    {'\xe8', '\x8d', '\xa3'},
    {'\xe8', '\x8e', '\xad'},
    {'\xe8', '\x8e', '\xbd'},
    /* 800 */ {'\xe8', '\x8f', '\x89'},
    {'\xe8', '\x8f', '\x8a'},
    {'\xe8', '\x8f', '\x8c'},
    {'\xe8', '\x8f', '\x9c'},
    {'\xe8', '\x8f', '\xa7'},
    {'\xe8', '\x8f', '\xaf'},
    {'\xe8', '\x8f', '\xb1'},
    {'\xe8', '\x90', '\xbd'},
    /* 808 */ {'\xe8', '\x91', '\x89'},
    {'\xe8', '\x91', '\x97'},
    {'\xe8', '\x93', '\xae'},
    {'\xe8', '\x93', '\xb1'},
    {'\xe8', '\x93', '\xb3'},
    {'\xe8', '\x93', '\xbc'},
    {'\xe8', '\x94', '\x96'},
    {'\xe8', '\x95', '\xa4'},
    /* 816 */ {'\xe8', '\x97', '\x8d'},
    {'\xe8', '\x97', '\xba'},
    {'\xe8', '\x98', '\x86'},
    {'\xe8', '\x98', '\x92'},
    {'\xe8', '\x98', '\xad'},
    {'\xe8', '\x98', '\xbf'},
    {'\xe8', '\x99', '\x90'},
    {'\xe8', '\x99', '\x9c'},
    /* 824 */ {'\xe8', '\x99', '\xa7'},
    {'\xe8', '\x99', '\xa9'},
    {'\xe8', '\x9a', '\x88'},
    {'\xe8', '\x9a', '\xa9'},
    {'\xe8', '\x9b', '\xa2'},
    {'\xe8', '\x9c', '\x8e'},
    {'\xe8', '\x9c', '\xa8'},
    {'\xe8', '\x9d', '\xab'},
    /* 832 */ {'\xe8', '\x9d', '\xb9'},
    {'\xe8', '\x9e', '\x86'},
    {'\xe8', '\x9e', '\xba'},
    {'\xe8', '\x9f', '\xa1'},
    {'\xe8', '\xa0', '\x81'},
    {'\xe8', '\xa0', '\x9f'},
    {'\xe8', '\xa1', '\x8c'},
    {'\xe8', '\xa1', '\xa0'},
    /* 840 */ {'\xe8', '\xa1', '\xa3'},
    {'\xe8', '\xa3', '\x82'},
    {'\xe8', '\xa3', '\x8f'},
    {'\xe8', '\xa3', '\x97'},
    {'\xe8', '\xa3', '\x9e'},
    {'\xe8', '\xa3', '\xa1'},
    {'\xe8', '\xa3', '\xb8'},
    {'\xe8', '\xa3', '\xba'},
    /* 848 */ {'\xe8', '\xa4', '\x90'},
    {'\xe8', '\xa5', '\x81'},
    {'\xe8', '\xa5', '\xa4'},
    {'\xe8', '\xa6', '\x86'},
    {'\xe8', '\xa6', '\x8b'},
    {'\xe8', '\xa6', '\x96'},
    {'\xe8', '\xaa', '\xa0'},
    {'\xe8', '\xaa', '\xaa'},
    /* 856 */ {'\xe8', '\xaa', '\xbf'},
    {'\xe8', '\xab', '\x8b'},
    {'\xe8', '\xab', '\x92'},
    {'\xe8', '\xab', '\x96'},
    {'\xe8', '\xab', '\xad'},
    {'\xe8', '\xab', '\xb8'},
    {'\xe8', '\xab', '\xbe'},
    {'\xe8', '\xac', '\x81'},
    /* 864 */ {'\xe8', '\xac', '\xb9'},
    {'\xe8', '\xad', '\x98'},
    {'\xe8', '\xae', '\x80'},
    {'\xe8', '\xae', '\x8a'},
    {'\xe8', '\xb1', '\x88'},
    {'\xe8', '\xb1', '\x95'},
    {'\xe8', '\xb2', '\xab'},
    {'\xe8', '\xb3', '\x81'},
    /* 872 */ {'\xe8', '\xb3', '\x82'},
    {'\xe8', '\xb3', '\x88'},
    {'\xe8', '\xb3', '\x93'},
    {'\xe8', '\xb4', '\x88'},
    {'\xe8', '\xb4', '\x9b'},
    {'\xe8', '\xb5', '\xb7'},
    {'\xe8', '\xb6', '\xbc'},
    {'\xe8', '\xb7', '\x8b'},
    /* 880 */ {'\xe8', '\xb7', '\xaf'},
    {'\xe8', '\xb7', '\xb0'},
    {'\xe8', '\xbb', '\x8a'},
    {'\xe8', '\xbb', '\x94'},
    {'\xe8', '\xbc', '\xa6'},
    {'\xe8', '\xbc', '\xaa'},
    {'\xe8', '\xbc', '\xb8'},
    {'\xe8', '\xbc', '\xbb'},
    /* 888 */ {'\xe8', '\xbd', '\xa2'},
    {'\xe8', '\xbe', '\x9e'},
    {'\xe8', '\xbe', '\xb0'},
    {'\xe8', '\xbe', '\xb6'},
    {'\xe9', '\x80', '\xa3'},
    {'\xe9', '\x80', '\xb8'},
    {'\xe9', '\x81', '\xb2'},
    {'\xe9', '\x81', '\xbc'},
    /* 896 */ {'\xe9', '\x82', '\x8f'},
    {'\xe9', '\x82', '\x94'},
    {'\xe9', '\x83', '\x8e'},
    {'\xe9', '\x83', '\x9e'},
    {'\xe9', '\x83', '\xb1'},
    {'\xe9', '\x83', '\xbd'},
    {'\xe9', '\x84', '\x91'},
    {'\xe9', '\x84', '\x9b'},
    /* 904 */ {'\xe9', '\x85', '\xaa'},
    {'\xe9', '\x86', '\x99'},
    {'\xe9', '\x86', '\xb4'},
    {'\xe9', '\x87', '\x8c'},
    {'\xe9', '\x87', '\x8f'},
    {'\xe9', '\x87', '\x91'},
    {'\xe9', '\x88', '\xb4'},
    {'\xe9', '\x88', '\xb8'},
    /* 912 */ {'\xe9', '\x89', '\xb6'},
    {'\xe9', '\x89', '\xbc'},
    {'\xe9', '\x8b', '\x97'},
    {'\xe9', '\x8b', '\x98'},
    {'\xe9', '\x8c', '\x84'},
    {'\xe9', '\x8d', '\x8a'},
    {'\xe9', '\x8f', '\xb9'},
    {'\xe9', '\x90', '\x95'},
    /* 920 */ {'\xe9', '\x96', '\x8b'},
    {'\xe9', '\x96', '\xad'},
    {'\xe9', '\x96', '\xb7'},
    {'\xe9', '\x98', '\xae'},
    {'\xe9', '\x99', '\x8b'},
    {'\xe9', '\x99', '\x8d'},
    {'\xe9', '\x99', '\xb5'},
    {'\xe9', '\x99', '\xb8'},
    /* 928 */ {'\xe9', '\x99', '\xbc'},
    {'\xe9', '\x9a', '\x86'},
    {'\xe9', '\x9a', '\xa3'},
    {'\xe9', '\x9a', '\xb7'},
    {'\xe9', '\x9a', '\xb8'},
    {'\xe9', '\x9b', '\x83'},
    {'\xe9', '\x9b', '\xa2'},
    {'\xe9', '\x9b', '\xa3'},
    /* 936 */ {'\xe9', '\x9b', '\xb6'},
    {'\xe9', '\x9b', '\xb7'},
    {'\xe9', '\x9c', '\xa3'},
    {'\xe9', '\x9c', '\xb2'},
    {'\xe9', '\x9d', '\x88'},
    {'\xe9', '\x9d', '\x96'},
    {'\xe9', '\x9f', '\x9b'},
    {'\xe9', '\x9f', '\xa0'},
    /* 944 */ {'\xe9', '\x9f', '\xbf'},
    {'\xe9', '\xa0', '\x8b'},
    {'\xe9', '\xa0', '\x98'},
    {'\xe9', '\xa0', '\xa9'},
    {'\xe9', '\xa0', '\xbb'},
    {'\xe9', '\xa1', '\x9e'},
    {'\xe9', '\xa3', '\xa2'},
    {'\xe9', '\xa3', '\xaf'},
    /* 952 */ {'\xe9', '\xa3', '\xbc'},
    {'\xe9', '\xa4', '\xa8'},
    {'\xe9', '\xa4', '\xa9'},
    {'\xe9', '\xa6', '\xa7'},
    {'\xe9', '\xa7', '\x82'},
    {'\xe9', '\xa7', '\xb1'},
    {'\xe9', '\xa7', '\xbe'},
    {'\xe9', '\xa9', '\xaa'},
    /* 960 */ {'\xe9', '\xac', '\x92'},
    {'\xe9', '\xad', '\xaf'},
    {'\xe9', '\xb1', '\x80'},
    {'\xe9', '\xb1', '\x97'},
    {'\xe9', '\xb3', '\xbd'},
    {'\xe9', '\xb5', '\xa7'},
    {'\xe9', '\xb6', '\xb4'},
    {'\xe9', '\xb7', '\xba'},
    /* 968 */ {'\xe9', '\xb8', '\x9e'},
    {'\xe9', '\xb9', '\xbf'},
    {'\xe9', '\xba', '\x97'},
    {'\xe9', '\xba', '\x9f'},
    {'\xe9', '\xba', '\xbb'},
    {'\xe9', '\xbb', '\x8e'},
    {'\xe9', '\xbb', '\xb9'},
    {'\xe9', '\xbb', '\xbe'},
    /* 976 */ {'\xe9', '\xbc', '\x85'},
    {'\xe9', '\xbc', '\x8f'},
    {'\xe9', '\xbc', '\x96'},
    {'\xe9', '\xbc', '\xbb'},
    {'\xe9', '\xbd', '\x83'},
    {'\xe9', '\xbe', '\x8d'},
    {'\xe9', '\xbe', '\x8e'},
    {'\xe9', '\xbe', '\x9c'},
    /* 984 */ {'a', '\xcc', '\x80'},
    {'a', '\xcc', '\x81'},
    {'a', '\xcc', '\x82'},
    {'a', '\xcc', '\x83'},
    {'a', '\xcc', '\x84'},
    {'a', '\xcc', '\x86'},
    {'a', '\xcc', '\x87'},
    {'a', '\xcc', '\x88'},
    /* 992 */ {'a', '\xcc', '\x89'},
    {'a', '\xcc', '\x8a'},
    {'a', '\xcc', '\x8c'},
    {'a', '\xcc', '\x8f'},
    {'a', '\xcc', '\x91'},
    {'a', '\xcc', '\xa3'},
    {'a', '\xcc', '\xa5'},
    {'a', '\xcc', '\xa8'},
    /* 1000 */ {'b', '\xcc', '\x87'},
    {'b', '\xcc', '\xa3'},
    {'b', '\xcc', '\xb1'},
    {'c', '\xcc', '\x81'},
    {'c', '\xcc', '\x82'},
    {'c', '\xcc', '\x87'},
    {'c', '\xcc', '\x8c'},
    {'c', '\xcc', '\xa7'},
    /* 1008 */ {'d', '\xcc', '\x87'},
    {'d', '\xcc', '\x8c'},
    {'d', '\xcc', '\xa3'},
    {'d', '\xcc', '\xa7'},
    {'d', '\xcc', '\xad'},
    {'d', '\xcc', '\xb1'},
    {'e', '\xcc', '\x80'},
    {'e', '\xcc', '\x81'},
    /* 1016 */ {'e', '\xcc', '\x82'},
    {'e', '\xcc', '\x83'},
    {'e', '\xcc', '\x84'},
    {'e', '\xcc', '\x86'},
    {'e', '\xcc', '\x87'},
    {'e', '\xcc', '\x88'},
    {'e', '\xcc', '\x89'},
    {'e', '\xcc', '\x8c'},
    /* 1024 */ {'e', '\xcc', '\x8f'},
    {'e', '\xcc', '\x91'},
    {'e', '\xcc', '\xa3'},
    {'e', '\xcc', '\xa7'},
    {'e', '\xcc', '\xa8'},
    {'e', '\xcc', '\xad'},
    {'e', '\xcc', '\xb0'},
    {'f', '\xcc', '\x87'},
    /* 1032 */ {'g', '\xcc', '\x81'},
    {'g', '\xcc', '\x82'},
    {'g', '\xcc', '\x84'},
    {'g', '\xcc', '\x86'},
    {'g', '\xcc', '\x87'},
    {'g', '\xcc', '\x8c'},
    {'g', '\xcc', '\xa7'},
    {'h', '\xcc', '\x82'},
    /* 1040 */ {'h', '\xcc', '\x87'},
    {'h', '\xcc', '\x88'},
    {'h', '\xcc', '\x8c'},
    {'h', '\xcc', '\xa3'},
    {'h', '\xcc', '\xa7'},
    {'h', '\xcc', '\xae'},
    {'h', '\xcc', '\xb1'},
    {'i', '\xcc', '\x80'},
    /* 1048 */ {'i', '\xcc', '\x81'},
    {'i', '\xcc', '\x82'},
    {'i', '\xcc', '\x83'},
    {'i', '\xcc', '\x84'},
    {'i', '\xcc', '\x86'},
    {'i', '\xcc', '\x88'},
    {'i', '\xcc', '\x89'},
    {'i', '\xcc', '\x8c'},
    /* 1056 */ {'i', '\xcc', '\x8f'},
    {'i', '\xcc', '\x91'},
    {'i', '\xcc', '\xa3'},
    {'i', '\xcc', '\xa8'},
    {'i', '\xcc', '\xb0'},
    {'j', '\xcc', '\x82'},
    {'j', '\xcc', '\x8c'},
    {'k', '\xcc', '\x81'},
    /* 1064 */ {'k', '\xcc', '\x8c'},
    {'k', '\xcc', '\xa3'},
    {'k', '\xcc', '\xa7'},
    {'k', '\xcc', '\xb1'},
    {'l', '\xcc', '\x81'},
    {'l', '\xcc', '\x8c'},
    {'l', '\xcc', '\xa3'},
    {'l', '\xcc', '\xa7'},
    /* 1072 */ {'l', '\xcc', '\xad'},
    {'l', '\xcc', '\xb1'},
    {'m', '\xcc', '\x81'},
    {'m', '\xcc', '\x87'},
    {'m', '\xcc', '\xa3'},
    {'n', '\xcc', '\x80'},
    {'n', '\xcc', '\x81'},
    {'n', '\xcc', '\x83'},
    /* 1080 */ {'n', '\xcc', '\x87'},
    {'n', '\xcc', '\x8c'},
    {'n', '\xcc', '\xa3'},
    {'n', '\xcc', '\xa7'},
    {'n', '\xcc', '\xad'},
    {'n', '\xcc', '\xb1'},
    {'o', '\xcc', '\x80'},
    {'o', '\xcc', '\x81'},
    /* 1088 */ {'o', '\xcc', '\x82'},
    {'o', '\xcc', '\x83'},
    {'o', '\xcc', '\x84'},
    {'o', '\xcc', '\x86'},
    {'o', '\xcc', '\x87'},
    {'o', '\xcc', '\x88'},
    {'o', '\xcc', '\x89'},
    {'o', '\xcc', '\x8b'},
    /* 1096 */ {'o', '\xcc', '\x8c'},
    {'o', '\xcc', '\x8f'},
    {'o', '\xcc', '\x91'},
    {'o', '\xcc', '\x9b'},
    {'o', '\xcc', '\xa3'},
    {'o', '\xcc', '\xa8'},
    {'p', '\xcc', '\x81'},
    {'p', '\xcc', '\x87'},
    /* 1104 */ {'r', '\xcc', '\x81'},
    {'r', '\xcc', '\x87'},
    {'r', '\xcc', '\x8c'},
    {'r', '\xcc', '\x8f'},
    {'r', '\xcc', '\x91'},
    {'r', '\xcc', '\xa3'},
    {'r', '\xcc', '\xa7'},
    {'r', '\xcc', '\xb1'},
    /* 1112 */ {'s', '\xcc', '\x81'},
    {'s', '\xcc', '\x82'},
    {'s', '\xcc', '\x87'},
    {'s', '\xcc', '\x8c'},
    {'s', '\xcc', '\xa3'},
    {'s', '\xcc', '\xa6'},
    {'s', '\xcc', '\xa7'},
    {'t', '\xcc', '\x87'},
    /* 1120 */ {'t', '\xcc', '\x88'},
    {'t', '\xcc', '\x8c'},
    {'t', '\xcc', '\xa3'},
    {'t', '\xcc', '\xa6'},
    {'t', '\xcc', '\xa7'},
    {'t', '\xcc', '\xad'},
    {'t', '\xcc', '\xb1'},
    {'u', '\xcc', '\x80'},
    /* 1128 */ {'u', '\xcc', '\x81'},
    {'u', '\xcc', '\x82'},
    {'u', '\xcc', '\x83'},
    {'u', '\xcc', '\x84'},
    {'u', '\xcc', '\x86'},
    {'u', '\xcc', '\x88'},
    {'u', '\xcc', '\x89'},
    {'u', '\xcc', '\x8a'},
    /* 1136 */ {'u', '\xcc', '\x8b'},
    {'u', '\xcc', '\x8c'},
    {'u', '\xcc', '\x8f'},
    {'u', '\xcc', '\x91'},
    {'u', '\xcc', '\x9b'},
    {'u', '\xcc', '\xa3'},
    {'u', '\xcc', '\xa4'},
    {'u', '\xcc', '\xa8'},
    /* 1144 */ {'u', '\xcc', '\xad'},
    {'u', '\xcc', '\xb0'},
    {'v', '\xcc', '\x83'},
    {'v', '\xcc', '\xa3'},
    {'w', '\xcc', '\x80'},
    {'w', '\xcc', '\x81'},
    {'w', '\xcc', '\x82'},
    {'w', '\xcc', '\x87'},
    /* 1152 */ {'w', '\xcc', '\x88'},
    {'w', '\xcc', '\x8a'},
    {'w', '\xcc', '\xa3'},
    {'x', '\xcc', '\x87'},
    {'x', '\xcc', '\x88'},
    {'y', '\xcc', '\x80'},
    {'y', '\xcc', '\x81'},
    {'y', '\xcc', '\x82'},
    /* 1160 */ {'y', '\xcc', '\x83'},
    {'y', '\xcc', '\x84'},
    {'y', '\xcc', '\x87'},
    {'y', '\xcc', '\x88'},
    {'y', '\xcc', '\x89'},
    {'y', '\xcc', '\x8a'},
    {'y', '\xcc', '\xa3'},
    {'z', '\xcc', '\x81'},
    /* 1168 */ {'z', '\xcc', '\x82'},
    {'z', '\xcc', '\x87'},
    {'z', '\xcc', '\x8c'},
    {'z', '\xcc', '\xa3'},
    {'z', '\xcc', '\xb1'}};

static const char UN8IF_canon_tbl_4[302][4] = {
    /*   0 */ {'\xc2', '\xa8', '\xcc', '\x80'}, {'\xc2', '\xa8', '\xcc', '\x81'}, {'\xc2', '\xa8', '\xcd', '\x82'}, {'\xc3', '\x86', '\xcc', '\x81'}, {'\xc3', '\x86', '\xcc', '\x84'}, {'\xc3', '\x98', '\xcc', '\x81'}, {'\xc3', '\xa6', '\xcc', '\x81'}, {'\xc3', '\xa6', '\xcc', '\x84'},
    /*   8 */ {'\xc3', '\xb8', '\xcc', '\x81'},
    {'\xc5', '\xbf', '\xcc', '\x87'},
    {'\xc6', '\xb7', '\xcc', '\x8c'},
    {'\xca', '\x92', '\xcc', '\x8c'},
    {'\xcc', '\x88', '\xcc', '\x81'},
    {'\xce', '\x91', '\xcc', '\x80'},
    {'\xce', '\x91', '\xcc', '\x81'},
    {'\xce', '\x91', '\xcc', '\x84'},
    /*  16 */ {'\xce', '\x91', '\xcc', '\x86'},
    {'\xce', '\x91', '\xcc', '\x93'},
    {'\xce', '\x91', '\xcc', '\x94'},
    {'\xce', '\x91', '\xcd', '\x85'},
    {'\xce', '\x95', '\xcc', '\x80'},
    {'\xce', '\x95', '\xcc', '\x81'},
    {'\xce', '\x95', '\xcc', '\x93'},
    {'\xce', '\x95', '\xcc', '\x94'},
    /*  24 */ {'\xce', '\x97', '\xcc', '\x80'},
    {'\xce', '\x97', '\xcc', '\x81'},
    {'\xce', '\x97', '\xcc', '\x93'},
    {'\xce', '\x97', '\xcc', '\x94'},
    {'\xce', '\x97', '\xcd', '\x85'},
    {'\xce', '\x99', '\xcc', '\x80'},
    {'\xce', '\x99', '\xcc', '\x81'},
    {'\xce', '\x99', '\xcc', '\x84'},
    /*  32 */ {'\xce', '\x99', '\xcc', '\x86'},
    {'\xce', '\x99', '\xcc', '\x88'},
    {'\xce', '\x99', '\xcc', '\x93'},
    {'\xce', '\x99', '\xcc', '\x94'},
    {'\xce', '\x9f', '\xcc', '\x80'},
    {'\xce', '\x9f', '\xcc', '\x81'},
    {'\xce', '\x9f', '\xcc', '\x93'},
    {'\xce', '\x9f', '\xcc', '\x94'},
    /*  40 */ {'\xce', '\xa1', '\xcc', '\x94'},
    {'\xce', '\xa5', '\xcc', '\x80'},
    {'\xce', '\xa5', '\xcc', '\x81'},
    {'\xce', '\xa5', '\xcc', '\x84'},
    {'\xce', '\xa5', '\xcc', '\x86'},
    {'\xce', '\xa5', '\xcc', '\x88'},
    {'\xce', '\xa5', '\xcc', '\x94'},
    {'\xce', '\xa9', '\xcc', '\x80'},
    /*  48 */ {'\xce', '\xa9', '\xcc', '\x81'},
    {'\xce', '\xa9', '\xcc', '\x93'},
    {'\xce', '\xa9', '\xcc', '\x94'},
    {'\xce', '\xa9', '\xcd', '\x85'},
    {'\xce', '\xb1', '\xcc', '\x80'},
    {'\xce', '\xb1', '\xcc', '\x81'},
    {'\xce', '\xb1', '\xcc', '\x84'},
    {'\xce', '\xb1', '\xcc', '\x86'},
    /*  56 */ {'\xce', '\xb1', '\xcc', '\x93'},
    {'\xce', '\xb1', '\xcc', '\x94'},
    {'\xce', '\xb1', '\xcd', '\x82'},
    {'\xce', '\xb1', '\xcd', '\x85'},
    {'\xce', '\xb5', '\xcc', '\x80'},
    {'\xce', '\xb5', '\xcc', '\x81'},
    {'\xce', '\xb5', '\xcc', '\x93'},
    {'\xce', '\xb5', '\xcc', '\x94'},
    /*  64 */ {'\xce', '\xb7', '\xcc', '\x80'},
    {'\xce', '\xb7', '\xcc', '\x81'},
    {'\xce', '\xb7', '\xcc', '\x93'},
    {'\xce', '\xb7', '\xcc', '\x94'},
    {'\xce', '\xb7', '\xcd', '\x82'},
    {'\xce', '\xb7', '\xcd', '\x85'},
    {'\xce', '\xb9', '\xcc', '\x80'},
    {'\xce', '\xb9', '\xcc', '\x81'},
    /*  72 */ {'\xce', '\xb9', '\xcc', '\x84'},
    {'\xce', '\xb9', '\xcc', '\x86'},
    {'\xce', '\xb9', '\xcc', '\x88'},
    {'\xce', '\xb9', '\xcc', '\x93'},
    {'\xce', '\xb9', '\xcc', '\x94'},
    {'\xce', '\xb9', '\xcd', '\x82'},
    {'\xce', '\xbf', '\xcc', '\x80'},
    {'\xce', '\xbf', '\xcc', '\x81'},
    /*  80 */ {'\xce', '\xbf', '\xcc', '\x93'},
    {'\xce', '\xbf', '\xcc', '\x94'},
    {'\xcf', '\x81', '\xcc', '\x93'},
    {'\xcf', '\x81', '\xcc', '\x94'},
    {'\xcf', '\x85', '\xcc', '\x80'},
    {'\xcf', '\x85', '\xcc', '\x81'},
    {'\xcf', '\x85', '\xcc', '\x84'},
    {'\xcf', '\x85', '\xcc', '\x86'},
    /*  88 */ {'\xcf', '\x85', '\xcc', '\x88'},
    {'\xcf', '\x85', '\xcc', '\x93'},
    {'\xcf', '\x85', '\xcc', '\x94'},
    {'\xcf', '\x85', '\xcd', '\x82'},
    {'\xcf', '\x89', '\xcc', '\x80'},
    {'\xcf', '\x89', '\xcc', '\x81'},
    {'\xcf', '\x89', '\xcc', '\x93'},
    {'\xcf', '\x89', '\xcc', '\x94'},
    /*  96 */ {'\xcf', '\x89', '\xcd', '\x82'},
    {'\xcf', '\x89', '\xcd', '\x85'},
    {'\xcf', '\x92', '\xcc', '\x81'},
    {'\xcf', '\x92', '\xcc', '\x88'},
    {'\xd0', '\x86', '\xcc', '\x88'},
    {'\xd0', '\x90', '\xcc', '\x86'},
    {'\xd0', '\x90', '\xcc', '\x88'},
    {'\xd0', '\x93', '\xcc', '\x81'},
    /* 104 */ {'\xd0', '\x95', '\xcc', '\x80'},
    {'\xd0', '\x95', '\xcc', '\x86'},
    {'\xd0', '\x95', '\xcc', '\x88'},
    {'\xd0', '\x96', '\xcc', '\x86'},
    {'\xd0', '\x96', '\xcc', '\x88'},
    {'\xd0', '\x97', '\xcc', '\x88'},
    {'\xd0', '\x98', '\xcc', '\x80'},
    {'\xd0', '\x98', '\xcc', '\x84'},
    /* 112 */ {'\xd0', '\x98', '\xcc', '\x86'},
    {'\xd0', '\x98', '\xcc', '\x88'},
    {'\xd0', '\x9a', '\xcc', '\x81'},
    {'\xd0', '\x9e', '\xcc', '\x88'},
    {'\xd0', '\xa3', '\xcc', '\x84'},
    {'\xd0', '\xa3', '\xcc', '\x86'},
    {'\xd0', '\xa3', '\xcc', '\x88'},
    {'\xd0', '\xa3', '\xcc', '\x8b'},
    /* 120 */ {'\xd0', '\xa7', '\xcc', '\x88'},
    {'\xd0', '\xab', '\xcc', '\x88'},
    {'\xd0', '\xad', '\xcc', '\x88'},
    {'\xd0', '\xb0', '\xcc', '\x86'},
    {'\xd0', '\xb0', '\xcc', '\x88'},
    {'\xd0', '\xb3', '\xcc', '\x81'},
    {'\xd0', '\xb5', '\xcc', '\x80'},
    {'\xd0', '\xb5', '\xcc', '\x86'},
    /* 128 */ {'\xd0', '\xb5', '\xcc', '\x88'},
    {'\xd0', '\xb6', '\xcc', '\x86'},
    {'\xd0', '\xb6', '\xcc', '\x88'},
    {'\xd0', '\xb7', '\xcc', '\x88'},
    {'\xd0', '\xb8', '\xcc', '\x80'},
    {'\xd0', '\xb8', '\xcc', '\x84'},
    {'\xd0', '\xb8', '\xcc', '\x86'},
    {'\xd0', '\xb8', '\xcc', '\x88'},
    /* 136 */ {'\xd0', '\xba', '\xcc', '\x81'},
    {'\xd0', '\xbe', '\xcc', '\x88'},
    {'\xd1', '\x83', '\xcc', '\x84'},
    {'\xd1', '\x83', '\xcc', '\x86'},
    {'\xd1', '\x83', '\xcc', '\x88'},
    {'\xd1', '\x83', '\xcc', '\x8b'},
    {'\xd1', '\x87', '\xcc', '\x88'},
    {'\xd1', '\x8b', '\xcc', '\x88'},
    /* 144 */ {'\xd1', '\x8d', '\xcc', '\x88'},
    {'\xd1', '\x96', '\xcc', '\x88'},
    {'\xd1', '\xb4', '\xcc', '\x8f'},
    {'\xd1', '\xb5', '\xcc', '\x8f'},
    {'\xd3', '\x98', '\xcc', '\x88'},
    {'\xd3', '\x99', '\xcc', '\x88'},
    {'\xd3', '\xa8', '\xcc', '\x88'},
    {'\xd3', '\xa9', '\xcc', '\x88'},
    /* 152 */ {'\xd7', '\x90', '\xd6', '\xb7'},
    {'\xd7', '\x90', '\xd6', '\xb8'},
    {'\xd7', '\x90', '\xd6', '\xbc'},
    {'\xd7', '\x91', '\xd6', '\xbc'},
    {'\xd7', '\x91', '\xd6', '\xbf'},
    {'\xd7', '\x92', '\xd6', '\xbc'},
    {'\xd7', '\x93', '\xd6', '\xbc'},
    {'\xd7', '\x94', '\xd6', '\xbc'},
    /* 160 */ {'\xd7', '\x95', '\xd6', '\xb9'},
    {'\xd7', '\x95', '\xd6', '\xbc'},
    {'\xd7', '\x96', '\xd6', '\xbc'},
    {'\xd7', '\x98', '\xd6', '\xbc'},
    {'\xd7', '\x99', '\xd6', '\xb4'},
    {'\xd7', '\x99', '\xd6', '\xbc'},
    {'\xd7', '\x9a', '\xd6', '\xbc'},
    {'\xd7', '\x9b', '\xd6', '\xbc'},
    /* 168 */ {'\xd7', '\x9b', '\xd6', '\xbf'},
    {'\xd7', '\x9c', '\xd6', '\xbc'},
    {'\xd7', '\x9e', '\xd6', '\xbc'},
    {'\xd7', '\xa0', '\xd6', '\xbc'},
    {'\xd7', '\xa1', '\xd6', '\xbc'},
    {'\xd7', '\xa3', '\xd6', '\xbc'},
    {'\xd7', '\xa4', '\xd6', '\xbc'},
    {'\xd7', '\xa4', '\xd6', '\xbf'},
    /* 176 */ {'\xd7', '\xa6', '\xd6', '\xbc'},
    {'\xd7', '\xa7', '\xd6', '\xbc'},
    {'\xd7', '\xa8', '\xd6', '\xbc'},
    {'\xd7', '\xa9', '\xd6', '\xbc'},
    {'\xd7', '\xa9', '\xd7', '\x81'},
    {'\xd7', '\xa9', '\xd7', '\x82'},
    {'\xd7', '\xaa', '\xd6', '\xbc'},
    {'\xd7', '\xb2', '\xd6', '\xb7'},
    /* 184 */ {'\xd8', '\xa7', '\xd9', '\x93'},
    {'\xd8', '\xa7', '\xd9', '\x94'},
    {'\xd8', '\xa7', '\xd9', '\x95'},
    {'\xd9', '\x88', '\xd9', '\x94'},
    {'\xd9', '\x8a', '\xd9', '\x94'},
    {'\xdb', '\x81', '\xd9', '\x94'},
    {'\xdb', '\x92', '\xd9', '\x94'},
    {'\xdb', '\x95', '\xd9', '\x94'},
    /* 192 */ {'\xf0', '\xa0', '\x84', '\xa2'},
    {'\xf0', '\xa0', '\x94', '\x9c'},
    {'\xf0', '\xa0', '\x94', '\xa5'},
    {'\xf0', '\xa0', '\x95', '\x8b'},
    {'\xf0', '\xa0', '\x98', '\xba'},
    {'\xf0', '\xa0', '\xa0', '\x84'},
    {'\xf0', '\xa0', '\xa3', '\x9e'},
    {'\xf0', '\xa0', '\xa8', '\xac'},
    /* 200 */ {'\xf0', '\xa0', '\xad', '\xa3'},
    {'\xf0', '\xa1', '\x93', '\xa4'},
    {'\xf0', '\xa1', '\x9a', '\xa8'},
    {'\xf0', '\xa1', '\x9b', '\xaa'},
    {'\xf0', '\xa1', '\xa7', '\x88'},
    {'\xf0', '\xa1', '\xac', '\x98'},
    {'\xf0', '\xa1', '\xb4', '\x8b'},
    {'\xf0', '\xa1', '\xb7', '\xa4'},
    /* 208 */ {'\xf0', '\xa1', '\xb7', '\xa6'},
    {'\xf0', '\xa2', '\x86', '\x83'},
    {'\xf0', '\xa2', '\x86', '\x9f'},
    {'\xf0', '\xa2', '\x8c', '\xb1'},
    {'\xf0', '\xa2', '\x9b', '\x94'},
    {'\xf0', '\xa2', '\xa1', '\x84'},
    {'\xf0', '\xa2', '\xa1', '\x8a'},
    {'\xf0', '\xa2', '\xac', '\x8c'},
    /* 216 */ {'\xf0', '\xa2', '\xaf', '\xb1'},
    {'\xf0', '\xa3', '\x80', '\x8a'},
    {'\xf0', '\xa3', '\x8a', '\xb8'},
    {'\xf0', '\xa3', '\x8d', '\x9f'},
    {'\xf0', '\xa3', '\x8e', '\x93'},
    {'\xf0', '\xa3', '\x8e', '\x9c'},
    {'\xf0', '\xa3', '\x8f', '\x83'},
    {'\xf0', '\xa3', '\x8f', '\x95'},
    /* 224 */ {'\xf0', '\xa3', '\x91', '\xad'},
    {'\xf0', '\xa3', '\x9a', '\xa3'},
    {'\xf0', '\xa3', '\xa2', '\xa7'},
    {'\xf0', '\xa3', '\xaa', '\x8d'},
    {'\xf0', '\xa3', '\xab', '\xba'},
    {'\xf0', '\xa3', '\xb2', '\xbc'},
    {'\xf0', '\xa3', '\xb4', '\x9e'},
    {'\xf0', '\xa3', '\xbb', '\x91'},
    /* 232 */ {'\xf0', '\xa3', '\xbd', '\x9e'},
    {'\xf0', '\xa3', '\xbe', '\x8e'},
    {'\xf0', '\xa4', '\x89', '\xa3'},
    {'\xf0', '\xa4', '\x8b', '\xae'},
    {'\xf0', '\xa4', '\x8e', '\xab'},
    {'\xf0', '\xa4', '\x98', '\x88'},
    {'\xf0', '\xa4', '\x9c', '\xb5'},
    {'\xf0', '\xa4', '\xa0', '\x94'},
    /* 240 */ {'\xf0', '\xa4', '\xb0', '\xb6'},
    {'\xf0', '\xa4', '\xb2', '\x92'},
    {'\xf0', '\xa4', '\xbe', '\xa1'},
    {'\xf0', '\xa4', '\xbe', '\xb8'},
    {'\xf0', '\xa5', '\x81', '\x84'},
    {'\xf0', '\xa5', '\x83', '\xb2'},
    {'\xf0', '\xa5', '\x83', '\xb3'},
    {'\xf0', '\xa5', '\x84', '\x99'},
    /* 248 */ {'\xf0', '\xa5', '\x84', '\xb3'},
    {'\xf0', '\xa5', '\x89', '\x89'},
    {'\xf0', '\xa5', '\x90', '\x9d'},
    {'\xf0', '\xa5', '\x98', '\xa6'},
    {'\xf0', '\xa5', '\x9a', '\x9a'},
    {'\xf0', '\xa5', '\x9b', '\x85'},
    {'\xf0', '\xa5', '\xa5', '\xbc'},
    {'\xf0', '\xa5', '\xaa', '\xa7'},
    /* 256 */ {'\xf0', '\xa5', '\xae', '\xab'},
    {'\xf0', '\xa5', '\xb2', '\x80'},
    {'\xf0', '\xa5', '\xb3', '\x90'},
    {'\xf0', '\xa5', '\xbe', '\x86'},
    {'\xf0', '\xa6', '\x87', '\x9a'},
    {'\xf0', '\xa6', '\x88', '\xa8'},
    {'\xf0', '\xa6', '\x89', '\x87'},
    {'\xf0', '\xa6', '\x8b', '\x99'},
    /* 264 */ {'\xf0', '\xa6', '\x8c', '\xbe'},
    {'\xf0', '\xa6', '\x93', '\x9a'},
    {'\xf0', '\xa6', '\x94', '\xa3'},
    {'\xf0', '\xa6', '\x96', '\xa8'},
    {'\xf0', '\xa6', '\x9e', '\xa7'},
    {'\xf0', '\xa6', '\x9e', '\xb5'},
    {'\xf0', '\xa6', '\xac', '\xbc'},
    {'\xf0', '\xa6', '\xb0', '\xb6'},
    /* 272 */ {'\xf0', '\xa6', '\xb3', '\x95'},
    {'\xf0', '\xa6', '\xb5', '\xab'},
    {'\xf0', '\xa6', '\xbc', '\xac'},
    {'\xf0', '\xa6', '\xbe', '\xb1'},
    {'\xf0', '\xa7', '\x83', '\x92'},
    {'\xf0', '\xa7', '\x8f', '\x8a'},
    {'\xf0', '\xa7', '\x99', '\xa7'},
    {'\xf0', '\xa7', '\xa2', '\xae'},
    /* 280 */ {'\xf0', '\xa7', '\xa5', '\xa6'},
    {'\xf0', '\xa7', '\xb2', '\xa8'},
    {'\xf0', '\xa7', '\xbb', '\x93'},
    {'\xf0', '\xa7', '\xbc', '\xaf'},
    {'\xf0', '\xa8', '\x97', '\x92'},
    {'\xf0', '\xa8', '\x97', '\xad'},
    {'\xf0', '\xa8', '\x9c', '\xae'},
    {'\xf0', '\xa8', '\xaf', '\xba'},
    /* 288 */ {'\xf0', '\xa8', '\xb5', '\xb7'},
    {'\xf0', '\xa9', '\x85', '\x85'},
    {'\xf0', '\xa9', '\x87', '\x9f'},
    {'\xf0', '\xa9', '\x88', '\x9a'},
    {'\xf0', '\xa9', '\x90', '\x8a'},
    {'\xf0', '\xa9', '\x92', '\x96'},
    {'\xf0', '\xa9', '\x96', '\xb6'},
    {'\xf0', '\xa9', '\xac', '\xb0'},
    /* 296 */ {'\xf0', '\xaa', '\x83', '\x8e'},
    {'\xf0', '\xaa', '\x84', '\x85'},
    {'\xf0', '\xaa', '\x88', '\x8e'},
    {'\xf0', '\xaa', '\x8a', '\x91'},
    {'\xf0', '\xaa', '\x8e', '\x92'},
    {'\xf0', '\xaa', '\x98', '\x80'}};

static const char UN8IF_canon_tbl_5[162][5] = {
    /*   0 */ {'A', '\xcc', '\x82', '\xcc', '\x80'}, {'A', '\xcc', '\x82', '\xcc', '\x81'}, {'A', '\xcc', '\x82', '\xcc', '\x83'}, {'A', '\xcc', '\x82', '\xcc', '\x89'}, {'A', '\xcc', '\x86', '\xcc', '\x80'}, {'A', '\xcc', '\x86', '\xcc', '\x81'}, {'A', '\xcc', '\x86', '\xcc', '\x83'}, {'A', '\xcc', '\x86', '\xcc', '\x89'},
    /*   8 */ {'A', '\xcc', '\x87', '\xcc', '\x84'},
    {'A', '\xcc', '\x88', '\xcc', '\x84'},
    {'A', '\xcc', '\x8a', '\xcc', '\x81'},
    {'A', '\xcc', '\xa3', '\xcc', '\x82'},
    {'A', '\xcc', '\xa3', '\xcc', '\x86'},
    {'C', '\xcc', '\xa7', '\xcc', '\x81'},
    {'E', '\xcc', '\x82', '\xcc', '\x80'},
    {'E', '\xcc', '\x82', '\xcc', '\x81'},
    /*  16 */ {'E', '\xcc', '\x82', '\xcc', '\x83'},
    {'E', '\xcc', '\x82', '\xcc', '\x89'},
    {'E', '\xcc', '\x84', '\xcc', '\x80'},
    {'E', '\xcc', '\x84', '\xcc', '\x81'},
    {'E', '\xcc', '\xa3', '\xcc', '\x82'},
    {'E', '\xcc', '\xa7', '\xcc', '\x86'},
    {'I', '\xcc', '\x88', '\xcc', '\x81'},
    {'L', '\xcc', '\xa3', '\xcc', '\x84'},
    /*  24 */ {'O', '\xcc', '\x82', '\xcc', '\x80'},
    {'O', '\xcc', '\x82', '\xcc', '\x81'},
    {'O', '\xcc', '\x82', '\xcc', '\x83'},
    {'O', '\xcc', '\x82', '\xcc', '\x89'},
    {'O', '\xcc', '\x83', '\xcc', '\x81'},
    {'O', '\xcc', '\x83', '\xcc', '\x84'},
    {'O', '\xcc', '\x83', '\xcc', '\x88'},
    {'O', '\xcc', '\x84', '\xcc', '\x80'},
    /*  32 */ {'O', '\xcc', '\x84', '\xcc', '\x81'},
    {'O', '\xcc', '\x87', '\xcc', '\x84'},
    {'O', '\xcc', '\x88', '\xcc', '\x84'},
    {'O', '\xcc', '\x9b', '\xcc', '\x80'},
    {'O', '\xcc', '\x9b', '\xcc', '\x81'},
    {'O', '\xcc', '\x9b', '\xcc', '\x83'},
    {'O', '\xcc', '\x9b', '\xcc', '\x89'},
    {'O', '\xcc', '\x9b', '\xcc', '\xa3'},
    /*  40 */ {'O', '\xcc', '\xa3', '\xcc', '\x82'},
    {'O', '\xcc', '\xa8', '\xcc', '\x84'},
    {'R', '\xcc', '\xa3', '\xcc', '\x84'},
    {'S', '\xcc', '\x81', '\xcc', '\x87'},
    {'S', '\xcc', '\x8c', '\xcc', '\x87'},
    {'S', '\xcc', '\xa3', '\xcc', '\x87'},
    {'U', '\xcc', '\x83', '\xcc', '\x81'},
    {'U', '\xcc', '\x84', '\xcc', '\x88'},
    /*  48 */ {'U', '\xcc', '\x88', '\xcc', '\x80'},
    {'U', '\xcc', '\x88', '\xcc', '\x81'},
    {'U', '\xcc', '\x88', '\xcc', '\x84'},
    {'U', '\xcc', '\x88', '\xcc', '\x8c'},
    {'U', '\xcc', '\x9b', '\xcc', '\x80'},
    {'U', '\xcc', '\x9b', '\xcc', '\x81'},
    {'U', '\xcc', '\x9b', '\xcc', '\x83'},
    {'U', '\xcc', '\x9b', '\xcc', '\x89'},
    /*  56 */ {'U', '\xcc', '\x9b', '\xcc', '\xa3'},
    {'\xe1', '\xbe', '\xbf', '\xcc', '\x80'},
    {'\xe1', '\xbe', '\xbf', '\xcc', '\x81'},
    {'\xe1', '\xbe', '\xbf', '\xcd', '\x82'},
    {'\xe1', '\xbf', '\xbe', '\xcc', '\x80'},
    {'\xe1', '\xbf', '\xbe', '\xcc', '\x81'},
    {'\xe1', '\xbf', '\xbe', '\xcd', '\x82'},
    {'\xe2', '\x86', '\x90', '\xcc', '\xb8'},
    /*  64 */ {'\xe2', '\x86', '\x92', '\xcc', '\xb8'},
    {'\xe2', '\x86', '\x94', '\xcc', '\xb8'},
    {'\xe2', '\x87', '\x90', '\xcc', '\xb8'},
    {'\xe2', '\x87', '\x92', '\xcc', '\xb8'},
    {'\xe2', '\x87', '\x94', '\xcc', '\xb8'},
    {'\xe2', '\x88', '\x83', '\xcc', '\xb8'},
    {'\xe2', '\x88', '\x88', '\xcc', '\xb8'},
    {'\xe2', '\x88', '\x8b', '\xcc', '\xb8'},
    /*  72 */ {'\xe2', '\x88', '\xa3', '\xcc', '\xb8'},
    {'\xe2', '\x88', '\xa5', '\xcc', '\xb8'},
    {'\xe2', '\x88', '\xbc', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\x83', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\x85', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\x88', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\x8d', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\xa1', '\xcc', '\xb8'},
    /*  80 */ {'\xe2', '\x89', '\xa4', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\xa5', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\xb2', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\xb3', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\xb6', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\xb7', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\xba', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\xbb', '\xcc', '\xb8'},
    /*  88 */ {'\xe2', '\x89', '\xbc', '\xcc', '\xb8'},
    {'\xe2', '\x89', '\xbd', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\x82', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\x83', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\x86', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\x87', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\x91', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\x92', '\xcc', '\xb8'},
    /*  96 */ {'\xe2', '\x8a', '\xa2', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\xa8', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\xa9', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\xab', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\xb2', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\xb3', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\xb4', '\xcc', '\xb8'},
    {'\xe2', '\x8a', '\xb5', '\xcc', '\xb8'},
    /* 104 */ {'\xe2', '\xab', '\x9d', '\xcc', '\xb8'},
    {'a', '\xcc', '\x82', '\xcc', '\x80'},
    {'a', '\xcc', '\x82', '\xcc', '\x81'},
    {'a', '\xcc', '\x82', '\xcc', '\x83'},
    {'a', '\xcc', '\x82', '\xcc', '\x89'},
    {'a', '\xcc', '\x86', '\xcc', '\x80'},
    {'a', '\xcc', '\x86', '\xcc', '\x81'},
    {'a', '\xcc', '\x86', '\xcc', '\x83'},
    /* 112 */ {'a', '\xcc', '\x86', '\xcc', '\x89'},
    {'a', '\xcc', '\x87', '\xcc', '\x84'},
    {'a', '\xcc', '\x88', '\xcc', '\x84'},
    {'a', '\xcc', '\x8a', '\xcc', '\x81'},
    {'a', '\xcc', '\xa3', '\xcc', '\x82'},
    {'a', '\xcc', '\xa3', '\xcc', '\x86'},
    {'c', '\xcc', '\xa7', '\xcc', '\x81'},
    {'e', '\xcc', '\x82', '\xcc', '\x80'},
    /* 120 */ {'e', '\xcc', '\x82', '\xcc', '\x81'},
    {'e', '\xcc', '\x82', '\xcc', '\x83'},
    {'e', '\xcc', '\x82', '\xcc', '\x89'},
    {'e', '\xcc', '\x84', '\xcc', '\x80'},
    {'e', '\xcc', '\x84', '\xcc', '\x81'},
    {'e', '\xcc', '\xa3', '\xcc', '\x82'},
    {'e', '\xcc', '\xa7', '\xcc', '\x86'},
    {'i', '\xcc', '\x88', '\xcc', '\x81'},
    /* 128 */ {'l', '\xcc', '\xa3', '\xcc', '\x84'},
    {'o', '\xcc', '\x82', '\xcc', '\x80'},
    {'o', '\xcc', '\x82', '\xcc', '\x81'},
    {'o', '\xcc', '\x82', '\xcc', '\x83'},
    {'o', '\xcc', '\x82', '\xcc', '\x89'},
    {'o', '\xcc', '\x83', '\xcc', '\x81'},
    {'o', '\xcc', '\x83', '\xcc', '\x84'},
    {'o', '\xcc', '\x83', '\xcc', '\x88'},
    /* 136 */ {'o', '\xcc', '\x84', '\xcc', '\x80'},
    {'o', '\xcc', '\x84', '\xcc', '\x81'},
    {'o', '\xcc', '\x87', '\xcc', '\x84'},
    {'o', '\xcc', '\x88', '\xcc', '\x84'},
    {'o', '\xcc', '\x9b', '\xcc', '\x80'},
    {'o', '\xcc', '\x9b', '\xcc', '\x81'},
    {'o', '\xcc', '\x9b', '\xcc', '\x83'},
    {'o', '\xcc', '\x9b', '\xcc', '\x89'},
    /* 144 */ {'o', '\xcc', '\x9b', '\xcc', '\xa3'},
    {'o', '\xcc', '\xa3', '\xcc', '\x82'},
    {'o', '\xcc', '\xa8', '\xcc', '\x84'},
    {'r', '\xcc', '\xa3', '\xcc', '\x84'},
    {'s', '\xcc', '\x81', '\xcc', '\x87'},
    {'s', '\xcc', '\x8c', '\xcc', '\x87'},
    {'s', '\xcc', '\xa3', '\xcc', '\x87'},
    {'u', '\xcc', '\x83', '\xcc', '\x81'},
    /* 152 */ {'u', '\xcc', '\x84', '\xcc', '\x88'},
    {'u', '\xcc', '\x88', '\xcc', '\x80'},
    {'u', '\xcc', '\x88', '\xcc', '\x81'},
    {'u', '\xcc', '\x88', '\xcc', '\x84'},
    {'u', '\xcc', '\x88', '\xcc', '\x8c'},
    {'u', '\xcc', '\x9b', '\xcc', '\x80'},
    {'u', '\xcc', '\x9b', '\xcc', '\x81'},
    {'u', '\xcc', '\x9b', '\xcc', '\x83'},
    /* 160 */ {'u', '\xcc', '\x9b', '\xcc', '\x89'},
    {'u', '\xcc', '\x9b', '\xcc', '\xa3'}};

/* the special-cased overlong entries */
typedef struct {
    const uint32_t cp;
    const char *v;
} UN8IF_canon_exc_t;
/* sorted for binary search */
#define UN8IF_canon_exc_size 317
static const UN8IF_canon_exc_t UN8IF_canon_exc[317] = {
    {0x390, "\xce\xb9\xcc\x88\xcc\x81"},
    {0x3b0, "\xcf\x85\xcc\x88\xcc\x81"},
    {0x929, "\xe0\xa4\xa8\xe0\xa4\xbc"},
    {0x931, "\xe0\xa4\xb0\xe0\xa4\xbc"},
    {0x934, "\xe0\xa4\xb3\xe0\xa4\xbc"},
    {0x958, "\xe0\xa4\x95\xe0\xa4\xbc"},
    {0x959, "\xe0\xa4\x96\xe0\xa4\xbc"},
    {0x95a, "\xe0\xa4\x97\xe0\xa4\xbc"},
    {0x95b, "\xe0\xa4\x9c\xe0\xa4\xbc"},
    {0x95c, "\xe0\xa4\xa1\xe0\xa4\xbc"},
    {0x95d, "\xe0\xa4\xa2\xe0\xa4\xbc"},
    {0x95e, "\xe0\xa4\xab\xe0\xa4\xbc"},
    {0x95f, "\xe0\xa4\xaf\xe0\xa4\xbc"},
    {0x9cb, "\xe0\xa7\x87\xe0\xa6\xbe"},
    {0x9cc, "\xe0\xa7\x87\xe0\xa7\x97"},
    {0x9dc, "\xe0\xa6\xa1\xe0\xa6\xbc"},
    {0x9dd, "\xe0\xa6\xa2\xe0\xa6\xbc"},
    {0x9df, "\xe0\xa6\xaf\xe0\xa6\xbc"},
    {0xa33, "\xe0\xa8\xb2\xe0\xa8\xbc"},
    {0xa36, "\xe0\xa8\xb8\xe0\xa8\xbc"},
    {0xa59, "\xe0\xa8\x96\xe0\xa8\xbc"},
    {0xa5a, "\xe0\xa8\x97\xe0\xa8\xbc"},
    {0xa5b, "\xe0\xa8\x9c\xe0\xa8\xbc"},
    {0xa5e, "\xe0\xa8\xab\xe0\xa8\xbc"},
    {0xb48, "\xe0\xad\x87\xe0\xad\x96"},
    {0xb4b, "\xe0\xad\x87\xe0\xac\xbe"},
    {0xb4c, "\xe0\xad\x87\xe0\xad\x97"},
    {0xb5c, "\xe0\xac\xa1\xe0\xac\xbc"},
    {0xb5d, "\xe0\xac\xa2\xe0\xac\xbc"},
    {0xb94, "\xe0\xae\x92\xe0\xaf\x97"},
    {0xbca, "\xe0\xaf\x86\xe0\xae\xbe"},
    {0xbcb, "\xe0\xaf\x87\xe0\xae\xbe"},
    {0xbcc, "\xe0\xaf\x86\xe0\xaf\x97"},
    {0xc48, "\xe0\xb1\x86\xe0\xb1\x96"},
    {0xcc0, "\xe0\xb2\xbf\xe0\xb3\x95"},
    {0xcc7, "\xe0\xb3\x86\xe0\xb3\x95"},
    {0xcc8, "\xe0\xb3\x86\xe0\xb3\x96"},
    {0xcca, "\xe0\xb3\x86\xe0\xb3\x82"},
    {0xccb, "\xe0\xb3\x86\xe0\xb3\x82\xe0\xb3\x95"},
    {0xd4a, "\xe0\xb5\x86\xe0\xb4\xbe"},
    {0xd4b, "\xe0\xb5\x87\xe0\xb4\xbe"},
    {0xd4c, "\xe0\xb5\x86\xe0\xb5\x97"},
    {0xdda, "\xe0\xb7\x99\xe0\xb7\x8a"},
    {0xddc, "\xe0\xb7\x99\xe0\xb7\x8f"},
    {0xddd, "\xe0\xb7\x99\xe0\xb7\x8f\xe0\xb7\x8a"},
    {0xdde, "\xe0\xb7\x99\xe0\xb7\x9f"},
    {0xf43, "\xe0\xbd\x82\xe0\xbe\xb7"},
    {0xf4d, "\xe0\xbd\x8c\xe0\xbe\xb7"},
    {0xf52, "\xe0\xbd\x91\xe0\xbe\xb7"},
    {0xf57, "\xe0\xbd\x96\xe0\xbe\xb7"},
    {0xf5c, "\xe0\xbd\x9b\xe0\xbe\xb7"},
    {0xf69, "\xe0\xbd\x80\xe0\xbe\xb5"},
    {0xf73, "\xe0\xbd\xb1\xe0\xbd\xb2"},
    {0xf75, "\xe0\xbd\xb1\xe0\xbd\xb4"},
    {0xf76, "\xe0\xbe\xb2\xe0\xbe\x80"},
    {0xf78, "\xe0\xbe\xb3\xe0\xbe\x80"},
    {0xf81, "\xe0\xbd\xb1\xe0\xbe\x80"},
    {0xf93, "\xe0\xbe\x92\xe0\xbe\xb7"},
    {0xf9d, "\xe0\xbe\x9c\xe0\xbe\xb7"},
    {0xfa2, "\xe0\xbe\xa1\xe0\xbe\xb7"},
    {0xfa7, "\xe0\xbe\xa6\xe0\xbe\xb7"},
    {0xfac, "\xe0\xbe\xab\xe0\xbe\xb7"},
    {0xfb9, "\xe0\xbe\x90\xe0\xbe\xb5"},
    {0x1026, "\xe1\x80\xa5\xe1\x80\xae"},
    {0x1b06, "\xe1\xac\x85\xe1\xac\xb5"},
    {0x1b08, "\xe1\xac\x87\xe1\xac\xb5"},
    {0x1b0a, "\xe1\xac\x89\xe1\xac\xb5"},
    {0x1b0c, "\xe1\xac\x8b\xe1\xac\xb5"},
    {0x1b0e, "\xe1\xac\x8d\xe1\xac\xb5"},
    {0x1b12, "\xe1\xac\x91\xe1\xac\xb5"},
    {0x1b3b, "\xe1\xac\xba\xe1\xac\xb5"},
    {0x1b3d, "\xe1\xac\xbc\xe1\xac\xb5"},
    {0x1b40, "\xe1\xac\xbe\xe1\xac\xb5"},
    {0x1b41, "\xe1\xac\xbf\xe1\xac\xb5"},
    {0x1b43, "\xe1\xad\x82\xe1\xac\xb5"},
    {0x1f02, "\xce\xb1\xcc\x93\xcc\x80"},
    {0x1f03, "\xce\xb1\xcc\x94\xcc\x80"},
    {0x1f04, "\xce\xb1\xcc\x93\xcc\x81"},
    {0x1f05, "\xce\xb1\xcc\x94\xcc\x81"},
    {0x1f06, "\xce\xb1\xcc\x93\xcd\x82"},
    {0x1f07, "\xce\xb1\xcc\x94\xcd\x82"},
    {0x1f0a, "\xce\x91\xcc\x93\xcc\x80"},
    {0x1f0b, "\xce\x91\xcc\x94\xcc\x80"},
    {0x1f0c, "\xce\x91\xcc\x93\xcc\x81"},
    {0x1f0d, "\xce\x91\xcc\x94\xcc\x81"},
    {0x1f0e, "\xce\x91\xcc\x93\xcd\x82"},
    {0x1f0f, "\xce\x91\xcc\x94\xcd\x82"},
    {0x1f12, "\xce\xb5\xcc\x93\xcc\x80"},
    {0x1f13, "\xce\xb5\xcc\x94\xcc\x80"},
    {0x1f14, "\xce\xb5\xcc\x93\xcc\x81"},
    {0x1f15, "\xce\xb5\xcc\x94\xcc\x81"},
    {0x1f1a, "\xce\x95\xcc\x93\xcc\x80"},
    {0x1f1b, "\xce\x95\xcc\x94\xcc\x80"},
    {0x1f1c, "\xce\x95\xcc\x93\xcc\x81"},
    {0x1f1d, "\xce\x95\xcc\x94\xcc\x81"},
    {0x1f22, "\xce\xb7\xcc\x93\xcc\x80"},
    {0x1f23, "\xce\xb7\xcc\x94\xcc\x80"},
    {0x1f24, "\xce\xb7\xcc\x93\xcc\x81"},
    {0x1f25, "\xce\xb7\xcc\x94\xcc\x81"},
    {0x1f26, "\xce\xb7\xcc\x93\xcd\x82"},
    {0x1f27, "\xce\xb7\xcc\x94\xcd\x82"},
    {0x1f2a, "\xce\x97\xcc\x93\xcc\x80"},
    {0x1f2b, "\xce\x97\xcc\x94\xcc\x80"},
    {0x1f2c, "\xce\x97\xcc\x93\xcc\x81"},
    {0x1f2d, "\xce\x97\xcc\x94\xcc\x81"},
    {0x1f2e, "\xce\x97\xcc\x93\xcd\x82"},
    {0x1f2f, "\xce\x97\xcc\x94\xcd\x82"},
    {0x1f32, "\xce\xb9\xcc\x93\xcc\x80"},
    {0x1f33, "\xce\xb9\xcc\x94\xcc\x80"},
    {0x1f34, "\xce\xb9\xcc\x93\xcc\x81"},
    {0x1f35, "\xce\xb9\xcc\x94\xcc\x81"},
    {0x1f36, "\xce\xb9\xcc\x93\xcd\x82"},
    {0x1f37, "\xce\xb9\xcc\x94\xcd\x82"},
    {0x1f3a, "\xce\x99\xcc\x93\xcc\x80"},
    {0x1f3b, "\xce\x99\xcc\x94\xcc\x80"},
    {0x1f3c, "\xce\x99\xcc\x93\xcc\x81"},
    {0x1f3d, "\xce\x99\xcc\x94\xcc\x81"},
    {0x1f3e, "\xce\x99\xcc\x93\xcd\x82"},
    {0x1f3f, "\xce\x99\xcc\x94\xcd\x82"},
    {0x1f42, "\xce\xbf\xcc\x93\xcc\x80"},
    {0x1f43, "\xce\xbf\xcc\x94\xcc\x80"},
    {0x1f44, "\xce\xbf\xcc\x93\xcc\x81"},
    {0x1f45, "\xce\xbf\xcc\x94\xcc\x81"},
    {0x1f4a, "\xce\x9f\xcc\x93\xcc\x80"},
    {0x1f4b, "\xce\x9f\xcc\x94\xcc\x80"},
    {0x1f4c, "\xce\x9f\xcc\x93\xcc\x81"},
    {0x1f4d, "\xce\x9f\xcc\x94\xcc\x81"},
    {0x1f52, "\xcf\x85\xcc\x93\xcc\x80"},
    {0x1f53, "\xcf\x85\xcc\x94\xcc\x80"},
    {0x1f54, "\xcf\x85\xcc\x93\xcc\x81"},
    {0x1f55, "\xcf\x85\xcc\x94\xcc\x81"},
    {0x1f56, "\xcf\x85\xcc\x93\xcd\x82"},
    {0x1f57, "\xcf\x85\xcc\x94\xcd\x82"},
    {0x1f5b, "\xce\xa5\xcc\x94\xcc\x80"},
    {0x1f5d, "\xce\xa5\xcc\x94\xcc\x81"},
    {0x1f5f, "\xce\xa5\xcc\x94\xcd\x82"},
    {0x1f62, "\xcf\x89\xcc\x93\xcc\x80"},
    {0x1f63, "\xcf\x89\xcc\x94\xcc\x80"},
    {0x1f64, "\xcf\x89\xcc\x93\xcc\x81"},
    {0x1f65, "\xcf\x89\xcc\x94\xcc\x81"},
    {0x1f66, "\xcf\x89\xcc\x93\xcd\x82"},
    {0x1f67, "\xcf\x89\xcc\x94\xcd\x82"},
    {0x1f6a, "\xce\xa9\xcc\x93\xcc\x80"},
    {0x1f6b, "\xce\xa9\xcc\x94\xcc\x80"},
    {0x1f6c, "\xce\xa9\xcc\x93\xcc\x81"},
    {0x1f6d, "\xce\xa9\xcc\x94\xcc\x81"},
    {0x1f6e, "\xce\xa9\xcc\x93\xcd\x82"},
    {0x1f6f, "\xce\xa9\xcc\x94\xcd\x82"},
    {0x1f80, "\xce\xb1\xcc\x93\xcd\x85"},
    {0x1f81, "\xce\xb1\xcc\x94\xcd\x85"},
    {0x1f82, "\xce\xb1\xcc\x93\xcc\x80\xcd\x85"},
    {0x1f83, "\xce\xb1\xcc\x94\xcc\x80\xcd\x85"},
    {0x1f84, "\xce\xb1\xcc\x93\xcc\x81\xcd\x85"},
    {0x1f85, "\xce\xb1\xcc\x94\xcc\x81\xcd\x85"},
    {0x1f86, "\xce\xb1\xcc\x93\xcd\x82\xcd\x85"},
    {0x1f87, "\xce\xb1\xcc\x94\xcd\x82\xcd\x85"},
    {0x1f88, "\xce\x91\xcc\x93\xcd\x85"},
    {0x1f89, "\xce\x91\xcc\x94\xcd\x85"},
    {0x1f8a, "\xce\x91\xcc\x93\xcc\x80\xcd\x85"},
    {0x1f8b, "\xce\x91\xcc\x94\xcc\x80\xcd\x85"},
    {0x1f8c, "\xce\x91\xcc\x93\xcc\x81\xcd\x85"},
    {0x1f8d, "\xce\x91\xcc\x94\xcc\x81\xcd\x85"},
    {0x1f8e, "\xce\x91\xcc\x93\xcd\x82\xcd\x85"},
    {0x1f8f, "\xce\x91\xcc\x94\xcd\x82\xcd\x85"},
    {0x1f90, "\xce\xb7\xcc\x93\xcd\x85"},
    {0x1f91, "\xce\xb7\xcc\x94\xcd\x85"},
    {0x1f92, "\xce\xb7\xcc\x93\xcc\x80\xcd\x85"},
    {0x1f93, "\xce\xb7\xcc\x94\xcc\x80\xcd\x85"},
    {0x1f94, "\xce\xb7\xcc\x93\xcc\x81\xcd\x85"},
    {0x1f95, "\xce\xb7\xcc\x94\xcc\x81\xcd\x85"},
    {0x1f96, "\xce\xb7\xcc\x93\xcd\x82\xcd\x85"},
    {0x1f97, "\xce\xb7\xcc\x94\xcd\x82\xcd\x85"},
    {0x1f98, "\xce\x97\xcc\x93\xcd\x85"},
    {0x1f99, "\xce\x97\xcc\x94\xcd\x85"},
    {0x1f9a, "\xce\x97\xcc\x93\xcc\x80\xcd\x85"},
    {0x1f9b, "\xce\x97\xcc\x94\xcc\x80\xcd\x85"},
    {0x1f9c, "\xce\x97\xcc\x93\xcc\x81\xcd\x85"},
    {0x1f9d, "\xce\x97\xcc\x94\xcc\x81\xcd\x85"},
    {0x1f9e, "\xce\x97\xcc\x93\xcd\x82\xcd\x85"},
    {0x1f9f, "\xce\x97\xcc\x94\xcd\x82\xcd\x85"},
    {0x1fa0, "\xcf\x89\xcc\x93\xcd\x85"},
    {0x1fa1, "\xcf\x89\xcc\x94\xcd\x85"},
    {0x1fa2, "\xcf\x89\xcc\x93\xcc\x80\xcd\x85"},
    {0x1fa3, "\xcf\x89\xcc\x94\xcc\x80\xcd\x85"},
    {0x1fa4, "\xcf\x89\xcc\x93\xcc\x81\xcd\x85"},
    {0x1fa5, "\xcf\x89\xcc\x94\xcc\x81\xcd\x85"},
    {0x1fa6, "\xcf\x89\xcc\x93\xcd\x82\xcd\x85"},
    {0x1fa7, "\xcf\x89\xcc\x94\xcd\x82\xcd\x85"},
    {0x1fa8, "\xce\xa9\xcc\x93\xcd\x85"},
    {0x1fa9, "\xce\xa9\xcc\x94\xcd\x85"},
    {0x1faa, "\xce\xa9\xcc\x93\xcc\x80\xcd\x85"},
    {0x1fab, "\xce\xa9\xcc\x94\xcc\x80\xcd\x85"},
    {0x1fac, "\xce\xa9\xcc\x93\xcc\x81\xcd\x85"},
    {0x1fad, "\xce\xa9\xcc\x94\xcc\x81\xcd\x85"},
    {0x1fae, "\xce\xa9\xcc\x93\xcd\x82\xcd\x85"},
    {0x1faf, "\xce\xa9\xcc\x94\xcd\x82\xcd\x85"},
    {0x1fb2, "\xce\xb1\xcc\x80\xcd\x85"},
    {0x1fb4, "\xce\xb1\xcc\x81\xcd\x85"},
    {0x1fb7, "\xce\xb1\xcd\x82\xcd\x85"},
    {0x1fc2, "\xce\xb7\xcc\x80\xcd\x85"},
    {0x1fc4, "\xce\xb7\xcc\x81\xcd\x85"},
    {0x1fc7, "\xce\xb7\xcd\x82\xcd\x85"},
    {0x1fd2, "\xce\xb9\xcc\x88\xcc\x80"},
    {0x1fd3, "\xce\xb9\xcc\x88\xcc\x81"},
    {0x1fd7, "\xce\xb9\xcc\x88\xcd\x82"},
    {0x1fe2, "\xcf\x85\xcc\x88\xcc\x80"},
    {0x1fe3, "\xcf\x85\xcc\x88\xcc\x81"},
    {0x1fe7, "\xcf\x85\xcc\x88\xcd\x82"},
    {0x1ff2, "\xcf\x89\xcc\x80\xcd\x85"},
    {0x1ff4, "\xcf\x89\xcc\x81\xcd\x85"},
    {0x1ff7, "\xcf\x89\xcd\x82\xcd\x85"},
    {0x304c, "\xe3\x81\x8b\xe3\x82\x99"},
    {0x304e, "\xe3\x81\x8d\xe3\x82\x99"},
    {0x3050, "\xe3\x81\x8f\xe3\x82\x99"},
    {0x3052, "\xe3\x81\x91\xe3\x82\x99"},
    {0x3054, "\xe3\x81\x93\xe3\x82\x99"},
    {0x3056, "\xe3\x81\x95\xe3\x82\x99"},
    {0x3058, "\xe3\x81\x97\xe3\x82\x99"},
    {0x305a, "\xe3\x81\x99\xe3\x82\x99"},
    {0x305c, "\xe3\x81\x9b\xe3\x82\x99"},
    {0x305e, "\xe3\x81\x9d\xe3\x82\x99"},
    {0x3060, "\xe3\x81\x9f\xe3\x82\x99"},
    {0x3062, "\xe3\x81\xa1\xe3\x82\x99"},
    {0x3065, "\xe3\x81\xa4\xe3\x82\x99"},
    {0x3067, "\xe3\x81\xa6\xe3\x82\x99"},
    {0x3069, "\xe3\x81\xa8\xe3\x82\x99"},
    {0x3070, "\xe3\x81\xaf\xe3\x82\x99"},
    {0x3071, "\xe3\x81\xaf\xe3\x82\x9a"},
    {0x3073, "\xe3\x81\xb2\xe3\x82\x99"},
    {0x3074, "\xe3\x81\xb2\xe3\x82\x9a"},
    {0x3076, "\xe3\x81\xb5\xe3\x82\x99"},
    {0x3077, "\xe3\x81\xb5\xe3\x82\x9a"},
    {0x3079, "\xe3\x81\xb8\xe3\x82\x99"},
    {0x307a, "\xe3\x81\xb8\xe3\x82\x9a"},
    {0x307c, "\xe3\x81\xbb\xe3\x82\x99"},
    {0x307d, "\xe3\x81\xbb\xe3\x82\x9a"},
    {0x3094, "\xe3\x81\x86\xe3\x82\x99"},
    {0x309e, "\xe3\x82\x9d\xe3\x82\x99"},
    {0x30ac, "\xe3\x82\xab\xe3\x82\x99"},
    {0x30ae, "\xe3\x82\xad\xe3\x82\x99"},
    {0x30b0, "\xe3\x82\xaf\xe3\x82\x99"},
    {0x30b2, "\xe3\x82\xb1\xe3\x82\x99"},
    {0x30b4, "\xe3\x82\xb3\xe3\x82\x99"},
    {0x30b6, "\xe3\x82\xb5\xe3\x82\x99"},
    {0x30b8, "\xe3\x82\xb7\xe3\x82\x99"},
    {0x30ba, "\xe3\x82\xb9\xe3\x82\x99"},
    {0x30bc, "\xe3\x82\xbb\xe3\x82\x99"},
    {0x30be, "\xe3\x82\xbd\xe3\x82\x99"},
    {0x30c0, "\xe3\x82\xbf\xe3\x82\x99"},
    {0x30c2, "\xe3\x83\x81\xe3\x82\x99"},
    {0x30c5, "\xe3\x83\x84\xe3\x82\x99"},
    {0x30c7, "\xe3\x83\x86\xe3\x82\x99"},
    {0x30c9, "\xe3\x83\x88\xe3\x82\x99"},
    {0x30d0, "\xe3\x83\x8f\xe3\x82\x99"},
    {0x30d1, "\xe3\x83\x8f\xe3\x82\x9a"},
    {0x30d3, "\xe3\x83\x92\xe3\x82\x99"},
    {0x30d4, "\xe3\x83\x92\xe3\x82\x9a"},
    {0x30d6, "\xe3\x83\x95\xe3\x82\x99"},
    {0x30d7, "\xe3\x83\x95\xe3\x82\x9a"},
    {0x30d9, "\xe3\x83\x98\xe3\x82\x99"},
    {0x30da, "\xe3\x83\x98\xe3\x82\x9a"},
    {0x30dc, "\xe3\x83\x9b\xe3\x82\x99"},
    {0x30dd, "\xe3\x83\x9b\xe3\x82\x9a"},
    {0x30f4, "\xe3\x82\xa6\xe3\x82\x99"},
    {0x30f7, "\xe3\x83\xaf\xe3\x82\x99"},
    {0x30f8, "\xe3\x83\xb0\xe3\x82\x99"},
    {0x30f9, "\xe3\x83\xb1\xe3\x82\x99"},
    {0x30fa, "\xe3\x83\xb2\xe3\x82\x99"},
    {0x30fe, "\xe3\x83\xbd\xe3\x82\x99"},
    {0xfb2c, "\xd7\xa9\xd6\xbc\xd7\x81"},
    {0xfb2d, "\xd7\xa9\xd6\xbc\xd7\x82"},
    {0x105c9, "\xf0\x90\x97\x92\xcc\x87"},
    {0x105e4, "\xf0\x90\x97\x9a\xcc\x87"},
    {0x1109a, "\xf0\x91\x82\x99\xf0\x91\x82\xba"},
    {0x1109c, "\xf0\x91\x82\x9b\xf0\x91\x82\xba"},
    {0x110ab, "\xf0\x91\x82\xa5\xf0\x91\x82\xba"},
    {0x1112e, "\xf0\x91\x84\xb1\xf0\x91\x84\xa7"},
    {0x1112f, "\xf0\x91\x84\xb2\xf0\x91\x84\xa7"},
    {0x1134b, "\xf0\x91\x8d\x87\xf0\x91\x8c\xbe"},
    {0x1134c, "\xf0\x91\x8d\x87\xf0\x91\x8d\x97"},
    {0x11383, "\xf0\x91\x8e\x82\xf0\x91\x8f\x89"},
    {0x11385, "\xf0\x91\x8e\x84\xf0\x91\x8e\xbb"},
    {0x1138e, "\xf0\x91\x8e\x8b\xf0\x91\x8f\x82"},
    {0x11391, "\xf0\x91\x8e\x90\xf0\x91\x8f\x89"},
    {0x113c5, "\xf0\x91\x8f\x82\xf0\x91\x8f\x82"},
    {0x113c7, "\xf0\x91\x8f\x82\xf0\x91\x8e\xb8"},
    {0x113c8, "\xf0\x91\x8f\x82\xf0\x91\x8f\x89"},
    {0x114bb, "\xf0\x91\x92\xb9\xf0\x91\x92\xba"},
    {0x114bc, "\xf0\x91\x92\xb9\xf0\x91\x92\xb0"},
    {0x114be, "\xf0\x91\x92\xb9\xf0\x91\x92\xbd"},
    {0x115ba, "\xf0\x91\x96\xb8\xf0\x91\x96\xaf"},
    {0x115bb, "\xf0\x91\x96\xb9\xf0\x91\x96\xaf"},
    {0x11938, "\xf0\x91\xa4\xb5\xf0\x91\xa4\xb0"},
    {0x16121, "\xf0\x96\x84\x9e\xf0\x96\x84\x9e"},
    {0x16122, "\xf0\x96\x84\x9e\xf0\x96\x84\xa9"},
    {0x16123, "\xf0\x96\x84\x9e\xf0\x96\x84\x9f"},
    {0x16124, "\xf0\x96\x84\xa9\xf0\x96\x84\x9f"},
    {0x16125, "\xf0\x96\x84\x9e\xf0\x96\x84\xa0"},
    {0x16126, "\xf0\x96\x84\x9e\xf0\x96\x84\x9e\xf0\x96\x84\x9f"},
    {0x16127, "\xf0\x96\x84\x9e\xf0\x96\x84\xa9\xf0\x96\x84\x9f"},
    {0x16128, "\xf0\x96\x84\x9e\xf0\x96\x84\x9e\xf0\x96\x84\xa0"},
    {0x16d68, "\xf0\x96\xb5\xa7\xf0\x96\xb5\xa7"},
    {0x16d69, "\xf0\x96\xb5\xa3\xf0\x96\xb5\xa7"},
    {0x16d6a, "\xf0\x96\xb5\xa3\xf0\x96\xb5\xa7\xf0\x96\xb5\xa7"},
    {0x1d15e, "\xf0\x9d\x85\x97\xf0\x9d\x85\xa5"},
    {0x1d15f, "\xf0\x9d\x85\x98\xf0\x9d\x85\xa5"},
    {0x1d160, "\xf0\x9d\x85\x98\xf0\x9d\x85\xa5\xf0\x9d\x85\xae"},
    {0x1d161, "\xf0\x9d\x85\x98\xf0\x9d\x85\xa5\xf0\x9d\x85\xaf"},
    {0x1d162, "\xf0\x9d\x85\x98\xf0\x9d\x85\xa5\xf0\x9d\x85\xb0"},
    {0x1d163, "\xf0\x9d\x85\x98\xf0\x9d\x85\xa5\xf0\x9d\x85\xb1"},
    {0x1d164, "\xf0\x9d\x85\x98\xf0\x9d\x85\xa5\xf0\x9d\x85\xb2"},
    {0x1d1bb, "\xf0\x9d\x86\xb9\xf0\x9d\x85\xa5"},
    {0x1d1bc, "\xf0\x9d\x86\xba\xf0\x9d\x85\xa5"},
    {0x1d1bd, "\xf0\x9d\x86\xb9\xf0\x9d\x85\xa5\xf0\x9d\x85\xae"},
    {0x1d1be, "\xf0\x9d\x86\xba\xf0\x9d\x85\xa5\xf0\x9d\x85\xae"},
    {0x1d1bf, "\xf0\x9d\x86\xb9\xf0\x9d\x85\xa5\xf0\x9d\x85\xaf"},
    {0x1d1c0, "\xf0\x9d\x86\xba\xf0\x9d\x85\xa5\xf0\x9d\x85\xaf"}};

static const char *UN8IF_canon_tbl[5] = {
    (const char *)UN8IF_canon_tbl_1,
    (const char *)UN8IF_canon_tbl_2,
    (const char *)UN8IF_canon_tbl_3,
    (const char *)UN8IF_canon_tbl_4,
    (const char *)UN8IF_canon_tbl_5};

/* the rows */
static const uint16_t UN8IF_canon_00_00[256] = {
    /*   0000 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0008 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0010 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0018 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0020 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0028 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0030 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0038 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0040 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0048 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0050 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0058 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0060 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0068 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0070 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0078 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0088 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0090 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0098 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   00a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   00a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   00b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   00b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   00c0 */ TBL(3) | 3, TBL(3) | 4, TBL(3) | 5, TBL(3) | 6, TBL(3) | 10, TBL(3) | 12, 0, TBL(3) | 26,
    /*   00c8 */ TBL(3) | 33, TBL(3) | 34, TBL(3) | 35, TBL(3) | 40, TBL(3) | 65, TBL(3) | 66, TBL(3) | 67, TBL(3) | 72,
    /*   00d0 */ 0, TBL(3) | 97, TBL(3) | 104, TBL(3) | 105, TBL(3) | 106, TBL(3) | 107, TBL(3) | 111, 0,
    /*   00d8 */ 0, TBL(3) | 144, TBL(3) | 145, TBL(3) | 146, TBL(3) | 150, TBL(3) | 174, 0, 0,
    /*   00e0 */ TBL(3) | 984, TBL(3) | 985, TBL(3) | 986, TBL(3) | 987, TBL(3) | 991, TBL(3) | 993, 0, TBL(3) | 1007,
    /*   00e8 */ TBL(3) | 1014, TBL(3) | 1015, TBL(3) | 1016, TBL(3) | 1021, TBL(3) | 1047, TBL(3) | 1048, TBL(3) | 1049, TBL(3) | 1053,
    /*   00f0 */ 0, TBL(3) | 1079, TBL(3) | 1086, TBL(3) | 1087, TBL(3) | 1088, TBL(3) | 1089, TBL(3) | 1093, 0,
    /*   00f8 */ 0, TBL(3) | 1127, TBL(3) | 1128, TBL(3) | 1129, TBL(3) | 1133, TBL(3) | 1158, 0, TBL(3) | 1163};

static const uint16_t UN8IF_canon_00_01[256] = {
    /*   0100 */ TBL(3) | 7, TBL(3) | 988, TBL(3) | 8, TBL(3) | 989, TBL(3) | 18, TBL(3) | 999, TBL(3) | 22, TBL(3) | 1003,
    /*   0108 */ TBL(3) | 23, TBL(3) | 1004, TBL(3) | 24, TBL(3) | 1005, TBL(3) | 25, TBL(3) | 1006, TBL(3) | 28, TBL(3) | 1009,
    /*   0110 */ 0, 0, TBL(3) | 37, TBL(3) | 1018, TBL(3) | 38, TBL(3) | 1019, TBL(3) | 39, TBL(3) | 1020,
    /*   0118 */ TBL(3) | 47, TBL(3) | 1028, TBL(3) | 42, TBL(3) | 1023, TBL(3) | 52, TBL(3) | 1033, TBL(3) | 54, TBL(3) | 1035,
    /*   0120 */ TBL(3) | 55, TBL(3) | 1036, TBL(3) | 57, TBL(3) | 1038, TBL(3) | 58, TBL(3) | 1039, 0, 0,
    /*   0128 */ TBL(3) | 68, TBL(3) | 1050, TBL(3) | 69, TBL(3) | 1051, TBL(3) | 70, TBL(3) | 1052, TBL(3) | 78, TBL(3) | 1059,
    /*   0130 */ TBL(3) | 71, 0, 0, 0, TBL(3) | 80, TBL(3) | 1061, TBL(3) | 84, TBL(3) | 1066,
    /*   0138 */ 0, TBL(3) | 86, TBL(3) | 1068, TBL(3) | 89, TBL(3) | 1071, TBL(3) | 87, TBL(3) | 1069, 0,
    /*   0140 */ 0, 0, 0, TBL(3) | 96, TBL(3) | 1078, TBL(3) | 101, TBL(3) | 1083, TBL(3) | 99,
    /*   0148 */ TBL(3) | 1081, 0, 0, 0, TBL(3) | 108, TBL(3) | 1090, TBL(3) | 109, TBL(3) | 1091,
    /*   0150 */ TBL(3) | 113, TBL(3) | 1095, 0, 0, TBL(3) | 122, TBL(3) | 1104, TBL(3) | 128, TBL(3) | 1110,
    /*   0158 */ TBL(3) | 124, TBL(3) | 1106, TBL(3) | 130, TBL(3) | 1112, TBL(3) | 131, TBL(3) | 1113, TBL(3) | 136, TBL(3) | 1118,
    /*   0160 */ TBL(3) | 133, TBL(3) | 1115, TBL(3) | 141, TBL(3) | 1124, TBL(3) | 138, TBL(3) | 1121, 0, 0,
    /*   0168 */ TBL(3) | 147, TBL(3) | 1130, TBL(3) | 148, TBL(3) | 1131, TBL(3) | 149, TBL(3) | 1132, TBL(3) | 152, TBL(3) | 1135,
    /*   0170 */ TBL(3) | 153, TBL(3) | 1136, TBL(3) | 160, TBL(3) | 1143, TBL(3) | 167, TBL(3) | 1150, TBL(3) | 175, TBL(3) | 1159,
    /*   0178 */ TBL(3) | 179, TBL(3) | 182, TBL(3) | 1167, TBL(3) | 184, TBL(3) | 1169, TBL(3) | 185, TBL(3) | 1170, 0,
    /*   0180 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0188 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0198 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   01a0 */ TBL(3) | 117, TBL(3) | 1099, 0, 0, 0, 0, 0, 0,
    /*   01a8 */ 0, 0, 0, 0, 0, 0, 0, TBL(3) | 157,
    /*   01b0 */ TBL(3) | 1140, 0, 0, 0, 0, 0, 0, 0,
    /*   01b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   01c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   01c8 */ 0, 0, 0, 0, 0, TBL(3) | 13, TBL(3) | 994, TBL(3) | 74,
    /*   01d0 */ TBL(3) | 1055, TBL(3) | 114, TBL(3) | 1096, TBL(3) | 154, TBL(3) | 1137, TBL(5) | 50, TBL(5) | 155, TBL(5) | 49,
    /*   01d8 */ TBL(5) | 154, TBL(5) | 51, TBL(5) | 156, TBL(5) | 48, TBL(5) | 153, 0, TBL(5) | 9, TBL(5) | 114,
    /*   01e0 */ TBL(5) | 8, TBL(5) | 113, TBL(4) | 4, TBL(4) | 7, 0, 0, TBL(3) | 56, TBL(3) | 1037,
    /*   01e8 */ TBL(3) | 82, TBL(3) | 1064, TBL(3) | 119, TBL(3) | 1101, TBL(5) | 41, TBL(5) | 146, TBL(4) | 10, TBL(4) | 11,
    /*   01f0 */ TBL(3) | 1062, 0, 0, 0, TBL(3) | 51, TBL(3) | 1032, 0, 0,
    /*   01f8 */ TBL(3) | 95, TBL(3) | 1077, TBL(5) | 10, TBL(5) | 115, TBL(4) | 3, TBL(4) | 6, TBL(4) | 5, TBL(4) | 8};

static const uint16_t UN8IF_canon_00_02[256] = {
    /*   0200 */ TBL(3) | 14, TBL(3) | 995, TBL(3) | 15, TBL(3) | 996, TBL(3) | 43, TBL(3) | 1024, TBL(3) | 44, TBL(3) | 1025,
    /*   0208 */ TBL(3) | 75, TBL(3) | 1056, TBL(3) | 76, TBL(3) | 1057, TBL(3) | 115, TBL(3) | 1097, TBL(3) | 116, TBL(3) | 1098,
    /*   0210 */ TBL(3) | 125, TBL(3) | 1107, TBL(3) | 126, TBL(3) | 1108, TBL(3) | 155, TBL(3) | 1138, TBL(3) | 156, TBL(3) | 1139,
    /*   0218 */ TBL(3) | 135, TBL(3) | 1117, TBL(3) | 140, TBL(3) | 1123, 0, 0, TBL(3) | 61, TBL(3) | 1042,
    /*   0220 */ 0, 0, 0, 0, 0, 0, TBL(3) | 9, TBL(3) | 990,
    /*   0228 */ TBL(3) | 46, TBL(3) | 1027, TBL(5) | 34, TBL(5) | 139, TBL(5) | 29, TBL(5) | 134, TBL(3) | 110, TBL(3) | 1092,
    /*   0230 */ TBL(5) | 33, TBL(5) | 138, TBL(3) | 177, TBL(3) | 1161, 0, 0, 0, 0,
    /*   0238 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0240 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0248 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0250 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0258 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0260 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0268 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0270 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0278 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0280 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0288 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0290 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0298 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   02f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_03[256] = {
    /*   0300 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0308 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0310 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0318 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0320 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0328 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0330 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0338 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0340 */ TBL(2) | 3, TBL(2) | 4, 0, TBL(2) | 5, TBL(4) | 12, 0, 0, 0,
    /*   0348 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0350 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0358 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0360 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0368 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0370 */ 0, 0, 0, 0, TBL(2) | 2, 0, 0, 0,
    /*   0378 */ 0, 0, 0, 0, 0, 0, TBL(1) | 0, 0,
    /*   0380 */ 0, 0, 0, 0, 0, TBL(4) | 1, TBL(4) | 14, TBL(2) | 1,
    /*   0388 */ TBL(4) | 21, TBL(4) | 25, TBL(4) | 30, 0, TBL(4) | 37, 0, TBL(4) | 42, TBL(4) | 48,
    /*   0390 */ (uint16_t)-1 /*TBL(6)|68*/, 0, 0, 0, 0, 0, 0, 0,
    /*   0398 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03a8 */ 0, 0, TBL(4) | 33, TBL(4) | 45, TBL(4) | 53, TBL(4) | 61, TBL(4) | 65, TBL(4) | 71,
    /*   03b0 */ (uint16_t)-1 /*TBL(6)|81*/, 0, 0, 0, 0, 0, 0, 0,
    /*   03b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03c8 */ 0, 0, TBL(4) | 74, TBL(4) | 88, TBL(4) | 79, TBL(4) | 85, TBL(4) | 93, 0,
    /*   03d0 */ 0, 0, 0, TBL(4) | 98, TBL(4) | 99, 0, 0, 0,
    /*   03d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_04[256] = {
    /*   0400 */ TBL(4) | 104, TBL(4) | 106, 0, TBL(4) | 103, 0, 0, 0, TBL(4) | 100,
    /*   0408 */ 0, 0, 0, 0, TBL(4) | 114, TBL(4) | 110, TBL(4) | 117, 0,
    /*   0410 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0418 */ 0, TBL(4) | 112, 0, 0, 0, 0, 0, 0,
    /*   0420 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0428 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0430 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0438 */ 0, TBL(4) | 134, 0, 0, 0, 0, 0, 0,
    /*   0440 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0448 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0450 */ TBL(4) | 126, TBL(4) | 128, 0, TBL(4) | 125, 0, 0, 0, TBL(4) | 145,
    /*   0458 */ 0, 0, 0, 0, TBL(4) | 136, TBL(4) | 132, TBL(4) | 139, 0,
    /*   0460 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0468 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0470 */ 0, 0, 0, 0, 0, 0, TBL(4) | 146, TBL(4) | 147,
    /*   0478 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0480 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0488 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0490 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0498 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04c0 */ 0, TBL(4) | 107, TBL(4) | 129, 0, 0, 0, 0, 0,
    /*   04c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04d0 */ TBL(4) | 101, TBL(4) | 123, TBL(4) | 102, TBL(4) | 124, 0, 0, TBL(4) | 105, TBL(4) | 127,
    /*   04d8 */ 0, 0, TBL(4) | 148, TBL(4) | 149, TBL(4) | 108, TBL(4) | 130, TBL(4) | 109, TBL(4) | 131,
    /*   04e0 */ 0, 0, TBL(4) | 111, TBL(4) | 133, TBL(4) | 113, TBL(4) | 135, TBL(4) | 115, TBL(4) | 137,
    /*   04e8 */ 0, 0, TBL(4) | 150, TBL(4) | 151, TBL(4) | 122, TBL(4) | 144, TBL(4) | 116, TBL(4) | 138,
    /*   04f0 */ TBL(4) | 118, TBL(4) | 140, TBL(4) | 119, TBL(4) | 141, TBL(4) | 120, TBL(4) | 142, 0, 0,
    /*   04f8 */ TBL(4) | 121, TBL(4) | 143, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_06[256] = {
    /*   0600 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0608 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0610 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0618 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0620 */ 0, 0, TBL(4) | 184, TBL(4) | 185, TBL(4) | 187, TBL(4) | 186, TBL(4) | 188, 0,
    /*   0628 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0630 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0638 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0640 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0648 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0650 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0658 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0660 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0668 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0670 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0678 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0680 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0688 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0690 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0698 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06c0 */ TBL(4) | 191, 0, TBL(4) | 189, 0, 0, 0, 0, 0,
    /*   06c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06d0 */ 0, 0, 0, TBL(4) | 190, 0, 0, 0, 0,
    /*   06d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_09[256] = {
    /*   0900 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0908 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0910 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0918 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0920 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0928 */ 0, (uint16_t)-1 /*TBL(6)|108*/, 0, 0, 0, 0, 0, 0,
    /*   0930 */ 0, (uint16_t)-1 /*TBL(6)|111*/, 0, 0, (uint16_t)-1 /*TBL(6)|112*/, 0, 0, 0,
    /*   0938 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0940 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0948 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0950 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0958 */ (uint16_t)-1 /*TBL(6)|102*/, (uint16_t)-1 /*TBL(6)|103*/, (uint16_t)-1 /*TBL(6)|104*/, (uint16_t)-1 /*TBL(6)|105*/, (uint16_t)-1 /*TBL(6)|106*/, (uint16_t)-1 /*TBL(6)|107*/, (uint16_t)-1 /*TBL(6)|109*/, (uint16_t)-1 /*TBL(6)|110*/,
    /*   0960 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0968 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0970 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0978 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0980 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0988 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0990 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0998 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09c8 */ 0, 0, 0, (uint16_t)-1 /*TBL(6)|116*/, (uint16_t)-1 /*TBL(6)|117*/, 0, 0, 0,
    /*   09d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09d8 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|113*/, (uint16_t)-1 /*TBL(6)|114*/, 0, (uint16_t)-1 /*TBL(6)|115*/,
    /*   09e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_0a[256] = {
    /*   0a00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a30 */ 0, 0, 0, (uint16_t)-1 /*TBL(6)|122*/, 0, 0, (uint16_t)-1 /*TBL(6)|123*/, 0,
    /*   0a38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a58 */ 0, (uint16_t)-1 /*TBL(6)|118*/, (uint16_t)-1 /*TBL(6)|119*/, (uint16_t)-1 /*TBL(6)|120*/, 0, 0, (uint16_t)-1 /*TBL(6)|121*/, 0,
    /*   0a60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0aa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0aa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ab0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ab8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ac0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ac8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ad0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ad8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ae0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ae8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0af0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0af8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_0b[256] = {
    /*   0b00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b48 */ (uint16_t)-1 /*TBL(6)|127*/, 0, 0, (uint16_t)-1 /*TBL(6)|126*/, (uint16_t)-1 /*TBL(6)|128*/, 0, 0, 0,
    /*   0b50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b58 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|124*/, (uint16_t)-1 /*TBL(6)|125*/, 0, 0,
    /*   0b60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b90 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|129*/, 0, 0, 0,
    /*   0b98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ba0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ba8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bc8 */ 0, 0, (uint16_t)-1 /*TBL(6)|130*/, (uint16_t)-1 /*TBL(6)|132*/, (uint16_t)-1 /*TBL(6)|131*/, 0, 0, 0,
    /*   0bd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0be0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0be8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_0c[256] = {
    /*   0c00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c48 */ (uint16_t)-1 /*TBL(6)|133*/, 0, 0, 0, 0, 0, 0, 0,
    /*   0c50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ca0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ca8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cc0 */ (uint16_t)-1 /*TBL(6)|134*/, 0, 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|136*/,
    /*   0cc8 */ (uint16_t)-1 /*TBL(6)|137*/, 0, (uint16_t)-1 /*TBL(6)|135*/, (uint16_t)-1 /*TBL(9)|0*/, 0, 0, 0, 0,
    /*   0cd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ce0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ce8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_0d[256] = {
    /*   0d00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d48 */ 0, 0, (uint16_t)-1 /*TBL(6)|138*/, (uint16_t)-1 /*TBL(6)|140*/, (uint16_t)-1 /*TBL(6)|139*/, 0, 0, 0,
    /*   0d50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0da0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0da8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0db0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0db8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0dc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0dc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0dd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0dd8 */ 0, 0, (uint16_t)-1 /*TBL(6)|141*/, 0, (uint16_t)-1 /*TBL(6)|142*/, (uint16_t)-1 /*TBL(9)|1*/, (uint16_t)-1 /*TBL(6)|143*/, 0,
    /*   0de0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0de8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0df0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0df8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_0f[256] = {
    /*   0f00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f40 */ 0, 0, 0, (uint16_t)-1 /*TBL(6)|145*/, 0, 0, 0, 0,
    /*   0f48 */ 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|146*/, 0, 0,
    /*   0f50 */ 0, 0, (uint16_t)-1 /*TBL(6)|147*/, 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|148*/,
    /*   0f58 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|149*/, 0, 0, 0,
    /*   0f60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f68 */ 0, (uint16_t)-1 /*TBL(6)|144*/, 0, 0, 0, 0, 0, 0,
    /*   0f70 */ 0, 0, 0, (uint16_t)-1 /*TBL(6)|150*/, 0, (uint16_t)-1 /*TBL(6)|151*/, (uint16_t)-1 /*TBL(6)|159*/, 0,
    /*   0f78 */ (uint16_t)-1 /*TBL(6)|160*/, 0, 0, 0, 0, 0, 0, 0,
    /*   0f80 */ 0, (uint16_t)-1 /*TBL(6)|152*/, 0, 0, 0, 0, 0, 0,
    /*   0f88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f90 */ 0, 0, 0, (uint16_t)-1 /*TBL(6)|154*/, 0, 0, 0, 0,
    /*   0f98 */ 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|155*/, 0, 0,
    /*   0fa0 */ 0, 0, (uint16_t)-1 /*TBL(6)|156*/, 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|157*/,
    /*   0fa8 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|158*/, 0, 0, 0,
    /*   0fb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fb8 */ 0, (uint16_t)-1 /*TBL(6)|153*/, 0, 0, 0, 0, 0, 0,
    /*   0fc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fe0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fe8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ff0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ff8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_10[256] = {
    /*   1000 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1008 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1010 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1018 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1020 */ 0, 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|161*/, 0,
    /*   1028 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1030 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1038 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1040 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1048 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1050 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1058 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1060 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1068 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1070 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1078 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1088 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1090 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1098 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_1b[256] = {
    /*   1b00 */ 0, 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|162*/, 0,
    /*   1b08 */ (uint16_t)-1 /*TBL(6)|163*/, 0, (uint16_t)-1 /*TBL(6)|164*/, 0, (uint16_t)-1 /*TBL(6)|165*/, 0, (uint16_t)-1 /*TBL(6)|166*/, 0,
    /*   1b10 */ 0, 0, (uint16_t)-1 /*TBL(6)|167*/, 0, 0, 0, 0, 0,
    /*   1b18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b38 */ 0, 0, 0, (uint16_t)-1 /*TBL(6)|168*/, 0, (uint16_t)-1 /*TBL(6)|169*/, 0, 0,
    /*   1b40 */ (uint16_t)-1 /*TBL(6)|170*/, (uint16_t)-1 /*TBL(6)|171*/, 0, (uint16_t)-1 /*TBL(6)|172*/, 0, 0, 0, 0,
    /*   1b48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1ba0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1ba8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1be0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1be8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_1e[256] = {
    /*   1e00 */ TBL(3) | 17, TBL(3) | 998, TBL(3) | 19, TBL(3) | 1000, TBL(3) | 20, TBL(3) | 1001, TBL(3) | 21, TBL(3) | 1002,
    /*   1e08 */ TBL(5) | 13, TBL(5) | 118, TBL(3) | 27, TBL(3) | 1008, TBL(3) | 29, TBL(3) | 1010, TBL(3) | 32, TBL(3) | 1013,
    /*   1e10 */ TBL(3) | 30, TBL(3) | 1011, TBL(3) | 31, TBL(3) | 1012, TBL(5) | 18, TBL(5) | 123, TBL(5) | 19, TBL(5) | 124,
    /*   1e18 */ TBL(3) | 48, TBL(3) | 1029, TBL(3) | 49, TBL(3) | 1030, TBL(5) | 21, TBL(5) | 126, TBL(3) | 50, TBL(3) | 1031,
    /*   1e20 */ TBL(3) | 53, TBL(3) | 1034, TBL(3) | 59, TBL(3) | 1040, TBL(3) | 62, TBL(3) | 1043, TBL(3) | 60, TBL(3) | 1041,
    /*   1e28 */ TBL(3) | 63, TBL(3) | 1044, TBL(3) | 64, TBL(3) | 1045, TBL(3) | 79, TBL(3) | 1060, TBL(5) | 22, TBL(5) | 127,
    /*   1e30 */ TBL(3) | 81, TBL(3) | 1063, TBL(3) | 83, TBL(3) | 1065, TBL(3) | 85, TBL(3) | 1067, TBL(3) | 88, TBL(3) | 1070,
    /*   1e38 */ TBL(5) | 23, TBL(5) | 128, TBL(3) | 91, TBL(3) | 1073, TBL(3) | 90, TBL(3) | 1072, TBL(3) | 92, TBL(3) | 1074,
    /*   1e40 */ TBL(3) | 93, TBL(3) | 1075, TBL(3) | 94, TBL(3) | 1076, TBL(3) | 98, TBL(3) | 1080, TBL(3) | 100, TBL(3) | 1082,
    /*   1e48 */ TBL(3) | 103, TBL(3) | 1085, TBL(3) | 102, TBL(3) | 1084, TBL(5) | 28, TBL(5) | 133, TBL(5) | 30, TBL(5) | 135,
    /*   1e50 */ TBL(5) | 31, TBL(5) | 136, TBL(5) | 32, TBL(5) | 137, TBL(3) | 120, TBL(3) | 1102, TBL(3) | 121, TBL(3) | 1103,
    /*   1e58 */ TBL(3) | 123, TBL(3) | 1105, TBL(3) | 127, TBL(3) | 1109, TBL(5) | 42, TBL(5) | 147, TBL(3) | 129, TBL(3) | 1111,
    /*   1e60 */ TBL(3) | 132, TBL(3) | 1114, TBL(3) | 134, TBL(3) | 1116, TBL(5) | 43, TBL(5) | 148, TBL(5) | 44, TBL(5) | 149,
    /*   1e68 */ TBL(5) | 45, TBL(5) | 150, TBL(3) | 137, TBL(3) | 1119, TBL(3) | 139, TBL(3) | 1122, TBL(3) | 143, TBL(3) | 1126,
    /*   1e70 */ TBL(3) | 142, TBL(3) | 1125, TBL(3) | 159, TBL(3) | 1142, TBL(3) | 162, TBL(3) | 1145, TBL(3) | 161, TBL(3) | 1144,
    /*   1e78 */ TBL(5) | 46, TBL(5) | 151, TBL(5) | 47, TBL(5) | 152, TBL(3) | 163, TBL(3) | 1146, TBL(3) | 164, TBL(3) | 1147,
    /*   1e80 */ TBL(3) | 165, TBL(3) | 1148, TBL(3) | 166, TBL(3) | 1149, TBL(3) | 169, TBL(3) | 1152, TBL(3) | 168, TBL(3) | 1151,
    /*   1e88 */ TBL(3) | 170, TBL(3) | 1154, TBL(3) | 171, TBL(3) | 1155, TBL(3) | 172, TBL(3) | 1156, TBL(3) | 178, TBL(3) | 1162,
    /*   1e90 */ TBL(3) | 183, TBL(3) | 1168, TBL(3) | 186, TBL(3) | 1171, TBL(3) | 187, TBL(3) | 1172, TBL(3) | 1046, TBL(3) | 1120,
    /*   1e98 */ TBL(3) | 1153, TBL(3) | 1165, 0, TBL(4) | 9, 0, 0, 0, 0,
    /*   1ea0 */ TBL(3) | 16, TBL(3) | 997, TBL(3) | 11, TBL(3) | 992, TBL(5) | 1, TBL(5) | 106, TBL(5) | 0, TBL(5) | 105,
    /*   1ea8 */ TBL(5) | 3, TBL(5) | 108, TBL(5) | 2, TBL(5) | 107, TBL(5) | 11, TBL(5) | 116, TBL(5) | 5, TBL(5) | 110,
    /*   1eb0 */ TBL(5) | 4, TBL(5) | 109, TBL(5) | 7, TBL(5) | 112, TBL(5) | 6, TBL(5) | 111, TBL(5) | 12, TBL(5) | 117,
    /*   1eb8 */ TBL(3) | 45, TBL(3) | 1026, TBL(3) | 41, TBL(3) | 1022, TBL(3) | 36, TBL(3) | 1017, TBL(5) | 15, TBL(5) | 120,
    /*   1ec0 */ TBL(5) | 14, TBL(5) | 119, TBL(5) | 17, TBL(5) | 122, TBL(5) | 16, TBL(5) | 121, TBL(5) | 20, TBL(5) | 125,
    /*   1ec8 */ TBL(3) | 73, TBL(3) | 1054, TBL(3) | 77, TBL(3) | 1058, TBL(3) | 118, TBL(3) | 1100, TBL(3) | 112, TBL(3) | 1094,
    /*   1ed0 */ TBL(5) | 25, TBL(5) | 130, TBL(5) | 24, TBL(5) | 129, TBL(5) | 27, TBL(5) | 132, TBL(5) | 26, TBL(5) | 131,
    /*   1ed8 */ TBL(5) | 40, TBL(5) | 145, TBL(5) | 36, TBL(5) | 141, TBL(5) | 35, TBL(5) | 140, TBL(5) | 38, TBL(5) | 143,
    /*   1ee0 */ TBL(5) | 37, TBL(5) | 142, TBL(5) | 39, TBL(5) | 144, TBL(3) | 158, TBL(3) | 1141, TBL(3) | 151, TBL(3) | 1134,
    /*   1ee8 */ TBL(5) | 53, TBL(5) | 158, TBL(5) | 52, TBL(5) | 157, TBL(5) | 55, TBL(5) | 160, TBL(5) | 54, TBL(5) | 159,
    /*   1ef0 */ TBL(5) | 56, TBL(5) | 161, TBL(3) | 173, TBL(3) | 1157, TBL(3) | 181, TBL(3) | 1166, TBL(3) | 180, TBL(3) | 1164,
    /*   1ef8 */ TBL(3) | 176, TBL(3) | 1160, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_1f[256] = {
    /*   1f00 */ TBL(4) | 56, TBL(4) | 57, (uint16_t)-1 /*TBL(6)|43*/, (uint16_t)-1 /*TBL(6)|47*/, (uint16_t)-1 /*TBL(6)|44*/, (uint16_t)-1 /*TBL(6)|48*/, (uint16_t)-1 /*TBL(6)|45*/, (uint16_t)-1 /*TBL(6)|49*/,
    /*   1f08 */ TBL(4) | 17, TBL(4) | 18, (uint16_t)-1 /*TBL(6)|0*/, (uint16_t)-1 /*TBL(6)|4*/, (uint16_t)-1 /*TBL(6)|1*/, (uint16_t)-1 /*TBL(6)|5*/, (uint16_t)-1 /*TBL(6)|2*/, (uint16_t)-1 /*TBL(6)|6*/,
    /*   1f10 */ TBL(4) | 62, TBL(4) | 63, (uint16_t)-1 /*TBL(6)|52*/, (uint16_t)-1 /*TBL(6)|54*/, (uint16_t)-1 /*TBL(6)|53*/, (uint16_t)-1 /*TBL(6)|55*/, 0, 0,
    /*   1f18 */ TBL(4) | 22, TBL(4) | 23, (uint16_t)-1 /*TBL(6)|8*/, (uint16_t)-1 /*TBL(6)|10*/, (uint16_t)-1 /*TBL(6)|9*/, (uint16_t)-1 /*TBL(6)|11*/, 0, 0,
    /*   1f20 */ TBL(4) | 66, TBL(4) | 67, (uint16_t)-1 /*TBL(6)|58*/, (uint16_t)-1 /*TBL(6)|62*/, (uint16_t)-1 /*TBL(6)|59*/, (uint16_t)-1 /*TBL(6)|63*/, (uint16_t)-1 /*TBL(6)|60*/, (uint16_t)-1 /*TBL(6)|64*/,
    /*   1f28 */ TBL(4) | 26, TBL(4) | 27, (uint16_t)-1 /*TBL(6)|12*/, (uint16_t)-1 /*TBL(6)|16*/, (uint16_t)-1 /*TBL(6)|13*/, (uint16_t)-1 /*TBL(6)|17*/, (uint16_t)-1 /*TBL(6)|14*/, (uint16_t)-1 /*TBL(6)|18*/,
    /*   1f30 */ TBL(4) | 75, TBL(4) | 76, (uint16_t)-1 /*TBL(6)|70*/, (uint16_t)-1 /*TBL(6)|73*/, (uint16_t)-1 /*TBL(6)|71*/, (uint16_t)-1 /*TBL(6)|74*/, (uint16_t)-1 /*TBL(6)|72*/, (uint16_t)-1 /*TBL(6)|75*/,
    /*   1f38 */ TBL(4) | 34, TBL(4) | 35, (uint16_t)-1 /*TBL(6)|20*/, (uint16_t)-1 /*TBL(6)|23*/, (uint16_t)-1 /*TBL(6)|21*/, (uint16_t)-1 /*TBL(6)|24*/, (uint16_t)-1 /*TBL(6)|22*/, (uint16_t)-1 /*TBL(6)|25*/,
    /*   1f40 */ TBL(4) | 80, TBL(4) | 81, (uint16_t)-1 /*TBL(6)|76*/, (uint16_t)-1 /*TBL(6)|78*/, (uint16_t)-1 /*TBL(6)|77*/, (uint16_t)-1 /*TBL(6)|79*/, 0, 0,
    /*   1f48 */ TBL(4) | 38, TBL(4) | 39, (uint16_t)-1 /*TBL(6)|26*/, (uint16_t)-1 /*TBL(6)|28*/, (uint16_t)-1 /*TBL(6)|27*/, (uint16_t)-1 /*TBL(6)|29*/, 0, 0,
    /*   1f50 */ TBL(4) | 89, TBL(4) | 90, (uint16_t)-1 /*TBL(6)|83*/, (uint16_t)-1 /*TBL(6)|86*/, (uint16_t)-1 /*TBL(6)|84*/, (uint16_t)-1 /*TBL(6)|87*/, (uint16_t)-1 /*TBL(6)|85*/, (uint16_t)-1 /*TBL(6)|88*/,
    /*   1f58 */ 0, TBL(4) | 46, 0, (uint16_t)-1 /*TBL(6)|30*/, 0, (uint16_t)-1 /*TBL(6)|31*/, 0, (uint16_t)-1 /*TBL(6)|32*/,
    /*   1f60 */ TBL(4) | 94, TBL(4) | 95, (uint16_t)-1 /*TBL(6)|91*/, (uint16_t)-1 /*TBL(6)|95*/, (uint16_t)-1 /*TBL(6)|92*/, (uint16_t)-1 /*TBL(6)|96*/, (uint16_t)-1 /*TBL(6)|93*/, (uint16_t)-1 /*TBL(6)|97*/,
    /*   1f68 */ TBL(4) | 49, TBL(4) | 50, (uint16_t)-1 /*TBL(6)|33*/, (uint16_t)-1 /*TBL(6)|37*/, (uint16_t)-1 /*TBL(6)|34*/, (uint16_t)-1 /*TBL(6)|38*/, (uint16_t)-1 /*TBL(6)|35*/, (uint16_t)-1 /*TBL(6)|39*/,
    /*   1f70 */ TBL(4) | 52, TBL(4) | 53, TBL(4) | 60, TBL(4) | 61, TBL(4) | 64, TBL(4) | 65, TBL(4) | 70, TBL(4) | 71,
    /*   1f78 */ TBL(4) | 78, TBL(4) | 79, TBL(4) | 84, TBL(4) | 85, TBL(4) | 92, TBL(4) | 93, 0, 0,
    /*   1f80 */ (uint16_t)-1 /*TBL(6)|46*/, (uint16_t)-1 /*TBL(6)|50*/, (uint16_t)-1 /*TBL(8)|18*/, (uint16_t)-1 /*TBL(8)|21*/, (uint16_t)-1 /*TBL(8)|19*/, (uint16_t)-1 /*TBL(8)|22*/, (uint16_t)-1 /*TBL(8)|20*/, (uint16_t)-1 /*TBL(8)|23*/,
    /*   1f88 */ (uint16_t)-1 /*TBL(6)|3*/, (uint16_t)-1 /*TBL(6)|7*/, (uint16_t)-1 /*TBL(8)|0*/, (uint16_t)-1 /*TBL(8)|3*/, (uint16_t)-1 /*TBL(8)|1*/, (uint16_t)-1 /*TBL(8)|4*/, (uint16_t)-1 /*TBL(8)|2*/, (uint16_t)-1 /*TBL(8)|5*/,
    /*   1f90 */ (uint16_t)-1 /*TBL(6)|61*/, (uint16_t)-1 /*TBL(6)|65*/, (uint16_t)-1 /*TBL(8)|24*/, (uint16_t)-1 /*TBL(8)|27*/, (uint16_t)-1 /*TBL(8)|25*/, (uint16_t)-1 /*TBL(8)|28*/, (uint16_t)-1 /*TBL(8)|26*/, (uint16_t)-1 /*TBL(8)|29*/,
    /*   1f98 */ (uint16_t)-1 /*TBL(6)|15*/, (uint16_t)-1 /*TBL(6)|19*/, (uint16_t)-1 /*TBL(8)|6*/, (uint16_t)-1 /*TBL(8)|9*/, (uint16_t)-1 /*TBL(8)|7*/, (uint16_t)-1 /*TBL(8)|10*/, (uint16_t)-1 /*TBL(8)|8*/, (uint16_t)-1 /*TBL(8)|11*/,
    /*   1fa0 */ (uint16_t)-1 /*TBL(6)|94*/, (uint16_t)-1 /*TBL(6)|98*/, (uint16_t)-1 /*TBL(8)|30*/, (uint16_t)-1 /*TBL(8)|33*/, (uint16_t)-1 /*TBL(8)|31*/, (uint16_t)-1 /*TBL(8)|34*/, (uint16_t)-1 /*TBL(8)|32*/, (uint16_t)-1 /*TBL(8)|35*/,
    /*   1fa8 */ (uint16_t)-1 /*TBL(6)|36*/, (uint16_t)-1 /*TBL(6)|40*/, (uint16_t)-1 /*TBL(8)|12*/, (uint16_t)-1 /*TBL(8)|15*/, (uint16_t)-1 /*TBL(8)|13*/, (uint16_t)-1 /*TBL(8)|16*/, (uint16_t)-1 /*TBL(8)|14*/, (uint16_t)-1 /*TBL(8)|17*/,
    /*   1fb0 */ TBL(4) | 55, TBL(4) | 54, (uint16_t)-1 /*TBL(6)|41*/, TBL(4) | 59, (uint16_t)-1 /*TBL(6)|42*/, 0, TBL(4) | 58, (uint16_t)-1 /*TBL(6)|51*/,
    /*   1fb8 */ TBL(4) | 16, TBL(4) | 15, TBL(4) | 13, TBL(4) | 14, TBL(4) | 19, 0, TBL(2) | 7, 0,
    /*   1fc0 */ 0, TBL(4) | 2, (uint16_t)-1 /*TBL(6)|56*/, TBL(4) | 69, (uint16_t)-1 /*TBL(6)|57*/, 0, TBL(4) | 68, (uint16_t)-1 /*TBL(6)|66*/,
    /*   1fc8 */ TBL(4) | 20, TBL(4) | 21, TBL(4) | 24, TBL(4) | 25, TBL(4) | 28, TBL(5) | 57, TBL(5) | 58, TBL(5) | 59,
    /*   1fd0 */ TBL(4) | 73, TBL(4) | 72, (uint16_t)-1 /*TBL(6)|67*/, (uint16_t)-1 /*TBL(6)|68*/, 0, 0, TBL(4) | 77, (uint16_t)-1 /*TBL(6)|69*/,
    /*   1fd8 */ TBL(4) | 32, TBL(4) | 31, TBL(4) | 29, TBL(4) | 30, 0, TBL(5) | 60, TBL(5) | 61, TBL(5) | 62,
    /*   1fe0 */ TBL(4) | 87, TBL(4) | 86, (uint16_t)-1 /*TBL(6)|80*/, (uint16_t)-1 /*TBL(6)|81*/, TBL(4) | 82, TBL(4) | 83, TBL(4) | 91, (uint16_t)-1 /*TBL(6)|82*/,
    /*   1fe8 */ TBL(4) | 44, TBL(4) | 43, TBL(4) | 41, TBL(4) | 42, TBL(4) | 40, TBL(4) | 0, TBL(4) | 1, TBL(1) | 2,
    /*   1ff0 */ 0, 0, (uint16_t)-1 /*TBL(6)|89*/, TBL(4) | 97, (uint16_t)-1 /*TBL(6)|90*/, 0, TBL(4) | 96, (uint16_t)-1 /*TBL(6)|99*/,
    /*   1ff8 */ TBL(4) | 36, TBL(4) | 37, TBL(4) | 47, TBL(4) | 48, TBL(4) | 51, TBL(2) | 0, 0, 0};

static const uint16_t UN8IF_canon_00_20[256] = {
    /*   2000 */ TBL(3) | 188, TBL(3) | 189, 0, 0, 0, 0, 0, 0,
    /*   2008 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2010 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2018 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2020 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2028 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2030 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2038 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2040 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2048 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2050 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2058 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2060 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2068 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2070 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2078 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2088 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2090 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2098 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_21[256] = {
    /*   2100 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2108 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2110 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2118 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2120 */ 0, 0, 0, 0, 0, 0, TBL(2) | 6, 0,
    /*   2128 */ 0, 0, TBL(1) | 1, TBL(3) | 12, 0, 0, 0, 0,
    /*   2130 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2138 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2140 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2148 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2150 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2158 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2160 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2168 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2170 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2178 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2180 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2188 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2198 */ 0, 0, TBL(5) | 63, TBL(5) | 64, 0, 0, 0, 0,
    /*   21a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   21a8 */ 0, 0, 0, 0, 0, 0, TBL(5) | 65, 0,
    /*   21b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   21b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   21c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   21c8 */ 0, 0, 0, 0, 0, TBL(5) | 66, TBL(5) | 68, TBL(5) | 67,
    /*   21d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   21d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   21e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   21e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   21f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   21f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_22[256] = {
    /*   2200 */ 0, 0, 0, 0, TBL(5) | 69, 0, 0, 0,
    /*   2208 */ 0, TBL(5) | 70, 0, 0, TBL(5) | 71, 0, 0, 0,
    /*   2210 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2218 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2220 */ 0, 0, 0, 0, TBL(5) | 72, 0, TBL(5) | 73, 0,
    /*   2228 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2230 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2238 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2240 */ 0, TBL(5) | 74, 0, 0, TBL(5) | 75, 0, 0, TBL(5) | 76,
    /*   2248 */ 0, TBL(5) | 77, 0, 0, 0, 0, 0, 0,
    /*   2250 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2258 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2260 */ TBL(3) | 1, 0, TBL(5) | 79, 0, 0, 0, 0, 0,
    /*   2268 */ 0, 0, 0, 0, 0, TBL(5) | 78, TBL(3) | 0, TBL(3) | 2,
    /*   2270 */ TBL(5) | 80, TBL(5) | 81, 0, 0, TBL(5) | 82, TBL(5) | 83, 0, 0,
    /*   2278 */ TBL(5) | 84, TBL(5) | 85, 0, 0, 0, 0, 0, 0,
    /*   2280 */ TBL(5) | 86, TBL(5) | 87, 0, 0, TBL(5) | 90, TBL(5) | 91, 0, 0,
    /*   2288 */ TBL(5) | 92, TBL(5) | 93, 0, 0, 0, 0, 0, 0,
    /*   2290 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2298 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   22a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   22a8 */ 0, 0, 0, 0, TBL(5) | 96, TBL(5) | 97, TBL(5) | 98, TBL(5) | 99,
    /*   22b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   22b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   22c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   22c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   22d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   22d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   22e0 */ TBL(5) | 88, TBL(5) | 89, TBL(5) | 94, TBL(5) | 95, 0, 0, 0, 0,
    /*   22e8 */ 0, 0, TBL(5) | 100, TBL(5) | 101, TBL(5) | 102, TBL(5) | 103, 0, 0,
    /*   22f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   22f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_23[256] = {
    /*   2300 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2308 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2310 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2318 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2320 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2328 */ 0, TBL(3) | 190, TBL(3) | 191, 0, 0, 0, 0, 0,
    /*   2330 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2338 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2340 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2348 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2350 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2358 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2360 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2368 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2370 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2378 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2380 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2388 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2390 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2398 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   23f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_2a[256] = {
    /*   2a00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2a98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2aa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2aa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ab0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ab8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ac0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ac8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ad0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ad8 */ 0, 0, 0, 0, TBL(5) | 104, 0, 0, 0,
    /*   2ae0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ae8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2af0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2af8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_30[256] = {
    /*   3000 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3008 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3010 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3018 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3020 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3028 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3030 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3038 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3040 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3048 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|174*/, 0, (uint16_t)-1 /*TBL(6)|175*/, 0,
    /*   3050 */ (uint16_t)-1 /*TBL(6)|176*/, 0, (uint16_t)-1 /*TBL(6)|177*/, 0, (uint16_t)-1 /*TBL(6)|178*/, 0, (uint16_t)-1 /*TBL(6)|179*/, 0,
    /*   3058 */ (uint16_t)-1 /*TBL(6)|180*/, 0, (uint16_t)-1 /*TBL(6)|181*/, 0, (uint16_t)-1 /*TBL(6)|182*/, 0, (uint16_t)-1 /*TBL(6)|183*/, 0,
    /*   3060 */ (uint16_t)-1 /*TBL(6)|184*/, 0, (uint16_t)-1 /*TBL(6)|185*/, 0, 0, (uint16_t)-1 /*TBL(6)|186*/, 0, (uint16_t)-1 /*TBL(6)|187*/,
    /*   3068 */ 0, (uint16_t)-1 /*TBL(6)|188*/, 0, 0, 0, 0, 0, 0,
    /*   3070 */ (uint16_t)-1 /*TBL(6)|189*/, (uint16_t)-1 /*TBL(6)|190*/, 0, (uint16_t)-1 /*TBL(6)|191*/, (uint16_t)-1 /*TBL(6)|192*/, 0, (uint16_t)-1 /*TBL(6)|193*/, (uint16_t)-1 /*TBL(6)|194*/,
    /*   3078 */ 0, (uint16_t)-1 /*TBL(6)|195*/, (uint16_t)-1 /*TBL(6)|196*/, 0, (uint16_t)-1 /*TBL(6)|197*/, (uint16_t)-1 /*TBL(6)|198*/, 0, 0,
    /*   3080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3088 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3090 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|173*/, 0, 0, 0,
    /*   3098 */ 0, 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|199*/, 0,
    /*   30a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30a8 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|201*/, 0, (uint16_t)-1 /*TBL(6)|202*/, 0,
    /*   30b0 */ (uint16_t)-1 /*TBL(6)|203*/, 0, (uint16_t)-1 /*TBL(6)|204*/, 0, (uint16_t)-1 /*TBL(6)|205*/, 0, (uint16_t)-1 /*TBL(6)|206*/, 0,
    /*   30b8 */ (uint16_t)-1 /*TBL(6)|207*/, 0, (uint16_t)-1 /*TBL(6)|208*/, 0, (uint16_t)-1 /*TBL(6)|209*/, 0, (uint16_t)-1 /*TBL(6)|210*/, 0,
    /*   30c0 */ (uint16_t)-1 /*TBL(6)|211*/, 0, (uint16_t)-1 /*TBL(6)|212*/, 0, 0, (uint16_t)-1 /*TBL(6)|213*/, 0, (uint16_t)-1 /*TBL(6)|214*/,
    /*   30c8 */ 0, (uint16_t)-1 /*TBL(6)|215*/, 0, 0, 0, 0, 0, 0,
    /*   30d0 */ (uint16_t)-1 /*TBL(6)|216*/, (uint16_t)-1 /*TBL(6)|217*/, 0, (uint16_t)-1 /*TBL(6)|218*/, (uint16_t)-1 /*TBL(6)|219*/, 0, (uint16_t)-1 /*TBL(6)|220*/, (uint16_t)-1 /*TBL(6)|221*/,
    /*   30d8 */ 0, (uint16_t)-1 /*TBL(6)|222*/, (uint16_t)-1 /*TBL(6)|223*/, 0, (uint16_t)-1 /*TBL(6)|224*/, (uint16_t)-1 /*TBL(6)|225*/, 0, 0,
    /*   30e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30f0 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|200*/, 0, 0, (uint16_t)-1 /*TBL(6)|226*/,
    /*   30f8 */ (uint16_t)-1 /*TBL(6)|227*/, (uint16_t)-1 /*TBL(6)|228*/, (uint16_t)-1 /*TBL(6)|229*/, 0, 0, 0, (uint16_t)-1 /*TBL(6)|230*/, 0};

static const uint16_t UN8IF_canon_00_f9[256] = {
    /*   f900 */ TBL(3) | 868, TBL(3) | 543, TBL(3) | 882, TBL(3) | 873, TBL(3) | 613, TBL(3) | 261, TBL(3) | 344, TBL(3) | 983,
    /*   f908 */ TBL(3) | 983, TBL(3) | 400, TBL(3) | 909, TBL(3) | 363, TBL(3) | 399, TBL(3) | 494, TBL(3) | 682, TBL(3) | 759,
    /*   f910 */ TBL(3) | 821, TBL(3) | 834, TBL(3) | 846, TBL(3) | 896, TBL(3) | 569, TBL(3) | 592, TBL(3) | 632, TBL(3) | 658,
    /*   f918 */ TBL(3) | 807, TBL(3) | 904, TBL(3) | 957, TBL(3) | 266, TBL(3) | 338, TBL(3) | 574, TBL(3) | 640, TBL(3) | 820,
    /*   f920 */ TBL(3) | 968, TBL(3) | 433, TBL(3) | 620, TBL(3) | 816, TBL(3) | 850, TBL(3) | 502, TBL(3) | 776, TBL(3) | 837,
    /*   f928 */ TBL(3) | 451, TBL(3) | 546, TBL(3) | 598, TBL(3) | 650, TBL(3) | 898, TBL(3) | 274, TBL(3) | 304, TBL(3) | 326,
    /*   f930 */ TBL(3) | 526, TBL(3) | 572, TBL(3) | 639, TBL(3) | 685, TBL(3) | 764, TBL(3) | 818, TBL(3) | 823, TBL(3) | 880,
    /*   f938 */ TBL(3) | 939, TBL(3) | 961, TBL(3) | 967, TBL(3) | 696, TBL(3) | 711, TBL(3) | 746, TBL(3) | 800, TBL(3) | 916,
    /*   f940 */ TBL(3) | 969, TBL(3) | 859, TBL(3) | 391, TBL(3) | 457, TBL(3) | 731, TBL(3) | 770, TBL(3) | 645, TBL(3) | 698,
    /*   f948 */ TBL(3) | 872, TBL(3) | 937, TBL(3) | 390, TBL(3) | 425, TBL(3) | 570, TBL(3) | 603, TBL(3) | 615, TBL(3) | 743,
    /*   f950 */ TBL(3) | 752, TBL(3) | 924, TBL(3) | 325, TBL(3) | 771, TBL(3) | 307, TBL(3) | 306, TBL(3) | 718, TBL(3) | 747,
    /*   f958 */ TBL(3) | 806, TBL(3) | 926, TBL(3) | 866, TBL(3) | 503, TBL(3) | 569, TBL(3) | 862, TBL(3) | 263, TBL(3) | 417,
    /*   f960 */ TBL(3) | 471, TBL(3) | 654, TBL(3) | 676, TBL(3) | 332, TBL(3) | 700, TBL(3) | 278, TBL(3) | 465, TBL(3) | 259,
    /*   f968 */ TBL(3) | 588, TBL(3) | 530, TBL(3) | 742, TBL(3) | 341, TBL(3) | 386, TBL(3) | 687, TBL(3) | 808, TBL(3) | 855,
    /*   f970 */ TBL(3) | 582, TBL(3) | 890, TBL(3) | 586, TBL(3) | 507, TBL(3) | 790, TBL(3) | 513, TBL(3) | 675, TBL(3) | 268,
    /*   f978 */ TBL(3) | 293, TBL(3) | 305, TBL(3) | 561, TBL(3) | 738, TBL(3) | 782, TBL(3) | 858, TBL(3) | 908, TBL(3) | 328,
    /*   f980 */ TBL(3) | 351, TBL(3) | 403, TBL(3) | 455, TBL(3) | 532, TBL(3) | 621, TBL(3) | 701, TBL(3) | 921, TBL(3) | 959,
    /*   f988 */ TBL(3) | 970, TBL(3) | 973, TBL(3) | 320, TBL(3) | 542, TBL(3) | 578, TBL(3) | 888, TBL(3) | 445, TBL(3) | 488,
    /*   f990 */ TBL(3) | 495, TBL(3) | 524, TBL(3) | 617, TBL(3) | 634, TBL(3) | 667, TBL(3) | 716, TBL(3) | 749, TBL(3) | 768,
    /*   f998 */ TBL(3) | 884, TBL(3) | 810, TBL(3) | 892, TBL(3) | 917, TBL(3) | 312, TBL(3) | 321, TBL(3) | 356, TBL(3) | 631,
    /*   f9a0 */ TBL(3) | 841, TBL(3) | 855, TBL(3) | 450, TBL(3) | 469, TBL(3) | 511, TBL(3) | 581, TBL(3) | 730, TBL(3) | 652,
    /*   f9a8 */ TBL(3) | 271, TBL(3) | 375, TBL(3) | 417, TBL(3) | 438, TBL(3) | 472, TBL(3) | 657, TBL(3) | 664, TBL(3) | 761,
    /*   f9b0 */ TBL(3) | 766, TBL(3) | 910, TBL(3) | 936, TBL(3) | 940, TBL(3) | 946, TBL(3) | 275, TBL(3) | 715, TBL(3) | 906,
    /*   f9b8 */ TBL(3) | 932, TBL(3) | 478, TBL(3) | 267, TBL(3) | 284, TBL(3) | 418, TBL(3) | 423, TBL(3) | 531, TBL(3) | 569,
    /*   f9c0 */ TBL(3) | 637, TBL(3) | 681, TBL(3) | 813, TBL(3) | 895, TBL(3) | 981, TBL(3) | 538, TBL(3) | 923, TBL(3) | 319,
    /*   f9c8 */ TBL(3) | 553, TBL(3) | 556, TBL(3) | 596, TBL(3) | 609, TBL(3) | 660, TBL(3) | 674, TBL(3) | 695, TBL(3) | 741,
    /*   f9d0 */ TBL(3) | 949, TBL(3) | 294, TBL(3) | 498, TBL(3) | 927, TBL(3) | 280, TBL(3) | 431, TBL(3) | 604, TBL(3) | 885,
    /*   f9d8 */ TBL(3) | 463, TBL(3) | 480, TBL(3) | 558, TBL(3) | 654, TBL(3) | 929, TBL(3) | 313, TBL(3) | 348, TBL(3) | 427,
    /*   f9e0 */ TBL(3) | 535, TBL(3) | 549, TBL(3) | 564, TBL(3) | 590, TBL(3) | 659, TBL(3) | 677, TBL(3) | 757, TBL(3) | 842,
    /*   f9e8 */ TBL(3) | 845, TBL(3) | 907, TBL(3) | 934, TBL(3) | 333, TBL(3) | 610, TBL(3) | 349, TBL(3) | 638, TBL(3) | 668,
    /*   f9f0 */ TBL(3) | 817, TBL(3) | 930, TBL(3) | 963, TBL(3) | 971, TBL(3) | 555, TBL(3) | 602, TBL(3) | 777, TBL(3) | 724,
    /*   f9f8 */ TBL(3) | 726, TBL(3) | 733, TBL(3) | 649, TBL(3) | 629, TBL(3) | 865, TBL(3) | 269, TBL(3) | 794, TBL(3) | 314};

static const uint16_t UN8IF_canon_00_fa[256] = {
    /*   fa00 */ TBL(3) | 311, TBL(3) | 446, TBL(3) | 504, TBL(3) | 736, TBL(3) | 414, TBL(3) | 593, TBL(3) | 541, TBL(3) | 887,
    /*   fa08 */ TBL(3) | 838, TBL(3) | 925, TBL(3) | 852, TBL(3) | 453, TBL(3) | 286, TBL(3) | 368, 0, 0,
    /*   fa10 */ TBL(3) | 385, 0, TBL(3) | 537, 0, 0, TBL(3) | 308, TBL(3) | 651, TBL(3) | 683,
    /*   fa18 */ TBL(3) | 702, TBL(3) | 709, TBL(3) | 710, TBL(3) | 714, TBL(3) | 941, TBL(3) | 734, TBL(3) | 762, 0,
    /*   fa20 */ TBL(3) | 819, 0, TBL(3) | 861, 0, 0, TBL(3) | 893, TBL(3) | 901, 0,
    /*   fa28 */ 0, 0, TBL(3) | 951, TBL(3) | 952, TBL(3) | 953, TBL(3) | 966, TBL(3) | 899, TBL(3) | 931,
    /*   fa30 */ TBL(3) | 276, TBL(3) | 285, TBL(3) | 288, TBL(3) | 324, TBL(3) | 327, TBL(3) | 335, TBL(3) | 365, TBL(3) | 371,
    /*   fa38 */ TBL(3) | 373, TBL(3) | 384, TBL(3) | 387, TBL(3) | 426, TBL(3) | 428, TBL(3) | 475, TBL(3) | 485, TBL(3) | 487,
    /*   fa40 */ TBL(3) | 493, TBL(3) | 527, TBL(3) | 533, TBL(3) | 539, TBL(3) | 562, TBL(3) | 599, TBL(3) | 606, TBL(3) | 616,
    /*   fa48 */ TBL(3) | 635, TBL(3) | 642, TBL(3) | 661, TBL(3) | 697, TBL(3) | 703, TBL(3) | 705, TBL(3) | 704, TBL(3) | 706,
    /*   fa50 */ TBL(3) | 707, TBL(3) | 708, TBL(3) | 712, TBL(3) | 713, TBL(3) | 719, TBL(3) | 722, TBL(3) | 727, TBL(3) | 749,
    /*   fa58 */ TBL(3) | 751, TBL(3) | 753, TBL(3) | 756, TBL(3) | 765, TBL(3) | 778, TBL(3) | 783, TBL(3) | 783, TBL(3) | 809,
    /*   fa60 */ TBL(3) | 848, TBL(3) | 853, TBL(3) | 863, TBL(3) | 864, TBL(3) | 874, TBL(3) | 875, TBL(3) | 891, TBL(3) | 893,
    /*   fa68 */ TBL(3) | 935, TBL(3) | 944, TBL(3) | 948, TBL(3) | 473, TBL(4) | 235, TBL(3) | 781, 0, 0,
    /*   fa70 */ TBL(3) | 260, TBL(3) | 303, TBL(3) | 292, TBL(3) | 273, TBL(3) | 287, TBL(3) | 296, TBL(3) | 323, TBL(3) | 329,
    /*   fa78 */ TBL(3) | 365, TBL(3) | 360, TBL(3) | 364, TBL(3) | 370, TBL(3) | 385, TBL(3) | 389, TBL(3) | 398, TBL(3) | 401,
    /*   fa80 */ TBL(3) | 408, TBL(3) | 412, TBL(3) | 452, TBL(3) | 454, TBL(3) | 461, TBL(3) | 466, TBL(3) | 477, TBL(3) | 483,
    /*   fa88 */ TBL(3) | 479, TBL(3) | 487, TBL(3) | 484, TBL(3) | 493, TBL(3) | 499, TBL(3) | 515, TBL(3) | 518, TBL(3) | 520,
    /*   fa90 */ TBL(3) | 528, TBL(3) | 537, TBL(3) | 546, TBL(3) | 547, TBL(3) | 551, TBL(3) | 579, TBL(3) | 582, TBL(3) | 596,
    /*   fa98 */ TBL(3) | 614, TBL(3) | 612, TBL(3) | 616, TBL(3) | 623, TBL(3) | 635, TBL(3) | 693, TBL(3) | 643, TBL(3) | 648,
    /*   faa0 */ TBL(3) | 651, TBL(3) | 665, TBL(3) | 670, TBL(3) | 672, TBL(3) | 679, TBL(3) | 680, TBL(3) | 683, TBL(3) | 684,
    /*   faa8 */ TBL(3) | 686, TBL(3) | 691, TBL(3) | 690, TBL(3) | 699, TBL(3) | 723, TBL(3) | 727, TBL(3) | 732, TBL(3) | 744,
    /*   fab0 */ TBL(3) | 749, TBL(3) | 755, TBL(3) | 765, TBL(3) | 795, TBL(3) | 805, TBL(3) | 832, TBL(3) | 849, TBL(3) | 851,
    /*   fab8 */ TBL(3) | 853, TBL(3) | 856, TBL(3) | 861, TBL(3) | 857, TBL(3) | 863, TBL(3) | 862, TBL(3) | 860, TBL(3) | 864,
    /*   fac0 */ TBL(3) | 867, TBL(3) | 875, TBL(3) | 886, TBL(3) | 894, TBL(3) | 905, TBL(3) | 912, TBL(3) | 928, TBL(3) | 935,
    /*   fac8 */ TBL(3) | 941, TBL(3) | 942, TBL(3) | 944, TBL(3) | 945, TBL(3) | 948, TBL(3) | 960, TBL(3) | 983, TBL(4) | 214,
    /*   fad0 */ TBL(4) | 213, TBL(4) | 223, TBL(3) | 213, TBL(3) | 223, TBL(3) | 224, TBL(4) | 249, TBL(4) | 258, TBL(4) | 282,
    /*   fad8 */ TBL(3) | 980, TBL(3) | 982, 0, 0, 0, 0, 0, 0,
    /*   fae0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fae8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   faf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   faf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_00_fb[256] = {
    /*   fb00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb18 */ 0, 0, 0, 0, 0, TBL(4) | 164, 0, TBL(4) | 183,
    /*   fb20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb28 */ 0, 0, TBL(4) | 180, TBL(4) | 181, (uint16_t)-1 /*TBL(6)|100*/, (uint16_t)-1 /*TBL(6)|101*/, TBL(4) | 152, TBL(4) | 153,
    /*   fb30 */ TBL(4) | 154, TBL(4) | 155, TBL(4) | 157, TBL(4) | 158, TBL(4) | 159, TBL(4) | 161, TBL(4) | 162, 0,
    /*   fb38 */ TBL(4) | 163, TBL(4) | 165, TBL(4) | 166, TBL(4) | 167, TBL(4) | 169, 0, TBL(4) | 170, 0,
    /*   fb40 */ TBL(4) | 171, TBL(4) | 172, 0, TBL(4) | 173, TBL(4) | 174, 0, TBL(4) | 176, TBL(4) | 177,
    /*   fb48 */ TBL(4) | 178, TBL(4) | 179, TBL(4) | 182, TBL(4) | 160, TBL(4) | 156, TBL(4) | 168, TBL(4) | 175, 0,
    /*   fb50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fba0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fba8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbe0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbe8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_05[256] = {
    /* 010500 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010508 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010510 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010518 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010520 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010528 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010530 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010538 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010540 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010548 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010550 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010558 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010560 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010568 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010570 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010578 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010580 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010588 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010590 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010598 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105c8 */ 0, (uint16_t)-1 /*TBL(6)|231*/, 0, 0, 0, 0, 0, 0,
    /* 0105d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105e0 */ 0, 0, 0, 0, (uint16_t)-1 /*TBL(6)|232*/, 0, 0, 0,
    /* 0105e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0105f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_10[256] = {
    /* 011000 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011008 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011010 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011018 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011020 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011028 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011030 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011038 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011040 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011048 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011050 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011058 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011060 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011068 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011070 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011078 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011088 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011090 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011098 */ 0, 0, (uint16_t)-1 /*TBL(8)|36*/, 0, (uint16_t)-1 /*TBL(8)|37*/, 0, 0, 0,
    /* 0110a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110a8 */ 0, 0, 0, (uint16_t)-1 /*TBL(8)|38*/, 0, 0, 0, 0,
    /* 0110b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_11[256] = {
    /* 011100 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011108 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011110 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011118 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011120 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011128 */ 0, 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(8)|39*/, (uint16_t)-1 /*TBL(8)|40*/,
    /* 011130 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011138 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011140 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011148 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011150 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011158 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011160 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011168 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011170 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011178 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011180 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011188 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011198 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_13[256] = {
    /* 011300 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011308 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011310 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011318 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011320 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011328 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011330 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011338 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011340 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011348 */ 0, 0, 0, (uint16_t)-1 /*TBL(8)|41*/, (uint16_t)-1 /*TBL(8)|42*/, 0, 0, 0,
    /* 011350 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011358 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011360 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011368 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011370 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011378 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011380 */ 0, 0, 0, (uint16_t)-1 /*TBL(8)|43*/, 0, (uint16_t)-1 /*TBL(8)|44*/, 0, 0,
    /* 011388 */ 0, 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(8)|45*/, 0,
    /* 011390 */ 0, (uint16_t)-1 /*TBL(8)|46*/, 0, 0, 0, 0, 0, 0,
    /* 011398 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113c0 */ 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(8)|48*/, 0, (uint16_t)-1 /*TBL(8)|47*/,
    /* 0113c8 */ (uint16_t)-1 /*TBL(8)|49*/, 0, 0, 0, 0, 0, 0, 0,
    /* 0113d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_14[256] = {
    /* 011400 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011408 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011410 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011418 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011420 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011428 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011430 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011438 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011440 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011448 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011450 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011458 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011460 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011468 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011470 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011478 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011480 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011488 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011490 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011498 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114b8 */ 0, 0, 0, (uint16_t)-1 /*TBL(8)|51*/, (uint16_t)-1 /*TBL(8)|50*/, 0, (uint16_t)-1 /*TBL(8)|52*/, 0,
    /* 0114c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_15[256] = {
    /* 011500 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011508 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011510 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011518 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011520 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011528 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011530 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011538 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011540 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011548 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011550 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011558 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011560 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011568 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011570 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011578 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011580 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011588 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011590 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011598 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115b8 */ 0, 0, (uint16_t)-1 /*TBL(8)|53*/, (uint16_t)-1 /*TBL(8)|54*/, 0, 0, 0, 0,
    /* 0115c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_19[256] = {
    /* 011900 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011908 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011910 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011918 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011920 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011928 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011930 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011938 */ (uint16_t)-1 /*TBL(8)|55*/, 0, 0, 0, 0, 0, 0, 0,
    /* 011940 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011948 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011950 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011958 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011960 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011968 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011970 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011978 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011980 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011988 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011990 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011998 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_61[256] = {
    /* 016100 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016108 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016110 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016118 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016120 */ 0, (uint16_t)-1 /*TBL(8)|56*/, (uint16_t)-1 /*TBL(8)|59*/, (uint16_t)-1 /*TBL(8)|57*/, (uint16_t)-1 /*TBL(8)|60*/, (uint16_t)-1 /*TBL(8)|58*/, (uint16_t)-1 /*TBL(12)|0*/, (uint16_t)-1 /*TBL(12)|2*/,
    /* 016128 */ (uint16_t)-1 /*TBL(12)|1*/, 0, 0, 0, 0, 0, 0, 0,
    /* 016130 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016138 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016140 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016148 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016150 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016158 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016160 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016168 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016170 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016178 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016180 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016188 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016198 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_6d[256] = {
    /* 016d00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d68 */ (uint16_t)-1 /*TBL(8)|62*/, (uint16_t)-1 /*TBL(8)|61*/, (uint16_t)-1 /*TBL(12)|3*/, 0, 0, 0, 0, 0,
    /* 016d70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016d98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016da0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016da8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016db0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016db8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016dc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016dc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016dd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016dd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016de0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016de8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016df0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016df8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_01_d1[256] = {
    /* 01d100 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d108 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d110 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d118 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d120 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d128 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d130 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d138 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d140 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d148 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d150 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d158 */ 0, 0, 0, 0, 0, 0, (uint16_t)-1 /*TBL(8)|63*/, (uint16_t)-1 /*TBL(8)|64*/,
    /* 01d160 */ (uint16_t)-1 /*TBL(12)|4*/, (uint16_t)-1 /*TBL(12)|5*/, (uint16_t)-1 /*TBL(12)|6*/, (uint16_t)-1 /*TBL(12)|7*/, (uint16_t)-1 /*TBL(12)|8*/, 0, 0, 0,
    /* 01d168 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d170 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d178 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d180 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d188 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d198 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1b8 */ 0, 0, 0, (uint16_t)-1 /*TBL(8)|65*/, (uint16_t)-1 /*TBL(8)|66*/, (uint16_t)-1 /*TBL(12)|9*/, (uint16_t)-1 /*TBL(12)|11*/, (uint16_t)-1 /*TBL(12)|10*/,
    /* 01d1c0 */ (uint16_t)-1 /*TBL(12)|12*/, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const uint16_t UN8IF_canon_02_f8[256] = {
    /* 02f800 */ TBL(3) | 264, TBL(3) | 262, TBL(3) | 265, TBL(4) | 192, TBL(3) | 272, TBL(3) | 276, TBL(3) | 277, TBL(3) | 279,
    /* 02f808 */ TBL(3) | 281, TBL(3) | 282, TBL(3) | 285, TBL(3) | 283, TBL(3) | 192, TBL(4) | 196, TBL(3) | 288, TBL(3) | 289,
    /* 02f810 */ TBL(3) | 290, TBL(3) | 295, TBL(4) | 193, TBL(3) | 193, TBL(3) | 291, TBL(3) | 297, TBL(4) | 195, TBL(3) | 300,
    /* 02f818 */ TBL(3) | 301, TBL(3) | 270, TBL(3) | 302, TBL(3) | 303, TBL(4) | 290, TBL(3) | 309, TBL(3) | 310, TBL(3) | 195,
    /* 02f820 */ TBL(3) | 315, TBL(3) | 316, TBL(3) | 317, TBL(3) | 318, TBL(3) | 196, TBL(3) | 323, TBL(3) | 324, TBL(3) | 327,
    /* 02f828 */ TBL(3) | 329, TBL(3) | 330, TBL(3) | 331, TBL(3) | 332, TBL(3) | 334, TBL(3) | 335, TBL(3) | 336, TBL(3) | 337,
    /* 02f830 */ TBL(3) | 339, TBL(3) | 340, TBL(3) | 340, TBL(3) | 340, TBL(4) | 199, TBL(3) | 626, TBL(3) | 342, TBL(3) | 343,
    /* 02f838 */ TBL(4) | 200, TBL(3) | 345, TBL(3) | 346, TBL(3) | 347, TBL(3) | 354, TBL(3) | 350, TBL(3) | 352, TBL(3) | 353,
    /* 02f840 */ TBL(3) | 355, TBL(3) | 357, TBL(3) | 358, TBL(3) | 359, TBL(3) | 361, TBL(3) | 362, TBL(3) | 362, TBL(3) | 364,
    /* 02f848 */ TBL(3) | 366, TBL(3) | 367, TBL(3) | 369, TBL(3) | 376, TBL(3) | 371, TBL(3) | 377, TBL(3) | 372, TBL(3) | 374,
    /* 02f850 */ TBL(3) | 311, TBL(3) | 392, TBL(3) | 379, TBL(3) | 380, TBL(3) | 381, TBL(3) | 378, TBL(3) | 383, TBL(3) | 382,
    /* 02f858 */ TBL(3) | 388, TBL(4) | 201, TBL(3) | 393, TBL(3) | 394, TBL(3) | 395, TBL(3) | 396, TBL(3) | 397, TBL(3) | 402,
    /* 02f860 */ TBL(4) | 202, TBL(4) | 203, TBL(3) | 405, TBL(3) | 406, TBL(3) | 407, TBL(3) | 404, TBL(3) | 409, TBL(3) | 197,
    /* 02f868 */ TBL(3) | 198, TBL(3) | 411, TBL(3) | 413, TBL(3) | 413, TBL(4) | 204, TBL(3) | 415, TBL(3) | 416, TBL(3) | 417,
    /* 02f870 */ TBL(3) | 419, TBL(4) | 205, TBL(3) | 420, TBL(3) | 421, TBL(3) | 459, TBL(3) | 422, TBL(3) | 199, TBL(3) | 424,
    /* 02f878 */ TBL(3) | 428, TBL(3) | 430, TBL(3) | 429, TBL(4) | 207, TBL(3) | 432, TBL(4) | 208, TBL(3) | 435, TBL(3) | 434,
    /* 02f880 */ TBL(3) | 436, TBL(3) | 439, TBL(3) | 440, TBL(3) | 200, TBL(3) | 441, TBL(3) | 442, TBL(3) | 443, TBL(3) | 444,
    /* 02f888 */ TBL(3) | 201, TBL(4) | 209, TBL(3) | 202, TBL(3) | 447, TBL(3) | 448, TBL(3) | 449, TBL(3) | 451, TBL(4) | 300,
    /* 02f890 */ TBL(3) | 456, TBL(4) | 211, TBL(4) | 211, TBL(3) | 779, TBL(3) | 458, TBL(3) | 458, TBL(3) | 203, TBL(4) | 218,
    /* 02f898 */ TBL(4) | 260, TBL(3) | 460, TBL(3) | 462, TBL(3) | 204, TBL(3) | 464, TBL(3) | 467, TBL(3) | 468, TBL(3) | 470,
    /* 02f8a0 */ TBL(3) | 474, TBL(3) | 206, TBL(3) | 205, TBL(3) | 475, TBL(4) | 212, TBL(3) | 476, TBL(3) | 481, TBL(3) | 482,
    /* 02f8a8 */ TBL(3) | 483, TBL(3) | 482, TBL(3) | 486, TBL(3) | 487, TBL(3) | 491, TBL(3) | 489, TBL(3) | 490, TBL(3) | 492,
    /* 02f8b0 */ TBL(3) | 493, TBL(3) | 494, TBL(3) | 496, TBL(3) | 497, TBL(3) | 500, TBL(3) | 501, TBL(3) | 505, TBL(3) | 509,
    /* 02f8b8 */ TBL(4) | 215, TBL(3) | 508, TBL(3) | 506, TBL(3) | 510, TBL(3) | 512, TBL(3) | 517, TBL(4) | 216, TBL(3) | 519,
    /* 02f8c0 */ TBL(3) | 516, TBL(3) | 514, TBL(3) | 207, TBL(3) | 521, TBL(3) | 523, TBL(3) | 525, TBL(3) | 522, TBL(3) | 208,
    /* 02f8c8 */ TBL(3) | 527, TBL(3) | 529, TBL(4) | 217, TBL(3) | 534, TBL(3) | 544, TBL(3) | 536, TBL(3) | 211, TBL(3) | 539,
    /* 02f8d0 */ TBL(3) | 210, TBL(3) | 209, TBL(3) | 298, TBL(3) | 299, TBL(3) | 545, TBL(3) | 540, TBL(3) | 772, TBL(3) | 236,
    /* 02f8d8 */ TBL(3) | 546, TBL(3) | 547, TBL(3) | 548, TBL(3) | 552, TBL(3) | 550, TBL(4) | 222, TBL(3) | 212, TBL(3) | 557,
    /* 02f8e0 */ TBL(3) | 554, TBL(3) | 560, TBL(3) | 562, TBL(4) | 224, TBL(3) | 563, TBL(3) | 559, TBL(3) | 565, TBL(3) | 213,
    /* 02f8e8 */ TBL(3) | 566, TBL(3) | 567, TBL(3) | 568, TBL(3) | 571, TBL(4) | 225, TBL(3) | 573, TBL(3) | 214, TBL(3) | 575,
    /* 02f8f0 */ TBL(4) | 226, TBL(3) | 576, TBL(3) | 215, TBL(3) | 577, TBL(3) | 580, TBL(3) | 582, TBL(3) | 583, TBL(4) | 227,
    /* 02f8f8 */ TBL(4) | 206, TBL(4) | 228, TBL(3) | 584, TBL(4) | 229, TBL(3) | 587, TBL(3) | 589, TBL(3) | 585, TBL(3) | 591};

static const uint16_t UN8IF_canon_02_f9[256] = {
    /* 02f900 */ TBL(3) | 595, TBL(3) | 599, TBL(3) | 596, TBL(3) | 597, TBL(3) | 600, TBL(3) | 601, TBL(4) | 230, TBL(3) | 594,
    /* 02f908 */ TBL(3) | 607, TBL(3) | 608, TBL(3) | 216, TBL(3) | 612, TBL(3) | 611, TBL(4) | 231, TBL(3) | 605, TBL(3) | 618,
    /* 02f910 */ TBL(4) | 232, TBL(4) | 233, TBL(3) | 619, TBL(3) | 624, TBL(3) | 623, TBL(3) | 622, TBL(3) | 217, TBL(3) | 625,
    /* 02f918 */ TBL(3) | 628, TBL(3) | 627, TBL(3) | 630, TBL(4) | 194, TBL(3) | 633, TBL(4) | 234, TBL(3) | 636, TBL(4) | 236,
    /* 02f920 */ TBL(3) | 641, TBL(3) | 643, TBL(3) | 644, TBL(4) | 237, TBL(3) | 646, TBL(3) | 647, TBL(4) | 238, TBL(4) | 239,
    /* 02f928 */ TBL(3) | 653, TBL(3) | 655, TBL(3) | 218, TBL(3) | 656, TBL(3) | 219, TBL(3) | 219, TBL(3) | 662, TBL(3) | 663,
    /* 02f930 */ TBL(3) | 665, TBL(3) | 666, TBL(3) | 669, TBL(3) | 220, TBL(3) | 671, TBL(4) | 240, TBL(3) | 673, TBL(4) | 241,
    /* 02f938 */ TBL(3) | 676, TBL(4) | 210, TBL(3) | 678, TBL(4) | 242, TBL(4) | 243, TBL(4) | 244, TBL(3) | 221, TBL(3) | 222,
    /* 02f940 */ TBL(3) | 686, TBL(4) | 246, TBL(4) | 245, TBL(4) | 247, TBL(4) | 248, TBL(3) | 688, TBL(3) | 689, TBL(3) | 689,
    /* 02f948 */ TBL(3) | 691, TBL(3) | 224, TBL(3) | 692, TBL(3) | 225, TBL(3) | 226, TBL(4) | 250, TBL(3) | 694, TBL(3) | 696,
    /* 02f950 */ TBL(3) | 699, TBL(3) | 227, TBL(4) | 251, TBL(3) | 707, TBL(4) | 252, TBL(4) | 253, TBL(3) | 714, TBL(3) | 717,
    /* 02f958 */ TBL(3) | 228, TBL(3) | 719, TBL(3) | 720, TBL(3) | 721, TBL(4) | 254, TBL(4) | 255, TBL(4) | 255, TBL(3) | 725,
    /* 02f960 */ TBL(3) | 229, TBL(4) | 256, TBL(3) | 728, TBL(3) | 729, TBL(3) | 230, TBL(4) | 257, TBL(3) | 735, TBL(3) | 231,
    /* 02f968 */ TBL(3) | 739, TBL(3) | 737, TBL(3) | 740, TBL(4) | 259, TBL(3) | 745, TBL(3) | 232, TBL(3) | 748, TBL(3) | 750,
    /* 02f970 */ TBL(3) | 754, TBL(3) | 233, TBL(4) | 261, TBL(4) | 262, TBL(3) | 234, TBL(4) | 263, TBL(3) | 758, TBL(4) | 264,
    /* 02f978 */ TBL(3) | 760, TBL(3) | 763, TBL(3) | 765, TBL(4) | 265, TBL(4) | 266, TBL(3) | 767, TBL(4) | 267, TBL(3) | 769,
    /* 02f980 */ TBL(4) | 219, TBL(3) | 235, TBL(3) | 773, TBL(3) | 774, TBL(3) | 237, TBL(3) | 775, TBL(3) | 410, TBL(4) | 268,
    /* 02f988 */ TBL(4) | 269, TBL(4) | 220, TBL(4) | 221, TBL(3) | 779, TBL(3) | 780, TBL(3) | 889, TBL(3) | 238, TBL(3) | 785,
    /* 02f990 */ TBL(3) | 784, TBL(3) | 786, TBL(3) | 322, TBL(3) | 787, TBL(3) | 788, TBL(3) | 789, TBL(3) | 791, TBL(4) | 270,
    /* 02f998 */ TBL(3) | 790, TBL(3) | 792, TBL(3) | 797, TBL(3) | 798, TBL(3) | 793, TBL(3) | 799, TBL(3) | 804, TBL(3) | 809,
    /* 02f9a0 */ TBL(3) | 796, TBL(3) | 801, TBL(3) | 802, TBL(3) | 803, TBL(4) | 271, TBL(4) | 273, TBL(4) | 272, TBL(3) | 239,
    /* 02f9a8 */ TBL(3) | 811, TBL(3) | 812, TBL(3) | 814, TBL(4) | 277, TBL(3) | 815, TBL(4) | 274, TBL(3) | 240, TBL(3) | 241,
    /* 02f9b0 */ TBL(4) | 275, TBL(4) | 276, TBL(3) | 242, TBL(3) | 822, TBL(3) | 823, TBL(3) | 824, TBL(3) | 825, TBL(3) | 827,
    /* 02f9b8 */ TBL(3) | 826, TBL(3) | 829, TBL(3) | 828, TBL(3) | 832, TBL(3) | 830, TBL(3) | 831, TBL(3) | 833, TBL(3) | 243,
    /* 02f9c0 */ TBL(3) | 835, TBL(3) | 836, TBL(3) | 244, TBL(3) | 839, TBL(3) | 840, TBL(4) | 278, TBL(3) | 843, TBL(3) | 844,
    /* 02f9c8 */ TBL(3) | 245, TBL(3) | 847, TBL(3) | 194, TBL(4) | 279, TBL(4) | 280, TBL(3) | 246, TBL(3) | 247, TBL(3) | 854,
    /* 02f9d0 */ TBL(3) | 860, TBL(3) | 867, TBL(3) | 869, TBL(4) | 281, TBL(3) | 870, TBL(3) | 871, TBL(3) | 876, TBL(3) | 877,
    /* 02f9d8 */ TBL(4) | 283, TBL(4) | 197, TBL(3) | 879, TBL(3) | 878, TBL(3) | 881, TBL(4) | 198, TBL(3) | 883, TBL(3) | 886,
    /* 02f9e0 */ TBL(4) | 284, TBL(4) | 285, TBL(3) | 897, TBL(3) | 900, TBL(3) | 902, TBL(4) | 286, TBL(3) | 903, TBL(3) | 911,
    /* 02f9e8 */ TBL(3) | 914, TBL(3) | 915, TBL(3) | 913, TBL(3) | 918, TBL(3) | 919, TBL(4) | 287, TBL(3) | 920, TBL(3) | 248,
    /* 02f9f0 */ TBL(3) | 922, TBL(4) | 288, TBL(3) | 249, TBL(3) | 933, TBL(3) | 437, TBL(3) | 938, TBL(4) | 289, TBL(4) | 291,
    /* 02f9f8 */ TBL(3) | 250, TBL(3) | 251, TBL(3) | 943, TBL(4) | 292, TBL(3) | 252, TBL(4) | 293, TBL(3) | 945, TBL(3) | 945};

static const uint16_t UN8IF_canon_02_fa[256] = {
    /* 02fa00 */ TBL(3) | 947, TBL(4) | 294, TBL(3) | 950, TBL(3) | 253, TBL(3) | 954, TBL(3) | 955, TBL(3) | 956, TBL(3) | 958,
    /* 02fa08 */ TBL(3) | 254, TBL(4) | 295, TBL(3) | 960, TBL(3) | 962, TBL(3) | 964, TBL(3) | 255, TBL(3) | 256, TBL(3) | 965,
    /* 02fa10 */ TBL(4) | 296, TBL(3) | 257, TBL(4) | 297, TBL(4) | 298, TBL(4) | 299, TBL(3) | 972, TBL(3) | 258, TBL(3) | 974,
    /* 02fa18 */ TBL(3) | 975, TBL(3) | 976, TBL(3) | 977, TBL(3) | 978, TBL(3) | 979, TBL(4) | 301, 0, 0,
    /* 02fa20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fa98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02faa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02faa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fab0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fab8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fac0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fac8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fad0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fad8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fae0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02fae8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02faf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 02faf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

/* the planes */
static const uint16_t *UN8IF_canon_00[256] = {
    UN8IF_canon_00_00,
    UN8IF_canon_00_01,
    UN8IF_canon_00_02,
    UN8IF_canon_00_03,
    UN8IF_canon_00_04,
    NULL, UN8IF_canon_00_06,
    NULL,
    NULL, UN8IF_canon_00_09,
    UN8IF_canon_00_0a,
    UN8IF_canon_00_0b,
    UN8IF_canon_00_0c,
    UN8IF_canon_00_0d,
    NULL, UN8IF_canon_00_0f,
    UN8IF_canon_00_10,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, UN8IF_canon_00_1b,
    NULL, NULL, UN8IF_canon_00_1e,
    UN8IF_canon_00_1f,
    UN8IF_canon_00_20,
    UN8IF_canon_00_21,
    UN8IF_canon_00_22,
    UN8IF_canon_00_23,
    NULL, NULL, NULL, NULL,
    NULL, NULL, UN8IF_canon_00_2a,
    NULL, NULL, NULL, NULL, NULL,
    UN8IF_canon_00_30,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, UN8IF_canon_00_f9,
    UN8IF_canon_00_fa,
    UN8IF_canon_00_fb,
    NULL, NULL, NULL, NULL};

static const uint16_t *UN8IF_canon_01[256] = {
    NULL, NULL, NULL, NULL, NULL, UN8IF_canon_01_05,
    NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    UN8IF_canon_01_10,
    UN8IF_canon_01_11,
    NULL, UN8IF_canon_01_13,
    UN8IF_canon_01_14,
    UN8IF_canon_01_15,
    NULL, NULL,
    NULL, UN8IF_canon_01_19,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, UN8IF_canon_01_61,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, UN8IF_canon_01_6d,
    NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, UN8IF_canon_01_d1,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const uint16_t *UN8IF_canon_02[256] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    UN8IF_canon_02_f8,
    UN8IF_canon_02_f9,
    UN8IF_canon_02_fa,
    NULL, NULL, NULL, NULL, NULL};

/* the main plane */
#undef TBL
static const uint16_t **UN8IF_canon[] = {
    UN8IF_canon_00,
    UN8IF_canon_01,
    UN8IF_canon_02,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL};

/* CombiningClass */
/* the rows */
static const STDCHAR UN8IF_combin_00_03[256] = {
    /*   0300 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   0308 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   0310 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)232, (STDCHAR)220, (STDCHAR)220,
    /*   0318 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)232, (STDCHAR)216, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /*   0320 */ (STDCHAR)220, (STDCHAR)202, (STDCHAR)202, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)202,
    /*   0328 */ (STDCHAR)202, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /*   0330 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)1, (STDCHAR)1, (STDCHAR)1, (STDCHAR)1,
    /*   0338 */ (STDCHAR)1, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   0340 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)240, (STDCHAR)230, (STDCHAR)220,
    /*   0348 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, 0,
    /*   0350 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230,
    /*   0358 */ (STDCHAR)232, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)233, (STDCHAR)234, (STDCHAR)234, (STDCHAR)233,
    /*   0360 */ (STDCHAR)234, (STDCHAR)234, (STDCHAR)233, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   0368 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   0370 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0378 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0380 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0388 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0390 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0398 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   03f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_04[256] = {
    /*   0400 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0408 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0410 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0418 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0420 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0428 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0430 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0438 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0440 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0448 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0450 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0458 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0460 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0468 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0470 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0478 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0480 */ 0, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   0488 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0490 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0498 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   04f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_05[256] = {
    /*   0500 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0508 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0510 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0518 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0520 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0528 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0530 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0538 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0540 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0548 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0550 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0558 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0560 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0568 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0570 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0578 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0580 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0588 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0590 */ 0, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230,
    /*   0598 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)222, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   05a0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /*   05a8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)222, (STDCHAR)228, (STDCHAR)230,
    /*   05b0 */ (STDCHAR)10, (STDCHAR)11, (STDCHAR)12, (STDCHAR)13, (STDCHAR)14, (STDCHAR)15, (STDCHAR)16, (STDCHAR)17,
    /*   05b8 */ (STDCHAR)18, (STDCHAR)19, (STDCHAR)19, (STDCHAR)20, (STDCHAR)21, (STDCHAR)22, 0, (STDCHAR)23,
    /*   05c0 */ 0, (STDCHAR)24, (STDCHAR)25, 0, (STDCHAR)230, (STDCHAR)220, 0, (STDCHAR)18,
    /*   05c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   05d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   05d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   05e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   05e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   05f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   05f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_06[256] = {
    /*   0600 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0608 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0610 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   0618 */ (STDCHAR)30, (STDCHAR)31, (STDCHAR)32, 0, 0, 0, 0, 0,
    /*   0620 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0628 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0630 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0638 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0640 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0648 */ 0, 0, 0, (STDCHAR)27, (STDCHAR)28, (STDCHAR)29, (STDCHAR)30, (STDCHAR)31,
    /*   0650 */ (STDCHAR)32, (STDCHAR)33, (STDCHAR)34, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230,
    /*   0658 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220,
    /*   0660 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0668 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0670 */ (STDCHAR)35, 0, 0, 0, 0, 0, 0, 0,
    /*   0678 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0680 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0688 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0690 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0698 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06d0 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230,
    /*   06d8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0, (STDCHAR)230,
    /*   06e0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, 0, 0, (STDCHAR)230,
    /*   06e8 */ (STDCHAR)230, 0, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, 0, 0,
    /*   06f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   06f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_07[256] = {
    /*   0700 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0708 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0710 */ 0, (STDCHAR)36, 0, 0, 0, 0, 0, 0,
    /*   0718 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0720 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0728 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0730 */ (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220,
    /*   0738 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230,
    /*   0740 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230,
    /*   0748 */ (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, 0, 0, 0, 0, 0,
    /*   0750 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0758 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0760 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0768 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0770 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0778 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0780 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0788 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0790 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0798 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   07e8 */ 0, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   07f0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, 0, 0, 0, 0,
    /*   07f8 */ 0, 0, 0, 0, 0, (STDCHAR)220, 0, 0};

static const STDCHAR UN8IF_combin_00_08[256] = {
    /*   0800 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0808 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0810 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230,
    /*   0818 */ (STDCHAR)230, (STDCHAR)230, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   0820 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   0828 */ 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0,
    /*   0830 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0838 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0840 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0848 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0850 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0858 */ 0, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, 0, 0, 0, 0,
    /*   0860 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0868 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0870 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0878 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0880 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0888 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0890 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)230,
    /*   0898 */ (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   08a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   08a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   08b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   08b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   08c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   08c8 */ 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220,
    /*   08d0 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   08d8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   08e0 */ (STDCHAR)230, (STDCHAR)230, 0, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230,
    /*   08e8 */ (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /*   08f0 */ (STDCHAR)27, (STDCHAR)28, (STDCHAR)29, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230,
    /*   08f8 */ (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230};

static const STDCHAR UN8IF_combin_00_09[256] = {
    /*   0900 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0908 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0910 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0918 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0920 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0928 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0930 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0938 */ 0, 0, 0, 0, (STDCHAR)7, 0, 0, 0,
    /*   0940 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0948 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   0950 */ 0, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, 0, 0, 0,
    /*   0958 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0960 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0968 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0970 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0978 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0980 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0988 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0990 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0998 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09b8 */ 0, 0, 0, 0, (STDCHAR)7, 0, 0, 0,
    /*   09c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09c8 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   09d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   09f8 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, 0};

static const STDCHAR UN8IF_combin_00_0a[256] = {
    /*   0a00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a38 */ 0, 0, 0, 0, (STDCHAR)7, 0, 0, 0,
    /*   0a40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a48 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   0a50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0a98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0aa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0aa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ab0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ab8 */ 0, 0, 0, 0, (STDCHAR)7, 0, 0, 0,
    /*   0ac0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ac8 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   0ad0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ad8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ae0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ae8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0af0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0af8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_0b[256] = {
    /*   0b00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b38 */ 0, 0, 0, 0, (STDCHAR)7, 0, 0, 0,
    /*   0b40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b48 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   0b50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0b98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ba0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ba8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bc8 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   0bd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0be0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0be8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0bf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_0c[256] = {
    /*   0c00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c38 */ 0, 0, 0, 0, (STDCHAR)7, 0, 0, 0,
    /*   0c40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c48 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   0c50 */ 0, 0, 0, 0, 0, (STDCHAR)84, (STDCHAR)91, 0,
    /*   0c58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0c98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ca0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ca8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cb8 */ 0, 0, 0, 0, (STDCHAR)7, 0, 0, 0,
    /*   0cc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cc8 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   0cd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ce0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ce8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0cf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_0d[256] = {
    /*   0d00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d38 */ 0, 0, 0, (STDCHAR)9, (STDCHAR)9, 0, 0, 0,
    /*   0d40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d48 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   0d50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0d98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0da0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0da8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0db0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0db8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0dc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0dc8 */ 0, 0, (STDCHAR)9, 0, 0, 0, 0, 0,
    /*   0dd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0dd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0de0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0de8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0df0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0df8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_0e[256] = {
    /*   0e00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e38 */ (STDCHAR)103, (STDCHAR)103, (STDCHAR)9, 0, 0, 0, 0, 0,
    /*   0e40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e48 */ (STDCHAR)107, (STDCHAR)107, (STDCHAR)107, (STDCHAR)107, 0, 0, 0, 0,
    /*   0e50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0e98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ea0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ea8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0eb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0eb8 */ (STDCHAR)118, (STDCHAR)118, (STDCHAR)9, 0, 0, 0, 0, 0,
    /*   0ec0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ec8 */ (STDCHAR)122, (STDCHAR)122, (STDCHAR)122, (STDCHAR)122, 0, 0, 0, 0,
    /*   0ed0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ed8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ee0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ee8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ef0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ef8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_0f[256] = {
    /*   0f00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f18 */ (STDCHAR)220, (STDCHAR)220, 0, 0, 0, 0, 0, 0,
    /*   0f20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f30 */ 0, 0, 0, 0, 0, (STDCHAR)220, 0, (STDCHAR)220,
    /*   0f38 */ 0, (STDCHAR)216, 0, 0, 0, 0, 0, 0,
    /*   0f40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f70 */ 0, (STDCHAR)129, (STDCHAR)130, 0, (STDCHAR)132, 0, 0, 0,
    /*   0f78 */ 0, 0, (STDCHAR)130, (STDCHAR)130, (STDCHAR)130, (STDCHAR)130, 0, 0,
    /*   0f80 */ (STDCHAR)130, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)9, 0, (STDCHAR)230, (STDCHAR)230,
    /*   0f88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0f98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fc0 */ 0, 0, 0, 0, 0, 0, (STDCHAR)220, 0,
    /*   0fc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fe0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0fe8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ff0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   0ff8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_10[256] = {
    /*   1000 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1008 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1010 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1018 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1020 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1028 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1030 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)7,
    /*   1038 */ 0, (STDCHAR)9, (STDCHAR)9, 0, 0, 0, 0, 0,
    /*   1040 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1048 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1050 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1058 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1060 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1068 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1070 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1078 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1088 */ 0, 0, 0, 0, 0, (STDCHAR)220, 0, 0,
    /*   1090 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1098 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   10f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_13[256] = {
    /*   1300 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1308 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1310 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1318 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1320 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1328 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1330 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1338 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1340 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1348 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1350 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1358 */ 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1360 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1368 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1370 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1378 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1380 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1388 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1390 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1398 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   13f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_17[256] = {
    /*   1700 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1708 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1710 */ 0, 0, 0, 0, (STDCHAR)9, (STDCHAR)9, 0, 0,
    /*   1718 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1720 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1728 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1730 */ 0, 0, 0, 0, (STDCHAR)9, 0, 0, 0,
    /*   1738 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1740 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1748 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1750 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1758 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1760 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1768 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1770 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1778 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1780 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1788 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1790 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1798 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17d0 */ 0, 0, (STDCHAR)9, 0, 0, 0, 0, 0,
    /*   17d8 */ 0, 0, 0, 0, 0, (STDCHAR)230, 0, 0,
    /*   17e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   17f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_18[256] = {
    /*   1800 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1808 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1810 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1818 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1820 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1828 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1830 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1838 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1840 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1848 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1850 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1858 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1860 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1868 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1870 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1878 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1880 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1888 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1890 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1898 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18a8 */ 0, (STDCHAR)228, 0, 0, 0, 0, 0, 0,
    /*   18b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   18f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_19[256] = {
    /*   1900 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1908 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1910 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1918 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1920 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1928 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1930 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1938 */ 0, (STDCHAR)222, (STDCHAR)230, (STDCHAR)220, 0, 0, 0, 0,
    /*   1940 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1948 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1950 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1958 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1960 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1968 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1970 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1978 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1980 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1988 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1990 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1998 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   19f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_1a[256] = {
    /*   1a00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a10 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)230,
    /*   1a18 */ (STDCHAR)220, 0, 0, 0, 0, 0, 0, 0,
    /*   1a20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a60 */ (STDCHAR)9, 0, 0, 0, 0, 0, 0, 0,
    /*   1a68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a70 */ 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1a78 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0, (STDCHAR)220,
    /*   1a80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1a98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1aa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1aa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1ab0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /*   1ab8 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, 0, (STDCHAR)220,
    /*   1ac0 */ (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1ac8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1ad0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1ad8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, 0, 0,
    /*   1ae0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230,
    /*   1ae8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)234, 0, 0, 0, 0,
    /*   1af0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1af8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_1b[256] = {
    /*   1b00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b30 */ 0, 0, 0, 0, (STDCHAR)7, 0, 0, 0,
    /*   1b38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b40 */ 0, 0, 0, 0, (STDCHAR)9, 0, 0, 0,
    /*   1b48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b68 */ 0, 0, 0, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1b70 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0, 0, 0,
    /*   1b78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1b98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1ba0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1ba8 */ 0, 0, (STDCHAR)9, (STDCHAR)9, 0, 0, 0, 0,
    /*   1bb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1be0 */ 0, 0, 0, 0, 0, 0, (STDCHAR)7, 0,
    /*   1be8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1bf0 */ 0, 0, (STDCHAR)9, (STDCHAR)9, 0, 0, 0, 0,
    /*   1bf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_1c[256] = {
    /*   1c00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c30 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)7,
    /*   1c38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1c98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1ca0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1ca8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1cb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1cb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1cc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1cc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1cd0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, (STDCHAR)1, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /*   1cd8 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /*   1ce0 */ (STDCHAR)230, 0, (STDCHAR)1, (STDCHAR)1, (STDCHAR)1, (STDCHAR)1, (STDCHAR)1, (STDCHAR)1,
    /*   1ce8 */ (STDCHAR)1, 0, 0, 0, 0, (STDCHAR)220, 0, 0,
    /*   1cf0 */ 0, 0, 0, 0, (STDCHAR)230, 0, 0, 0,
    /*   1cf8 */ (STDCHAR)230, (STDCHAR)230, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_1d[256] = {
    /*   1d00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1d98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1da0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1da8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1db0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1db8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   1dc0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1dc8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230, (STDCHAR)234, (STDCHAR)214, (STDCHAR)220,
    /*   1dd0 */ (STDCHAR)202, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1dd8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1de0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1de8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   1df0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)232, (STDCHAR)228,
    /*   1df8 */ (STDCHAR)228, (STDCHAR)220, (STDCHAR)218, (STDCHAR)230, (STDCHAR)233, (STDCHAR)220, (STDCHAR)230, (STDCHAR)220};

static const STDCHAR UN8IF_combin_00_20[256] = {
    /*   2000 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2008 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2010 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2018 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2020 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2028 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2030 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2038 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2040 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2048 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2050 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2058 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2060 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2068 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2070 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2078 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2088 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2090 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2098 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   20d0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)1, (STDCHAR)1, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   20d8 */ (STDCHAR)1, (STDCHAR)1, (STDCHAR)1, (STDCHAR)230, (STDCHAR)230, 0, 0, 0,
    /*   20e0 */ 0, (STDCHAR)230, 0, 0, 0, (STDCHAR)1, (STDCHAR)1, (STDCHAR)230,
    /*   20e8 */ (STDCHAR)220, (STDCHAR)230, (STDCHAR)1, (STDCHAR)1, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /*   20f0 */ (STDCHAR)230, 0, 0, 0, 0, 0, 0, 0,
    /*   20f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_2c[256] = {
    /*   2c00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2c98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ca0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ca8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2cb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2cb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2cc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2cc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2cd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2cd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ce0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2ce8 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)230,
    /*   2cf0 */ (STDCHAR)230, (STDCHAR)230, 0, 0, 0, 0, 0, 0,
    /*   2cf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_2d[256] = {
    /*   2d00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d78 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)9,
    /*   2d80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2d98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2da0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2da8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2db0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2db8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2dc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2dc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2dd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2dd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   2de0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   2de8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   2df0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   2df8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230};

static const STDCHAR UN8IF_combin_00_30[256] = {
    /*   3000 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3008 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3010 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3018 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3020 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3028 */ 0, 0, (STDCHAR)218, (STDCHAR)228, (STDCHAR)232, (STDCHAR)222, (STDCHAR)224, (STDCHAR)224,
    /*   3030 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3038 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3040 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3048 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3050 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3058 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3060 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3068 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3070 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3078 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3088 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3090 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   3098 */ 0, (STDCHAR)8, (STDCHAR)8, 0, 0, 0, 0, 0,
    /*   30a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   30f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_a6[256] = {
    /*   a600 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a608 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a610 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a618 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a620 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a628 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a630 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a638 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a640 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a648 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a650 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a658 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a660 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a668 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)230,
    /*   a670 */ 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   a678 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0,
    /*   a680 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a688 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a690 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a698 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230,
    /*   a6a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a6f0 */ (STDCHAR)230, (STDCHAR)230, 0, 0, 0, 0, 0, 0,
    /*   a6f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_a8[256] = {
    /*   a800 */ 0, 0, 0, 0, 0, 0, (STDCHAR)9, 0,
    /*   a808 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a810 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a818 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a820 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a828 */ 0, 0, 0, 0, (STDCHAR)9, 0, 0, 0,
    /*   a830 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a838 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a840 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a848 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a850 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a858 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a860 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a868 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a870 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a878 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a880 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a888 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a890 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a898 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a8a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a8a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a8b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a8b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a8c0 */ 0, 0, 0, 0, (STDCHAR)9, 0, 0, 0,
    /*   a8c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a8d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a8d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a8e0 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   a8e8 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /*   a8f0 */ (STDCHAR)230, (STDCHAR)230, 0, 0, 0, 0, 0, 0,
    /*   a8f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_a9[256] = {
    /*   a900 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a908 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a910 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a918 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a920 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a928 */ 0, 0, 0, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, 0, 0,
    /*   a930 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a938 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a940 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a948 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a950 */ 0, 0, 0, (STDCHAR)9, 0, 0, 0, 0,
    /*   a958 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a960 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a968 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a970 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a978 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a980 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a988 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a990 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a998 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9b0 */ 0, 0, 0, (STDCHAR)7, 0, 0, 0, 0,
    /*   a9b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9c0 */ (STDCHAR)9, 0, 0, 0, 0, 0, 0, 0,
    /*   a9c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   a9f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_aa[256] = {
    /*   aa00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aa98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aaa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aaa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aab0 */ (STDCHAR)230, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, 0, 0, (STDCHAR)230,
    /*   aab8 */ (STDCHAR)230, 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230,
    /*   aac0 */ 0, (STDCHAR)230, 0, 0, 0, 0, 0, 0,
    /*   aac8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aad0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aad8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aae0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aae8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aaf0 */ 0, 0, 0, 0, 0, 0, (STDCHAR)9, 0,
    /*   aaf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_ab[256] = {
    /*   ab00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   ab98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aba0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   aba8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   abb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   abb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   abc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   abc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   abd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   abd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   abe0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   abe8 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /*   abf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   abf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_fb[256] = {
    /*   fb00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb18 */ 0, 0, 0, 0, 0, 0, (STDCHAR)26, 0,
    /*   fb20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fb98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fba0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fba8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbe0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbe8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fbf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_00_fe[256] = {
    /*   fe00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe20 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220,
    /*   fe28 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)230, (STDCHAR)230,
    /*   fe30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fe98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fea0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fea8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   feb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   feb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fec0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fec8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fed0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fed8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fee0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fee8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fef0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /*   fef8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_01[256] = {
    /* 010100 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010108 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010110 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010118 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010120 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010128 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010130 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010138 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010140 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010148 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010150 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010158 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010160 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010168 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010170 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010178 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010180 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010188 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010198 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0101f8 */ 0, 0, 0, 0, 0, (STDCHAR)220, 0, 0};

static const STDCHAR UN8IF_combin_01_02[256] = {
    /* 010200 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010208 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010210 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010218 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010220 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010228 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010230 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010238 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010240 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010248 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010250 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010258 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010260 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010268 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010270 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010278 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010280 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010288 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010290 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010298 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102e0 */ (STDCHAR)220, 0, 0, 0, 0, 0, 0, 0,
    /* 0102e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0102f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_03[256] = {
    /* 010300 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010308 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010310 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010318 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010320 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010328 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010330 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010338 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010340 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010348 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010350 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010358 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010360 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010368 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010370 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230,
    /* 010378 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0, 0, 0, 0,
    /* 010380 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010388 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010390 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010398 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0103f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_0a[256] = {
    /* 010a00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a08 */ 0, 0, 0, 0, 0, (STDCHAR)220, 0, (STDCHAR)230,
    /* 010a10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a38 */ (STDCHAR)230, (STDCHAR)1, (STDCHAR)220, 0, 0, 0, 0, (STDCHAR)9,
    /* 010a40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010a98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010aa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010aa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ab0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ab8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ac0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ac8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ad0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ad8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ae0 */ 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)220, 0,
    /* 010ae8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010af0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010af8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_0d[256] = {
    /* 010d00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d20 */ 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /* 010d28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d68 */ 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0,
    /* 010d70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010d98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010da0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010da8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010db0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010db8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010dc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010dc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010dd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010dd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010de0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010de8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010df0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010df8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_0e[256] = {
    /* 010e00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010e98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ea0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ea8 */ 0, 0, 0, (STDCHAR)230, (STDCHAR)230, 0, 0, 0,
    /* 010eb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010eb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ec0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ec8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ed0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ed8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ee0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ee8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ef0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ef8 */ 0, 0, (STDCHAR)220, (STDCHAR)220, 0, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220};

static const STDCHAR UN8IF_combin_01_0f[256] = {
    /* 010f00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f40 */ 0, 0, 0, 0, 0, 0, (STDCHAR)220, (STDCHAR)220,
    /* 010f48 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /* 010f50 */ (STDCHAR)220, 0, 0, 0, 0, 0, 0, 0,
    /* 010f58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f80 */ 0, 0, (STDCHAR)230, (STDCHAR)220, (STDCHAR)230, (STDCHAR)220, 0, 0,
    /* 010f88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010f98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fe0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010fe8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ff0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 010ff8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_10[256] = {
    /* 011000 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011008 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011010 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011018 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011020 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011028 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011030 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011038 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011040 */ 0, 0, 0, 0, 0, 0, (STDCHAR)9, 0,
    /* 011048 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011050 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011058 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011060 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011068 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011070 */ (STDCHAR)9, 0, 0, 0, 0, 0, 0, 0,
    /* 011078 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)9,
    /* 011080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011088 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011090 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011098 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110b8 */ 0, (STDCHAR)9, (STDCHAR)7, 0, 0, 0, 0, 0,
    /* 0110c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0110f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_11[256] = {
    /* 011100 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0, 0, 0, 0,
    /* 011108 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011110 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011118 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011120 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011128 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011130 */ 0, 0, 0, (STDCHAR)9, (STDCHAR)9, 0, 0, 0,
    /* 011138 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011140 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011148 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011150 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011158 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011160 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011168 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011170 */ 0, 0, 0, (STDCHAR)7, 0, 0, 0, 0,
    /* 011178 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011180 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011188 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011198 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111c0 */ (STDCHAR)9, 0, 0, 0, 0, 0, 0, 0,
    /* 0111c8 */ 0, 0, (STDCHAR)7, 0, 0, 0, 0, 0,
    /* 0111d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0111f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_12[256] = {
    /* 011200 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011208 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011210 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011218 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011220 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011228 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011230 */ 0, 0, 0, 0, 0, (STDCHAR)9, (STDCHAR)7, 0,
    /* 011238 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011240 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011248 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011250 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011258 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011260 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011268 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011270 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011278 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011280 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011288 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011290 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011298 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112e8 */ 0, (STDCHAR)7, (STDCHAR)9, 0, 0, 0, 0, 0,
    /* 0112f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0112f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_13[256] = {
    /* 011300 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011308 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011310 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011318 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011320 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011328 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011330 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011338 */ 0, 0, 0, (STDCHAR)7, (STDCHAR)7, 0, 0, 0,
    /* 011340 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011348 */ 0, 0, 0, 0, 0, (STDCHAR)9, 0, 0,
    /* 011350 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011358 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011360 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230,
    /* 011368 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0, 0,
    /* 011370 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0, 0,
    /* 011378 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011380 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011388 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011390 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011398 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113c8 */ 0, 0, 0, 0, 0, 0, (STDCHAR)9, (STDCHAR)9,
    /* 0113d0 */ (STDCHAR)9, 0, 0, 0, 0, 0, 0, 0,
    /* 0113d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0113f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_14[256] = {
    /* 011400 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011408 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011410 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011418 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011420 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011428 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011430 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011438 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011440 */ 0, 0, (STDCHAR)9, 0, 0, 0, (STDCHAR)7, 0,
    /* 011448 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011450 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011458 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, 0,
    /* 011460 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011468 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011470 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011478 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011480 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011488 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011490 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011498 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114c0 */ 0, 0, (STDCHAR)9, (STDCHAR)7, 0, 0, 0, 0,
    /* 0114c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0114f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_15[256] = {
    /* 011500 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011508 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011510 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011518 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011520 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011528 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011530 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011538 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011540 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011548 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011550 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011558 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011560 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011568 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011570 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011578 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011580 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011588 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011590 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011598 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115b8 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)9,
    /* 0115c0 */ (STDCHAR)7, 0, 0, 0, 0, 0, 0, 0,
    /* 0115c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0115f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_16[256] = {
    /* 011600 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011608 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011610 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011618 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011620 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011628 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011630 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011638 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)9,
    /* 011640 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011648 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011650 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011658 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011660 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011668 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011670 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011678 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011680 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011688 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011690 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011698 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116b0 */ 0, 0, 0, 0, 0, 0, (STDCHAR)9, (STDCHAR)7,
    /* 0116b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0116f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_17[256] = {
    /* 011700 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011708 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011710 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011718 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011720 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011728 */ 0, 0, 0, (STDCHAR)9, 0, 0, 0, 0,
    /* 011730 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011738 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011740 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011748 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011750 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011758 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011760 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011768 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011770 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011778 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011780 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011788 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011790 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011798 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0117f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_18[256] = {
    /* 011800 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011808 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011810 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011818 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011820 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011828 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011830 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011838 */ 0, (STDCHAR)9, (STDCHAR)7, 0, 0, 0, 0, 0,
    /* 011840 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011848 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011850 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011858 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011860 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011868 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011870 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011878 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011880 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011888 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011890 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011898 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0118f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_19[256] = {
    /* 011900 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011908 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011910 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011918 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011920 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011928 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011930 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011938 */ 0, 0, 0, 0, 0, (STDCHAR)9, (STDCHAR)9, 0,
    /* 011940 */ 0, 0, 0, (STDCHAR)7, 0, 0, 0, 0,
    /* 011948 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011950 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011958 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011960 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011968 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011970 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011978 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011980 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011988 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011990 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011998 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119e0 */ (STDCHAR)9, 0, 0, 0, 0, 0, 0, 0,
    /* 0119e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0119f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_1a[256] = {
    /* 011a00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a30 */ 0, 0, 0, 0, (STDCHAR)9, 0, 0, 0,
    /* 011a38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a40 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)9,
    /* 011a48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011a98 */ 0, (STDCHAR)9, 0, 0, 0, 0, 0, 0,
    /* 011aa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011aa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ab0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ab8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ac0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ac8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ad0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ad8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ae0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ae8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011af0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011af8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_1c[256] = {
    /* 011c00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c38 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)9,
    /* 011c40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011c98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ca0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ca8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011cb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011cb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011cc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011cc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011cd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011cd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ce0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ce8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011cf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011cf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_1d[256] = {
    /* 011d00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d40 */ 0, 0, (STDCHAR)7, 0, (STDCHAR)9, (STDCHAR)9, 0, 0,
    /* 011d48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011d90 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)9,
    /* 011d98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011da0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011da8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011db0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011db8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011dc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011dc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011dd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011dd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011de0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011de8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011df0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011df8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_1f[256] = {
    /* 011f00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f40 */ 0, (STDCHAR)9, (STDCHAR)9, 0, 0, 0, 0, 0,
    /* 011f48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011f98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fe0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011fe8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ff0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 011ff8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_61[256] = {
    /* 016100 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016108 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016110 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016118 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016120 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016128 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)9,
    /* 016130 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016138 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016140 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016148 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016150 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016158 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016160 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016168 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016170 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016178 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016180 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016188 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016198 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0161f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_6a[256] = {
    /* 016a00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016a98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016aa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016aa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ab0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ab8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ac0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ac8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ad0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ad8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ae0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ae8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016af0 */ (STDCHAR)1, (STDCHAR)1, (STDCHAR)1, (STDCHAR)1, (STDCHAR)1, 0, 0, 0,
    /* 016af8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_6b[256] = {
    /* 016b00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b30 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0,
    /* 016b38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016b98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ba0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ba8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016bb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016bb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016bc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016bc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016bd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016bd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016be0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016be8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016bf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016bf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_6f[256] = {
    /* 016f00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016f98 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fa0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fa8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fe0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016fe8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 016ff0 */ (STDCHAR)6, (STDCHAR)6, 0, 0, 0, 0, 0, 0,
    /* 016ff8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_bc[256] = {
    /* 01bc00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc18 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc30 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc38 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc40 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc48 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc50 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc78 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc80 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc88 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc90 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bc98 */ 0, 0, 0, 0, 0, 0, (STDCHAR)1, 0,
    /* 01bca0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bca8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bcb0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bcb8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bcc0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bcc8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bcd0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bcd8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bce0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bce8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bcf0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01bcf8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_d1[256] = {
    /* 01d100 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d108 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d110 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d118 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d120 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d128 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d130 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d138 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d140 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d148 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d150 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d158 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d160 */ 0, 0, 0, 0, 0, (STDCHAR)216, (STDCHAR)216, (STDCHAR)1,
    /* 01d168 */ (STDCHAR)1, (STDCHAR)1, 0, 0, 0, (STDCHAR)226, (STDCHAR)216, (STDCHAR)216,
    /* 01d170 */ (STDCHAR)216, (STDCHAR)216, (STDCHAR)216, 0, 0, 0, 0, 0,
    /* 01d178 */ 0, 0, 0, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220,
    /* 01d180 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /* 01d188 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)220, (STDCHAR)220, 0, 0, 0, 0,
    /* 01d190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d198 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1a8 */ 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0,
    /* 01d1b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d1f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_d2[256] = {
    /* 01d200 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d208 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d210 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d218 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d220 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d228 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d230 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d238 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d240 */ 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0, 0,
    /* 01d248 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d250 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d258 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d260 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d268 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d270 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d278 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d280 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d288 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d290 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d298 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01d2f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_e0[256] = {
    /* 01e000 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0,
    /* 01e008 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /* 01e010 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /* 01e018 */ (STDCHAR)230, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /* 01e020 */ (STDCHAR)230, (STDCHAR)230, 0, (STDCHAR)230, (STDCHAR)230, 0, (STDCHAR)230, (STDCHAR)230,
    /* 01e028 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0, 0, 0, 0, 0,
    /* 01e030 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e038 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e040 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e048 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e050 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e058 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e060 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e068 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e070 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e078 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e080 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e088 */ 0, 0, 0, 0, 0, 0, 0, (STDCHAR)230,
    /* 01e090 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e098 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e0f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_e1[256] = {
    /* 01e100 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e108 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e110 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e118 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e120 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e128 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e130 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, 0,
    /* 01e138 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e140 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e148 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e150 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e158 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e160 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e168 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e170 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e178 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e180 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e188 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e190 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e198 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e1f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_e2[256] = {
    /* 01e200 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e208 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e210 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e218 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e220 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e228 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e230 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e238 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e240 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e248 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e250 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e258 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e260 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e268 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e270 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e278 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e280 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e288 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e290 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e298 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2a8 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, 0,
    /* 01e2b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2e8 */ 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /* 01e2f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e2f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_e4[256] = {
    /* 01e400 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e408 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e410 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e418 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e420 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e428 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e430 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e438 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e440 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e448 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e450 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e458 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e460 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e468 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e470 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e478 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e480 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e488 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e490 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e498 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4e8 */ 0, 0, 0, 0, (STDCHAR)232, (STDCHAR)232, (STDCHAR)220, (STDCHAR)230,
    /* 01e4f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e4f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_e5[256] = {
    /* 01e500 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e508 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e510 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e518 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e520 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e528 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e530 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e538 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e540 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e548 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e550 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e558 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e560 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e568 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e570 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e578 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e580 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e588 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e590 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e598 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5e8 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)220,
    /* 01e5f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e5f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_e6[256] = {
    /* 01e600 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e608 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e610 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e618 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e620 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e628 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e630 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e638 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e640 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e648 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e650 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e658 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e660 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e668 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e670 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e678 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e680 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e688 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e690 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e698 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e6a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e6a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e6b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e6b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e6c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e6c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e6d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e6d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e6e0 */ 0, 0, 0, (STDCHAR)230, 0, 0, (STDCHAR)230, 0,
    /* 01e6e8 */ 0, 0, 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230,
    /* 01e6f0 */ 0, 0, 0, 0, 0, (STDCHAR)230, 0, 0,
    /* 01e6f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_e8[256] = {
    /* 01e800 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e808 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e810 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e818 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e820 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e828 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e830 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e838 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e840 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e848 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e850 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e858 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e860 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e868 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e870 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e878 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e880 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e888 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e890 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e898 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8d0 */ (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, (STDCHAR)220, 0,
    /* 01e8d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e8f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

static const STDCHAR UN8IF_combin_01_e9[256] = {
    /* 01e900 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e908 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e910 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e918 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e920 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e928 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e930 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e938 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e940 */ 0, 0, 0, 0, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230, (STDCHAR)230,
    /* 01e948 */ (STDCHAR)230, (STDCHAR)230, (STDCHAR)7, 0, 0, 0, 0, 0,
    /* 01e950 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e958 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e960 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e968 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e970 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e978 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e980 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e988 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e990 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e998 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9a0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9a8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9b0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9b8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9c0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9c8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9d0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9d8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9e0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9e8 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9f0 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 01e9f8 */ 0, 0, 0, 0, 0, 0, 0, 0};

/* the planes */
static const STDCHAR *UN8IF_combin_00[256] = {
    NULL, NULL, NULL, UN8IF_combin_00_03,
    UN8IF_combin_00_04,
    UN8IF_combin_00_05,
    UN8IF_combin_00_06,
    UN8IF_combin_00_07,
    UN8IF_combin_00_08,
    UN8IF_combin_00_09,
    UN8IF_combin_00_0a,
    UN8IF_combin_00_0b,
    UN8IF_combin_00_0c,
    UN8IF_combin_00_0d,
    UN8IF_combin_00_0e,
    UN8IF_combin_00_0f,
    UN8IF_combin_00_10,
    NULL, NULL, UN8IF_combin_00_13,
    NULL, NULL, NULL, UN8IF_combin_00_17,
    UN8IF_combin_00_18,
    UN8IF_combin_00_19,
    UN8IF_combin_00_1a,
    UN8IF_combin_00_1b,
    UN8IF_combin_00_1c,
    UN8IF_combin_00_1d,
    NULL, NULL,
    UN8IF_combin_00_20,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, UN8IF_combin_00_2c,
    UN8IF_combin_00_2d,
    NULL, NULL,
    UN8IF_combin_00_30,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_combin_00_a6,
    NULL,
    UN8IF_combin_00_a8,
    UN8IF_combin_00_a9,
    UN8IF_combin_00_aa,
    UN8IF_combin_00_ab,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, UN8IF_combin_00_fb,
    NULL, NULL, UN8IF_combin_00_fe,
    NULL};

static const STDCHAR *UN8IF_combin_01[256] = {
    NULL, UN8IF_combin_01_01,
    UN8IF_combin_01_02,
    UN8IF_combin_01_03,
    NULL, NULL, NULL, NULL,
    NULL, NULL, UN8IF_combin_01_0a,
    NULL, NULL, UN8IF_combin_01_0d,
    UN8IF_combin_01_0e,
    UN8IF_combin_01_0f,
    UN8IF_combin_01_10,
    UN8IF_combin_01_11,
    UN8IF_combin_01_12,
    UN8IF_combin_01_13,
    UN8IF_combin_01_14,
    UN8IF_combin_01_15,
    UN8IF_combin_01_16,
    UN8IF_combin_01_17,
    UN8IF_combin_01_18,
    UN8IF_combin_01_19,
    UN8IF_combin_01_1a,
    NULL, UN8IF_combin_01_1c,
    UN8IF_combin_01_1d,
    NULL, UN8IF_combin_01_1f,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, UN8IF_combin_01_61,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, UN8IF_combin_01_6a,
    UN8IF_combin_01_6b,
    NULL, NULL, NULL, UN8IF_combin_01_6f,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, UN8IF_combin_01_bc,
    NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, UN8IF_combin_01_d1,
    UN8IF_combin_01_d2,
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    UN8IF_combin_01_e0,
    UN8IF_combin_01_e1,
    UN8IF_combin_01_e2,
    NULL, UN8IF_combin_01_e4,
    UN8IF_combin_01_e5,
    UN8IF_combin_01_e6,
    NULL,
    UN8IF_combin_01_e8,
    UN8IF_combin_01_e9,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/* the main plane */
static const STDCHAR **UN8IF_combin[] = {
    UN8IF_combin_00,
    UN8IF_combin_01,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL};

/*
        HANGUL (Korean) has special normalization rules.
        Unicode 9.0
*/
// clang-format off
#define Hangul_SBase  0xAC00
#define Hangul_SFinal 0xD7A3
#define Hangul_SCount  11172

#define Hangul_NCount    588

#define Hangul_LBase  0x1100
#define Hangul_LFinal 0x1112
#define Hangul_LCount     19

#define Hangul_VBase  0x1161
#define Hangul_VFinal 0x1175
#define Hangul_VCount     21

#define Hangul_TBase  0x11A7
#define Hangul_TFinal 0x11C2
#define Hangul_TCount     28

#define Hangul_IsS(u)  ((Hangul_SBase <= (u)) && ((u) <= Hangul_SFinal))
#define Hangul_IsN(u)  (((u) - Hangul_SBase) % Hangul_TCount == 0)
#define Hangul_IsLV(u) (Hangul_IsS(u) && Hangul_IsN(u))
#define Hangul_IsL(u)  ((Hangul_LBase <= (u)) && ((u) <= Hangul_LFinal))
#define Hangul_IsV(u)  ((Hangul_VBase <= (u)) && ((u) <= Hangul_VFinal))
#define Hangul_IsT(u)  ((Hangul_TBase  < (u)) && ((u) <= Hangul_TFinal))

/* much better would be the simple Hangul_IsS(cp) check */
/*** perl5/cperl GENERATED CODE for all Hangul codepoints ***/
#define is_HANGUL_cp_high(cp)                               \
    ( ( 0x1100 <= cp && cp <= 0x11FF ) || ( 0x11FF < cp &&  \
    ( 0x302E == cp || ( 0x302E < cp &&                      \
    ( 0x302F == cp || ( 0x302F < cp &&                      \
    ( ( 0x3131 <= cp && cp <= 0x318E ) || ( 0x318E < cp &&  \
    ( ( 0x3200 <= cp && cp <= 0x321E ) || ( 0x321E < cp &&  \
    ( ( 0x3260 <= cp && cp <= 0x327E ) || ( 0x327E < cp &&  \
    ( ( 0xA960 <= cp && cp <= 0xA97C ) || ( 0xA97C < cp &&  \
    ( ( 0xAC00 <= cp && cp <= 0xD7A3 ) || ( 0xD7A3 < cp &&  \
    ( ( 0xD7B0 <= cp && cp <= 0xD7C6 ) || ( 0xD7C6 < cp &&  \
    ( ( 0xD7CB <= cp && cp <= 0xD7FB ) || ( 0xD7FB < cp &&  \
    ( ( 0xFFA0 <= cp && cp <= 0xFFBE ) || ( 0xFFBE < cp &&  \
    ( ( 0xFFC2 <= cp && cp <= 0xFFC7 ) || ( 0xFFC7 < cp &&  \
    ( ( 0xFFCA <= cp && cp <= 0xFFCF ) || ( 0xFFCF < cp &&  \
    ( ( 0xFFD2 <= cp && cp <= 0xFFD7 ) || ( 0xFFDA <= cp && cp <= 0xFFDC ) \
    )))))))))))))))))))))))))) )
// clang-format on

typedef bool func_tr31(const uint32_t cp);
struct func_tr31_s {
    func_tr31 *start;
    func_tr31 *cont;
};
static const struct sc_tr39 *isTR39_start(const uint32_t cp);
static const struct sc_tr39 *isTR39_cont(const uint32_t cp);

/* Provide a mapping of the 174 Script properties to an index byte.
   Sorted into usages.
 */
static const char *const all_scripts[] = {
    // clang-format off
    // Recommended Scripts (not need to add them)
    // https://www.unicode.org/reports/tr31/#Table_Recommended_Scripts
    "Common",	// 0
    "Inherited",	// 1
    "Latin",	// 2
    "Arabic",	// 3
    "Armenian",	// 4
    "Bengali",	// 5
    "Bopomofo",	// 6
    "Cyrillic",	// 7
    "Devanagari",	// 8
    "Ethiopic",	// 9
    "Georgian",	// 10
    "Greek",	// 11
    "Gujarati",	// 12
    "Gurmukhi",	// 13
    "Hangul",	// 14
    "Han",	// 15
    "Hebrew",	// 16
    "Hiragana",	// 17
    "Katakana",	// 18
    "Kannada",	// 19
    "Khmer",	// 20
    "Lao",	// 21
    "Malayalam",	// 22
    "Myanmar",	// 23
    "Oriya",	// 24
    "Sinhala",	// 25
    "Tamil",	// 26
    "Telugu",	// 27
    "Thaana",	// 28
    "Thai",	// 29
    "Tibetan",	// 30
    // Excluded Scripts (but can be added expliclitly)
    // https://www.unicode.org/reports/tr31/#Table_Candidate_Characters_for_Exclusion_from_Identifiers
    "Ahom",	// 31
    "Anatolian_Hieroglyphs",	// 32
    "Avestan",	// 33
    "Bassa_Vah",	// 34
    "Beria_Erfe",	// 35
    "Bhaiksuki",	// 36
    "Brahmi",	// 37
    "Braille",	// 38
    "Buginese",	// 39
    "Buhid",	// 40
    "Carian",	// 41
    "Caucasian_Albanian",	// 42
    "Chorasmian",	// 43
    "Coptic",	// 44
    "Cuneiform",	// 45
    "Cypriot",	// 46
    "Cypro_Minoan",	// 47
    "Deseret",	// 48
    "Dives_Akuru",	// 49
    "Dogra",	// 50
    "Duployan",	// 51
    "Egyptian_Hieroglyphs",	// 52
    "Elbasan",	// 53
    "Elymaic",	// 54
    "Garay",	// 55
    "Glagolitic",	// 56
    "Gothic",	// 57
    "Grantha",	// 58
    "Gunjala_Gondi",	// 59
    "Gurung_Khema",	// 60
    "Hanunoo",	// 61
    "Hatran",	// 62
    "Imperial_Aramaic",	// 63
    "Inscriptional_Pahlavi",	// 64
    "Inscriptional_Parthian",	// 65
    "Kaithi",	// 66
    "Kawi",	// 67
    "Kharoshthi",	// 68
    "Khitan_Small_Script",	// 69
    "Khojki",	// 70
    "Khudawadi",	// 71
    "Kirat_Rai",	// 72
    "Linear_A",	// 73
    "Linear_B",	// 74
    "Lycian",	// 75
    "Lydian",	// 76
    "Mahajani",	// 77
    "Makasar",	// 78
    "Manichaean",	// 79
    "Marchen",	// 80
    "Masaram_Gondi",	// 81
    "Medefaidrin",	// 82
    "Mende_Kikakui",	// 83
    "Meroitic_Cursive",	// 84
    "Meroitic_Hieroglyphs",	// 85
    "Modi",	// 86
    "Mongolian",	// 87
    "Mro",	// 88
    "Multani",	// 89
    "Nabataean",	// 90
    "Nag_Mundari",	// 91
    "Nandinagari",	// 92
    "Nushu",	// 93
    "Ogham",	// 94
    "Ol_Onal",	// 95
    "Old_Hungarian",	// 96
    "Old_Italic",	// 97
    "Old_North_Arabian",	// 98
    "Old_Permic",	// 99
    "Old_Persian",	// 100
    "Old_Sogdian",	// 101
    "Old_South_Arabian",	// 102
    "Old_Turkic",	// 103
    "Old_Uyghur",	// 104
    "Osmanya",	// 105
    "Pahawh_Hmong",	// 106
    "Palmyrene",	// 107
    "Pau_Cin_Hau",	// 108
    "Phags_Pa",	// 109
    "Phoenician",	// 110
    "Psalter_Pahlavi",	// 111
    "Rejang",	// 112
    "Runic",	// 113
    "Samaritan",	// 114
    "Sharada",	// 115
    "Shavian",	// 116
    "Siddham",	// 117
    "Sidetic",	// 118
    "SignWriting",	// 119
    "Sogdian",	// 120
    "Sora_Sompeng",	// 121
    "Soyombo",	// 122
    "Sunuwar",	// 123
    "Tagalog",	// 124
    "Tagbanwa",	// 125
    "Tai_Yo",	// 126
    "Takri",	// 127
    "Tangsa",	// 128
    "Tangut",	// 129
    "Tirhuta",	// 130
    "Todhri",	// 131
    "Tolong_Siki",	// 132
    "Toto",	// 133
    "Tulu_Tigalari",	// 134
    "Ugaritic",	// 135
    "Vithkuqi",	// 136
    "Warang_Citi",	// 137
    "Yezidi",	// 138
    "Zanabazar_Square",	// 139
    // Limited Use Scripts
    // https://www.unicode.org/reports/tr31/#Table_Limited_Use_Scripts
    "Adlam",	// 140
    "Balinese",	// 141
    "Bamum",	// 142
    "Batak",	// 143
    "Canadian_Aboriginal",	// 144
    "Chakma",	// 145
    "Cham",	// 146
    "Cherokee",	// 147
    "Hanifi_Rohingya",	// 148
    "Javanese",	// 149
    "Kayah_Li",	// 150
    "Lepcha",	// 151
    "Limbu",	// 152
    "Lisu",	// 153
    "Mandaic",	// 154
    "Meetei_Mayek",	// 155
    "Miao",	// 156
    "New_Tai_Lue",	// 157
    "Newa",	// 158
    "Nko",	// 159
    "Nyiakeng_Puachue_Hmong",	// 160
    "Ol_Chiki",	// 161
    "Osage",	// 162
    "Saurashtra",	// 163
    "Sundanese",	// 164
    "Syloti_Nagri",	// 165
    "Syriac",	// 166
    "Tai_Le",	// 167
    "Tai_Tham",	// 168
    "Tai_Viet",	// 169
    "Tifinagh",	// 170
    "Vai",	// 171
    "Wancho",	// 172
    "Yi",	// 173
    "Unknown",	// 174
    // clang-format on
};

struct scx {
    uint32_t from;
    uint32_t to;
    uint8_t gc; // enum u8id_gc is too large
    const char *scx; // indices into sc
};

// Maybe remove all Limited Use SC's from the list on hardcoded profiles 3-5.
// But 120 entries is small enough.
static const struct scx scx_list[] = {
    // clang-format off
    {0x02C7, 0x02C7, GC_Lm, "\x06\x02"},	// Bopo Latn
    {0x02C9, 0x02CB, GC_Lm, "\x06\x02"},	// Bopo Latn
    {0x02CD, 0x02CD, GC_Lm, "\x02\x99"},	// Latn Lisu
    {0x02D7, 0x02D7, GC_Sk, "\x02\x1d"},	// Latn Thai                      (Not_XID)
    {0x02D9, 0x02D9, GC_Sk, "\x06\x02"},	// Bopo Latn                      (Not_NFKC)
    {0x0302, 0x0302, GC_Mn, "\x93\x07\x02\xaa"},	// Cher Cyrl Latn Tfng
    {0x0303, 0x0303, GC_Mn, "\x38\x02\x7b\xa6\x1d"},	// Glag Latn Sunu Syrc Thai
    {0x0305, 0x0305, GC_Mn, "\x2c\x35\x38\x39\x12\x02"},	// Copt Elba Glag Goth Kana Latn  (Uncommon_Use)
    {0x0306, 0x0306, GC_Mn, "\x07\x0b\x02\x63\xaa"},	// Cyrl Grek Latn Perm Tfng
    {0x0309, 0x0309, GC_Mn, "\x02\xaa"},	// Latn Tfng
    {0x030A, 0x030A, GC_Mn, "\x33\x02\xa6"},	// Dupl Latn Syrc
    {0x030B, 0x030B, GC_Mn, "\x93\x07\x02\xa2"},	// Cher Cyrl Latn Osge
    {0x030C, 0x030C, GC_Mn, "\x93\x02\xa7"},	// Cher Latn Tale
    {0x030D, 0x030D, GC_Mn, "\x02\x7b"},	// Latn Sunu                      (Uncommon_Use)
    {0x030E, 0x030E, GC_Mn, "\x09\x02"},	// Ethi Latn
    {0x0310, 0x0310, GC_Mn, "\x02\x7b"},	// Latn Sunu
    {0x0311, 0x0311, GC_Mn, "\x07\x02\x83"},	// Cyrl Latn Todr
    {0x0313, 0x0313, GC_Mn, "\x0b\x02\x63\x83"},	// Grek Latn Perm Todr
    {0x0323, 0x0323, GC_Mn, "\x93\x33\x12\x02\xa6\xaa"},	// Cher Dupl Kana Latn Syrc Tfng
    {0x0324, 0x0324, GC_Mn, "\x93\x33\x02\xa6"},	// Cher Dupl Latn Syrc
    {0x0325, 0x0325, GC_Mn, "\x02\xa6"},	// Latn Syrc
    {0x032D, 0x032D, GC_Mn, "\x02\x7b\xa6"},	// Latn Sunu Syrc
    {0x032E, 0x032E, GC_Mn, "\x02\xa6"},	// Latn Syrc
    {0x0330, 0x0330, GC_Mn, "\x93\x02\xa6"},	// Cher Latn Syrc
    {0x0342, 0x0345, GC_Mn, "\x0b"},	// Grek
    {0x0358, 0x0358, GC_Mn, "\x02\xa2"},	// Latn Osge                      (Uncommon_Use)
    {0x035E, 0x035E, GC_Mn, "\x2a\x02\x83"},	// Aghb Latn Todr
    {0x0363, 0x036F, GC_Mn, "\x02"},	// Latn                           (Obsolete)
    {0x0374, 0x0375, GC_Lm, "\x2c\x0b"},	// Copt Grek                      (Not_NFKC)
    {0x0483, 0x0483, GC_Mn, "\x07\x63"},	// Cyrl Perm                      (Obsolete)
    {0x0484, 0x0484, GC_Mn, "\x07\x38"},	// Cyrl Glag                      (Technical Obsolete)
    {0x0485, 0x0486, GC_Mn, "\x07\x02"},	// Cyrl Latn                      (Technical Obsolete)
    {0x0487, 0x0487, GC_Mn, "\x07\x38"},	// Cyrl Glag                      (Technical Obsolete)
    {0x0589, 0x0589, GC_Po, "\x04\x0a\x38"},	// Armn Geor Glag                 (Not_XID)
    {0x061C, 0x061C, GC_Cf, "\x03\xa6\x1c"},	// Arab Syrc Thaa                 (Default_Ignorable)
    {0x064B, 0x0655, GC_Mn, "\x03\xa6"},	// Arab Syrc
    {0x0660, 0x0669, GC_Nd, "\x03\x1c\x8a"},	// Arab Thaa Yezi
    {0x0670, 0x0670, GC_Mn, "\x03\xa6"},	// Arab Syrc
    {0x06D4, 0x06D4, GC_Po, "\x03\x94"},	// Arab Rohg                      (Not_XID)
    {0x0966, 0x096F, GC_Nd, "\x08\x32\x42\x4d"},	// Deva Dogr Kthi Mahj
    {0x09E6, 0x09EF, GC_Nd, "\x05\x91\xa5"},	// Beng Cakm Sylo
    {0x0A66, 0x0A6F, GC_Nd, "\x0d\x59"},	// Guru Mult                      (Uncommon_Use)
    {0x0AE6, 0x0AEF, GC_Nd, "\x0c\x46"},	// Gujr Khoj
    {0x0BE6, 0x0BF3, GC_Nd, "\x3a\x1a"},	// Gran Taml                      (Uncommon_Use)
    {0x0CE6, 0x0CEF, GC_Nd, "\x13\x5c\x86"},	// Knda Nand Tutg
    {0x1040, 0x1049, GC_Nd, "\x91\x17\xa7"},	// Cakm Mymr Tale
    {0x10FB, 0x10FB, GC_Po, "\x0a\x38\x02"},	// Geor Glag Latn                 (Not_XID)
    {0x16EB, 0x16ED, GC_Po, "\x71"},	// Runr                           (Exclusion Not_XID)
    {0x1735, 0x1736, GC_Po, "\x28\x3d\x7d\x7c"},	// Buhd Hano Tagb Tglg            (Exclusion Not_XID)
    {0x1802, 0x1803, GC_Po, "\x57\x6d"},	// Mong Phag                      (Exclusion Not_XID)
    {0x1805, 0x1805, GC_Po, "\x57\x6d"},	// Mong Phag                      (Exclusion Not_XID)
    {0x1CD0, 0x1CD0, GC_Mn, "\x05\x08\x3a\x13"},	// Beng Deva Gran Knda            (Obsolete)
    {0x1CD1, 0x1CD1, GC_Mn, "\x08"},	// Deva                           (Obsolete)
    {0x1CD2, 0x1CD2, GC_Mn, "\x05\x08\x3a\x13"},	// Beng Deva Gran Knda            (Obsolete)
    {0x1CD3, 0x1CD3, GC_Po, "\x08\x3a\x13"},	// Deva Gran Knda                 (Obsolete Not_XID)
    {0x1CD4, 0x1CD4, GC_Mn, "\x08"},	// Deva                           (Obsolete)
    {0x1CD5, 0x1CD5, GC_Mn, "\x05\x08\x9e\x1b\x82"},	// Beng Deva Newa Telu Tirh       (Obsolete)
    {0x1CD6, 0x1CD6, GC_Mn, "\x05\x08\x1b"},	// Beng Deva Telu                 (Obsolete)
    {0x1CD7, 0x1CD7, GC_Mn, "\x08\x9e\x73"},	// Deva Newa Shrd                 (Obsolete)
    {0x1CD8, 0x1CD8, GC_Mn, "\x05\x08\x9e\x1b"},	// Beng Deva Newa Telu            (Obsolete)
    {0x1CD9, 0x1CD9, GC_Mn, "\x08\x73"},	// Deva Shrd                      (Obsolete)
    {0x1CDA, 0x1CDA, GC_Mn, "\x08\x13\x16\x18\x1a\x1b"},	// Deva Knda Mlym Orya Taml Telu  (Obsolete)
    {0x1CDB, 0x1CDB, GC_Mn, "\x08"},	// Deva                           (Obsolete)
    {0x1CDC, 0x1CDD, GC_Mn, "\x08\x73"},	// Deva Shrd                      (Obsolete)
    {0x1CDE, 0x1CDF, GC_Mn, "\x08"},	// Deva                           (Obsolete)
    {0x1CE0, 0x1CE0, GC_Mn, "\x08\x73"},	// Deva Shrd                      (Obsolete)
    {0x1CE1, 0x1CE1, GC_Mc, "\x05\x08"},	// Beng Deva                      (Obsolete)
    {0x1CE2, 0x1CE2, GC_Mn, "\x08\x9e\x82"},	// Deva Newa Tirh                 (Obsolete)
    {0x1CE3, 0x1CE8, GC_Mn, "\x08"},	// Deva                           (Obsolete)
    {0x1CE9, 0x1CE9, GC_Lo, "\x08\x5c\x9e"},	// Deva Nand Newa                 (Obsolete)
    {0x1CEA, 0x1CEA, GC_Lo, "\x05\x08\x73"},	// Beng Deva Shrd                 (Obsolete)
    {0x1CEB, 0x1CEB, GC_Lo, "\x08\x9e"},	// Deva Newa                      (Obsolete)
    {0x1CEC, 0x1CEC, GC_Lo, "\x08"},	// Deva                           (Obsolete)
    {0x1CED, 0x1CED, GC_Mn, "\x05\x08\x9e\x73"},	// Beng Deva Newa Shrd            (Obsolete)
    {0x1CEE, 0x1CF1, GC_Lo, "\x08"},	// Deva                           (Obsolete)
    {0x1CF3, 0x1CF3, GC_Lo, "\x08\x3a"},	// Deva Gran                      (Obsolete)
    {0x1CF4, 0x1CF4, GC_Mn, "\x08\x3a\x13\x86"},	// Deva Gran Knda Tutg            (Obsolete)
    {0x1CF5, 0x1CF6, GC_Lo, "\x05\x08"},	// Beng Deva                      (Obsolete)
    {0x1CF7, 0x1CF7, GC_Mc, "\x05"},	// Beng                           (Obsolete)
    {0x1CF8, 0x1CF9, GC_Mn, "\x08\x3a"},	// Deva Gran                      (Obsolete)
    {0x1CFA, 0x1CFA, GC_Lo, "\x5c"},	// Nand                           (Exclusion)
    {0x1DC0, 0x1DC1, GC_Mn, "\x0b"},	// Grek                           (Technical Obsolete)
    {0x1DF8, 0x1DF8, GC_Mn, "\x07\x02\xa6"},	// Cyrl Latn Syrc
    {0x1DFA, 0x1DFA, GC_Mn, "\xa6"},	// Syrc                           (Limited_Use Technical)
    {0x202F, 0x202F, GC_Zs, "\x02\x57\x6d"},	// Latn Mong Phag                 (Not_NFKC)
    {0x204F, 0x204F, GC_Po, "\x8c\x03"},	// Adlm Arab                      (Not_XID)
    {0x205A, 0x205A, GC_Po, "\x29\x0a\x38\x60\x4b\x67"},	// Cari Geor Glag Hung Lyci Orkh  (Obsolete Not_XID)
    {0x205D, 0x205D, GC_Po, "\x29\x0b\x60\x55"},	// Cari Grek Hung Mero            (Obsolete Not_XID)
    {0x20F0, 0x20F0, GC_Mn, "\x08\x3a\x02"},	// Deva Gran Latn
    {0x2E17, 0x2E17, GC_Pd, "\x2c\x02"},	// Copt Latn                      (Not_XID)
    {0x2E30, 0x2E30, GC_Po, "\x21\x67"},	// Avst Orkh                      (Exclusion Not_XID)
    {0x2E3C, 0x2E3C, GC_Po, "\x33"},	// Dupl                           (Exclusion Not_XID)
    {0x2E41, 0x2E41, GC_Po, "\x8c\x03\x60"},	// Adlm Arab Hung                 (Not_XID)
    {0x2E43, 0x2E43, GC_Po, "\x07\x38"},	// Cyrl Glag                      (Not_XID)
    {0x2FF0, 0x2FFF, GC_So, "\x0f\x81"},	// Hani Tang                      (Not_XID)
    {0x3003, 0x3003, GC_Po, "\x06\x0e\x0f\x11\x12"},	// Bopo Hang Hani Hira Kana       (Not_XID)
    {0x3006, 0x3006, GC_Lo, "\x0f"},	// Hani
    {0x300C, 0x3011, GC_Ps, "\x06\x0e\x0f\x11\x12\xad"},	// Bopo Hang Hani Hira Kana Yiii  (Not_XID)
    {0x3013, 0x3013, GC_So, "\x06\x0e\x0f\x11\x12"},	// Bopo Hang Hani Hira Kana       (Not_XID)
    {0x3014, 0x301B, GC_Ps, "\x06\x0e\x0f\x11\x12\xad"},	// Bopo Hang Hani Hira Kana Yiii  (Not_XID)
    {0x301C, 0x301F, GC_Pd, "\x06\x0e\x0f\x11\x12"},	// Bopo Hang Hani Hira Kana       (Not_XID)
    {0x302A, 0x302D, GC_Mn, "\x06\x0f"},	// Bopo Hani
    {0x3030, 0x3030, GC_Pd, "\x06\x0e\x0f\x11\x12"},	// Bopo Hang Hani Hira Kana       (Not_XID)
    {0x3031, 0x3035, GC_Lm, "\x11\x12"},	// Hira Kana
    {0x3037, 0x3037, GC_So, "\x06\x0e\x0f\x11\x12"},	// Bopo Hang Hani Hira Kana       (Not_XID)
    {0x303C, 0x303D, GC_Lo, "\x0f\x11\x12"},	// Hani Hira Kana
    {0x303E, 0x303F, GC_So, "\x0f"},	// Hani                           (Not_XID)
    {0x3099, 0x309C, GC_Mn, "\x11\x12"},	// Hira Kana                      (Uncommon_Use)
    {0x30A0, 0x30A0, GC_Pd, "\x11\x12"},	// Hira Kana
    {0x30FB, 0x30FB, GC_Po, "\x06\x0e\x0f\x11\x12\xad"},	// Bopo Hang Hani Hira Kana Yiii
    {0x30FC, 0x30FC, GC_Lm, "\x11\x12"},	// Hira Kana
    {0x3190, 0x319F, GC_So, "\x0f"},	// Hani                           (Not_XID)
    {0x31C0, 0x31E5, GC_So, "\x0f"},	// Hani                           (Not_XID)
    {0x31EF, 0x31EF, GC_So, "\x0f\x81"},	// Hani Tang                      (Not_XID)
    {0x3220, 0x3247, GC_No, "\x0f"},	// Hani                           (Not_NFKC)
    {0x3280, 0x32B0, GC_No, "\x0f"},	// Hani                           (Not_NFKC)
    {0x32C0, 0x32CB, GC_So, "\x0f"},	// Hani                           (Not_NFKC)
    {0x32FF, 0x32FF, GC_So, "\x0f"},	// Hani                           (Not_NFKC)
    {0x3358, 0x3370, GC_So, "\x0f"},	// Hani                           (Not_NFKC)
    {0x337B, 0x337F, GC_So, "\x0f"},	// Hani                           (Not_NFKC)
    {0x33E0, 0x33FE, GC_So, "\x0f"},	// Hani                           (Not_NFKC)
    {0xA66F, 0xA66F, GC_Mn, "\x07\x38"},	// Cyrl Glag                      (Uncommon_Use)
    {0xA700, 0xA707, GC_Sk, "\x0f\x02"},	// Hani Latn                      (Obsolete Not_XID)
    {0xA8F1, 0xA8F1, GC_Mn, "\x05\x08\x86"},	// Beng Deva Tutg                 (Obsolete)
    {0xA8F3, 0xA8F3, GC_Lo, "\x08\x1a"},	// Deva Taml                      (Obsolete)
    {0xA92E, 0xA92E, GC_Po, "\x96\x02\x17"},	// Kali Latn Mymr                 (Not_XID)
    {0xA9CF, 0xA9CF, GC_Lm, "\x27\x95"},	// Bugi Java                      (Limited_Use Uncommon_Use)
    {0xFD3E, 0xFD3F, GC_Pe, "\x03\x9f"},	// Arab Nkoo                      (Technical Not_XID)
    {0xFDF2, 0xFDF2, GC_Lo, "\x03\x1c"},	// Arab Thaa                      (Not_NFKC)
    {0xFDFD, 0xFDFD, GC_So, "\x03\x1c"},	// Arab Thaa                      (Technical Not_XID)
    {0xFE45, 0xFE46, GC_Po, "\x06\x0e\x0f\x11\x12"},	// Bopo Hang Hani Hira Kana       (Technical Not_XID)
    {0xFF61, 0xFF65, GC_Po, "\x06\x0e\x0f\x11\x12\xad"},	// Bopo Hang Hani Hira Kana Yiii  (Not_NFKC)
    {0xFF70, 0xFF70, GC_Lm, "\x11\x12"},	// Hira Kana                      (Not_NFKC)
    {0xFF9E, 0xFF9F, GC_Lm, "\x11\x12"},	// Hira Kana                      (Not_NFKC)
    {0x10100, 0x10101, GC_Po, "\x2f\x2e\x4a"},	// Cpmn Cprt Linb                 (Exclusion Not_XID)
    {0x10102, 0x10102, GC_Po, "\x2e\x4a"},	// Cprt Linb                      (Exclusion Not_XID)
    {0x10107, 0x10133, GC_No, "\x2e\x49\x4a"},	// Cprt Lina Linb                 (Exclusion Not_XID)
    {0x10137, 0x1013F, GC_So, "\x2e\x4a"},	// Cprt Linb                      (Exclusion Not_XID)
    {0x102E0, 0x102FB, GC_Mn, "\x03\x2c"},	// Arab Copt                      (Obsolete)
    {0x10AF2, 0x10AF2, GC_Po, "\x4f\x68"},	// Mani Ougr                      (Exclusion Not_XID)
    {0x11301, 0x11301, GC_Mn, "\x3a\x1a"},	// Gran Taml
    {0x11303, 0x11303, GC_Mc, "\x3a\x1a"},	// Gran Taml
    {0x1133B, 0x1133C, GC_Mn, "\x3a\x1a"},	// Gran Taml                      (Uncommon_Use)
    {0x11FD0, 0x11FD1, GC_No, "\x3a\x1a"},	// Gran Taml                      (Not_XID)
    {0x11FD3, 0x11FD3, GC_No, "\x3a\x1a"},	// Gran Taml                      (Not_XID)
    {0x1BCA0, 0x1BCA3, GC_Cf, "\x33"},	// Dupl                           (Default_Ignorable)
    {0x1D360, 0x1D371, GC_No, "\x0f"},	// Hani                           (Not_XID)
    {0x1F250, 0x1F251, GC_So, "\x0f"},	// Hani                           (Not_NFKC)
    // clang-format on
}; // 55 ranges, 93 single codepoints

// NFC_Quick_Check=No
static const struct range_bool NFC_N_list[] = {
    // clang-format off
    {0x0340, 0x0341},
    {0x0343, 0x0344},
    {0x0374, 0x0374},
    {0x037E, 0x037E},
    {0x0387, 0x0387},
    {0x0958, 0x095F},
    {0x09DC, 0x09DD},
    {0x09DF, 0x09DF},
    {0x0A33, 0x0A33},
    {0x0A36, 0x0A36},
    {0x0A59, 0x0A5B},
    {0x0A5E, 0x0A5E},
    {0x0B5C, 0x0B5D},
    {0x0F43, 0x0F43},
    {0x0F4D, 0x0F4D},
    {0x0F52, 0x0F52},
    {0x0F57, 0x0F57},
    {0x0F5C, 0x0F5C},
    {0x0F69, 0x0F69},
    {0x0F73, 0x0F73},
    {0x0F75, 0x0F76},
    {0x0F78, 0x0F78},
    {0x0F81, 0x0F81},
    {0x0F93, 0x0F93},
    {0x0F9D, 0x0F9D},
    {0x0FA2, 0x0FA2},
    {0x0FA7, 0x0FA7},
    {0x0FAC, 0x0FAC},
    {0x0FB9, 0x0FB9},
    {0x1F71, 0x1F71},
    {0x1F73, 0x1F73},
    {0x1F75, 0x1F75},
    {0x1F77, 0x1F77},
    {0x1F79, 0x1F79},
    {0x1F7B, 0x1F7B},
    {0x1F7D, 0x1F7D},
    {0x1FBB, 0x1FBB},
    {0x1FBE, 0x1FBE},
    {0x1FC9, 0x1FC9},
    {0x1FCB, 0x1FCB},
    {0x1FD3, 0x1FD3},
    {0x1FDB, 0x1FDB},
    {0x1FE3, 0x1FE3},
    {0x1FEB, 0x1FEB},
    {0x1FEE, 0x1FEF},
    {0x1FF9, 0x1FF9},
    {0x1FFB, 0x1FFB},
    {0x1FFD, 0x1FFD},
    {0x2000, 0x2001},
    {0x2126, 0x2126},
    {0x212A, 0x212B},
    {0x2329, 0x2329},
    {0x232A, 0x232A},
    {0x2ADC, 0x2ADC},
    {0xF900, 0xFA0D},
    {0xFA10, 0xFA10},
    {0xFA12, 0xFA12},
    {0xFA15, 0xFA1E},
    {0xFA20, 0xFA20},
    {0xFA22, 0xFA22},
    {0xFA25, 0xFA26},
    {0xFA2A, 0xFA6D},
    {0xFA70, 0xFAD9},
    {0xFB1D, 0xFB1D},
    {0xFB1F, 0xFB1F},
    {0xFB2A, 0xFB36},
    {0xFB38, 0xFB3C},
    {0xFB3E, 0xFB3E},
    {0xFB40, 0xFB41},
    {0xFB43, 0xFB44},
    {0xFB46, 0xFB4E},
    {0x1D15E, 0x1D164},
    {0x1D1BB, 0x1D1C0},
    {0x2F800, 0x2FA1D},
    // clang-format on
}; // 23 ranges, 51 single codepoints

// NFC_Quick_Check=Maybe
static const struct range_bool NFC_M_list[] = {
    // clang-format off
    {0x0300, 0x0304},
    {0x0306, 0x030C},
    {0x030F, 0x030F},
    {0x0311, 0x0311},
    {0x0313, 0x0314},
    {0x031B, 0x031B},
    {0x0323, 0x0328},
    {0x032D, 0x032E},
    {0x0330, 0x0331},
    {0x0338, 0x0338},
    {0x0342, 0x0342},
    {0x0345, 0x0345},
    {0x0653, 0x0655},
    {0x093C, 0x093C},
    {0x09BE, 0x09BE},
    {0x09D7, 0x09D7},
    {0x0B3E, 0x0B3E},
    {0x0B56, 0x0B56},
    {0x0B57, 0x0B57},
    {0x0BBE, 0x0BBE},
    {0x0BD7, 0x0BD7},
    {0x0C56, 0x0C56},
    {0x0CC2, 0x0CC2},
    {0x0CD5, 0x0CD6},
    {0x0D3E, 0x0D3E},
    {0x0D57, 0x0D57},
    {0x0DCA, 0x0DCA},
    {0x0DCF, 0x0DCF},
    {0x0DDF, 0x0DDF},
    {0x102E, 0x102E},
    {0x1161, 0x1175},
    {0x11A8, 0x11C2},
    {0x1B35, 0x1B35},
    {0x3099, 0x309A},
    {0x110BA, 0x110BA},
    {0x11127, 0x11127},
    {0x1133E, 0x1133E},
    {0x11357, 0x11357},
    {0x113B8, 0x113B8},
    {0x113BB, 0x113BB},
    {0x113C2, 0x113C2},
    {0x113C5, 0x113C5},
    {0x113C7, 0x113C9},
    {0x114B0, 0x114B0},
    {0x114BA, 0x114BA},
    {0x114BD, 0x114BD},
    {0x115AF, 0x115AF},
    {0x11930, 0x11930},
    {0x1611E, 0x16129},
    {0x16D67, 0x16D68},
    // clang-format on
}; // 14 ranges, 36 single codepoints

// Bidi formatting characters for reordering attacks.
// Only valid with RTL scripts, such as Hebrew and Arabic.
static const struct range_bool bidi_list[] = {
    // clang-format off
    { 0x202A, 0x202E }, // LRE, RLE, PDF, LRO, RLO
    { 0x2066, 0x2069 }, // LRI, RLI, FSI, PDI
    // clang-format on
};

// Greek-Latin confusables. See doc/P2538R0.md (Appendix F - Greek Confusables)
// for TR39
static const uint32_t greek_confus_list[] = {
    0x0370, // ( Ͱ → Ⱶ ) GREEK CAPITAL LETTER HETA → LATIN CAPITAL LETTER HALF H
    0x0377, // ( ͷ → ᴎ ) GREEK SMALL LETTER PAMPHYLIAN DIGAMMA → LATIN LETTER
    // SMALL CAPITAL REVERSED N
    0x037B, // ( ͻ → ɔ ) GREEK SMALL REVERSED LUNATE SIGMA SYMBOL → LATIN SMALL
    // LETTER OPEN O
    0x037D, // ( ͽ → ꜿ ) GREEK SMALL REVERSED DOTTED LUNATE SIGMA SYMBOL → LATIN
    // SMALL LETTER REVERSED C WITH DOT
    0x037F, // ( Ϳ → J ) GREEK CAPITAL LETTER YOT → LATIN CAPITAL LETTER J
    0x0391, // ( Α → A ) GREEK CAPITAL LETTER ALPHA → LATIN CAPITAL LETTER A
    0x0392, // ( Β → B ) GREEK CAPITAL LETTER BETA → LATIN CAPITAL LETTER B
    0x0395, // ( Ε → E ) GREEK CAPITAL LETTER EPSILON → LATIN CAPITAL LETTER E
    0x0396, // ( Ζ → Z ) GREEK CAPITAL LETTER ZETA → LATIN CAPITAL LETTER Z
    0x0397, // ( Η → H ) GREEK CAPITAL LETTER ETA → LATIN CAPITAL LETTER H
    0x0399, // ( Ι → l ) GREEK CAPITAL LETTER IOTA → LATIN SMALL LETTER L
    0x039A, // ( Κ → K ) GREEK CAPITAL LETTER KAPPA → LATIN CAPITAL LETTER K
    0x039B, // ( Λ → Ʌ ) GREEK CAPITAL LETTER LAMDA → LATIN CAPITAL LETTER
    // TURNED V
    0x039C, // ( Μ → M ) GREEK CAPITAL LETTER MU → LATIN CAPITAL LETTER M
    0x039D, // ( Ν → N ) GREEK CAPITAL LETTER NU → LATIN CAPITAL LETTER N
    0x039F, // ( Ο → O ) GREEK CAPITAL LETTER OMICRON → LATIN CAPITAL LETTER O
    0x03A1, // ( Ρ → P ) GREEK CAPITAL LETTER RHO → LATIN CAPITAL LETTER P
    0x03A3, // ( Σ → Ʃ ) GREEK CAPITAL LETTER SIGMA → LATIN CAPITAL LETTER ESH
    0x03A4, // ( Τ → T ) GREEK CAPITAL LETTER TAU → LATIN CAPITAL LETTER T
    0x03A5, // ( Υ → Y ) GREEK CAPITAL LETTER UPSILON → LATIN CAPITAL LETTER Y
    0x03A7, // ( Χ → X ) GREEK CAPITAL LETTER CHI → LATIN CAPITAL LETTER X
    0x03B1, // ( α → a ) GREEK SMALL LETTER ALPHA → LATIN SMALL LETTER A
    0x03B2, // ( β → ß ) GREEK SMALL LETTER BETA → LATIN SMALL LETTER SHARP S
    0x03B3, // ( γ → y ) GREEK SMALL LETTER GAMMA → LATIN SMALL LETTER Y
    0x03B4, // ( δ → ẟ ) GREEK SMALL LETTER DELTA → LATIN SMALL LETTER DELTA
    0x03BA, // ( κ → ĸ ) GREEK SMALL LETTER KAPPA → LATIN SMALL LETTER KRA
    0x03BB, // ( ꟛ → λ ) LATIN SMALL LETTER LAMBDA → GREEK SMALL LETTER
    // LAMDA
    0x03BF, // ( ο → o ) GREEK SMALL LETTER OMICRON → LATIN SMALL LETTER O
    0x03C1, // ( ρ → p ) GREEK SMALL LETTER RHO → LATIN SMALL LETTER P
    0x03C4, // ( τ → ᴛ ) GREEK SMALL LETTER TAU → LATIN LETTER SMALL CAPITAL T
    0x03C5, // ( υ → u ) GREEK SMALL LETTER UPSILON → LATIN SMALL LETTER U
    0x03C6, // ( φ → ɸ ) GREEK SMALL LETTER PHI → LATIN SMALL LETTER PHI
    0x03C7, // ( ꭕ → χ ) LATIN SMALL LETTER CHI WITH LOW LEFT SERIF → GREEK
    // SMALL LETTER CHI
    0x03C9, // ( ꞷ → ω ) LATIN SMALL LETTER OMEGA → GREEK SMALL LETTER OMEGA
    0x03D0, // ( ϐ → ß ) GREEK BETA SYMBOL → LATIN SMALL LETTER SHARP S
    0x03D2, // ( ϒ → Y ) GREEK UPSILON WITH HOOK SYMBOL → LATIN CAPITAL LETTER Y
    0x03D5, // ( ϕ → ɸ ) GREEK PHI SYMBOL → LATIN SMALL LETTER PHI
    0x03DC, // ( Ϝ → F ) GREEK LETTER DIGAMMA → LATIN CAPITAL LETTER F
    0x03F0, // ( ϰ → ĸ ) GREEK KAPPA SYMBOL → LATIN SMALL LETTER KRA
    0x03F2, // ( ϲ → c ) GREEK LUNATE SIGMA SYMBOL → LATIN SMALL LETTER C
    0x03F3, // ( ϳ → j ) GREEK LETTER YOT → LATIN SMALL LETTER J
    0x03F5, // ( ϵ → ꞓ ) GREEK LUNATE EPSILON SYMBOL → LATIN SMALL LETTER C WITH
    // BAR
    0x03F7, // ( Ϸ → Þ ) GREEK CAPITAL LETTER SHO → LATIN CAPITAL LETTER THORN
    0x03F8, // ( ϸ → p ) GREEK SMALL LETTER SHO → LATIN SMALL LETTER P
    0x03F9, // ( Ϲ → C ) GREEK CAPITAL LUNATE SIGMA SYMBOL → LATIN CAPITAL
    // LETTER C
    0x03FA, // ( Ϻ → M ) GREEK CAPITAL LETTER SAN → LATIN CAPITAL LETTER M
    0x03FD, // ( Ͻ → Ɔ ) GREEK CAPITAL REVERSED LUNATE SIGMA SYMBOL → LATIN
    // CAPITAL LETTER OPEN O
    0x03FF, // ( Ͽ → Ꜿ ) GREEK CAPITAL REVERSED DOTTED LUNATE SIGMA SYMBOL →
    // LATIN CAPITAL LETTER REVERSED C WITH DOT
    0x1D26, // ( ᴦ → r ) GREEK LETTER SMALL CAPITAL GAMMA → LATIN SMALL LETTER R
    0x1D27, // ( ᴧ → ʌ ) GREEK LETTER SMALL CAPITAL LAMDA → LATIN SMALL LETTER
    // TURNED V
    0x1D29, // ( ᴩ → ᴘ ) GREEK LETTER SMALL CAPITAL RHO → LATIN LETTER SMALL
    // CAPITAL P
    0x1FBE, // ( ι → i ) GREEK PROSGEGRAMMENI → LATIN SMALL LETTER I
    0x2129, // ( ℩ → ɿ ) TURNED GREEK SMALL LETTER IOTA → LATIN SMALL LETTER
    // REVERSED R WITH FISHHOOK
    0x1D20D, // ( 𝈍 → V ) GREEK VOCAL NOTATION SYMBOL-14 → LATIN CAPITAL LETTER
    // V
    0x1D213, // ( 𝈓 → F ) GREEK VOCAL NOTATION SYMBOL-20 → LATIN CAPITAL LETTER
    // F
    0x1D216, // ( 𝈖 → R ) GREEK VOCAL NOTATION SYMBOL-23 → LATIN CAPITAL LETTER
    // R
    0x1D217, // ( 𝈗 → Ɐ ) GREEK VOCAL NOTATION SYMBOL-24 → LATIN CAPITAL LETTER
    // TURNED A
    0x1D21A, // ( 𝈚 → O̵ ) GREEK VOCAL NOTATION SYMBOL-52 → LATIN CAPITAL LETTER
    // O, COMBINING SHORT STROKE OVERLAY
    0x1D221, // ( 𝈡 → Ɛ ) GREEK INSTRUMENTAL NOTATION SYMBOL-7 → LATIN CAPITAL
    // LETTER OPEN E
    0x1D22A, // ( 𝈪 → L ) GREEK INSTRUMENTAL NOTATION SYMBOL-23 → LATIN CAPITAL
    // LETTER L
    0x1D230, // ( 𝈰 → ꟻ ) GREEK INSTRUMENTAL NOTATION SYMBOL-30 → LATIN
    // EPIGRAPHIC LETTER REVERSED F
};


/* Composite exclusions */

static bool isExclusion(uint32_t uv) {
    return (2392 <= uv && uv <= 2399) || (2524 <= uv && uv <= 2525) || uv == 2527 || uv == 2611 || uv == 2614 || (2649 <= uv && uv <= 2651) || uv == 2654 || (2908 <= uv && uv <= 2909) || uv == 3907 || uv == 3917 || uv == 3922 || uv == 3927 || uv == 3932 || uv == 3945 || uv == 3958 || uv == 3960 || uv == 3987 || uv == 3997 || uv == 4002 || uv == 4007 || uv == 4012 || uv == 4025 || uv == 10972 || uv == 64285 || uv == 64287 || (64298 <= uv && uv <= 64310) || (64312 <= uv && uv <= 64316) || uv == 64318 || (64320 <= uv && uv <= 64321) || (64323 <= uv && uv <= 64324) || (64326 <= uv && uv <= 64334) || (119134 <= uv && uv <= 119140) || (119227 <= uv && uv <= 119232)
        ? TRUE
        : FALSE;
}

/* Composition */
typedef struct {
    uint32_t nextchar;
    uint32_t composite;
} UN8IF_complist;
typedef struct {
    uint16_t nextchar;
    uint16_t composite;
} UN8IF_complist_s;

/* max nextchar: 119154/0x1d172, max composite: 119232/0x1d1c0, max length: 20 */
/* 31/456 lists > short (0xffff) */

#define UN8IF_COMPLIST_FIRST_LONG 0x0105d2

static const UN8IF_complist_s UN8IF_complist_00003c[2] = {
    {0x338, 0x226e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00003d[2] = {
    {0x338, 0x2260}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00003e[2] = {
    {0x338, 0x226f}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000041[17] = {
    {0x300, 0xc0},
    {0x301, 0xc1},
    {0x302, 0xc2},
    {0x303, 0xc3},
    {0x304, 0x100},
    {0x306, 0x102},
    {0x307, 0x226},
    {0x308, 0xc4},
    {0x309, 0x1ea2},
    {0x30a, 0xc5},
    {0x30c, 0x1cd},
    {0x30f, 0x200},
    {0x311, 0x202},
    {0x323, 0x1ea0},
    {0x325, 0x1e00},
    {0x328, 0x104},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000042[4] = {
    {0x307, 0x1e02},
    {0x323, 0x1e04},
    {0x331, 0x1e06},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000043[6] = {
    {0x301, 0x106},
    {0x302, 0x108},
    {0x307, 0x10a},
    {0x30c, 0x10c},
    {0x327, 0xc7},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000044[7] = {
    {0x307, 0x1e0a},
    {0x30c, 0x10e},
    {0x323, 0x1e0c},
    {0x327, 0x1e10},
    {0x32d, 0x1e12},
    {0x331, 0x1e0e},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000045[18] = {
    {0x300, 0xc8},
    {0x301, 0xc9},
    {0x302, 0xca},
    {0x303, 0x1ebc},
    {0x304, 0x112},
    {0x306, 0x114},
    {0x307, 0x116},
    {0x308, 0xcb},
    {0x309, 0x1eba},
    {0x30c, 0x11a},
    {0x30f, 0x204},
    {0x311, 0x206},
    {0x323, 0x1eb8},
    {0x327, 0x228},
    {0x328, 0x118},
    {0x32d, 0x1e18},
    {0x330, 0x1e1a},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000046[2] = {
    {0x307, 0x1e1e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000047[8] = {
    {0x301, 0x1f4},
    {0x302, 0x11c},
    {0x304, 0x1e20},
    {0x306, 0x11e},
    {0x307, 0x120},
    {0x30c, 0x1e6},
    {0x327, 0x122},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000048[8] = {
    {0x302, 0x124},
    {0x307, 0x1e22},
    {0x308, 0x1e26},
    {0x30c, 0x21e},
    {0x323, 0x1e24},
    {0x327, 0x1e28},
    {0x32e, 0x1e2a},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000049[16] = {
    {0x300, 0xcc},
    {0x301, 0xcd},
    {0x302, 0xce},
    {0x303, 0x128},
    {0x304, 0x12a},
    {0x306, 0x12c},
    {0x307, 0x130},
    {0x308, 0xcf},
    {0x309, 0x1ec8},
    {0x30c, 0x1cf},
    {0x30f, 0x208},
    {0x311, 0x20a},
    {0x323, 0x1eca},
    {0x328, 0x12e},
    {0x330, 0x1e2c},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00004a[2] = {
    {0x302, 0x134}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00004b[6] = {
    {0x301, 0x1e30},
    {0x30c, 0x1e8},
    {0x323, 0x1e32},
    {0x327, 0x136},
    {0x331, 0x1e34},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00004c[7] = {
    {0x301, 0x139},
    {0x30c, 0x13d},
    {0x323, 0x1e36},
    {0x327, 0x13b},
    {0x32d, 0x1e3c},
    {0x331, 0x1e3a},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00004d[4] = {
    {0x301, 0x1e3e},
    {0x307, 0x1e40},
    {0x323, 0x1e42},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00004e[10] = {
    {0x300, 0x1f8},
    {0x301, 0x143},
    {0x303, 0xd1},
    {0x307, 0x1e44},
    {0x30c, 0x147},
    {0x323, 0x1e46},
    {0x327, 0x145},
    {0x32d, 0x1e4a},
    {0x331, 0x1e48},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00004f[17] = {
    {0x300, 0xd2},
    {0x301, 0xd3},
    {0x302, 0xd4},
    {0x303, 0xd5},
    {0x304, 0x14c},
    {0x306, 0x14e},
    {0x307, 0x22e},
    {0x308, 0xd6},
    {0x309, 0x1ece},
    {0x30b, 0x150},
    {0x30c, 0x1d1},
    {0x30f, 0x20c},
    {0x311, 0x20e},
    {0x31b, 0x1a0},
    {0x323, 0x1ecc},
    {0x328, 0x1ea},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000050[3] = {
    {0x301, 0x1e54},
    {0x307, 0x1e56},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000052[9] = {
    {0x301, 0x154},
    {0x307, 0x1e58},
    {0x30c, 0x158},
    {0x30f, 0x210},
    {0x311, 0x212},
    {0x323, 0x1e5a},
    {0x327, 0x156},
    {0x331, 0x1e5e},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000053[8] = {
    {0x301, 0x15a},
    {0x302, 0x15c},
    {0x307, 0x1e60},
    {0x30c, 0x160},
    {0x323, 0x1e62},
    {0x326, 0x218},
    {0x327, 0x15e},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000054[8] = {
    {0x307, 0x1e6a},
    {0x30c, 0x164},
    {0x323, 0x1e6c},
    {0x326, 0x21a},
    {0x327, 0x162},
    {0x32d, 0x1e70},
    {0x331, 0x1e6e},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000055[20] = {
    {0x300, 0xd9},
    {0x301, 0xda},
    {0x302, 0xdb},
    {0x303, 0x168},
    {0x304, 0x16a},
    {0x306, 0x16c},
    {0x308, 0xdc},
    {0x309, 0x1ee6},
    {0x30a, 0x16e},
    {0x30b, 0x170},
    {0x30c, 0x1d3},
    {0x30f, 0x214},
    {0x311, 0x216},
    {0x31b, 0x1af},
    {0x323, 0x1ee4},
    {0x324, 0x1e72},
    {0x328, 0x172},
    {0x32d, 0x1e76},
    {0x330, 0x1e74},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000056[3] = {
    {0x303, 0x1e7c},
    {0x323, 0x1e7e},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000057[7] = {
    {0x300, 0x1e80},
    {0x301, 0x1e82},
    {0x302, 0x174},
    {0x307, 0x1e86},
    {0x308, 0x1e84},
    {0x323, 0x1e88},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000058[3] = {
    {0x307, 0x1e8a},
    {0x308, 0x1e8c},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000059[10] = {
    {0x300, 0x1ef2},
    {0x301, 0xdd},
    {0x302, 0x176},
    {0x303, 0x1ef8},
    {0x304, 0x232},
    {0x307, 0x1e8e},
    {0x308, 0x178},
    {0x309, 0x1ef6},
    {0x323, 0x1ef4},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00005a[7] = {
    {0x301, 0x179},
    {0x302, 0x1e90},
    {0x307, 0x17b},
    {0x30c, 0x17d},
    {0x323, 0x1e92},
    {0x331, 0x1e94},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000061[17] = {
    {0x300, 0xe0},
    {0x301, 0xe1},
    {0x302, 0xe2},
    {0x303, 0xe3},
    {0x304, 0x101},
    {0x306, 0x103},
    {0x307, 0x227},
    {0x308, 0xe4},
    {0x309, 0x1ea3},
    {0x30a, 0xe5},
    {0x30c, 0x1ce},
    {0x30f, 0x201},
    {0x311, 0x203},
    {0x323, 0x1ea1},
    {0x325, 0x1e01},
    {0x328, 0x105},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000062[4] = {
    {0x307, 0x1e03},
    {0x323, 0x1e05},
    {0x331, 0x1e07},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000063[6] = {
    {0x301, 0x107},
    {0x302, 0x109},
    {0x307, 0x10b},
    {0x30c, 0x10d},
    {0x327, 0xe7},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000064[7] = {
    {0x307, 0x1e0b},
    {0x30c, 0x10f},
    {0x323, 0x1e0d},
    {0x327, 0x1e11},
    {0x32d, 0x1e13},
    {0x331, 0x1e0f},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000065[18] = {
    {0x300, 0xe8},
    {0x301, 0xe9},
    {0x302, 0xea},
    {0x303, 0x1ebd},
    {0x304, 0x113},
    {0x306, 0x115},
    {0x307, 0x117},
    {0x308, 0xeb},
    {0x309, 0x1ebb},
    {0x30c, 0x11b},
    {0x30f, 0x205},
    {0x311, 0x207},
    {0x323, 0x1eb9},
    {0x327, 0x229},
    {0x328, 0x119},
    {0x32d, 0x1e19},
    {0x330, 0x1e1b},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000066[2] = {
    {0x307, 0x1e1f}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000067[8] = {
    {0x301, 0x1f5},
    {0x302, 0x11d},
    {0x304, 0x1e21},
    {0x306, 0x11f},
    {0x307, 0x121},
    {0x30c, 0x1e7},
    {0x327, 0x123},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000068[9] = {
    {0x302, 0x125},
    {0x307, 0x1e23},
    {0x308, 0x1e27},
    {0x30c, 0x21f},
    {0x323, 0x1e25},
    {0x327, 0x1e29},
    {0x32e, 0x1e2b},
    {0x331, 0x1e96},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000069[15] = {
    {0x300, 0xec},
    {0x301, 0xed},
    {0x302, 0xee},
    {0x303, 0x129},
    {0x304, 0x12b},
    {0x306, 0x12d},
    {0x308, 0xef},
    {0x309, 0x1ec9},
    {0x30c, 0x1d0},
    {0x30f, 0x209},
    {0x311, 0x20b},
    {0x323, 0x1ecb},
    {0x328, 0x12f},
    {0x330, 0x1e2d},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00006a[3] = {
    {0x302, 0x135},
    {0x30c, 0x1f0},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00006b[6] = {
    {0x301, 0x1e31},
    {0x30c, 0x1e9},
    {0x323, 0x1e33},
    {0x327, 0x137},
    {0x331, 0x1e35},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00006c[7] = {
    {0x301, 0x13a},
    {0x30c, 0x13e},
    {0x323, 0x1e37},
    {0x327, 0x13c},
    {0x32d, 0x1e3d},
    {0x331, 0x1e3b},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00006d[4] = {
    {0x301, 0x1e3f},
    {0x307, 0x1e41},
    {0x323, 0x1e43},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00006e[10] = {
    {0x300, 0x1f9},
    {0x301, 0x144},
    {0x303, 0xf1},
    {0x307, 0x1e45},
    {0x30c, 0x148},
    {0x323, 0x1e47},
    {0x327, 0x146},
    {0x32d, 0x1e4b},
    {0x331, 0x1e49},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00006f[17] = {
    {0x300, 0xf2},
    {0x301, 0xf3},
    {0x302, 0xf4},
    {0x303, 0xf5},
    {0x304, 0x14d},
    {0x306, 0x14f},
    {0x307, 0x22f},
    {0x308, 0xf6},
    {0x309, 0x1ecf},
    {0x30b, 0x151},
    {0x30c, 0x1d2},
    {0x30f, 0x20d},
    {0x311, 0x20f},
    {0x31b, 0x1a1},
    {0x323, 0x1ecd},
    {0x328, 0x1eb},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000070[3] = {
    {0x301, 0x1e55},
    {0x307, 0x1e57},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000072[9] = {
    {0x301, 0x155},
    {0x307, 0x1e59},
    {0x30c, 0x159},
    {0x30f, 0x211},
    {0x311, 0x213},
    {0x323, 0x1e5b},
    {0x327, 0x157},
    {0x331, 0x1e5f},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000073[8] = {
    {0x301, 0x15b},
    {0x302, 0x15d},
    {0x307, 0x1e61},
    {0x30c, 0x161},
    {0x323, 0x1e63},
    {0x326, 0x219},
    {0x327, 0x15f},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000074[9] = {
    {0x307, 0x1e6b},
    {0x308, 0x1e97},
    {0x30c, 0x165},
    {0x323, 0x1e6d},
    {0x326, 0x21b},
    {0x327, 0x163},
    {0x32d, 0x1e71},
    {0x331, 0x1e6f},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000075[20] = {
    {0x300, 0xf9},
    {0x301, 0xfa},
    {0x302, 0xfb},
    {0x303, 0x169},
    {0x304, 0x16b},
    {0x306, 0x16d},
    {0x308, 0xfc},
    {0x309, 0x1ee7},
    {0x30a, 0x16f},
    {0x30b, 0x171},
    {0x30c, 0x1d4},
    {0x30f, 0x215},
    {0x311, 0x217},
    {0x31b, 0x1b0},
    {0x323, 0x1ee5},
    {0x324, 0x1e73},
    {0x328, 0x173},
    {0x32d, 0x1e77},
    {0x330, 0x1e75},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000076[3] = {
    {0x303, 0x1e7d},
    {0x323, 0x1e7f},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000077[8] = {
    {0x300, 0x1e81},
    {0x301, 0x1e83},
    {0x302, 0x175},
    {0x307, 0x1e87},
    {0x308, 0x1e85},
    {0x30a, 0x1e98},
    {0x323, 0x1e89},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000078[3] = {
    {0x307, 0x1e8b},
    {0x308, 0x1e8d},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000079[11] = {
    {0x300, 0x1ef3},
    {0x301, 0xfd},
    {0x302, 0x177},
    {0x303, 0x1ef9},
    {0x304, 0x233},
    {0x307, 0x1e8f},
    {0x308, 0xff},
    {0x309, 0x1ef7},
    {0x30a, 0x1e99},
    {0x323, 0x1ef5},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00007a[7] = {
    {0x301, 0x17a},
    {0x302, 0x1e91},
    {0x307, 0x17c},
    {0x30c, 0x17e},
    {0x323, 0x1e93},
    {0x331, 0x1e95},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000a8[4] = {
    {0x300, 0x1fed},
    {0x301, 0x385},
    {0x342, 0x1fc1},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000c2[5] = {
    {0x300, 0x1ea6},
    {0x301, 0x1ea4},
    {0x303, 0x1eaa},
    {0x309, 0x1ea8},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000c4[2] = {
    {0x304, 0x1de}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000c5[2] = {
    {0x301, 0x1fa}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000c6[3] = {
    {0x301, 0x1fc},
    {0x304, 0x1e2},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000c7[2] = {
    {0x301, 0x1e08}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000ca[5] = {
    {0x300, 0x1ec0},
    {0x301, 0x1ebe},
    {0x303, 0x1ec4},
    {0x309, 0x1ec2},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000cf[2] = {
    {0x301, 0x1e2e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000d4[5] = {
    {0x300, 0x1ed2},
    {0x301, 0x1ed0},
    {0x303, 0x1ed6},
    {0x309, 0x1ed4},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000d5[4] = {
    {0x301, 0x1e4c},
    {0x304, 0x22c},
    {0x308, 0x1e4e},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000d6[2] = {
    {0x304, 0x22a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000d8[2] = {
    {0x301, 0x1fe}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000dc[5] = {
    {0x300, 0x1db},
    {0x301, 0x1d7},
    {0x304, 0x1d5},
    {0x30c, 0x1d9},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000e2[5] = {
    {0x300, 0x1ea7},
    {0x301, 0x1ea5},
    {0x303, 0x1eab},
    {0x309, 0x1ea9},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000e4[2] = {
    {0x304, 0x1df}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000e5[2] = {
    {0x301, 0x1fb}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000e6[3] = {
    {0x301, 0x1fd},
    {0x304, 0x1e3},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000e7[2] = {
    {0x301, 0x1e09}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000ea[5] = {
    {0x300, 0x1ec1},
    {0x301, 0x1ebf},
    {0x303, 0x1ec5},
    {0x309, 0x1ec3},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000ef[2] = {
    {0x301, 0x1e2f}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000f4[5] = {
    {0x300, 0x1ed3},
    {0x301, 0x1ed1},
    {0x303, 0x1ed7},
    {0x309, 0x1ed5},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000f5[4] = {
    {0x301, 0x1e4d},
    {0x304, 0x22d},
    {0x308, 0x1e4f},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000f6[2] = {
    {0x304, 0x22b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000f8[2] = {
    {0x301, 0x1ff}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0000fc[5] = {
    {0x300, 0x1dc},
    {0x301, 0x1d8},
    {0x304, 0x1d6},
    {0x30c, 0x1da},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000102[5] = {
    {0x300, 0x1eb0},
    {0x301, 0x1eae},
    {0x303, 0x1eb4},
    {0x309, 0x1eb2},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000103[5] = {
    {0x300, 0x1eb1},
    {0x301, 0x1eaf},
    {0x303, 0x1eb5},
    {0x309, 0x1eb3},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000112[3] = {
    {0x300, 0x1e14},
    {0x301, 0x1e16},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000113[3] = {
    {0x300, 0x1e15},
    {0x301, 0x1e17},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00014c[3] = {
    {0x300, 0x1e50},
    {0x301, 0x1e52},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00014d[3] = {
    {0x300, 0x1e51},
    {0x301, 0x1e53},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00015a[2] = {
    {0x307, 0x1e64}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00015b[2] = {
    {0x307, 0x1e65}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000160[2] = {
    {0x307, 0x1e66}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000161[2] = {
    {0x307, 0x1e67}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000168[2] = {
    {0x301, 0x1e78}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000169[2] = {
    {0x301, 0x1e79}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00016a[2] = {
    {0x308, 0x1e7a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00016b[2] = {
    {0x308, 0x1e7b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00017f[2] = {
    {0x307, 0x1e9b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0001a0[6] = {
    {0x300, 0x1edc},
    {0x301, 0x1eda},
    {0x303, 0x1ee0},
    {0x309, 0x1ede},
    {0x323, 0x1ee2},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0001a1[6] = {
    {0x300, 0x1edd},
    {0x301, 0x1edb},
    {0x303, 0x1ee1},
    {0x309, 0x1edf},
    {0x323, 0x1ee3},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0001af[6] = {
    {0x300, 0x1eea},
    {0x301, 0x1ee8},
    {0x303, 0x1eee},
    {0x309, 0x1eec},
    {0x323, 0x1ef0},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0001b0[6] = {
    {0x300, 0x1eeb},
    {0x301, 0x1ee9},
    {0x303, 0x1eef},
    {0x309, 0x1eed},
    {0x323, 0x1ef1},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0001b7[2] = {
    {0x30c, 0x1ee}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0001ea[2] = {
    {0x304, 0x1ec}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0001eb[2] = {
    {0x304, 0x1ed}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000226[2] = {
    {0x304, 0x1e0}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000227[2] = {
    {0x304, 0x1e1}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000228[2] = {
    {0x306, 0x1e1c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000229[2] = {
    {0x306, 0x1e1d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00022e[2] = {
    {0x304, 0x230}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00022f[2] = {
    {0x304, 0x231}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000292[2] = {
    {0x30c, 0x1ef}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000391[8] = {
    {0x300, 0x1fba},
    {0x301, 0x386},
    {0x304, 0x1fb9},
    {0x306, 0x1fb8},
    {0x313, 0x1f08},
    {0x314, 0x1f09},
    {0x345, 0x1fbc},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000395[5] = {
    {0x300, 0x1fc8},
    {0x301, 0x388},
    {0x313, 0x1f18},
    {0x314, 0x1f19},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000397[6] = {
    {0x300, 0x1fca},
    {0x301, 0x389},
    {0x313, 0x1f28},
    {0x314, 0x1f29},
    {0x345, 0x1fcc},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000399[8] = {
    {0x300, 0x1fda},
    {0x301, 0x38a},
    {0x304, 0x1fd9},
    {0x306, 0x1fd8},
    {0x308, 0x3aa},
    {0x313, 0x1f38},
    {0x314, 0x1f39},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00039f[5] = {
    {0x300, 0x1ff8},
    {0x301, 0x38c},
    {0x313, 0x1f48},
    {0x314, 0x1f49},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003a1[2] = {
    {0x314, 0x1fec}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003a5[7] = {
    {0x300, 0x1fea},
    {0x301, 0x38e},
    {0x304, 0x1fe9},
    {0x306, 0x1fe8},
    {0x308, 0x3ab},
    {0x314, 0x1f59},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003a9[6] = {
    {0x300, 0x1ffa},
    {0x301, 0x38f},
    {0x313, 0x1f68},
    {0x314, 0x1f69},
    {0x345, 0x1ffc},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003ac[2] = {
    {0x345, 0x1fb4}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003ae[2] = {
    {0x345, 0x1fc4}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003b1[9] = {
    {0x300, 0x1f70},
    {0x301, 0x3ac},
    {0x304, 0x1fb1},
    {0x306, 0x1fb0},
    {0x313, 0x1f00},
    {0x314, 0x1f01},
    {0x342, 0x1fb6},
    {0x345, 0x1fb3},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003b5[5] = {
    {0x300, 0x1f72},
    {0x301, 0x3ad},
    {0x313, 0x1f10},
    {0x314, 0x1f11},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003b7[7] = {
    {0x300, 0x1f74},
    {0x301, 0x3ae},
    {0x313, 0x1f20},
    {0x314, 0x1f21},
    {0x342, 0x1fc6},
    {0x345, 0x1fc3},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003b9[9] = {
    {0x300, 0x1f76},
    {0x301, 0x3af},
    {0x304, 0x1fd1},
    {0x306, 0x1fd0},
    {0x308, 0x3ca},
    {0x313, 0x1f30},
    {0x314, 0x1f31},
    {0x342, 0x1fd6},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003bf[5] = {
    {0x300, 0x1f78},
    {0x301, 0x3cc},
    {0x313, 0x1f40},
    {0x314, 0x1f41},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003c1[3] = {
    {0x313, 0x1fe4},
    {0x314, 0x1fe5},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003c5[9] = {
    {0x300, 0x1f7a},
    {0x301, 0x3cd},
    {0x304, 0x1fe1},
    {0x306, 0x1fe0},
    {0x308, 0x3cb},
    {0x313, 0x1f50},
    {0x314, 0x1f51},
    {0x342, 0x1fe6},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003c9[7] = {
    {0x300, 0x1f7c},
    {0x301, 0x3ce},
    {0x313, 0x1f60},
    {0x314, 0x1f61},
    {0x342, 0x1ff6},
    {0x345, 0x1ff3},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003ca[4] = {
    {0x300, 0x1fd2},
    {0x301, 0x390},
    {0x342, 0x1fd7},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003cb[4] = {
    {0x300, 0x1fe2},
    {0x301, 0x3b0},
    {0x342, 0x1fe7},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003ce[2] = {
    {0x345, 0x1ff4}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0003d2[3] = {
    {0x301, 0x3d3},
    {0x308, 0x3d4},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000406[2] = {
    {0x308, 0x407}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000410[3] = {
    {0x306, 0x4d0},
    {0x308, 0x4d2},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000413[2] = {
    {0x301, 0x403}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000415[4] = {
    {0x300, 0x400},
    {0x306, 0x4d6},
    {0x308, 0x401},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000416[3] = {
    {0x306, 0x4c1},
    {0x308, 0x4dc},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000417[2] = {
    {0x308, 0x4de}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000418[5] = {
    {0x300, 0x40d},
    {0x304, 0x4e2},
    {0x306, 0x419},
    {0x308, 0x4e4},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00041a[2] = {
    {0x301, 0x40c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00041e[2] = {
    {0x308, 0x4e6}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000423[5] = {
    {0x304, 0x4ee},
    {0x306, 0x40e},
    {0x308, 0x4f0},
    {0x30b, 0x4f2},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000427[2] = {
    {0x308, 0x4f4}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00042b[2] = {
    {0x308, 0x4f8}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00042d[2] = {
    {0x308, 0x4ec}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000430[3] = {
    {0x306, 0x4d1},
    {0x308, 0x4d3},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000433[2] = {
    {0x301, 0x453}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000435[4] = {
    {0x300, 0x450},
    {0x306, 0x4d7},
    {0x308, 0x451},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000436[3] = {
    {0x306, 0x4c2},
    {0x308, 0x4dd},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000437[2] = {
    {0x308, 0x4df}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000438[5] = {
    {0x300, 0x45d},
    {0x304, 0x4e3},
    {0x306, 0x439},
    {0x308, 0x4e5},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00043a[2] = {
    {0x301, 0x45c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00043e[2] = {
    {0x308, 0x4e7}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000443[5] = {
    {0x304, 0x4ef},
    {0x306, 0x45e},
    {0x308, 0x4f1},
    {0x30b, 0x4f3},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000447[2] = {
    {0x308, 0x4f5}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00044b[2] = {
    {0x308, 0x4f9}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00044d[2] = {
    {0x308, 0x4ed}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000456[2] = {
    {0x308, 0x457}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000474[2] = {
    {0x30f, 0x476}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000475[2] = {
    {0x30f, 0x477}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0004d8[2] = {
    {0x308, 0x4da}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0004d9[2] = {
    {0x308, 0x4db}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0004e8[2] = {
    {0x308, 0x4ea}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0004e9[2] = {
    {0x308, 0x4eb}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005d0[4] = {
    {0x5b7, 0xfb2e},
    {0x5b8, 0xfb2f},
    {0x5bc, 0xfb30},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005d1[3] = {
    {0x5bc, 0xfb31},
    {0x5bf, 0xfb4c},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005d2[2] = {
    {0x5bc, 0xfb32}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005d3[2] = {
    {0x5bc, 0xfb33}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005d4[2] = {
    {0x5bc, 0xfb34}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005d5[3] = {
    {0x5b9, 0xfb4b},
    {0x5bc, 0xfb35},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005d6[2] = {
    {0x5bc, 0xfb36}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005d8[2] = {
    {0x5bc, 0xfb38}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005d9[3] = {
    {0x5b4, 0xfb1d},
    {0x5bc, 0xfb39},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005da[2] = {
    {0x5bc, 0xfb3a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005db[3] = {
    {0x5bc, 0xfb3b},
    {0x5bf, 0xfb4d},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005dc[2] = {
    {0x5bc, 0xfb3c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005de[2] = {
    {0x5bc, 0xfb3e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005e0[2] = {
    {0x5bc, 0xfb40}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005e1[2] = {
    {0x5bc, 0xfb41}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005e3[2] = {
    {0x5bc, 0xfb43}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005e4[3] = {
    {0x5bc, 0xfb44},
    {0x5bf, 0xfb4e},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005e6[2] = {
    {0x5bc, 0xfb46}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005e7[2] = {
    {0x5bc, 0xfb47}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005e8[2] = {
    {0x5bc, 0xfb48}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005e9[4] = {
    {0x5bc, 0xfb49},
    {0x5c1, 0xfb2a},
    {0x5c2, 0xfb2b},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005ea[2] = {
    {0x5bc, 0xfb4a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0005f2[2] = {
    {0x5b7, 0xfb1f}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000627[4] = {
    {0x653, 0x622},
    {0x654, 0x623},
    {0x655, 0x625},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000648[2] = {
    {0x654, 0x624}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00064a[2] = {
    {0x654, 0x626}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0006c1[2] = {
    {0x654, 0x6c2}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0006d2[2] = {
    {0x654, 0x6d3}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0006d5[2] = {
    {0x654, 0x6c0}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000915[2] = {
    {0x93c, 0x958}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000916[2] = {
    {0x93c, 0x959}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000917[2] = {
    {0x93c, 0x95a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00091c[2] = {
    {0x93c, 0x95b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000921[2] = {
    {0x93c, 0x95c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000922[2] = {
    {0x93c, 0x95d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000928[2] = {
    {0x93c, 0x929}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00092b[2] = {
    {0x93c, 0x95e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00092f[2] = {
    {0x93c, 0x95f}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000930[2] = {
    {0x93c, 0x931}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000933[2] = {
    {0x93c, 0x934}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0009a1[2] = {
    {0x9bc, 0x9dc}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0009a2[2] = {
    {0x9bc, 0x9dd}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0009af[2] = {
    {0x9bc, 0x9df}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0009c7[3] = {
    {0x9be, 0x9cb},
    {0x9d7, 0x9cc},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000a16[2] = {
    {0xa3c, 0xa59}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000a17[2] = {
    {0xa3c, 0xa5a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000a1c[2] = {
    {0xa3c, 0xa5b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000a2b[2] = {
    {0xa3c, 0xa5e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000a32[2] = {
    {0xa3c, 0xa33}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000a38[2] = {
    {0xa3c, 0xa36}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000b21[2] = {
    {0xb3c, 0xb5c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000b22[2] = {
    {0xb3c, 0xb5d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000b47[4] = {
    {0xb3e, 0xb4b},
    {0xb56, 0xb48},
    {0xb57, 0xb4c},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000b92[2] = {
    {0xbd7, 0xb94}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000bc6[3] = {
    {0xbbe, 0xbca},
    {0xbd7, 0xbcc},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000bc7[2] = {
    {0xbbe, 0xbcb}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000c46[2] = {
    {0xc56, 0xc48}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000cbf[2] = {
    {0xcd5, 0xcc0}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000cc6[4] = {
    {0xcc2, 0xcca},
    {0xcd5, 0xcc7},
    {0xcd6, 0xcc8},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000cca[2] = {
    {0xcd5, 0xccb}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000d46[3] = {
    {0xd3e, 0xd4a},
    {0xd57, 0xd4c},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000d47[2] = {
    {0xd3e, 0xd4b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000dd9[4] = {
    {0xdca, 0xdda},
    {0xdcf, 0xddc},
    {0xddf, 0xdde},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000ddc[2] = {
    {0xdca, 0xddd}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000f40[2] = {
    {0xfb5, 0xf69}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000f42[2] = {
    {0xfb7, 0xf43}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000f4c[2] = {
    {0xfb7, 0xf4d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000f51[2] = {
    {0xfb7, 0xf52}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000f56[2] = {
    {0xfb7, 0xf57}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000f5b[2] = {
    {0xfb7, 0xf5c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000f90[2] = {
    {0xfb5, 0xfb9}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000f92[2] = {
    {0xfb7, 0xf93}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000f9c[2] = {
    {0xfb7, 0xf9d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000fa1[2] = {
    {0xfb7, 0xfa2}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000fa6[2] = {
    {0xfb7, 0xfa7}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000fab[2] = {
    {0xfb7, 0xfac}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000fb2[2] = {
    {0xf80, 0xf76}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_000fb3[2] = {
    {0xf80, 0xf78}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001025[2] = {
    {0x102e, 0x1026}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b05[2] = {
    {0x1b35, 0x1b06}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b07[2] = {
    {0x1b35, 0x1b08}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b09[2] = {
    {0x1b35, 0x1b0a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b0b[2] = {
    {0x1b35, 0x1b0c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b0d[2] = {
    {0x1b35, 0x1b0e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b11[2] = {
    {0x1b35, 0x1b12}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b3a[2] = {
    {0x1b35, 0x1b3b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b3c[2] = {
    {0x1b35, 0x1b3d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b3e[2] = {
    {0x1b35, 0x1b40}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b3f[2] = {
    {0x1b35, 0x1b41}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001b42[2] = {
    {0x1b35, 0x1b43}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001e36[2] = {
    {0x304, 0x1e38}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001e37[2] = {
    {0x304, 0x1e39}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001e5a[2] = {
    {0x304, 0x1e5c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001e5b[2] = {
    {0x304, 0x1e5d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001e62[2] = {
    {0x307, 0x1e68}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001e63[2] = {
    {0x307, 0x1e69}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001ea0[3] = {
    {0x302, 0x1eac},
    {0x306, 0x1eb6},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001ea1[3] = {
    {0x302, 0x1ead},
    {0x306, 0x1eb7},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001eb8[2] = {
    {0x302, 0x1ec6}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001eb9[2] = {
    {0x302, 0x1ec7}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001ecc[2] = {
    {0x302, 0x1ed8}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001ecd[2] = {
    {0x302, 0x1ed9}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f00[5] = {
    {0x300, 0x1f02},
    {0x301, 0x1f04},
    {0x342, 0x1f06},
    {0x345, 0x1f80},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f01[5] = {
    {0x300, 0x1f03},
    {0x301, 0x1f05},
    {0x342, 0x1f07},
    {0x345, 0x1f81},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f02[2] = {
    {0x345, 0x1f82}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f03[2] = {
    {0x345, 0x1f83}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f04[2] = {
    {0x345, 0x1f84}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f05[2] = {
    {0x345, 0x1f85}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f06[2] = {
    {0x345, 0x1f86}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f07[2] = {
    {0x345, 0x1f87}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f08[5] = {
    {0x300, 0x1f0a},
    {0x301, 0x1f0c},
    {0x342, 0x1f0e},
    {0x345, 0x1f88},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f09[5] = {
    {0x300, 0x1f0b},
    {0x301, 0x1f0d},
    {0x342, 0x1f0f},
    {0x345, 0x1f89},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f0a[2] = {
    {0x345, 0x1f8a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f0b[2] = {
    {0x345, 0x1f8b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f0c[2] = {
    {0x345, 0x1f8c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f0d[2] = {
    {0x345, 0x1f8d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f0e[2] = {
    {0x345, 0x1f8e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f0f[2] = {
    {0x345, 0x1f8f}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f10[3] = {
    {0x300, 0x1f12},
    {0x301, 0x1f14},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f11[3] = {
    {0x300, 0x1f13},
    {0x301, 0x1f15},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f18[3] = {
    {0x300, 0x1f1a},
    {0x301, 0x1f1c},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f19[3] = {
    {0x300, 0x1f1b},
    {0x301, 0x1f1d},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f20[5] = {
    {0x300, 0x1f22},
    {0x301, 0x1f24},
    {0x342, 0x1f26},
    {0x345, 0x1f90},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f21[5] = {
    {0x300, 0x1f23},
    {0x301, 0x1f25},
    {0x342, 0x1f27},
    {0x345, 0x1f91},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f22[2] = {
    {0x345, 0x1f92}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f23[2] = {
    {0x345, 0x1f93}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f24[2] = {
    {0x345, 0x1f94}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f25[2] = {
    {0x345, 0x1f95}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f26[2] = {
    {0x345, 0x1f96}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f27[2] = {
    {0x345, 0x1f97}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f28[5] = {
    {0x300, 0x1f2a},
    {0x301, 0x1f2c},
    {0x342, 0x1f2e},
    {0x345, 0x1f98},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f29[5] = {
    {0x300, 0x1f2b},
    {0x301, 0x1f2d},
    {0x342, 0x1f2f},
    {0x345, 0x1f99},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f2a[2] = {
    {0x345, 0x1f9a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f2b[2] = {
    {0x345, 0x1f9b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f2c[2] = {
    {0x345, 0x1f9c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f2d[2] = {
    {0x345, 0x1f9d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f2e[2] = {
    {0x345, 0x1f9e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f2f[2] = {
    {0x345, 0x1f9f}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f30[4] = {
    {0x300, 0x1f32},
    {0x301, 0x1f34},
    {0x342, 0x1f36},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f31[4] = {
    {0x300, 0x1f33},
    {0x301, 0x1f35},
    {0x342, 0x1f37},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f38[4] = {
    {0x300, 0x1f3a},
    {0x301, 0x1f3c},
    {0x342, 0x1f3e},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f39[4] = {
    {0x300, 0x1f3b},
    {0x301, 0x1f3d},
    {0x342, 0x1f3f},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f40[3] = {
    {0x300, 0x1f42},
    {0x301, 0x1f44},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f41[3] = {
    {0x300, 0x1f43},
    {0x301, 0x1f45},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f48[3] = {
    {0x300, 0x1f4a},
    {0x301, 0x1f4c},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f49[3] = {
    {0x300, 0x1f4b},
    {0x301, 0x1f4d},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f50[4] = {
    {0x300, 0x1f52},
    {0x301, 0x1f54},
    {0x342, 0x1f56},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f51[4] = {
    {0x300, 0x1f53},
    {0x301, 0x1f55},
    {0x342, 0x1f57},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f59[4] = {
    {0x300, 0x1f5b},
    {0x301, 0x1f5d},
    {0x342, 0x1f5f},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f60[5] = {
    {0x300, 0x1f62},
    {0x301, 0x1f64},
    {0x342, 0x1f66},
    {0x345, 0x1fa0},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f61[5] = {
    {0x300, 0x1f63},
    {0x301, 0x1f65},
    {0x342, 0x1f67},
    {0x345, 0x1fa1},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f62[2] = {
    {0x345, 0x1fa2}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f63[2] = {
    {0x345, 0x1fa3}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f64[2] = {
    {0x345, 0x1fa4}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f65[2] = {
    {0x345, 0x1fa5}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f66[2] = {
    {0x345, 0x1fa6}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f67[2] = {
    {0x345, 0x1fa7}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f68[5] = {
    {0x300, 0x1f6a},
    {0x301, 0x1f6c},
    {0x342, 0x1f6e},
    {0x345, 0x1fa8},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f69[5] = {
    {0x300, 0x1f6b},
    {0x301, 0x1f6d},
    {0x342, 0x1f6f},
    {0x345, 0x1fa9},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f6a[2] = {
    {0x345, 0x1faa}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f6b[2] = {
    {0x345, 0x1fab}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f6c[2] = {
    {0x345, 0x1fac}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f6d[2] = {
    {0x345, 0x1fad}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f6e[2] = {
    {0x345, 0x1fae}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f6f[2] = {
    {0x345, 0x1faf}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f70[2] = {
    {0x345, 0x1fb2}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f74[2] = {
    {0x345, 0x1fc2}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001f7c[2] = {
    {0x345, 0x1ff2}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001fb6[2] = {
    {0x345, 0x1fb7}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001fbf[4] = {
    {0x300, 0x1fcd},
    {0x301, 0x1fce},
    {0x342, 0x1fcf},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001fc6[2] = {
    {0x345, 0x1fc7}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001ff6[2] = {
    {0x345, 0x1ff7}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_001ffe[4] = {
    {0x300, 0x1fdd},
    {0x301, 0x1fde},
    {0x342, 0x1fdf},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002190[2] = {
    {0x338, 0x219a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002192[2] = {
    {0x338, 0x219b}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002194[2] = {
    {0x338, 0x21ae}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0021d0[2] = {
    {0x338, 0x21cd}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0021d2[2] = {
    {0x338, 0x21cf}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0021d4[2] = {
    {0x338, 0x21ce}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002203[2] = {
    {0x338, 0x2204}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002208[2] = {
    {0x338, 0x2209}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00220b[2] = {
    {0x338, 0x220c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002223[2] = {
    {0x338, 0x2224}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002225[2] = {
    {0x338, 0x2226}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00223c[2] = {
    {0x338, 0x2241}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002243[2] = {
    {0x338, 0x2244}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002245[2] = {
    {0x338, 0x2247}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002248[2] = {
    {0x338, 0x2249}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00224d[2] = {
    {0x338, 0x226d}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002261[2] = {
    {0x338, 0x2262}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002264[2] = {
    {0x338, 0x2270}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002265[2] = {
    {0x338, 0x2271}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002272[2] = {
    {0x338, 0x2274}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002273[2] = {
    {0x338, 0x2275}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002276[2] = {
    {0x338, 0x2278}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002277[2] = {
    {0x338, 0x2279}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00227a[2] = {
    {0x338, 0x2280}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00227b[2] = {
    {0x338, 0x2281}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00227c[2] = {
    {0x338, 0x22e0}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00227d[2] = {
    {0x338, 0x22e1}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002282[2] = {
    {0x338, 0x2284}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002283[2] = {
    {0x338, 0x2285}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002286[2] = {
    {0x338, 0x2288}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002287[2] = {
    {0x338, 0x2289}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002291[2] = {
    {0x338, 0x22e2}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002292[2] = {
    {0x338, 0x22e3}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0022a2[2] = {
    {0x338, 0x22ac}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0022a8[2] = {
    {0x338, 0x22ad}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0022a9[2] = {
    {0x338, 0x22ae}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0022ab[2] = {
    {0x338, 0x22af}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0022b2[2] = {
    {0x338, 0x22ea}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0022b3[2] = {
    {0x338, 0x22eb}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0022b4[2] = {
    {0x338, 0x22ec}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0022b5[2] = {
    {0x338, 0x22ed}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_002add[2] = {
    {0x338, 0x2adc}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003046[2] = {
    {0x3099, 0x3094}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00304b[2] = {
    {0x3099, 0x304c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00304d[2] = {
    {0x3099, 0x304e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00304f[2] = {
    {0x3099, 0x3050}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003051[2] = {
    {0x3099, 0x3052}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003053[2] = {
    {0x3099, 0x3054}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003055[2] = {
    {0x3099, 0x3056}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003057[2] = {
    {0x3099, 0x3058}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003059[2] = {
    {0x3099, 0x305a}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00305b[2] = {
    {0x3099, 0x305c}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00305d[2] = {
    {0x3099, 0x305e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00305f[2] = {
    {0x3099, 0x3060}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003061[2] = {
    {0x3099, 0x3062}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003064[2] = {
    {0x3099, 0x3065}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003066[2] = {
    {0x3099, 0x3067}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003068[2] = {
    {0x3099, 0x3069}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00306f[3] = {
    {0x3099, 0x3070},
    {0x309a, 0x3071},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003072[3] = {
    {0x3099, 0x3073},
    {0x309a, 0x3074},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003075[3] = {
    {0x3099, 0x3076},
    {0x309a, 0x3077},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_003078[3] = {
    {0x3099, 0x3079},
    {0x309a, 0x307a},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00307b[3] = {
    {0x3099, 0x307c},
    {0x309a, 0x307d},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00309d[2] = {
    {0x3099, 0x309e}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030a6[2] = {
    {0x3099, 0x30f4}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030ab[2] = {
    {0x3099, 0x30ac}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030ad[2] = {
    {0x3099, 0x30ae}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030af[2] = {
    {0x3099, 0x30b0}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030b1[2] = {
    {0x3099, 0x30b2}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030b3[2] = {
    {0x3099, 0x30b4}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030b5[2] = {
    {0x3099, 0x30b6}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030b7[2] = {
    {0x3099, 0x30b8}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030b9[2] = {
    {0x3099, 0x30ba}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030bb[2] = {
    {0x3099, 0x30bc}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030bd[2] = {
    {0x3099, 0x30be}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030bf[2] = {
    {0x3099, 0x30c0}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030c1[2] = {
    {0x3099, 0x30c2}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030c4[2] = {
    {0x3099, 0x30c5}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030c6[2] = {
    {0x3099, 0x30c7}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030c8[2] = {
    {0x3099, 0x30c9}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030cf[3] = {
    {0x3099, 0x30d0},
    {0x309a, 0x30d1},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030d2[3] = {
    {0x3099, 0x30d3},
    {0x309a, 0x30d4},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030d5[3] = {
    {0x3099, 0x30d6},
    {0x309a, 0x30d7},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030d8[3] = {
    {0x3099, 0x30d9},
    {0x309a, 0x30da},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030db[3] = {
    {0x3099, 0x30dc},
    {0x309a, 0x30dd},
    {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030ef[2] = {
    {0x3099, 0x30f7}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030f0[2] = {
    {0x3099, 0x30f8}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030f1[2] = {
    {0x3099, 0x30f9}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030f2[2] = {
    {0x3099, 0x30fa}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_0030fd[2] = {
    {0x3099, 0x30fe}, {0, 0}};
static const UN8IF_complist_s UN8IF_complist_00fb49[3] = {
    {0x5c1, 0xfb2c},
    {0x5c2, 0xfb2d},
    {0, 0}};
static const UN8IF_complist UN8IF_complist_0105d2[2] = {
    {0x307, 0x105c9}, {0, 0}};
static const UN8IF_complist UN8IF_complist_0105da[2] = {
    {0x307, 0x105e4}, {0, 0}};
static const UN8IF_complist UN8IF_complist_011099[2] = {
    {0x110ba, 0x1109a}, {0, 0}};
static const UN8IF_complist UN8IF_complist_01109b[2] = {
    {0x110ba, 0x1109c}, {0, 0}};
static const UN8IF_complist UN8IF_complist_0110a5[2] = {
    {0x110ba, 0x110ab}, {0, 0}};
static const UN8IF_complist UN8IF_complist_011131[2] = {
    {0x11127, 0x1112e}, {0, 0}};
static const UN8IF_complist UN8IF_complist_011132[2] = {
    {0x11127, 0x1112f}, {0, 0}};
static const UN8IF_complist UN8IF_complist_011347[3] = {
    {0x1133e, 0x1134b},
    {0x11357, 0x1134c},
    {0, 0}};
static const UN8IF_complist UN8IF_complist_011382[2] = {
    {0x113c9, 0x11383}, {0, 0}};
static const UN8IF_complist UN8IF_complist_011384[2] = {
    {0x113bb, 0x11385}, {0, 0}};
static const UN8IF_complist UN8IF_complist_01138b[2] = {
    {0x113c2, 0x1138e}, {0, 0}};
static const UN8IF_complist UN8IF_complist_011390[2] = {
    {0x113c9, 0x11391}, {0, 0}};
static const UN8IF_complist UN8IF_complist_0113c2[4] = {
    {0x113b8, 0x113c7},
    {0x113c2, 0x113c5},
    {0x113c9, 0x113c8},
    {0, 0}};
static const UN8IF_complist UN8IF_complist_0114b9[4] = {
    {0x114b0, 0x114bc},
    {0x114ba, 0x114bb},
    {0x114bd, 0x114be},
    {0, 0}};
static const UN8IF_complist UN8IF_complist_0115b8[2] = {
    {0x115af, 0x115ba}, {0, 0}};
static const UN8IF_complist UN8IF_complist_0115b9[2] = {
    {0x115af, 0x115bb}, {0, 0}};
static const UN8IF_complist UN8IF_complist_011935[2] = {
    {0x11930, 0x11938}, {0, 0}};
static const UN8IF_complist UN8IF_complist_01611e[5] = {
    {0x1611e, 0x16121},
    {0x1611f, 0x16123},
    {0x16120, 0x16125},
    {0x16129, 0x16122},
    {0, 0}};
static const UN8IF_complist UN8IF_complist_016121[3] = {
    {0x1611f, 0x16126},
    {0x16120, 0x16128},
    {0, 0}};
static const UN8IF_complist UN8IF_complist_016122[2] = {
    {0x1611f, 0x16127}, {0, 0}};
static const UN8IF_complist UN8IF_complist_016129[2] = {
    {0x1611f, 0x16124}, {0, 0}};
static const UN8IF_complist UN8IF_complist_016d63[2] = {
    {0x16d67, 0x16d69}, {0, 0}};
static const UN8IF_complist UN8IF_complist_016d67[2] = {
    {0x16d67, 0x16d68}, {0, 0}};
static const UN8IF_complist UN8IF_complist_016d69[2] = {
    {0x16d67, 0x16d6a}, {0, 0}};
static const UN8IF_complist UN8IF_complist_01d157[2] = {
    {0x1d165, 0x1d15e}, {0, 0}};
static const UN8IF_complist UN8IF_complist_01d158[2] = {
    {0x1d165, 0x1d15f}, {0, 0}};
static const UN8IF_complist UN8IF_complist_01d15f[6] = {
    {0x1d16e, 0x1d160},
    {0x1d16f, 0x1d161},
    {0x1d170, 0x1d162},
    {0x1d171, 0x1d163},
    {0x1d172, 0x1d164},
    {0, 0}};
static const UN8IF_complist UN8IF_complist_01d1b9[2] = {
    {0x1d165, 0x1d1bb}, {0, 0}};
static const UN8IF_complist UN8IF_complist_01d1ba[2] = {
    {0x1d165, 0x1d1bc}, {0, 0}};
static const UN8IF_complist UN8IF_complist_01d1bb[3] = {
    {0x1d16e, 0x1d1bd},
    {0x1d16f, 0x1d1bf},
    {0, 0}};
static const UN8IF_complist UN8IF_complist_01d1bc[3] = {
    {0x1d16e, 0x1d1be},
    {0x1d16f, 0x1d1c0},
    {0, 0}};
/* the rows */
static const UN8IF_complist_s *UN8IF_compos_00_00[256] = {
    /*   0000 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0008 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0010 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0018 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0020 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0028 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0030 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0038 */ NULL, NULL, NULL, NULL, UN8IF_complist_00003c, UN8IF_complist_00003d, UN8IF_complist_00003e, NULL,
    /*   0040 */ NULL, UN8IF_complist_000041, UN8IF_complist_000042, UN8IF_complist_000043, UN8IF_complist_000044, UN8IF_complist_000045, UN8IF_complist_000046, UN8IF_complist_000047,
    /*   0048 */ UN8IF_complist_000048, UN8IF_complist_000049, UN8IF_complist_00004a, UN8IF_complist_00004b, UN8IF_complist_00004c, UN8IF_complist_00004d, UN8IF_complist_00004e, UN8IF_complist_00004f,
    /*   0050 */ UN8IF_complist_000050, NULL, UN8IF_complist_000052, UN8IF_complist_000053, UN8IF_complist_000054, UN8IF_complist_000055, UN8IF_complist_000056, UN8IF_complist_000057,
    /*   0058 */ UN8IF_complist_000058, UN8IF_complist_000059, UN8IF_complist_00005a, NULL, NULL, NULL, NULL, NULL,
    /*   0060 */ NULL, UN8IF_complist_000061, UN8IF_complist_000062, UN8IF_complist_000063, UN8IF_complist_000064, UN8IF_complist_000065, UN8IF_complist_000066, UN8IF_complist_000067,
    /*   0068 */ UN8IF_complist_000068, UN8IF_complist_000069, UN8IF_complist_00006a, UN8IF_complist_00006b, UN8IF_complist_00006c, UN8IF_complist_00006d, UN8IF_complist_00006e, UN8IF_complist_00006f,
    /*   0070 */ UN8IF_complist_000070, NULL, UN8IF_complist_000072, UN8IF_complist_000073, UN8IF_complist_000074, UN8IF_complist_000075, UN8IF_complist_000076, UN8IF_complist_000077,
    /*   0078 */ UN8IF_complist_000078, UN8IF_complist_000079, UN8IF_complist_00007a, NULL, NULL, NULL, NULL, NULL,
    /*   0080 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0088 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0090 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0098 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   00a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   00a8 */ UN8IF_complist_0000a8, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   00b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   00b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   00c0 */ NULL, NULL, UN8IF_complist_0000c2, NULL, UN8IF_complist_0000c4, UN8IF_complist_0000c5, UN8IF_complist_0000c6, UN8IF_complist_0000c7,
    /*   00c8 */ NULL, NULL, UN8IF_complist_0000ca, NULL, NULL, NULL, NULL, UN8IF_complist_0000cf,
    /*   00d0 */ NULL, NULL, NULL, NULL, UN8IF_complist_0000d4, UN8IF_complist_0000d5, UN8IF_complist_0000d6, NULL,
    /*   00d8 */ UN8IF_complist_0000d8, NULL, NULL, NULL, UN8IF_complist_0000dc, NULL, NULL, NULL,
    /*   00e0 */ NULL, NULL, UN8IF_complist_0000e2, NULL, UN8IF_complist_0000e4, UN8IF_complist_0000e5, UN8IF_complist_0000e6, UN8IF_complist_0000e7,
    /*   00e8 */ NULL, NULL, UN8IF_complist_0000ea, NULL, NULL, NULL, NULL, UN8IF_complist_0000ef,
    /*   00f0 */ NULL, NULL, NULL, NULL, UN8IF_complist_0000f4, UN8IF_complist_0000f5, UN8IF_complist_0000f6, NULL,
    /*   00f8 */ UN8IF_complist_0000f8, NULL, NULL, NULL, UN8IF_complist_0000fc, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_01[256] = {
    /*   0100 */ NULL, NULL, UN8IF_complist_000102, UN8IF_complist_000103, NULL, NULL, NULL, NULL,
    /*   0108 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0110 */ NULL, NULL, UN8IF_complist_000112, UN8IF_complist_000113, NULL, NULL, NULL, NULL,
    /*   0118 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0120 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0128 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0130 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0138 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0140 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0148 */ NULL, NULL, NULL, NULL, UN8IF_complist_00014c, UN8IF_complist_00014d, NULL, NULL,
    /*   0150 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0158 */ NULL, NULL, UN8IF_complist_00015a, UN8IF_complist_00015b, NULL, NULL, NULL, NULL,
    /*   0160 */ UN8IF_complist_000160, UN8IF_complist_000161, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0168 */ UN8IF_complist_000168, UN8IF_complist_000169, UN8IF_complist_00016a, UN8IF_complist_00016b, NULL, NULL, NULL, NULL,
    /*   0170 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0178 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_00017f,
    /*   0180 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0188 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0190 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0198 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   01a0 */ UN8IF_complist_0001a0, UN8IF_complist_0001a1, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   01a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_0001af,
    /*   01b0 */ UN8IF_complist_0001b0, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_0001b7,
    /*   01b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   01c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   01c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   01d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   01d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   01e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   01e8 */ NULL, NULL, UN8IF_complist_0001ea, UN8IF_complist_0001eb, NULL, NULL, NULL, NULL,
    /*   01f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   01f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_02[256] = {
    /*   0200 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0208 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0210 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0218 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0220 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000226, UN8IF_complist_000227,
    /*   0228 */ UN8IF_complist_000228, UN8IF_complist_000229, NULL, NULL, NULL, NULL, UN8IF_complist_00022e, UN8IF_complist_00022f,
    /*   0230 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0238 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0240 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0248 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0250 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0258 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0260 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0268 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0270 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0278 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0280 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0288 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0290 */ NULL, NULL, UN8IF_complist_000292, NULL, NULL, NULL, NULL, NULL,
    /*   0298 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   02f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_03[256] = {
    /*   0300 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0308 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0310 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0318 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0320 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0328 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0330 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0338 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0340 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0348 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0350 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0358 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0360 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0368 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0370 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0378 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0380 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0388 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0390 */ NULL, UN8IF_complist_000391, NULL, NULL, NULL, UN8IF_complist_000395, NULL, UN8IF_complist_000397,
    /*   0398 */ NULL, UN8IF_complist_000399, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_00039f,
    /*   03a0 */ NULL, UN8IF_complist_0003a1, NULL, NULL, NULL, UN8IF_complist_0003a5, NULL, NULL,
    /*   03a8 */ NULL, UN8IF_complist_0003a9, NULL, NULL, UN8IF_complist_0003ac, NULL, UN8IF_complist_0003ae, NULL,
    /*   03b0 */ NULL, UN8IF_complist_0003b1, NULL, NULL, NULL, UN8IF_complist_0003b5, NULL, UN8IF_complist_0003b7,
    /*   03b8 */ NULL, UN8IF_complist_0003b9, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_0003bf,
    /*   03c0 */ NULL, UN8IF_complist_0003c1, NULL, NULL, NULL, UN8IF_complist_0003c5, NULL, NULL,
    /*   03c8 */ NULL, UN8IF_complist_0003c9, UN8IF_complist_0003ca, UN8IF_complist_0003cb, NULL, NULL, UN8IF_complist_0003ce, NULL,
    /*   03d0 */ NULL, NULL, UN8IF_complist_0003d2, NULL, NULL, NULL, NULL, NULL,
    /*   03d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   03e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   03e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   03f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   03f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_04[256] = {
    /*   0400 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000406, NULL,
    /*   0408 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0410 */ UN8IF_complist_000410, NULL, NULL, UN8IF_complist_000413, NULL, UN8IF_complist_000415, UN8IF_complist_000416, UN8IF_complist_000417,
    /*   0418 */ UN8IF_complist_000418, NULL, UN8IF_complist_00041a, NULL, NULL, NULL, UN8IF_complist_00041e, NULL,
    /*   0420 */ NULL, NULL, NULL, UN8IF_complist_000423, NULL, NULL, NULL, UN8IF_complist_000427,
    /*   0428 */ NULL, NULL, NULL, UN8IF_complist_00042b, NULL, UN8IF_complist_00042d, NULL, NULL,
    /*   0430 */ UN8IF_complist_000430, NULL, NULL, UN8IF_complist_000433, NULL, UN8IF_complist_000435, UN8IF_complist_000436, UN8IF_complist_000437,
    /*   0438 */ UN8IF_complist_000438, NULL, UN8IF_complist_00043a, NULL, NULL, NULL, UN8IF_complist_00043e, NULL,
    /*   0440 */ NULL, NULL, NULL, UN8IF_complist_000443, NULL, NULL, NULL, UN8IF_complist_000447,
    /*   0448 */ NULL, NULL, NULL, UN8IF_complist_00044b, NULL, UN8IF_complist_00044d, NULL, NULL,
    /*   0450 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000456, NULL,
    /*   0458 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0460 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0468 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0470 */ NULL, NULL, NULL, NULL, UN8IF_complist_000474, UN8IF_complist_000475, NULL, NULL,
    /*   0478 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0480 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0488 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0490 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0498 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04d8 */ UN8IF_complist_0004d8, UN8IF_complist_0004d9, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04e8 */ UN8IF_complist_0004e8, UN8IF_complist_0004e9, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   04f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_05[256] = {
    /*   0500 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0508 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0510 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0518 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0520 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0528 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0530 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0538 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0540 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0548 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0550 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0558 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0560 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0568 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0570 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0578 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0580 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0588 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0590 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0598 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   05a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   05a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   05b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   05b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   05c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   05c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   05d0 */ UN8IF_complist_0005d0, UN8IF_complist_0005d1, UN8IF_complist_0005d2, UN8IF_complist_0005d3, UN8IF_complist_0005d4, UN8IF_complist_0005d5, UN8IF_complist_0005d6, NULL,
    /*   05d8 */ UN8IF_complist_0005d8, UN8IF_complist_0005d9, UN8IF_complist_0005da, UN8IF_complist_0005db, UN8IF_complist_0005dc, NULL, UN8IF_complist_0005de, NULL,
    /*   05e0 */ UN8IF_complist_0005e0, UN8IF_complist_0005e1, NULL, UN8IF_complist_0005e3, UN8IF_complist_0005e4, NULL, UN8IF_complist_0005e6, UN8IF_complist_0005e7,
    /*   05e8 */ UN8IF_complist_0005e8, UN8IF_complist_0005e9, UN8IF_complist_0005ea, NULL, NULL, NULL, NULL, NULL,
    /*   05f0 */ NULL, NULL, UN8IF_complist_0005f2, NULL, NULL, NULL, NULL, NULL,
    /*   05f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_06[256] = {
    /*   0600 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0608 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0610 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0618 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0620 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000627,
    /*   0628 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0630 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0638 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0640 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0648 */ UN8IF_complist_000648, NULL, UN8IF_complist_00064a, NULL, NULL, NULL, NULL, NULL,
    /*   0650 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0658 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0660 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0668 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0670 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0678 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0680 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0688 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0690 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0698 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06c0 */ NULL, UN8IF_complist_0006c1, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06d0 */ NULL, NULL, UN8IF_complist_0006d2, NULL, NULL, UN8IF_complist_0006d5, NULL, NULL,
    /*   06d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   06f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_09[256] = {
    /*   0900 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0908 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0910 */ NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000915, UN8IF_complist_000916, UN8IF_complist_000917,
    /*   0918 */ NULL, NULL, NULL, NULL, UN8IF_complist_00091c, NULL, NULL, NULL,
    /*   0920 */ NULL, UN8IF_complist_000921, UN8IF_complist_000922, NULL, NULL, NULL, NULL, NULL,
    /*   0928 */ UN8IF_complist_000928, NULL, NULL, UN8IF_complist_00092b, NULL, NULL, NULL, UN8IF_complist_00092f,
    /*   0930 */ UN8IF_complist_000930, NULL, NULL, UN8IF_complist_000933, NULL, NULL, NULL, NULL,
    /*   0938 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0940 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0948 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0950 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0958 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0960 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0968 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0970 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0978 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0980 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0988 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0990 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0998 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   09a0 */ NULL, UN8IF_complist_0009a1, UN8IF_complist_0009a2, NULL, NULL, NULL, NULL, NULL,
    /*   09a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_0009af,
    /*   09b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   09b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   09c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_0009c7,
    /*   09c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   09d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   09d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   09e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   09e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   09f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   09f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_0a[256] = {
    /*   0a00 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a08 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a10 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000a16, UN8IF_complist_000a17,
    /*   0a18 */ NULL, NULL, NULL, NULL, UN8IF_complist_000a1c, NULL, NULL, NULL,
    /*   0a20 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a28 */ NULL, NULL, NULL, UN8IF_complist_000a2b, NULL, NULL, NULL, NULL,
    /*   0a30 */ NULL, NULL, UN8IF_complist_000a32, NULL, NULL, NULL, NULL, NULL,
    /*   0a38 */ UN8IF_complist_000a38, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a40 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a48 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a50 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a58 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a60 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a68 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a90 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0a98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0aa0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0aa8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ab0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ab8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ac0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ac8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ad0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ad8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ae0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ae8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0af0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0af8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_0b[256] = {
    /*   0b00 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b08 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b10 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b18 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b20 */ NULL, UN8IF_complist_000b21, UN8IF_complist_000b22, NULL, NULL, NULL, NULL, NULL,
    /*   0b28 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b30 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b38 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b40 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000b47,
    /*   0b48 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b50 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b58 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b60 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b68 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0b90 */ NULL, NULL, UN8IF_complist_000b92, NULL, NULL, NULL, NULL, NULL,
    /*   0b98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ba0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ba8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0bb0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0bb8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0bc0 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000bc6, UN8IF_complist_000bc7,
    /*   0bc8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0bd0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0bd8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0be0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0be8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0bf0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0bf8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_0c[256] = {
    /*   0c00 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c08 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c10 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c18 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c20 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c28 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c30 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c38 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c40 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000c46, NULL,
    /*   0c48 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c50 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c58 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c60 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c68 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c90 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0c98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ca0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ca8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0cb0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0cb8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000cbf,
    /*   0cc0 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000cc6, NULL,
    /*   0cc8 */ NULL, NULL, UN8IF_complist_000cca, NULL, NULL, NULL, NULL, NULL,
    /*   0cd0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0cd8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ce0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ce8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0cf0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0cf8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_0d[256] = {
    /*   0d00 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d08 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d10 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d18 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d20 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d28 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d30 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d38 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d40 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_000d46, UN8IF_complist_000d47,
    /*   0d48 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d50 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d58 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d60 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d68 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d90 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0d98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0da0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0da8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0db0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0db8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0dc0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0dc8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0dd0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0dd8 */ NULL, UN8IF_complist_000dd9, NULL, NULL, UN8IF_complist_000ddc, NULL, NULL, NULL,
    /*   0de0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0de8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0df0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0df8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_0f[256] = {
    /*   0f00 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f08 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f10 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f18 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f20 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f28 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f30 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f38 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f40 */ UN8IF_complist_000f40, NULL, UN8IF_complist_000f42, NULL, NULL, NULL, NULL, NULL,
    /*   0f48 */ NULL, NULL, NULL, NULL, UN8IF_complist_000f4c, NULL, NULL, NULL,
    /*   0f50 */ NULL, UN8IF_complist_000f51, NULL, NULL, NULL, NULL, UN8IF_complist_000f56, NULL,
    /*   0f58 */ NULL, NULL, NULL, UN8IF_complist_000f5b, NULL, NULL, NULL, NULL,
    /*   0f60 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f68 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0f90 */ UN8IF_complist_000f90, NULL, UN8IF_complist_000f92, NULL, NULL, NULL, NULL, NULL,
    /*   0f98 */ NULL, NULL, NULL, NULL, UN8IF_complist_000f9c, NULL, NULL, NULL,
    /*   0fa0 */ NULL, UN8IF_complist_000fa1, NULL, NULL, NULL, NULL, UN8IF_complist_000fa6, NULL,
    /*   0fa8 */ NULL, NULL, NULL, UN8IF_complist_000fab, NULL, NULL, NULL, NULL,
    /*   0fb0 */ NULL, NULL, UN8IF_complist_000fb2, UN8IF_complist_000fb3, NULL, NULL, NULL, NULL,
    /*   0fb8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0fc0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0fc8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0fd0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0fd8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0fe0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0fe8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ff0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   0ff8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_10[256] = {
    /*   1000 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1008 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1010 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1018 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1020 */ NULL, NULL, NULL, NULL, NULL, UN8IF_complist_001025, NULL, NULL,
    /*   1028 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1030 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1038 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1040 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1048 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1050 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1058 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1060 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1068 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1070 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1078 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1080 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1088 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1090 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1098 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   10f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_1b[256] = {
    /*   1b00 */ NULL, NULL, NULL, NULL, NULL, UN8IF_complist_001b05, NULL, UN8IF_complist_001b07,
    /*   1b08 */ NULL, UN8IF_complist_001b09, NULL, UN8IF_complist_001b0b, NULL, UN8IF_complist_001b0d, NULL, NULL,
    /*   1b10 */ NULL, UN8IF_complist_001b11, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b18 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b20 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b28 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b30 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b38 */ NULL, NULL, UN8IF_complist_001b3a, NULL, UN8IF_complist_001b3c, NULL, UN8IF_complist_001b3e, UN8IF_complist_001b3f,
    /*   1b40 */ NULL, NULL, UN8IF_complist_001b42, NULL, NULL, NULL, NULL, NULL,
    /*   1b48 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b50 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b58 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b60 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b68 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b90 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1b98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ba0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ba8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1bb0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1bb8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1bc0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1bc8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1bd0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1bd8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1be0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1be8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1bf0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1bf8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_1e[256] = {
    /*   1e00 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e08 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e10 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e18 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e20 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e28 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e30 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_001e36, UN8IF_complist_001e37,
    /*   1e38 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e40 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e48 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e50 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e58 */ NULL, NULL, UN8IF_complist_001e5a, UN8IF_complist_001e5b, NULL, NULL, NULL, NULL,
    /*   1e60 */ NULL, NULL, UN8IF_complist_001e62, UN8IF_complist_001e63, NULL, NULL, NULL, NULL,
    /*   1e68 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e90 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1e98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ea0 */ UN8IF_complist_001ea0, UN8IF_complist_001ea1, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ea8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1eb0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1eb8 */ UN8IF_complist_001eb8, UN8IF_complist_001eb9, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ec0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ec8 */ NULL, NULL, NULL, NULL, UN8IF_complist_001ecc, UN8IF_complist_001ecd, NULL, NULL,
    /*   1ed0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ed8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ee0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ee8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ef0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ef8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_1f[256] = {
    /*   1f00 */ UN8IF_complist_001f00, UN8IF_complist_001f01, UN8IF_complist_001f02, UN8IF_complist_001f03, UN8IF_complist_001f04, UN8IF_complist_001f05, UN8IF_complist_001f06, UN8IF_complist_001f07,
    /*   1f08 */ UN8IF_complist_001f08, UN8IF_complist_001f09, UN8IF_complist_001f0a, UN8IF_complist_001f0b, UN8IF_complist_001f0c, UN8IF_complist_001f0d, UN8IF_complist_001f0e, UN8IF_complist_001f0f,
    /*   1f10 */ UN8IF_complist_001f10, UN8IF_complist_001f11, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f18 */ UN8IF_complist_001f18, UN8IF_complist_001f19, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f20 */ UN8IF_complist_001f20, UN8IF_complist_001f21, UN8IF_complist_001f22, UN8IF_complist_001f23, UN8IF_complist_001f24, UN8IF_complist_001f25, UN8IF_complist_001f26, UN8IF_complist_001f27,
    /*   1f28 */ UN8IF_complist_001f28, UN8IF_complist_001f29, UN8IF_complist_001f2a, UN8IF_complist_001f2b, UN8IF_complist_001f2c, UN8IF_complist_001f2d, UN8IF_complist_001f2e, UN8IF_complist_001f2f,
    /*   1f30 */ UN8IF_complist_001f30, UN8IF_complist_001f31, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f38 */ UN8IF_complist_001f38, UN8IF_complist_001f39, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f40 */ UN8IF_complist_001f40, UN8IF_complist_001f41, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f48 */ UN8IF_complist_001f48, UN8IF_complist_001f49, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f50 */ UN8IF_complist_001f50, UN8IF_complist_001f51, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f58 */ NULL, UN8IF_complist_001f59, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f60 */ UN8IF_complist_001f60, UN8IF_complist_001f61, UN8IF_complist_001f62, UN8IF_complist_001f63, UN8IF_complist_001f64, UN8IF_complist_001f65, UN8IF_complist_001f66, UN8IF_complist_001f67,
    /*   1f68 */ UN8IF_complist_001f68, UN8IF_complist_001f69, UN8IF_complist_001f6a, UN8IF_complist_001f6b, UN8IF_complist_001f6c, UN8IF_complist_001f6d, UN8IF_complist_001f6e, UN8IF_complist_001f6f,
    /*   1f70 */ UN8IF_complist_001f70, NULL, NULL, NULL, UN8IF_complist_001f74, NULL, NULL, NULL,
    /*   1f78 */ NULL, NULL, NULL, NULL, UN8IF_complist_001f7c, NULL, NULL, NULL,
    /*   1f80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f90 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1f98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1fa0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1fa8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1fb0 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_001fb6, NULL,
    /*   1fb8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_001fbf,
    /*   1fc0 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_001fc6, NULL,
    /*   1fc8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1fd0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1fd8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1fe0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1fe8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   1ff0 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_001ff6, NULL,
    /*   1ff8 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_001ffe, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_21[256] = {
    /*   2100 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2108 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2110 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2118 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2120 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2128 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2130 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2138 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2140 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2148 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2150 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2158 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2160 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2168 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2170 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2178 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2180 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2188 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2190 */ UN8IF_complist_002190, NULL, UN8IF_complist_002192, NULL, UN8IF_complist_002194, NULL, NULL, NULL,
    /*   2198 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21d0 */ UN8IF_complist_0021d0, NULL, UN8IF_complist_0021d2, NULL, UN8IF_complist_0021d4, NULL, NULL, NULL,
    /*   21d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   21f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_22[256] = {
    /*   2200 */ NULL, NULL, NULL, UN8IF_complist_002203, NULL, NULL, NULL, NULL,
    /*   2208 */ UN8IF_complist_002208, NULL, NULL, UN8IF_complist_00220b, NULL, NULL, NULL, NULL,
    /*   2210 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2218 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2220 */ NULL, NULL, NULL, UN8IF_complist_002223, NULL, UN8IF_complist_002225, NULL, NULL,
    /*   2228 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2230 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2238 */ NULL, NULL, NULL, NULL, UN8IF_complist_00223c, NULL, NULL, NULL,
    /*   2240 */ NULL, NULL, NULL, UN8IF_complist_002243, NULL, UN8IF_complist_002245, NULL, NULL,
    /*   2248 */ UN8IF_complist_002248, NULL, NULL, NULL, NULL, UN8IF_complist_00224d, NULL, NULL,
    /*   2250 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2258 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2260 */ NULL, UN8IF_complist_002261, NULL, NULL, UN8IF_complist_002264, UN8IF_complist_002265, NULL, NULL,
    /*   2268 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2270 */ NULL, NULL, UN8IF_complist_002272, UN8IF_complist_002273, NULL, NULL, UN8IF_complist_002276, UN8IF_complist_002277,
    /*   2278 */ NULL, NULL, UN8IF_complist_00227a, UN8IF_complist_00227b, UN8IF_complist_00227c, UN8IF_complist_00227d, NULL, NULL,
    /*   2280 */ NULL, NULL, UN8IF_complist_002282, UN8IF_complist_002283, NULL, NULL, UN8IF_complist_002286, UN8IF_complist_002287,
    /*   2288 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2290 */ NULL, UN8IF_complist_002291, UN8IF_complist_002292, NULL, NULL, NULL, NULL, NULL,
    /*   2298 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   22a0 */ NULL, NULL, UN8IF_complist_0022a2, NULL, NULL, NULL, NULL, NULL,
    /*   22a8 */ UN8IF_complist_0022a8, UN8IF_complist_0022a9, NULL, UN8IF_complist_0022ab, NULL, NULL, NULL, NULL,
    /*   22b0 */ NULL, NULL, UN8IF_complist_0022b2, UN8IF_complist_0022b3, UN8IF_complist_0022b4, UN8IF_complist_0022b5, NULL, NULL,
    /*   22b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   22c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   22c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   22d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   22d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   22e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   22e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   22f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   22f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_2a[256] = {
    /*   2a00 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a08 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a10 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a18 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a20 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a28 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a30 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a38 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a40 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a48 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a50 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a58 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a60 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a68 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a90 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2a98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2aa0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2aa8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2ab0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2ab8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2ac0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2ac8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2ad0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2ad8 */ NULL, NULL, NULL, NULL, NULL, UN8IF_complist_002add, NULL, NULL,
    /*   2ae0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2ae8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2af0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   2af8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_30[256] = {
    /*   3000 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3008 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3010 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3018 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3020 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3028 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3030 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3038 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3040 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_003046, NULL,
    /*   3048 */ NULL, NULL, NULL, UN8IF_complist_00304b, NULL, UN8IF_complist_00304d, NULL, UN8IF_complist_00304f,
    /*   3050 */ NULL, UN8IF_complist_003051, NULL, UN8IF_complist_003053, NULL, UN8IF_complist_003055, NULL, UN8IF_complist_003057,
    /*   3058 */ NULL, UN8IF_complist_003059, NULL, UN8IF_complist_00305b, NULL, UN8IF_complist_00305d, NULL, UN8IF_complist_00305f,
    /*   3060 */ NULL, UN8IF_complist_003061, NULL, NULL, UN8IF_complist_003064, NULL, UN8IF_complist_003066, NULL,
    /*   3068 */ UN8IF_complist_003068, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_00306f,
    /*   3070 */ NULL, NULL, UN8IF_complist_003072, NULL, NULL, UN8IF_complist_003075, NULL, NULL,
    /*   3078 */ UN8IF_complist_003078, NULL, NULL, UN8IF_complist_00307b, NULL, NULL, NULL, NULL,
    /*   3080 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3088 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3090 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   3098 */ NULL, NULL, NULL, NULL, NULL, UN8IF_complist_00309d, NULL, NULL,
    /*   30a0 */ NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_0030a6, NULL,
    /*   30a8 */ NULL, NULL, NULL, UN8IF_complist_0030ab, NULL, UN8IF_complist_0030ad, NULL, UN8IF_complist_0030af,
    /*   30b0 */ NULL, UN8IF_complist_0030b1, NULL, UN8IF_complist_0030b3, NULL, UN8IF_complist_0030b5, NULL, UN8IF_complist_0030b7,
    /*   30b8 */ NULL, UN8IF_complist_0030b9, NULL, UN8IF_complist_0030bb, NULL, UN8IF_complist_0030bd, NULL, UN8IF_complist_0030bf,
    /*   30c0 */ NULL, UN8IF_complist_0030c1, NULL, NULL, UN8IF_complist_0030c4, NULL, UN8IF_complist_0030c6, NULL,
    /*   30c8 */ UN8IF_complist_0030c8, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_0030cf,
    /*   30d0 */ NULL, NULL, UN8IF_complist_0030d2, NULL, NULL, UN8IF_complist_0030d5, NULL, NULL,
    /*   30d8 */ UN8IF_complist_0030d8, NULL, NULL, UN8IF_complist_0030db, NULL, NULL, NULL, NULL,
    /*   30e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   30e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, UN8IF_complist_0030ef,
    /*   30f0 */ UN8IF_complist_0030f0, UN8IF_complist_0030f1, UN8IF_complist_0030f2, NULL, NULL, NULL, NULL, NULL,
    /*   30f8 */ NULL, NULL, NULL, NULL, NULL, UN8IF_complist_0030fd, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_00_fb[256] = {
    /*   fb00 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb08 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb10 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb18 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb20 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb28 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb30 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb38 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb40 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb48 */ NULL, UN8IF_complist_00fb49, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb50 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb58 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb60 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb68 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb90 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fb98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fba0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fba8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbb0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbb8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbc0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbc8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbd0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbd8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbe0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbe8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbf0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /*   fbf8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_05[256] = {
    /* 010500 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010508 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010510 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010518 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010520 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010528 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010530 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010538 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010540 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010548 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010550 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010558 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010560 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010568 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010570 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010578 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010580 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010588 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010590 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 010598 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105d0 */ NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_0105d2, NULL, NULL, NULL, NULL, NULL,
    /* 0105d8 */ NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_0105da, NULL, NULL, NULL, NULL, NULL,
    /* 0105e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0105f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_10[256] = {
    /* 011000 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011008 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011010 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011018 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011020 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011028 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011030 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011038 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011040 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011048 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011050 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011058 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011060 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011068 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011070 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011078 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011080 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011088 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011090 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011098 */ NULL, (const UN8IF_complist_s *)UN8IF_complist_011099, NULL, (const UN8IF_complist_s *)UN8IF_complist_01109b, NULL, NULL, NULL, NULL,
    /* 0110a0 */ NULL, NULL, NULL, NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_0110a5, NULL, NULL,
    /* 0110a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0110f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_11[256] = {
    /* 011100 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011108 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011110 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011118 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011120 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011128 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011130 */ NULL, (const UN8IF_complist_s *)UN8IF_complist_011131, (const UN8IF_complist_s *)UN8IF_complist_011132, NULL, NULL, NULL, NULL, NULL,
    /* 011138 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011140 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011148 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011150 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011158 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011160 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011168 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011170 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011178 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011180 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011188 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011190 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011198 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0111f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_13[256] = {
    /* 011300 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011308 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011310 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011318 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011320 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011328 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011330 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011338 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011340 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_011347,
    /* 011348 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011350 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011358 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011360 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011368 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011370 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011378 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011380 */ NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_011382, NULL, (const UN8IF_complist_s *)UN8IF_complist_011384, NULL, NULL, NULL,
    /* 011388 */ NULL, NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_01138b, NULL, NULL, NULL, NULL,
    /* 011390 */ (const UN8IF_complist_s *)UN8IF_complist_011390, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011398 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113c0 */ NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_0113c2, NULL, NULL, NULL, NULL, NULL,
    /* 0113c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0113f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_14[256] = {
    /* 011400 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011408 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011410 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011418 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011420 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011428 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011430 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011438 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011440 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011448 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011450 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011458 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011460 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011468 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011470 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011478 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011480 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011488 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011490 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011498 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114b8 */ NULL, (const UN8IF_complist_s *)UN8IF_complist_0114b9, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0114f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_15[256] = {
    /* 011500 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011508 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011510 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011518 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011520 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011528 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011530 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011538 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011540 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011548 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011550 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011558 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011560 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011568 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011570 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011578 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011580 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011588 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011590 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011598 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115b8 */ (const UN8IF_complist_s *)UN8IF_complist_0115b8, (const UN8IF_complist_s *)UN8IF_complist_0115b9, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0115f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_19[256] = {
    /* 011900 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011908 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011910 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011918 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011920 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011928 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011930 */ NULL, NULL, NULL, NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_011935, NULL, NULL,
    /* 011938 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011940 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011948 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011950 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011958 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011960 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011968 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011970 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011978 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011980 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011988 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011990 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 011998 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0119f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_61[256] = {
    /* 016100 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016108 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016110 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016118 */ NULL, NULL, NULL, NULL, NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_01611e, NULL,
    /* 016120 */ NULL, (const UN8IF_complist_s *)UN8IF_complist_016121, (const UN8IF_complist_s *)UN8IF_complist_016122, NULL, NULL, NULL, NULL, NULL,
    /* 016128 */ NULL, (const UN8IF_complist_s *)UN8IF_complist_016129, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016130 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016138 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016140 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016148 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016150 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016158 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016160 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016168 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016170 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016178 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016180 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016188 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016190 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016198 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161b8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 0161f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_6d[256] = {
    /* 016d00 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d08 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d10 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d18 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d20 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d28 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d30 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d38 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d40 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d48 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d50 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d58 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d60 */ NULL, NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_016d63, NULL, NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_016d67,
    /* 016d68 */ NULL, (const UN8IF_complist_s *)UN8IF_complist_016d69, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d70 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d78 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d80 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d88 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d90 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016d98 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016da0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016da8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016db0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016db8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016dc0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016dc8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016dd0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016dd8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016de0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016de8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016df0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 016df8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static const UN8IF_complist_s *UN8IF_compos_01_d1[256] = {
    /* 01d100 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d108 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d110 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d118 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d120 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d128 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d130 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d138 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d140 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d148 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d150 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_01d157,
    /* 01d158 */ (const UN8IF_complist_s *)UN8IF_complist_01d158, NULL, NULL, NULL, NULL, NULL, NULL, (const UN8IF_complist_s *)UN8IF_complist_01d15f,
    /* 01d160 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d168 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d170 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d178 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d180 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d188 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d190 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d198 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1a0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1a8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1b0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1b8 */ NULL, (const UN8IF_complist_s *)UN8IF_complist_01d1b9, (const UN8IF_complist_s *)UN8IF_complist_01d1ba, (const UN8IF_complist_s *)UN8IF_complist_01d1bb, (const UN8IF_complist_s *)UN8IF_complist_01d1bc, NULL, NULL, NULL,
    /* 01d1c0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1c8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1d0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1d8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1e0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1e8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1f0 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 01d1f8 */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/* the planes */
static const UN8IF_complist_s **UN8IF_compos_00[256] = {
    UN8IF_compos_00_00,
    UN8IF_compos_00_01,
    UN8IF_compos_00_02,
    UN8IF_compos_00_03,
    UN8IF_compos_00_04,
    UN8IF_compos_00_05,
    UN8IF_compos_00_06,
    NULL,
    NULL, UN8IF_compos_00_09,
    UN8IF_compos_00_0a,
    UN8IF_compos_00_0b,
    UN8IF_compos_00_0c,
    UN8IF_compos_00_0d,
    NULL, UN8IF_compos_00_0f,
    UN8IF_compos_00_10,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, UN8IF_compos_00_1b,
    NULL, NULL, UN8IF_compos_00_1e,
    UN8IF_compos_00_1f,
    NULL, UN8IF_compos_00_21,
    UN8IF_compos_00_22,
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, UN8IF_compos_00_2a,
    NULL, NULL, NULL, NULL, NULL,
    UN8IF_compos_00_30,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, UN8IF_compos_00_fb,
    NULL, NULL, NULL, NULL};

static const UN8IF_complist_s **UN8IF_compos_01[256] = {
    NULL, NULL, NULL, NULL, NULL, UN8IF_compos_01_05,
    NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    UN8IF_compos_01_10,
    UN8IF_compos_01_11,
    NULL, UN8IF_compos_01_13,
    UN8IF_compos_01_14,
    UN8IF_compos_01_15,
    NULL, NULL,
    NULL, UN8IF_compos_01_19,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, UN8IF_compos_01_61,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, UN8IF_compos_01_6d,
    NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, UN8IF_compos_01_d1,
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/* the main plane */
static const UN8IF_complist_s ***UN8IF_compos[] = {
    UN8IF_compos_00,
    UN8IF_compos_01,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL};
