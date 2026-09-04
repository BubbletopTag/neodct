/* platform_test.h -- the three things every platform-services test needs.
 *
 * No framework: a check macro, a per-case scratch root, and a file writer.
 *
 * ============ HOW THE SCRATCH ROOT WORKS ============
 *
 * Every path in the system is opened through nd_path_resolve(), which
 * prepends ND_ROOT. So a test does not need a fake filesystem or a patched
 * open(): it points ND_ROOT at a fresh empty directory and then talks about
 * "/settings.prop" as if it were an absolute runtime path. That is the direct
 * C equivalent of uistub.py's PathRemap, and it is why the modules under test
 * need no test-only branches.
 *
 * A fresh directory PER CASE, not per binary, because several of these tests
 * assert on whether a file exists and would otherwise see the previous case's
 * leftovers.
 */

#ifndef PLATFORM_TEST_H_INCLUDED
#define PLATFORM_TEST_H_INCLUDED

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_paths.h"
#include "nd_types.h"

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                         \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

#define CHECK_STR(got, want)                                                            \
    do {                                                                                \
        const char *g_ = (got);                                                         \
        const char *w_ = (want);                                                        \
        g_checks++;                                                                     \
        if (g_ == NULL || strcmp(g_, w_) != 0) {                                        \
            g_failures++;                                                               \
            fprintf(stderr, "FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
                    g_ != NULL ? g_ : "(null)", w_);                                    \
        }                                                                               \
    } while (0)

#define CHECK_INT(got, want)                                                                 \
    do {                                                                                     \
        long long g_ = (long long)(got);                                                     \
        long long w_ = (long long)(want);                                                    \
        g_checks++;                                                                          \
        if (g_ != w_) {                                                                      \
            g_failures++;                                                                    \
            fprintf(stderr, "FAIL %s:%d  got %lld want %lld\n", __FILE__, __LINE__, g_, w_); \
        }                                                                                    \
    } while (0)

#define RUN(fn)        \
    do {               \
        pt_new_case(); \
        fn();          \
    } while (0)

static char g_case_root[ND_PATH_MAX];
static char g_roots[128][ND_PATH_MAX];
static size_t g_root_count;

ND_UNUSED_FN static int pt_unlink_cb(const char *path, const struct stat *st, int flag,
                                     struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

/* Fresh, empty, and pointed at by ND_ROOT. */
ND_UNUSED_FN static void pt_new_case(void)
{
    const char *base = getenv(ND_ENV_ROOT);
    char tmpl[ND_PATH_MAX];

    if (base == NULL || base[0] == '\0')
        base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0')
        base = "/tmp";

    if (snprintf(tmpl, sizeof tmpl, "%s/ndcase-XXXXXX", base) < 0) {
        fprintf(stderr, "cannot build a scratch template\n");
        exit(1);
    }
    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp under %s: %s\n", base, strerror(errno));
        exit(1);
    }

    /* mkdtemp gives 0700, and 0700 is wrong for a fixture that code under
     * test is going to drop privilege into.
     *
     * apps/Browser spawns netsurf as ndusr_ut. Against this fixture the
     * "netsurf" it spawns is a script UNDER THIS DIRECTORY, so a 0700 case
     * root means the dropped child cannot resolve the path to the program it
     * was told to run -- and the test then observes a browser that produced
     * no output, which is indistinguishable from every kind of real bug.
     *
     * 0711 is the same shape as the phone: / and /NeoDCT/System are
     * traversable by everyone, which is exactly how netsurf reaches
     * /usr/bin/netsurf-fb there. It grants traversal and not listing, so the
     * fixture stays as private as 0700 made it to anything that does not
     * already know a name inside it. */
    if (chmod(tmpl, 0711) != 0) {
        fprintf(stderr, "chmod 0711 %s: %s\n", tmpl, strerror(errno));
        exit(1);
    }

    /* And give it to ndusr, where there is one and we are root.
     *
     * Traversal alone is not enough. The case root stands in for the phone's
     * WRITABLE storage -- fixtures put a fake aplay's output, a settings file,
     * a database in it -- and on a phone that storage belongs to ndusr,
     * because S00userdata hands /NeoDCT/User over on the first boot. A
     * root-owned 0711 fixture lets a dropped child walk in and write nothing,
     * which is not a state any phone is ever in.
     *
     * Best-effort, and the two conditions line up exactly: it needs root to
     * chown, and without root nothing drops privilege in the first place. */
    {
        struct passwd *pw = geteuid() == 0u ? getpwnam("ndusr") : NULL;

        if (pw != NULL && chown(tmpl, pw->pw_uid, pw->pw_gid) != 0) {
            fprintf(stderr, "chown %s to ndusr: %s\n", tmpl, strerror(errno));
            exit(1);
        }
    }

    (void)nd_strlcpy(g_case_root, tmpl, sizeof g_case_root);
    if (g_root_count < ND_ARRAY_LEN(g_roots))
        (void)nd_strlcpy(g_roots[g_root_count++], tmpl, ND_PATH_MAX);

    if (nd_path_set_root(tmpl) != ND_OK) {
        fprintf(stderr, "nd_path_set_root failed\n");
        exit(1);
    }
}

