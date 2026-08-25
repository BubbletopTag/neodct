/* apps/Update/staging.c -- the pending-update record, and the note the
 * applier leaves behind.
 *
 * A port of the three functions of System/core/UpdateService/staging.py that
 * the Update app calls: stage_package(), read_result() and clear_result().
 * The other seven (stage, read_pending, clear_pending, note_attempt,
 * record_installed, read_installed, record_result) belong to the boot-time
 * applier and to the recovery script, neither of which is C, so they are not
 * here. update_app.h says why this file exists at all rather than being
 * declared missing with the rest of UpdateService.
 *
 * ============ THIS IS A WIRE FORMAT WITH A SHELL SCRIPT ============
 *
 * neodct/initramfs/ndsys-apply.sh runs in busybox ash before any of this code
 * exists, reads pending.prop with
 *
 *     sed -n "s/^$1=//p" "$2" | head -n1
 *
 * and NEVER sources it, so a changelog full of backticks cannot execute. Two
 * consequences the writer below has to honour:
 *
 *   - one line per key, KEY=value, no quoting and no escaping. A value
 *     containing a newline would silently become two records, so it is
 *     refused rather than written. staging.py raises ValueError for the same
 *     reason.
 *   - the key spellings are the shell script's. image_bytes, verity_root_hash
 *     and the rest are grepped for literally by ndsys-apply.sh:apply_pending
 *     and verity_table. Renaming one here breaks the boot, not the build.
 *
 * ============ THE ORDERING IS THE WHOLE POINT ============
 *
 * staging.py's own header: "The image lands on disk first and the record file
 * is written last with an atomic rename, so the applier can only ever see a
 * pending update whose image is already complete."
 *
 * stage_package() has no image to land -- nothing is copied, the applier
 * reads the .ndsw where it already sits on the card -- so what is left is the
 * removal order. The record goes FIRST and the stale pending.img second, so a
 * power cut between the two reads as "nothing pending" and never as "pending,
 * image missing". Then the new record is written temp-file-fsync-rename, and
 * the directory is fsynced so the rename itself survives a power cut.
 *
 * ============ WHY NOT nd_props ============
 *
 * nd_props.h has three prop dialects and a writer, and this is a fourth. The
 * differences are small and all of them are observable:
 *
 *   - _read_record strips the LINE and then splits on the first '=', keeping
 *     the value exactly as it stands. All three nd_props dialects strip the
 *     value too, so "version= 1.0" parses differently.
 *   - _write_record writes to path + ".new"; nd_props_write_atomic writes to
 *     path + ".tmp". The applier's own putprop_file() also uses ".new", and
 *     an interrupted stage leaves a file the applier's glob-free reads ignore
 *     either way -- but the name is part of what a person debugging a phone
 *     over the serial console expects to find.
 *   - _write_record FSYNCS THE PARENT DIRECTORY. nd_props.h is explicit that
 *     its writer does not, and asks callers not to add it there.
 *
 * So the forty lines are written out rather than bent to fit.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"

#include "update_app.h"

/* ------------------------------------------------------------------ *
 * Reading
 * ------------------------------------------------------------------ */

/* Python's str.strip() over the ASCII whitespace a prop file can contain.
 * Trims in place and returns the first non-blank byte. */
static char *strip(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' || *s == '\f')
        s++;
    end = s + strlen(s);
    while (end > s) {
        char c = end[-1];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f')
            break;
        end--;
    }
    *end = '\0';
    return s;
}

/* staging._read_record(path). NULL/absent/unreadable all read as "no record",
 * which the Python spells as `return None` and the caller tests with
 * `if not values`. An empty map reads the same way, which is why the count is
 * what this returns rather than a found/not-found flag. */
