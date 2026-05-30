#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>

void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

int    atoi(const char *s);
long   atol(const char *s);

extern char **environ;
char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    putenv(char *string);

void   exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));

#endif /* LIBC_STDLIB_H */
