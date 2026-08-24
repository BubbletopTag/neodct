/*
 * NeoDCT browser chrome: URL helpers.
 *
 * Pure string logic, no netsurf dependencies, unit tested in test/.
 */

#ifndef NEODCT_URL_H
#define NEODCT_URL_H

#include <stddef.h>

/**
 * Normalise user-entered text into a fetchable URL.
 *
 * Strips surrounding whitespace and prefixes "https://" when no
 * scheme ("://") is present.
 *
 * \param input user text, may be NULL
 * \param buf output buffer
 * \param buflen size of buf
 * \return buf on success, NULL if input is empty/NULL or buf too small
 */
const char *neodct_url_normalize(const char *input, char *buf, size_t buflen);

/**
 * Extract the host part of a URL for status display
 * ("Waiting for <host>...").
 *
 * \param url url string, may be NULL
 * \param buf output buffer
 * \param buflen size of buf
 * \return buf on success (falls back to the input on degenerate urls),
 *         NULL if url is NULL or buf too small
 */
const char *neodct_host_from_url(const char *url, char *buf, size_t buflen);

#endif
