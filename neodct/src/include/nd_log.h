/* nd_log.h -- the colourful serial log, preserved byte for byte.
 *
 * The project owner asked specifically that this survive the rewrite
 * unchanged, and "unchanged" here is stricter than it sounds. The Python
 * (System/core/logstyle.py) does four separate things:
 *
 *   1. a named palette -- MODEM blue, CORE green, CRASH red;
 *   2. a DERIVED colour for the eleven app tags, walking a purple/pink band;
 *   3. a DERIVED colour for any tag it has never heard of, computed from the
 *      tag's own characters so a subsystem added later is consistent from its
 *      first boot;
 *   4. line-oriented painting: a line that begins "[TAG]" gets the bracketed
 *      tag painted bold, and the rest of the line left alone.
 *
 * Items 2 and 3 are arithmetic, and a port that gets the palette right and the
 * arithmetic wrong looks correct until someone adds a tag. The exact expected
 * bytes for 22 named tags, 11 app tags, 12 unregistered tags and 14 splitting
 * edge cases are recorded in neodct/tests/golden/log/logref.json -- that file
 * is the oracle for this module, and test/unit/test_nd_log.c checks against it.
 *
 * Everything the shell side does lives in /etc/neodct-colors.sh and uses the
 * same palette; a tag is the same colour whoever printed it.
 */

#ifndef ND_LOG_H
#define ND_LOG_H

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The tag vocabulary
 * ------------------------------------------------------------------ *
 *
 * Tags are plain strings, not an enum, for two reasons: the Python emits some
 * of them from string variables (app names), and the derived-colour paths
 * exist precisely so that an unregistered tag still works. Use the constants
 * where one exists so a typo is a link error rather than a wrong colour.
 */

/* Named palette -- these have an explicit entry in TAG_COLOURS. */
#define ND_LOG_MODEM    "MODEM"    /* 39  bright blue -- the network stack   */
#define ND_LOG_NDSYS    "ndsys"    /* 33  deeper blue -- initramfs / applier */
#define ND_LOG_UPDATE   "UPDATE"   /* 33                                     */
#define ND_LOG_CORE     "CORE"     /* 46  green -- the OS itself             */
#define ND_LOG_OS       "OS"       /* 46                                     */
#define ND_LOG_LAUNCHER "Launcher" /* 82  lighter green                      */
#define ND_LOG_BATT     "BATT"     /* 226 yellow -- power                    */
#define ND_LOG_FUEL     "FUEL"     /* 226                                    */
#define ND_LOG_NOTIFY   "NOTIFY"   /* 201 magenta -- user-facing events      */
#define ND_LOG_INPUT    "INPUT"    /* 51  cyan -- keypad / events            */
#define ND_LOG_KEYMAP   "KEYMAP"   /* 87                                     */
#define ND_LOG_SETUP    "SETUP"    /* 214 orange -- first-boot wizards       */
#define ND_LOG_UI       "UI"       /* 120 pale green                         */
#define ND_LOG_FB       "FB"       /* 123 pale cyan                          */
#define ND_LOG_KERNEL   "KERNEL"   /* 244 grey -- background noise           */
#define ND_LOG_SDCARD   "sdcard"   /* 180                                    */
#define ND_LOG_CLOCK    "CLOCK"    /* 129 violet -- time, and what set it    */
#define ND_LOG_RSHELL   "RSHELL"   /* 162 deep pink -- reachable from outside*/
#define ND_LOG_BROWSER  "Browser"  /* 141 purple -- netsurf and its noise    */
#define ND_LOG_CRASH    "CRASH"    /* 196 red -- something broke             */
#define ND_LOG_ERROR    "ERROR"    /* 196                                    */
#define ND_LOG_FATAL    "FATAL"    /* 196                                    */

/* App tags -- no explicit entry; coloured by 141 + (sum of bytes % 36). */
#define ND_LOG_KOKI       "Koki"
#define ND_LOG_MUSIC      "Music"
#define ND_LOG_CALLLOG    "CallLog"
#define ND_LOG_SETTINGS   "Settings"
#define ND_LOG_PB         "PB"
#define ND_LOG_TONES      "Tones"
#define ND_LOG_GAMES      "Games"
#define ND_LOG_MESSAGES   "Messages"
#define ND_LOG_CLOCK_APP  "Clock"
#define ND_LOG_CALCULATOR "Calculator"
#define ND_LOG_POWER      "Power"

/* The colour every line on stderr is painted, tag or no tag. */
#define ND_LOG_ERROR_COLOUR 196

/* Longest tag the splitter will recognise. "ZZ_LONG_TAG_NAME" is 16; 64 is
 * slack for an app name nobody has invented yet. */
#define ND_LOG_TAG_MAX 64

/* One formatted log line, before painting. Longer messages are truncated with
 * no marker, exactly as a fixed serial buffer would. */
