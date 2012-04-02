#ifndef MOSYNC_STRING_H
#define MOSYNC_STRING_H

#include <mastring.h>
#include <stdio.h>

int strcoll(const char *s1, const char *s2);
char* strerror(int errnum);
int fprintf(FILE *stream, const char *format, ...);

#endif
