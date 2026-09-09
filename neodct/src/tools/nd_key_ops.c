/* nd_key_ops.c -- token resolution and argument parsing for nd-key.
 *
 * Split from main() so test_ndkey can link it. See nd_key.h for why the
 * single-digit rule is what it is.
 */

#include "nd_key.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_keycodes.h"
#include "nd_keypadsetup.h"
#include "nd_types.h"

/* Lowercase a token into buf. Returns false if it does not fit, which for a
 * key name means it was never going to resolve anyway. */
static bool lower_copy(const char *src, char *buf, size_t buf_sz)
{
    size_t i;

    for (i = 0u; src[i] != '\0'; i++) {
        if (i + 1u >= buf_sz)
            return false;
        buf[i] = (src[i] >= 'A' && src[i] <= 'Z') ? (char)(src[i] - 'A' + 'a') : src[i];
    }
    if (i >= buf_sz)
        return false;
    buf[i] = '\0';
    return true;
}

static bool all_digits(const char *s)
{
    size_t i;

    if (s[0] == '\0')
        return false;
    for (i = 0u; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

/* "50" -> 50, bounded to a plausible evdev code. -1 on anything else. */
static int32_t parse_raw(const char *s)
{
    char *end = NULL;
    long v;

    if (!all_digits(s))
        return -1;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return -1;
    if (v <= 0 || v > 0xFFFF)
        return -1;
    return (int32_t)v;
}

int32_t nd_key_token_to_code(const char *token)
{
    char lower[64];
    char named[72];
    int32_t code;

    if (token == NULL || token[0] == '\0')
        return -1;

    /* The keys whose printed legend is a symbol. Doing these first means a
     * shell that ate the quotes around * still gets a sensible answer. */
    if (strcmp(token, "*") == 0)
        return ND_KEY_STAR;
    if (strcmp(token, "#") == 0)
        return ND_KEY_HASH;

    /* raw:N -- the escape hatch that makes single-digit raw codes reachable. */
    if (strncmp(token, "raw:", 4) == 0)
        return parse_raw(token + 4);

    if (all_digits(token)) {
        /* One digit is the key with that digit printed on it; the table in
         * nd_keycodes.c owns the 1..9,0 -> 2..10,11 mapping and this must not
         * repeat it. Two or more digits is a raw code. */
        if (token[1] == '\0') {
            (void)snprintf(named, sizeof named, "num_%c", token[0]);
            return nd_keycode_for_name(named);
        }
        return parse_raw(token);
    }

    if (!lower_copy(token, lower, sizeof lower))
        return -1;

    /* The project's own name table first: navikey, clear, up, down, menu,
     * enter, back, num_N, star, hash. */
    code = nd_keycode_for_name(lower);
    if (code >= 0)
        return code;

    /* Aliases the table does not carry but a person reaching for this tool
     * will type. Deliberately few -- every alias is a second spelling that
     * has to keep working. */
    if (strcmp(lower, "ok") == 0 || strcmp(lower, "select") == 0 || strcmp(lower, "center") == 0)
        return ND_KEY_NAVIKEY;
    if (strcmp(lower, "c") == 0 || strcmp(lower, "cancel") == 0)
        return ND_KEY_CLEAR;
    if (strcmp(lower, "space") == 0)
        return ND_KEY_SPACE;
    if (strcmp(lower, "minus") == 0 || strcmp(lower, "dash") == 0)
        return ND_KEY_MINUS;
    if (strcmp(lower, "dot") == 0 || strcmp(lower, "period") == 0)
        return ND_KEY_DOT;
    if (strcmp(lower, "comma") == 0)
        return ND_KEY_COMMA;

    return -1;
}

bool nd_key_on_matrix(int32_t code)
{
    size_t i;

    /* Derived from the wizard's own target list, so "which keys exist" has
     * exactly one definition in the tree. */
    for (i = 0u; i < ND_KPSETUP_N_TARGETS; i++) {
        if (nd_keycode_for_name(nd_kpsetup_targets[i].name) == code)
            return true;
    }
    return false;
}

bool nd_key_parse_args(int argc, char **argv, nd_key_opts *out, char *err, size_t err_sz)
{
    int i;

    if (out == NULL || err == NULL || err_sz == 0u)
        return false;

    memset(out, 0, sizeof *out);
    out->action = ND_KEY_ACT_TAP;
    out->delay_ms = ND_KEY_DEFAULT_DELAY_MS;
    out->socket_path = NULL;
    out->first_key = argc;
    err[0] = '\0';

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            out->help = true;
            out->first_key = argc;
            return true;
        }
        if (strcmp(a, "--list") == 0) {
            out->list = true;
            out->first_key = argc;
            return true;
        }
        if (strcmp(a, "--hold") == 0) {
            out->action = ND_KEY_ACT_HOLD;
            continue;
        }
        if (strcmp(a, "--release") == 0) {
            out->action = ND_KEY_ACT_RELEASE;
            continue;
        }
        if (strcmp(a, "--delay") == 0) {
            char *end = NULL;
            long v;

            if (i + 1 >= argc) {
                (void)snprintf(err, err_sz, "--delay needs a value in milliseconds");
                return false;
            }
            v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 0 || v > 60000) {
                (void)snprintf(err, err_sz, "--delay wants 0..60000 ms, got \"%s\"", argv[i]);
                return false;
            }
            out->delay_ms = (unsigned int)v;
            continue;
        }
        if (strcmp(a, "--socket") == 0) {
            if (i + 1 >= argc) {
                (void)snprintf(err, err_sz, "--socket needs a path");
                return false;
            }
            out->socket_path = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            (void)snprintf(err, err_sz, "unknown option \"%s\"", a);
            return false;
        }
        /* The first thing that is not an option starts the key list, and
         * everything after it is a key -- including something that looks like
         * an option, so a key can never be eaten by a typo'd flag. */
        out->first_key = i;
        break;
    }

    if (!out->help && !out->list && out->first_key >= argc) {
        (void)snprintf(err, err_sz, "no keys given");
        return false;
    }

    /* Resolve every key now, so a typo in the last key does not press the
     * first three and then fail. */
    for (i = out->first_key; i < argc; i++) {
        if (nd_key_token_to_code(argv[i]) < 0) {
            (void)snprintf(err, err_sz, "unknown key \"%s\" (try --list)", argv[i]);
            return false;
        }
    }
    return true;
}
