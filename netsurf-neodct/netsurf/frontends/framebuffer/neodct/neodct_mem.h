/*
 * NeoDCT browser chrome: memory usage reporting.
 *
 * Parses /proc/self/status fields so the shell can log RSS to the
 * serial console periodically (watching for memory pressure on the
 * 64MB target).
 */

#ifndef NEODCT_MEM_H
#define NEODCT_MEM_H

#include <stddef.h>

/**
 * Extract a memory field value in kB from /proc/self/status or
 * /proc/meminfo text.
 *
 * \param status_text full text of the proc file, may be NULL
 * \param field field name without colon, e.g. "VmRSS", "MemAvailable"
 * \return value in kB, or -1 when the field is absent
 */
long neodct_mem_parse_field(const char *status_text, const char *field);

/**
 * Format a number into buf, async-signal-safely (usable from the
 * crash handler, unlike snprintf).
 *
 * \return the string length, or -1 when buf is too small
 */
int neodct_mem_format_long(char *buf, size_t buflen, long value);

#endif
