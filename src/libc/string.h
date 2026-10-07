/* Freestanding string.h for the Nano image: the shared common/ code (fb.c)
 * includes <string.h>, and so does FatFs. Definitions are in src/app/main.c. */
#ifndef RV_STRING_H
#define RV_STRING_H
#include <stddef.h>
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char *strchr(const char *s, int c);
#endif
