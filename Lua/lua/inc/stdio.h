#ifndef MOSYNC_STDIO_H
#define MOSYNC_STDIO_H

#include <MAFS/File.h>

#define	BUFSIZ 1024
#define L_tmpnam FILENAME_MAX

#define _IOFBF 0 /* setvbuf should set fully buffered */
#define _IOLBF 1 /* setvbuf should set line buffered */
#define _IONBF 2 /* setvbuf should set unbuffered */

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

FILE* freopen(const char* path, const char* mode, FILE* fp);
//int setvbuf(FILE *stream, char *buf, int mode, size_t size);
FILE* tmpfile(void);

#endif
