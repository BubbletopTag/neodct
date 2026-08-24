/* modem_probe.c -- what the Modem app's DATA page reads off the filesystem.
 *
 * Port of the eight module-level helpers in
 * System/engineering/apps/Modem/main.py that touch /dev, /sys, /proc and
 * /etc. Every one of them swallows its errors and answers None -- the app is
 * there to show you a phone that is not working, so a missing file is an
 * answer rather than a failure, and none of these logs anything.
 *
 * ============ THE PATHS RESOLVE, INCLUDING /sys AND /proc ============
 *
 * OPEN-QUESTIONS.md M-9 settled this for the modem service and the same rule
 * applies here: nd_path_resolve() prepends ND_ROOT, which is empty in
 * production and a staging directory under test. It is what lets
 * test_modem_app.c hand this file a fake /sys/class/net with a driver symlink
 * in it, and it also makes the capture deterministic -- without it,
 * golden/eng-modem.png's "PORTS  no ttyUSB nodes!" would become
 * "PORTS  ttyUSB0" on any developer's machine with a USB serial adaptor
 * plugged in, and the frame would fail for a reason that has nothing to do
 * with the port. The Python is the one that reads the build host's real /dev
 * here; it gets away with it because uistub's PathRemap only covers /NeoDCT.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "nd_paths.h"
#include "nd_types.h"

#include "modem_probe.h"

/* One directory's worth of names. /sys/class/net has a handful and /dev a few
 * hundred; 512 is well past either and the cap is here because
 * CODING-STANDARDS.md 1.5 forbids sizing anything by what readdir returns. */
#define PROBE_MAX_ENTRIES 512
#define PROBE_NAME_MAX    64

typedef struct {
    char name[PROBE_MAX_ENTRIES][PROBE_NAME_MAX];
    size_t n;
} probe_names;

/* ------------------------------------------------------------------ *
 * String helpers
 * ------------------------------------------------------------------ */

void nd_modemapp_strip(char *s)
{
    size_t len;
    size_t start = 0u;

    if (s == NULL)
        return;
    len = strlen(s);
    while (len > start && isspace((unsigned char)s[len - 1u]) != 0)
        len--;
    while (start < len && isspace((unsigned char)s[start]) != 0)
        start++;
    memmove(s, s + start, len - start);
    s[len - start] = '\0';
}

const char *nd_modemapp_shorten(const char *text, size_t limit, char *out, size_t out_sz)
{
    size_t len;
    size_t keep;

    if (out == NULL || out_sz == 0u)
        return out;
    if (text == NULL)
        text = "";

    len = strlen(text);
    if (len <= limit) {
        (void)nd_strlcpy(out, text, out_sz);
        return out;
    }

    /* keep = (limit - 2) // 2, applied to BOTH halves, so an odd limit comes
     * out one short of itself. The Python does the same arithmetic and the
     * only limit anything passes is the default 24. */
    keep = limit >= 2u ? (limit - 2u) / 2u : 0u;
    if (keep + 2u + keep + 1u > out_sz) {
        (void)nd_strlcpy(out, text, out_sz);
        return out;
    }
    memcpy(out, text, keep);
    out[keep] = '.';
    out[keep + 1u] = '.';
    memcpy(out + keep + 2u, text + (len - keep), keep);
    out[keep + 2u + keep] = '\0';
    return out;
}

