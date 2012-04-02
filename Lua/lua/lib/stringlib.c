#include <string.h>

int strcoll(const char *s1, const char *s2)
{
	// We don't care about locale specific comparisons for now.
	return strcmp(s1, s2);
}

// Pseudo implementation.
char* strerror(int errnum)
{
	static char buf[255];
	sprintf(buf, "Error Number: %d", errnum);
	return buf;
}

// Just here to make code compile.
int fprintf(FILE *stream, const char *format, ...)
{
	return -1;
}