static bool read_record(const char *path, nd_upd_record *out)
{
    char resolved[ND_PATH_MAX];
    char line[ND_UPDREC_KEY_MAX + ND_UPDREC_VALUE_MAX + 8];
    FILE *f;

    if (out == NULL)
        return false;
    memset(out, 0, sizeof *out);
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;

    f = fopen(resolved, "r");
    if (f == NULL)
        return false;

    while (fgets(line, (int)sizeof line, f) != NULL) {
        char *text = strip(line);
        char *eq;
        char *key;
        size_t i;

        /* `if not line or line.startswith("#") or "=" not in line: continue` */
        if (text[0] == '\0' || text[0] == '#')
            continue;
        eq = strchr(text, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        /* key.strip(); the value is taken AS IT STANDS after the split. */
        key = strip(text);
        if (key[0] == '\0')
            continue;

        /* "last duplicate wins", which a dict assignment gives for free. */
        for (i = 0u; i < out->n; i++) {
            if (strcmp(out->keys[i], key) == 0)
                break;
        }
        if (i == out->n) {
            if (out->n >= ND_UPDREC_MAX_KEYS)
                continue;
            (void)nd_strlcpy(out->keys[out->n], key, ND_UPDREC_KEY_MAX);
            out->n++;
        }
        (void)nd_strlcpy(out->values[i], eq + 1, ND_UPDREC_VALUE_MAX);
    }
    (void)fclose(f);
    return out->n > 0u;
}

const char *nd_upd_record_get(const nd_upd_record *rec, const char *key, const char *dflt)
{
    size_t i;

    if (rec == NULL || key == NULL)
        return dflt;
    for (i = 0u; i < rec->n; i++) {
        if (strcmp(rec->keys[i], key) == 0)
            return rec->values[i];
    }
    return dflt;
}

bool nd_upd_read_result(nd_upd_record *out)
{
    return read_record(ND_UPDATE_RESULT_RECORD, out);
}

void nd_upd_clear_result(void)
{
    char resolved[ND_PATH_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, ND_UPDATE_RESULT_RECORD) != ND_OK)
        return;
    /* staging._unlink: a missing file is not an error. */
    (void)unlink(resolved);
}

/* ------------------------------------------------------------------ *
 * Writing
 * ------------------------------------------------------------------ */

/* staging._sync_dir(). "Make a rename durable -- otherwise a power cut can
 * lose the record." Every failure is swallowed, as the Python's is. */
static void sync_dir(const char *resolved_dir)
{
    int fd = open(resolved_dir, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return;
    (void)fsync(fd);
    (void)close(fd);
}

static void unlink_resolved(const char *path)
{
    char resolved[ND_PATH_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, path) == ND_OK)
        (void)unlink(resolved);
}

/* One KEY=value pair on its way into the record. */
typedef struct {
    const char *key;
    char value[ND_UPDREC_VALUE_MAX];
} pair;

static int pair_cmp(const void *a, const void *b)
{
    /* `for key in sorted(values)`: Python's sorted() is code-point order, and
     * these keys are ASCII, so strcmp is it. Never strcoll -- a locale must
     * not change the byte layout of a file the initramfs parses. */
    return strcmp(((const pair *)a)->key, ((const pair *)b)->key);
}

static nd_err set_pair(pair *p, const char *key, const char *fmt, ...) ND_PRINTF(3, 4);

static nd_err set_pair(pair *p, const char *key, const char *fmt, ...)
{
    va_list ap;
    int n;

    p->key = key;
    va_start(ap, fmt);
    n = vsnprintf(p->value, sizeof p->value, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof p->value)
        return ND_ERR_TOOLONG;
    /* "%s contains a newline: records are one line per key". The Python
     * raises ValueError; there is nothing to do with such a value but
     * refuse it, because the applier would read it as two records. */
    if (strpbrk(p->value, "\r\n") != NULL)
        return ND_ERR_INVAL;
    return ND_OK;
}

/* staging._write_record(path, values): temp file, fsync, rename, fsync dir. */
static nd_err write_record(const char *path, pair *pairs, size_t n)
{
    char resolved[ND_PATH_MAX];
    char temp[ND_PATH_MAX + 8];
    char dir[ND_PATH_MAX];
    char *slash;
    FILE *f;
    size_t i;

    qsort(pairs, n, sizeof pairs[0], pair_cmp);

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        errno = ENAMETOOLONG;
        return ND_ERR_TOOLONG;
    }
    (void)nd_strlcpy(dir, resolved, sizeof dir);
    slash = strrchr(dir, '/');
    if (slash == NULL) {
        errno = EINVAL;
        return ND_ERR_INVAL;
    }
    *slash = '\0';
    if (snprintf(temp, sizeof temp, "%s.new", resolved) < 0) {
        errno = ENAMETOOLONG;
        return ND_ERR_TOOLONG;
    }

    f = fopen(temp, "w");
    if (f == NULL)
        return ND_ERR_IO;
    for (i = 0u; i < n; i++) {
        if (fprintf(f, "%s=%s\n", pairs[i].key, pairs[i].value) < 0)
            goto io_failed;
    }
    if (fflush(f) != 0)
        goto io_failed;
    if (fsync(fileno(f)) != 0)
        goto io_failed;
    if (fclose(f) != 0) {
        (void)unlink(temp);
        return ND_ERR_IO;
    }

    /* os.replace: atomic, and it is what makes a half-written record
     * impossible for the applier to see. */
    if (rename(temp, resolved) != 0) {
        int saved = errno;

        (void)unlink(temp);
        errno = saved;
        return ND_ERR_IO;
    }
    sync_dir(dir);
    return ND_OK;

