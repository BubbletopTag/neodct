/*
 * NeoDCT browser chrome: URL helpers.
 */

#include <string.h>

#include "neodct_url.h"

/* copy [start, start+len) into buf; NULL if it does not fit */
static const char *copy_span(const char *start, size_t len,
			     char *buf, size_t buflen)
{
	if (len + 1 > buflen)
		return NULL;
	memcpy(buf, start, len);
	buf[len] = '\0';
	return buf;
}

static void trim(const char **start, const char **end)
{
	while (*start < *end &&
	       (**start == ' ' || **start == '\t' ||
		**start == '\n' || **start == '\r'))
		(*start)++;
	while (*end > *start &&
	       ((*end)[-1] == ' ' || (*end)[-1] == '\t' ||
		(*end)[-1] == '\n' || (*end)[-1] == '\r'))
		(*end)--;
}

/* find "://" within [start, end) without needing GNU memmem */
static const char *find_scheme_sep(const char *start, const char *end)
{
	const char *p;

	for (p = start; p + 3 <= end; p++) {
		if (p[0] == ':' && p[1] == '/' && p[2] == '/')
			return p;
	}
	return NULL;
}

const char *neodct_url_normalize(const char *input, char *buf, size_t buflen)
{
	const char *start, *end;
	static const char scheme[] = "https://";
	size_t len, slen;

	if (input == NULL)
		return NULL;

	start = input;
	end = input + strlen(input);
	trim(&start, &end);
	len = (size_t)(end - start);

	if (len == 0)
		return NULL;

	if (find_scheme_sep(start, end) != NULL)
		return copy_span(start, len, buf, buflen);

	slen = sizeof(scheme) - 1;
	if (slen + len + 1 > buflen)
		return NULL;
	memcpy(buf, scheme, slen);
	memcpy(buf + slen, start, len);
	buf[slen + len] = '\0';
	return buf;
}

const char *neodct_host_from_url(const char *url, char *buf, size_t buflen)
{
	const char *start, *end, *p;

	if (url == NULL)
		return NULL;

	start = url;
	end = url + strlen(url);
	trim(&start, &end);

	/* skip past scheme separator if present */
	p = find_scheme_sep(start, end);
	if (p != NULL)
		start = p + 3;

	/* cut at first path or query separator */
	for (p = start; p < end; p++) {
		if (*p == '/' || *p == '?') {
			end = p;
			break;
		}
	}

	if (start == end) {
		/* degenerate: fall back to the full input */
		return copy_span(url, strlen(url), buf, buflen);
	}

	return copy_span(start, (size_t)(end - start), buf, buflen);
}
