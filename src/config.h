/* Minimal config.h for libu8ident integration in rcc */
#ifndef __U8ID_CONF_H__
#define __U8ID_CONF_H__

#define HAVE_CONFIG_H 1
#define HAVE_ASSERT_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_C11 1
#define HAVE___BUILTIN_FFS 1
#define restrict __restrict__
#define U8ID_NORM NFC
#define U8ID_PROFILE TR39_4
/* #undef U8ID_PROFILE_TR39 */
#define U8ID_TR31 TR39

#endif /* __U8ID_CONF_H__ */
