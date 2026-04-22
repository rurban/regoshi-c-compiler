#ifndef RCC_STDIO_H
#define RCC_STDIO_H

#include <stddef.h>

typedef struct __rcc_FILE FILE;

#ifdef _WIN32
FILE *__acrt_iob_func(unsigned idx);
#define stdin (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
#else
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#endif
#define EOF (-1)

int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
char *fgets(char *buf, int size, FILE *stream);
int fflush(FILE *stream);

#endif