/* Called from main() on the way out: CODING-STANDARDS.md section 1.7 wants a
 * clean teardown so a leak detector's output stays worth reading, and that
 * applies to the filesystem too. */
ND_UNUSED_FN static void pt_cleanup(void)
{
    size_t i;

    for (i = 0u; i < g_root_count; i++)
        (void)nftw(g_roots[i], pt_unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
    (void)nd_path_set_root(NULL);
}

/* Writes bytes at a VIRTUAL path (i.e. under the case root), creating parents.
 * Deliberately not going through nd_props_write_atomic: a test that builds its
 * fixture with the code under test cannot catch that code being wrong. */
ND_UNUSED_FN static void pt_write(const char *path, const void *data, size_t len)
{
    char resolved[ND_PATH_MAX];
    const char *slash = strrchr(path, '/');
    FILE *f;

    if (slash != NULL && slash != path) {
        char dir[ND_PATH_MAX];

        (void)nd_strlcpy(dir, path, (size_t)(slash - path) + 1u);
        if (nd_mkdir_p(dir, 0755u) != ND_OK) {
            fprintf(stderr, "mkdir -p %s failed\n", dir);
            exit(1);
        }
    }
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        fprintf(stderr, "resolve %s failed\n", path);
        exit(1);
    }
    f = fopen(resolved, "wb");
    if (f == NULL) {
        fprintf(stderr, "open %s: %s\n", resolved, strerror(errno));
        exit(1);
    }
    if (len > 0u && fwrite(data, 1u, len, f) != len) {
        fprintf(stderr, "write %s failed\n", resolved);
        exit(1);
    }
    (void)fclose(f);
}

ND_UNUSED_FN static void pt_write_text(const char *path, const char *text)
{
    pt_write(path, text, strlen(text));
}

/* Reads a virtual path into a caller buffer, NUL-terminated. Returns the
 * length, or (size_t)-1 when the file is not there. */
ND_UNUSED_FN static size_t pt_read_text(const char *path, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    size_t n;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return (size_t)-1;
    f = fopen(resolved, "rb");
    if (f == NULL)
        return (size_t)-1;
    n = fread(out, 1u, out_sz - 1u, f);
    out[n] = '\0';
    (void)fclose(f);
    return n;
}

ND_UNUSED_FN static void pt_mkdir(const char *path)
{
    if (nd_mkdir_p(path, 0755u) != ND_OK) {
        fprintf(stderr, "mkdir -p %s failed\n", path);
        exit(1);
    }
}

ND_UNUSED_FN static int pt_report(const char *name)
{
    pt_cleanup();
    if (g_failures != 0) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", name, g_failures, g_checks);
        return 1;
    }
    printf("%s: %d checks passed\n", name, g_checks);
    return 0;
}

#endif /* PLATFORM_TEST_H_INCLUDED */