io_failed: {
    int saved = errno;

    (void)fclose(f);
    (void)unlink(temp);
    errno = saved;
    return ND_ERR_IO;
}
}

nd_err nd_upd_stage_package(const nd_upd_manifest *m, const char *package_path, int64_t image_bytes)
{
    pair pairs[11];
    char state_resolved[ND_PATH_MAX];
    const char *base;
    nd_err rc;
    size_t n = 0u;

    if (m == NULL || package_path == NULL) {
        errno = EINVAL;
        return ND_ERR_INVAL;
    }

    rc = nd_mkdir_p(ND_UPDATE_STATE_DIR, 0755u);
    if (rc != ND_OK)
        return rc;

    /* "Drop any earlier attempt, record first, so a crash between the two
     * cannot leave a record pointing at an image that has been deleted." */
    unlink_resolved(ND_UPDATE_PENDING_RECORD);
    unlink_resolved(ND_UPDATE_PENDING_IMAGE);

    /* os.path.basename: a basename and never a path. The card is mounted
     * somewhere different in the initramfs than it was here, so
     * ndsys-apply.sh:find_package() looks the file up by name. */
    base = strrchr(package_path, '/');
    base = (base != NULL) ? base + 1 : package_path;

    rc = set_pair(&pairs[n++], "package", "%s", base);
    /* The whole rootfs.squashfs member: the squashfs AND the verity tree
     * appended to it. Not the squashfs alone -- the sha256 below covers both,
     * and the applier hashes that many bytes back off the device afterwards. */
    if (rc == ND_OK)
        rc = set_pair(&pairs[n++], "image_bytes", "%lld", (long long)image_bytes);
    if (rc == ND_OK)
        rc = set_pair(&pairs[n++], "sha256", "%s", m->sha256);
    if (rc == ND_OK)
        rc = set_pair(&pairs[n++], "version", "%s", m->version);
    if (rc == ND_OK)
        rc = set_pair(&pairs[n++], "buildtime", "%lld", (long long)m->buildtime);
    if (rc == ND_OK)
        rc = set_pair(&pairs[n++], "platform", "%s", m->platform);
    if (rc == ND_OK)
        rc = set_pair(&pairs[n++], "verity_root_hash", "%s", m->verity_root_hash);
    if (rc == ND_OK)
        rc = set_pair(&pairs[n++], "verity_block_size", "%lld", (long long)m->verity_block_size);
    if (rc == ND_OK)
        rc =
            set_pair(&pairs[n++], "verity_image_blocks", "%lld", (long long)m->verity_image_blocks);
    if (rc == ND_OK)
        rc = set_pair(&pairs[n++], "verity_salt", "%s", m->verity_salt);
    /* attempts=0: ndsys-apply.sh counts up from here and gives up at 3. */
    if (rc == ND_OK)
        rc = set_pair(&pairs[n++], "attempts", "%d", 0);
    if (rc != ND_OK) {
        nd_log_err(ND_LOG_UPDATE, "staging record field is not writable: %s", nd_strerror(rc));
        errno = EINVAL;
        return rc;
    }

    rc = write_record(ND_UPDATE_PENDING_RECORD, pairs, n);
    if (rc != ND_OK)
        return rc;

    /* The Python syncs the directory a second time here, after
     * _write_record has already done it. Harmless, and kept. */
    if (nd_path_resolve(state_resolved, sizeof state_resolved, ND_UPDATE_STATE_DIR) == ND_OK)
        sync_dir(state_resolved);
    return ND_OK;
}