#define ND_LOG_LINE_MAX 1024

/* Widest banner line nd_log_banner_lines() will return. */
#define ND_LOG_BANNER_COLS 128

/* ------------------------------------------------------------------ *
 * Emitting
 * ------------------------------------------------------------------ */

/* Print "[TAG] <formatted>\n" to stdout, with "[TAG]" painted bold in the
 * tag's colour and the remainder left plain. This is the exact shape the
 * Python's print("[TAG] ...") produced once logstyle's stdout wrapper had run,
 * including the space after the bracket belonging to the unpainted remainder.
 *
 * Thread-safe: one line is written with a single write(2) so two threads
 * cannot interleave halves of a line. */
void nd_log(const char *tag, const char *fmt, ...) ND_PRINTF(2, 3);
void nd_logv(const char *tag, const char *fmt, va_list ap);

/* The same line on stderr, painted red (196) in its ENTIRETY -- tag included,
 * not bold. The Python paints everything on stderr because tracebacks arrive
 * there as untagged lines, and this reproduces that. */
void nd_log_err(const char *tag, const char *fmt, ...) ND_PRINTF(2, 3);
void nd_log_errv(const char *tag, const char *fmt, va_list ap);

/* Paint and emit a line that was built elsewhere and may or may not carry a
 * "[TAG]" prefix. This is the direct equivalent of the Python's stdout
 * wrapper, and is what to use when relaying another program's output. The
 * line must not contain '\n'; a trailing newline is added. */
void nd_log_line(const char *line);

/* ------------------------------------------------------------------ *
 * The pieces, exposed because the unit test checks each one separately
 * ------------------------------------------------------------------ */

/* NO_COLOR set to anything at all (including empty) disables colour.
 * Otherwise NEODCT_COLOR disables it when it is exactly "0", "no" or "off".
 * Evaluated once and cached; nd_log_set_colour() overrides for tests. */
bool nd_log_colour_enabled(void);
void nd_log_set_colour(bool on);

/* The 256-colour code for a tag: the named palette, else 141 + (sum % 36) for
 * an app tag, else 22 + (sum % 180). "sum" is the sum of the tag's bytes.
 * Deterministic, never fails, never allocates. */
int nd_log_colour_for(const char *tag);

/* Write "<bold?><ESC>[38;5;<code>m<text><ESC>[0m" into out. With colour off,
 * writes text unchanged. Returns the length it wanted, snprintf-style, so
 * (return >= out_sz) means truncation. */
size_t nd_log_paint(char *out, size_t out_sz, const char *text, int code, bool bold);

/* Split "[TAG] rest" the way logstyle._split_tag does:
 *   - the line must begin with '[';
 *   - the first ']' must be at index >= 2, so "[]" is not a tag;
 *   - every character between must be alphanumeric, '_' or '-'.
 * On success copies the tag into tag_out, points *rest_out at the character
 * after ']' (which is usually a space -- it belongs to the remainder, not the
 * tag) and returns true. On failure returns false and leaves both untouched.
 * A tag longer than tag_sz-1 is a failure, not a truncation. */
bool nd_log_split_tag(const char *line, char *tag_out, size_t tag_sz, const char **rest_out);

/* Render one line exactly as the stdout painter would, into out. Whitespace-
 * only and untagged lines come back unchanged. Returns the wanted length. */
size_t nd_log_render(char *out, size_t out_sz, const char *line);

/* The full-width divider drawn either side of the boot banner: `width` copies
 * of `ch`, painted bold in `code` (46, green, is the Python default).
 * Returns the wanted length. */
size_t nd_log_rule(char *out, size_t out_sz, char ch, size_t width, int code);

/* Read the pre-rendered boot banner. Returns the number of lines written,
 * 0 when the file is missing (which is not an error -- images without a
 * banner are normal). Trailing newlines are stripped, as in the Python. */
size_t nd_log_banner_lines(const char *path, char out[][ND_LOG_BANNER_COLS], size_t max);

/* ------------------------------------------------------------------ *
 * The serial console
 * ------------------------------------------------------------------ */

/* launcher.py's _redirect_stdio_to_serial(). Picks /dev/ttyFIQ0 if it exists
 * (real Rockchip/Luckfox), else /dev/ttyAMA0 (QEMU PL011), else
 * $NEODCT_SERIAL_DEVICE, opens it for writing and points stdout and stderr at
 * it. Logs "[Launcher] Serial console active: <dev>" on success and
 * "[Launcher] Serial redirect failed for <dev>: <errno>" on failure.
 *
 * Boot continues either way -- the caller ignores the return value in
 * production and only the tests check it. Writes the chosen device into
 * chosen_out when that is non-NULL. */
nd_err nd_log_redirect_serial(char *chosen_out, size_t chosen_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_LOG_H */
