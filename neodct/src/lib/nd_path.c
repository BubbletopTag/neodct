/* nd_path.c -- the ND_ROOT prefix hook, and the handful of path questions
 * everything else asks.
 *
 * The runtime paths in nd_paths.h are absolute and stay absolute: AGENTS.md
 * is explicit about it, half the shell scripts in the image hard-code them,
 * and the initramfs applier is one of those scripts.
 *
 * The host tests obviously cannot write to /NeoDCT. So every path that is
 * OPENED passes through nd_path_resolve(), which prepends the NEODCT_ROOT
 * environment variable. In production that variable is unset and the whole
 * mechanism costs one comparison against '\0'.
 *
 * This is introduced at the very start of the port on purpose. Retrofitting it
 * across roughly seventy call sites later is a day of tedious, error-prone
 * work, and every site missed is a test that quietly writes to the real
 * filesystem.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nd_paths.h"

/* Resolved once. Changing NEODCT_ROOT after the first path operation has no
 * effect, which is deliberate: a test that moves the root half way through a
 * run would leave files in two places. Tests call nd_path_set_root(). */
static bool g_root_resolved;
static char g_root[ND_PATH_MAX];

const char *nd_path_root(void)
{
    if (!g_root_resolved) {
        const char *env = getenv(ND_ENV_ROOT);

        if (env != NULL && env[0] != '\0') {
            size_t len = strlen(env);

            /* A trailing slash would produce "//NeoDCT/..." -- harmless on
             * Linux, but it makes every logged path look wrong. */
            while (len > 0u && env[len - 1u] == '/')
                len--;
            if (len >= sizeof g_root)
                len = sizeof g_root - 1u;
            memcpy(g_root, env, len);
            g_root[len] = '\0';
        } else {
            g_root[0] = '\0';
        }
        g_root_resolved = true;
    }
    return g_root;
}

nd_err nd_path_set_root(const char *root)
{
    size_t len;

    if (root == NULL || root[0] == '\0') {
        g_root[0] = '\0';
        g_root_resolved = true;
        return ND_OK;
    }

    len = strlen(root);
    while (len > 0u && root[len - 1u] == '/')
        len--;
    if (len >= sizeof g_root)
        return ND_ERR_TOOLONG;

    memcpy(g_root, root, len);
    g_root[len] = '\0';
    g_root_resolved = true;
    return ND_OK;
}

nd_err nd_path_resolve(char *out, size_t out_sz, const char *path)
{
    const char *root;
    int n;

    if (out == NULL || out_sz == 0u || path == NULL)
        return ND_ERR_INVAL;

    root = nd_path_root();

    /* A relative path is left alone. Only the absolute /NeoDCT-style paths are
     * ours to redirect; a relative one came from the command line or a test
     * fixture and means what it says. */
    if (root[0] == '\0' || path[0] != '/')
        n = snprintf(out, out_sz, "%s", path);
    else
        n = snprintf(out, out_sz, "%s%s", root, path);

    if (n < 0 || (size_t)n >= out_sz)
        return ND_ERR_TOOLONG;

    return ND_OK;
}

nd_err nd_path_join(char *out, size_t out_sz, const char *dir, const char *child)
{
    char joined[ND_PATH_MAX];
    int n;

    if (dir == NULL || child == NULL)
        return ND_ERR_INVAL;

    n = snprintf(joined, sizeof joined, "%s/%s", dir, child);
    if (n < 0 || (size_t)n >= sizeof joined)
        return ND_ERR_TOOLONG;

    return nd_path_resolve(out, out_sz, joined);
}

nd_err nd_mkdir_p(const char *path, unsigned int mode)
{
    char resolved[ND_PATH_MAX];
    nd_err rc;
    size_t i;
    size_t len;

    rc = nd_path_resolve(resolved, sizeof resolved, path);
    if (rc != ND_OK)
        return rc;

    len = strlen(resolved);
    if (len == 0u)
        return ND_ERR_INVAL;

    /* Walk the string creating each component, temporarily terminating at
     * each separator. Starting at 1 skips the leading '/' so we never try to
     * create the root itself. */
    for (i = 1u; i <= len; i++) {
        char saved;

        if (i < len && resolved[i] != '/')
            continue;

        saved = resolved[i];
        resolved[i] = '\0';

        if (mkdir(resolved, (mode_t)mode) != 0 && errno != EEXIST) {
            resolved[i] = saved;
            return ND_ERR_IO;
        }

        resolved[i] = saved;
    }

    return ND_OK;
}

