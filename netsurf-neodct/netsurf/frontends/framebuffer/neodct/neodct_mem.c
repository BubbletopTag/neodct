/*
 * NeoDCT browser chrome: memory usage reporting.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "neodct_mem.h"

long neodct_mem_parse_field(const char *status_text, const char *field)
{
	const char *p;
	size_t flen;

	if (status_text == NULL || field == NULL)
		return -1;

	flen = strlen(field);
	p = status_text;
	while (p != NULL) {
		/* fields match only at line starts */
		if ((p == status_text || p[-1] == '\n') &&
		    strncmp(p, field, flen) == 0 && p[flen] == ':') {
			return strtol(p + flen + 1, NULL, 10);
		}
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	return -1;
}

int neodct_mem_format_long(char *buf, size_t buflen, long value)
{
	char tmp[24];
	int i = 0, len = 0;
	unsigned long v;

	if (value < 0) {
		if (len + 1 >= (int)buflen)
			return -1;
		buf[len++] = '-';
		v = (unsigned long)-value;
	} else {
		v = (unsigned long)value;
	}

	do {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v > 0 && i < (int)sizeof(tmp));

	if (len + i + 1 > (int)buflen)
		return -1;
	while (i > 0)
		buf[len++] = tmp[--i];
	buf[len] = '\0';
	return len;
}