/* Python's str.upper().startswith(prefix.upper()) over ASCII. */
static bool starts_with_ci(const char *s, const char *prefix)
{
    size_t i;

    for (i = 0u; prefix[i] != '\0'; i++) {
        if (toupper((unsigned char)s[i]) != toupper((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

bool nd_modemapp_first_content(const char *const *lines, size_t n_lines, const char *prefix,
                               char *out, size_t out_sz)
{
    size_t i;

    if (out == NULL || out_sz == 0u)
        return false;
    out[0] = '\0';
    if (lines == NULL || prefix == NULL)
        return false;

    for (i = 0u; i < n_lines; i++) {
        char buf[ND_MODEM_LINE_TEXT_MAX];
        const char *colon;

        if (lines[i] == NULL)
            continue;
        (void)nd_strlcpy(buf, lines[i], sizeof buf);
        nd_modemapp_strip(buf);
        if (buf[0] == '\0')
            continue;

        if (starts_with_ci(buf, prefix)) {
            /* line.split(":", 1)[1] -- the FIRST colon, which for a
             * "+CPIN: READY" is the tag's own. A line that matched the prefix
             * and has no colon at all cannot happen (every prefix here ends
             * in one), but Python would raise IndexError, so answer "" and
             * carry on rather than inventing a value. */
            colon = strchr(buf, ':');
            if (colon == NULL)
                return false;
            (void)nd_strlcpy(out, colon + 1, out_sz);
            nd_modemapp_strip(out);
            return true;
        }
        (void)nd_strlcpy(out, buf, out_sz);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * Files and directories
 * ------------------------------------------------------------------ */

bool nd_modemapp_read_file(const char *path, char *out, size_t out_sz)
{
    char real[ND_PATH_MAX];
    FILE *f;
    size_t got;

    if (out == NULL || out_sz == 0u)
        return false;
    out[0] = '\0';
    if (path == NULL || nd_path_resolve(real, sizeof real, path) != ND_OK)
        return false;

    f = fopen(real, "rb");
    if (f == NULL)
        return false;
    got = fread(out, 1u, out_sz - 1u, f);
    out[got] = '\0';
    (void)fclose(f);
    /* A NUL inside the file truncates the string, which is what Python's
     * open() in text mode would NOT do -- but the four files read here are
     * plain text and a binary one is a broken phone either way. */
    nd_modemapp_strip(out);
    return true;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* sorted(os.listdir(path)). Python sorts strings by code point; strcmp sorts
 * by unsigned byte, and the two agree for ASCII, which every name in /dev and
 * /sys/class/net is. */
static bool listdir_sorted(const char *path, probe_names *out)
{
    char real[ND_PATH_MAX];
    DIR *d;
    struct dirent *e;

    out->n = 0u;
    if (nd_path_resolve(real, sizeof real, path) != ND_OK)
        return false;
    d = opendir(real);
    if (d == NULL)
        return false;
    while ((e = readdir(d)) != NULL && out->n < PROBE_MAX_ENTRIES) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (strlen(e->d_name) >= PROBE_NAME_MAX)
            continue;
        (void)nd_strlcpy(out->name[out->n], e->d_name, PROBE_NAME_MAX);
        out->n++;
    }
    (void)closedir(d);
    qsort(out->name, out->n, PROBE_NAME_MAX, name_cmp);
    return true;
}

size_t nd_modemapp_ttyusb_list(char *out, size_t out_sz)
{
    /* 512 * 64 = 32 KB, too much for a stack this deep inside an app. One
     * static copy, and this app is single-threaded by construction. */
    static probe_names names;
    size_t i;
    size_t n = 0u;

    if (out == NULL || out_sz == 0u)
        return 0u;
    out[0] = '\0';
    if (!listdir_sorted(ND_MODEMAPP_DEV_DIR, &names))
        return 0u;

    for (i = 0u; i < names.n; i++) {
        if (strncmp(names.name[i], "ttyUSB", 6u) != 0)
            continue;
        if (n > 0u)
            (void)nd_strlcat(out, ",", out_sz);
        (void)nd_strlcat(out, names.name[i], out_sz);
        n++;
    }
    return n;
}

/* os.path.basename(os.path.realpath(p)). realpath() fails on a path that does
 * not exist; Python's os.path.realpath returns it unchanged, so a failure
 * falls back to the input, which is what makes a broken symlink compare
 * unequal to "qmi_wwan" instead of aborting the scan. */
static void driver_basename(const char *sys_path, char *out, size_t out_sz)
{
    char real[ND_PATH_MAX];
    char *resolved;
    const char *base;

    out[0] = '\0';
    if (nd_path_resolve(real, sizeof real, sys_path) != ND_OK)
        return;

    resolved = realpath(real, NULL); /* owned here; freed below */
    base = resolved != NULL ? resolved : real;
    {
        const char *slash = strrchr(base, '/');

        if (slash != NULL && slash[1] != '\0')
            base = slash + 1;
    }
    (void)nd_strlcpy(out, base, out_sz);
    free(resolved);
}

bool nd_modemapp_wwan_interface(char *out, size_t out_sz)
{
    static probe_names names;
    static const char *const PREFIXES[] = {"ww", "rmnet", "usb"};
    size_t i;
    size_t p;

    if (out == NULL || out_sz == 0u)
        return false;
    out[0] = '\0';
    if (!listdir_sorted(ND_MODEMAPP_NET_DIR, &names))
        return false;

    /* "eudev predictable naming renames wwan0 to wwp<path>; trust the bound
     * driver over the name." */
    for (i = 0u; i < names.n; i++) {
        char link[ND_PATH_MAX];
        char base[PROBE_NAME_MAX];

        if (nd_snprintf(link, sizeof link, "%s/%s/device/driver", ND_MODEMAPP_NET_DIR,
                        names.name[i]) != ND_OK)
            continue;
        driver_basename(link, base, sizeof base);
        if (strcmp(base, ND_MODEMAPP_QMI_DRIVER) == 0) {
            (void)nd_strlcpy(out, names.name[i], out_sz);
            return true;
        }
    }

    /* The fallbacks are tried PREFIX-major: every "ww*" before any "rmnet*",
     * not the first name that matches any prefix. */
    for (p = 0u; p < ND_ARRAY_LEN(PREFIXES); p++) {
        size_t len = strlen(PREFIXES[p]);

        for (i = 0u; i < names.n; i++) {
            if (strncmp(names.name[i], PREFIXES[p], len) == 0) {
                (void)nd_strlcpy(out, names.name[i], out_sz);
                return true;
            }
        }
    }
    return false;
}

bool nd_modemapp_iface_up(const char *name)
{
    char path[ND_PATH_MAX];
    char text[64];
    char *end = NULL;
    unsigned long flags;

    if (name == NULL || name[0] == '\0')
        return false;
    if (nd_snprintf(path, sizeof path, "%s/%s/flags", ND_MODEMAPP_NET_DIR, name) != ND_OK)
        return false;
    if (!nd_modemapp_read_file(path, text, sizeof text))
        return false;

    /* int(flags, 16). The file is "0x1003"; base 16 accepts the 0x prefix in
     * both languages. Trailing junk is Python's ValueError, i.e. False. */
    errno = 0;
    flags = strtoul(text, &end, 16);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    return (flags & 1uL) != 0uL;
}

bool nd_modemapp_global_ipv6(const char *ifname, char *out, size_t out_sz)
{
    char real[ND_PATH_MAX];
    FILE *f;
    char line[256];
    bool found = false;

    if (out == NULL || out_sz == 0u)
        return false;
    out[0] = '\0';
    if (ifname == NULL || ifname[0] == '\0')
        return false;
    if (nd_path_resolve(real, sizeof real, ND_MODEMAPP_IF_INET6) != ND_OK)
        return false;
    f = fopen(real, "r");
    if (f == NULL)
        return false;

    while (!found && fgets(line, (int)sizeof line, f) != NULL) {
        char *save = NULL;
        char *field[6];
        size_t i;
        uint8_t addr[16];
        char text[INET6_ADDRSTRLEN];

        for (i = 0u; i < 6u; i++) {
            field[i] = strtok_r(i == 0u ? line : NULL, " \t\n", &save);
            if (field[i] == NULL)
                break;
        }
        /* len(fields) >= 6 and fields[5] == ifname and fields[3] == "00" --
         * scope 0 is global. */
        if (i < 6u)
            continue;
        if (strcmp(field[5], ifname) != 0 || strcmp(field[3], "00") != 0)
            continue;
        if (strlen(field[0]) != 32u)
            continue;

        for (i = 0u; i < 16u; i++) {
            char byte[3] = {field[0][i * 2u], field[0][i * 2u + 1u], '\0'};
            char *end = NULL;
            unsigned long v = strtoul(byte, &end, 16);

            if (end != byte + 2)
                break;
            addr[i] = (uint8_t)v;
        }
        if (i < 16u)
            continue;
        if (inet_ntop(AF_INET6, addr, text, sizeof text) == NULL)
            continue;
        (void)nd_strlcpy(out, text, out_sz);
        found = true;
    }
    (void)fclose(f);
    return found;
}

/* ------------------------------------------------------------------ *
 * The two configuration rows
 * ------------------------------------------------------------------ */

/* Python's str.strip('"'): every leading and trailing quote, not just one. */
static void strip_quotes(char *s)
{
    size_t len = strlen(s);
    size_t start = 0u;

    while (len > start && s[len - 1u] == '"')
        len--;
    while (start < len && s[start] == '"')
        start++;
    memmove(s, s + start, len - start);
    s[len - start] = '\0';
}

/* One line of a file whose lines the caller wants to walk. Python's
 * str.splitlines() also breaks on \v, \f, \x1c-\x1e and \x85; the C splits on
 * \n and \r\n only, which is the same call P-3 already recorded for the
 * settings parser. No file here uses anything else. */
static char *next_line(char **cursor)
{
    char *start = *cursor;
    char *nl;

    if (start == NULL || *start == '\0')
        return NULL;
    nl = strchr(start, '\n');
    if (nl != NULL) {
        *nl = '\0';
        *cursor = nl + 1;
    } else {
        *cursor = start + strlen(start);
    }
    {
        size_t len = strlen(start);

        if (len > 0u && start[len - 1u] == '\r')
            start[len - 1u] = '\0';
    }
    return start;
}

void nd_modemapp_configured_apn(char *out, size_t out_sz)
{
    char content[2048];
    char *cursor = content;
    char *line;

    if (out == NULL || out_sz == 0u)
        return;
    (void)nd_strlcpy(out, ND_MODEMAPP_DEFAULT_APN, out_sz);
    if (!nd_modemapp_read_file(ND_MODEMAPP_DEFAULTS_FILE, content, sizeof content))
        return;

    while ((line = next_line(&cursor)) != NULL) {
        char value[256];

        nd_modemapp_strip(line);
        if (strncmp(line, "MODEM_APN=", 10u) != 0)
            continue;
        (void)nd_strlcpy(value, line + 10, sizeof value);
        nd_modemapp_strip(value);
        strip_quotes(value);
        /* `... or DEFAULT_APN`: an empty MODEM_APN= falls back rather than
         * showing a blank row. */
        if (value[0] != '\0')
            (void)nd_strlcpy(out, value, out_sz);
        return;
    }
}

void nd_modemapp_dns_row(char *out, size_t out_sz)
{
    char content[4096];
    char first[128];
    char *cursor = content;
    char *line;
    bool have_first = false;

    if (out == NULL || out_sz == 0u)
        return;
    (void)nd_strlcpy(out, "--", out_sz);
    first[0] = '\0';
    if (!nd_modemapp_read_file(ND_MODEMAPP_RESOLV_FILE, content, sizeof content))
        return;

    while ((line = next_line(&cursor)) != NULL) {
        char *save = NULL;
        const char *keyword;
        const char *server;

        /* `ln.startswith("nameserver")` is tested on the RAW line, before any
         * strip, so an indented nameserver line is ignored. Kept. */
        if (strncmp(line, "nameserver", 10u) != 0)
            continue;
        keyword = strtok_r(line, " \t\r", &save);
        if (keyword == NULL)
            continue;
        server = strtok_r(NULL, " \t\r", &save);
        if (server == NULL)
            continue; /* len(ln.split()) > 1 */
        if (!have_first) {
            (void)nd_strlcpy(first, server, sizeof first);
            have_first = true;
        }
        /* "prefer the DNS64/IPv6 entry" -- the first one containing a colon
         * wins outright, wherever it is in the file. */
        if (strchr(server, ':') != NULL) {
            (void)nd_strlcpy(out, server, out_sz);
            return;
        }
    }
    if (have_first)
        (void)nd_strlcpy(out, first, out_sz);
}