static bool stat_resolved(const char *path, struct stat *st)
{
    char resolved[ND_PATH_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;

    return stat(resolved, st) == 0;
}

bool nd_path_exists(const char *path)
{
    struct stat st;

    return stat_resolved(path, &st);
}

bool nd_path_is_dir(const char *path)
{
    struct stat st;

    return stat_resolved(path, &st) && S_ISDIR(st.st_mode);
}

bool nd_path_is_file(const char *path)
{
    struct stat st;

    return stat_resolved(path, &st) && S_ISREG(st.st_mode);
}

/* ============ ROOT'S FILES IN SOMEBODY ELSE'S DIRECTORY ============
 *
 * nd-core is root from exec until core/nd_main.c step 4b, and in that window
 * it can create files under /NeoDCT/User -- a partition S00userdata has
 * already handed to ndusr, under run_neodct.sh's umask of 0027. The file
 * comes out root:root 0640. The moment nd-core becomes ndusr it cannot read
 * a file it wrote a second ago, and nothing fixes that until the NEXT boot,
 * when S00userdata's ownership pass chowns it.
 *
 * nd_settings.c met this first, with settings.prop, and answered it by
 * having root decline to write (root_would_orphan_the_file). That is the
 * right answer when the write is optional. The first-boot keypad wizard's
 * keymap is not optional -- writing it is the wizard's entire job, and it
 * has to run as root to probe the bus -- so this is the other answer: write
 * the file, then give it to whoever owns the directory, because that is who
 * is going to need it.
 *
 * The condition is deliberately not `geteuid() == 0` alone. On an image
 * built without the users table there is no ndusr, nd-core stays root for
 * its whole life, the partition is root's, and root's file in root's
 * directory is exactly right. The question asked is the same one
 * nd_settings.c asks: does somebody other than me own the place this file
 * is in? Only then is the file theirs. */
bool nd_path_give_to_dir_owner(const char *path)
{
    char resolved[ND_PATH_MAX];
    struct stat dir_st;
    struct stat file_st;
    char *slash;

    if (path == NULL)
        return false;
    if (geteuid() != 0u)
        return true; /* not root: the file is already its writer's */

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;
    if (stat(resolved, &file_st) != 0)
        return false;

    /* The directory, resolved the same way the file was. A bare name lives in
     * the working directory and says nothing about ownership; leave it. */
    slash = strrchr(resolved, '/');
    if (slash == NULL)
        return true;
    if (slash == resolved) {
        if (stat("/", &dir_st) != 0)
            return false;
    } else {
        *slash = '\0';
        if (stat(resolved, &dir_st) != 0)
            return false;
        *slash = '/';
    }

    if (dir_st.st_uid == 0u)
        return true; /* root's own directory: root's file is correct there */
    if (file_st.st_uid == dir_st.st_uid && file_st.st_gid == dir_st.st_gid)
        return true;

    return chown(resolved, dir_st.st_uid, dir_st.st_gid) == 0;
}

/* ============ AND THE SAME QUESTION WHERE THE NAME CANNOT BE TRUSTED =====
 *
 * nd_paths.h has the reasoning. What is here is the mechanics, and there are
 * only two of them that matter.
 *
 * The FILE is reached through a descriptor and never again through its name.
 * fstat(2), fchown(2) and fchmod(2) all act on the open object, so a name
 * that means something else by the time the second syscall runs changes
 * nothing: the inode that was checked is the inode that is changed. This is
 * the whole fix, and it is why the caller in ftp.c holds the descriptor it
 * created rather than reopening the path it wrote to.
 *
 * The DIRECTORY is still reached by name, and that is deliberate rather than
 * an oversight. Its owner is the answer being copied down, and the
 * directories in question -- the card root's untrusted/, music/, an app's
 * games/ -- are created by root or by ndusr in a card root that is 0751
 * ndusr:ndusr. An untrusted process cannot make a name there, so there is
 * nothing to plant. Asking through openat(O_DIRECTORY|O_NOFOLLOW) instead
 * would buy nothing and would cost a caller that only has a name a second
 * descriptor to get wrong. */
static nd_err dir_owner_of(const char *path, struct stat *dir_st)
{
    char resolved[ND_PATH_MAX];
    char *slash;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return ND_ERR_TOOLONG;

    /* A bare name lives in the working directory and says nothing about
     * ownership -- the same answer nd_path_give_to_dir_owner() gives. */
    slash = strrchr(resolved, '/');
    if (slash == NULL)
        return ND_ERR_NOTFOUND;
    if (slash == resolved)
        return (stat("/", dir_st) == 0) ? ND_OK : ND_ERR_IO;
    *slash = '\0';
    return (stat(resolved, dir_st) == 0) ? ND_OK : ND_ERR_IO;
}

nd_err nd_path_give_fd_to_dir_owner(int fd, const char *path, unsigned int mode)
{
    struct stat file_st;
    struct stat dir_st;
    bool have_dir;
    nd_err rc;

    if (fd < 0 || path == NULL)
        return ND_ERR_INVAL;
    if (fstat(fd, &file_st) != 0)
        return ND_ERR_IO;
    /* Not a plain file, or not the only link to it. The link count is the
     * cheap half of the hard-link case nd_paths.h names: this image sets no
     * fs.protected_hardlinks, so a process that can write the directory can
     * keep a second name for the file somewhere it owns and still hold it
     * after root has changed the owner. One link is what a download has. */
    if (!S_ISREG(file_st.st_mode) || file_st.st_nlink != 1u)
        return ND_ERR_PERM;

    /* ND_ERR_NOTFOUND here means the path carries no directory at all -- a
     * bare name, in whatever the working directory is, which says nothing
     * about who should own the file. The mode still stands; the owner has
     * nobody to be copied from. */
    rc = dir_owner_of(path, &dir_st);
    have_dir = (rc == ND_OK);
    if (rc != ND_OK && rc != ND_ERR_NOTFOUND)
        return rc;

    /* Ownership first, mode second, for the reason fetch_give_to_reader()
     * states: the halfway state after a failed chmod is still a file its
     * reader owns, and the one after a failed chown is a file nobody can
     * open. Skipped entirely when the caller is not root (it cannot give a
     * file away and does not need to -- the file is already its writer's) or
     * when the directory is root's own, which is an image with no ndusr. */
    if (have_dir && geteuid() == 0u && dir_st.st_uid != 0u &&
        (file_st.st_uid != dir_st.st_uid || file_st.st_gid != dir_st.st_gid)) {
        if (fchown(fd, dir_st.st_uid, dir_st.st_gid) != 0)
            return ND_ERR_IO;
    }
    if (mode != ND_PATH_MODE_KEEP && fchmod(fd, (mode_t)mode) != 0)
        return ND_ERR_IO;
    return ND_OK;
}

nd_err nd_path_give_to_dir_owner_nofollow(const char *path, unsigned int mode)
{
    char resolved[ND_PATH_MAX];
    nd_err rc;
    int fd;

    if (path == NULL)
        return ND_ERR_INVAL;
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return ND_ERR_TOOLONG;

    /* O_NOFOLLOW is the point: a symlink here is ELOOP rather than a root
     * fchmod of whatever it names. O_NONBLOCK is for the other thing that
     * can be planted under a name -- a fifo, whose open would otherwise wait
     * for a writer that is never coming, with the UI thread inside it.
     * O_RDONLY because nothing is written through this descriptor and root
     * opening a file for writing is a habit worth not having. */
    fd = open(resolved, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ELOOP)
            return ND_ERR_PERM; /* a symlink was planted; say so, do nothing */
        return (errno == ENOENT) ? ND_ERR_NOTFOUND : ND_ERR_IO;
    }
    rc = nd_path_give_fd_to_dir_owner(fd, path, mode);
    (void)close(fd);
    return rc;
}

/* ------------------------------------------------------------------ *
 * The installed-apps generation counter -- see ND_PATH_APPGEN
 * ------------------------------------------------------------------ */

unsigned long nd_appgen_value(void)
{
    char resolved[ND_PATH_MAX];
    unsigned long value = 0ul;
    FILE *f;

    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_APPGEN) != ND_OK)
        return 0ul;
    f = fopen(resolved, "rb");
    if (f == NULL)
        return 0ul;
    if (fscanf(f, "%lu", &value) != 1)
        value = 0ul;
    (void)fclose(f);
    return value;
}

bool nd_appgen_bump(void)
{
    char resolved[ND_PATH_MAX];
    unsigned long value = nd_appgen_value();
    FILE *f;
    bool ok;

    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_APPGEN) != ND_OK)
        return false;
    f = fopen(resolved, "wb");
    if (f == NULL)
        return false;
    ok = fprintf(f, "%lu\n", value + 1ul) > 0;
    /* fclose can fail where fprintf did not; a counter half-written is a
     * counter that may read back as something else entirely. */
    if (fclose(f) != 0)
        ok = false;
    return ok;
}
