/* fetch_app.h -- Fetch, app id 9009: pull files off an FTPS server onto the
 * memory card, and put each one where the phone already looks for its kind.
 *
 * ============ WHAT IT IS FOR ============
 *
 * There is no way to get a file onto this phone that does not involve taking
 * the card out. The browser downloads into untrusted/ and nothing sorts what
 * lands there; sftp reaches the phone but needs a key and a PC at the other
 * end. Fetch is the third thing: a server the owner runs, a password typed on
 * the keypad, a list of what is up there, and a download that ends with the
 * file in the folder whose app reads it -- music in music/, a disc image
 * inside the PSX app's games/, a .nap where Settings' installer looks.
 *
 * It is an ENGINEERING app on purpose. It types a password over a network
 * connection whose certificate is not verified (see below), it writes into
 * other apps' directories, and it is a development convenience rather than
 * something a phone owner needs. Engineering mode is where those live.
 *
 * ============ WHY IT MUST BE A SYSTEM APP AND NOT A .nap ============
 *
 * The destinations decide it. music/ is 0750 ndusr:ndusr and apps/PSX/ is
 * 0755 ndusr:ndusr (neodct-sdcard's CARD_LAYOUT), so a card-installed app --
 * which runs as ndusr_ut with a private mount namespace and may write only
 * its own data/ -- can write to neither, and correctly so. Packaging this as
 * a .nap would produce an app that lists a server beautifully and then cannot
 * save anything.
 *
 * An app under System/engineering/apps runs as ROOT in engineering mode
 * (nd_proc_app_needs_root), so it can. What it writes lands root-owned and
 * 0644 under the core's umask, which is what the readers need: MusicPlayer
 * is ndusr and PSX is ndusr_ut, and both only ever read these files.
 *
 * ============ THE TRANSPORT IS SPAWNED curl, AND WHY NOT nd_remote ============
 *
 * Same reasoning as lib/nd_remote.c's header, for the same reasons: curl is
 * already in both defconfigs, linking libcurl into libneodct would cost idle
 * RSS in every process that maps it, and a download happens rarely. So this
 * spawns /usr/bin/curl with nd_proc_spawn() exactly as nd_remote does, and
 * a stand-in `curl` earlier on PATH is the test seam.
 *
 * It does NOT reuse nd_remote_download(). That function is one job: resume a
 * 53 MB .ndsw from GitHub over HTTPS with byte ranges, checking status lines
 * out of a -D pipe and appending to a partial file. None of it applies here
 * -- FTP has no status lines, no redirects and no Range header, the files are
 * small enough that a failed transfer is worth restarting rather than
 * resuming, and the size is already known from the listing. Generalising it
 * would mean rewriting the one code path in this tree that must not break
 * quietly, to serve a development tool. A second, much smaller driver is the
 * cheaper mistake.
 *
 * ============ TLS IS REQUIRED, THE CERTIFICATE IS NOT CHECKED ============
 *
 * curl is passed --ssl-reqd, so the password and the file are encrypted and a
 * server that will not do AUTH TLS is refused rather than silently spoken to
 * in the clear. It is also passed -k, because the server is a droplet with an
 * IP address, a self-signed certificate and no domain name to put on a real
 * one. That means passive eavesdropping is defeated and an active
 * man-in-the-middle is not. The account at the other end is therefore meant
 * to be worth nothing -- see neodct/tools/ftp-server-setup.sh, which builds
 * it that way -- and the password is never written down anywhere on the phone.
 *
 * ============ THE PASSWORD DOES NOT GO IN argv ============
 *
 * argv is world-readable through /proc for as long as curl runs, and the
 * phone has a shell app. So the credentials go into a 0600 netrc file under
 * /tmp (tmpfs, gone at reboot), curl is given --netrc-file, and the file is
 * unlinked the moment curl has exited. The password itself lives in a
 * fetch_conn on the app's stack and dies with the app; nothing stores it, so
 * every launch asks again.
 */

#ifndef FETCH_APP_H_INCLUDED
#define FETCH_APP_H_INCLUDED

#include "nd_paths.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The app's id, and the breadcrumb root the widgets draw. 9009 was the next
 * free number in engineering/apps. */
#define ND_FETCH_APP_ID 9009

/* The droplet from [[remote-shell-vps]] -- the same box the phone already
 * dials for Remote Shell, so there is one server to keep alive rather than
 * two. Overridable without a rebuild through settings, because a fork of
 * this OS has no business talking to that address. */
#define ND_FETCH_HOST_DEFAULT "67.205.190.49"
#define ND_FETCH_USER_DEFAULT "neodct"
#define ND_FETCH_KEY_HOST     "fetch.host"
#define ND_FETCH_KEY_USER     "fetch.user"

/* Bounds. Every one of these covers text an FTP SERVER sent, which is to say
 * an attacker's text on a phone that will use it as a file name. A field that
 * does not fit means the entry is skipped, never truncated into use. */
#define ND_FETCH_NAME_MAX  96  /* one listing entry's file name          */
#define ND_FETCH_ENTRIES   64  /* entries shown for one directory        */
#define ND_FETCH_LINE_MAX  512 /* one line of LIST output                */
#define ND_FETCH_LIST_MAX  (128u * 1024u) /* the whole listing, at most  */
#define ND_FETCH_WHY_MAX   160 /* the reason a person reads              */
#define ND_FETCH_PASS_MAX  64
#define ND_FETCH_HOST_MAX  128
#define ND_FETCH_URL_MAX   512

/* Long enough for a slow file over a carrier, short enough that a wedged
 * server does not hold the app forever. Same split as nd_remote: reaching
 * the server is a different job from reading from it. */
#define ND_FETCH_CONNECT_TIMEOUT 20
#define ND_FETCH_STALL_TIMEOUT   90

/* ------------------------------------------------------------------ *
 * A listing entry
 * ------------------------------------------------------------------ */

typedef struct {
    char name[ND_FETCH_NAME_MAX];
    int64_t size; /* bytes, or -1 when the server did not say  */
    bool is_dir;
} fetch_entry;

/* ------------------------------------------------------------------ *
 * Where a file goes
 * ------------------------------------------------------------------ */

typedef enum {
    FETCH_DEST_MUSIC = 0, /* sdcard/music                           */
    FETCH_DEST_GAME,      /* sdcard/apps/PSX/games/<title>/         */
    FETCH_DEST_BIOS,      /* sdcard/apps/PSX/bios/scph1001.bin      */
    FETCH_DEST_NAP,       /* sdcard/untrusted -- Settings finds it  */
    FETCH_DEST_OTHER      /* sdcard/untrusted                       */
} fetch_dest_kind;

/* What the extension means. Unknown and extensionless names are
 * FETCH_DEST_OTHER rather than refused: an unrecognised file is still a file
 * the owner asked for, and untrusted/ is where the browser puts those too. */
fetch_dest_kind fetch_classify(const char *name);

/* Whether a name a SERVER sent may be used as a path component at all.
 *
 * False for: empty, longer than the field, containing '/', '\\' or any byte
 * below 0x20, exactly "." or "..", or starting with '-' (which curl would
 * read as an option if the name ever reached an argv). This is the only
 * check between the network and a filename, so it refuses rather than
 * sanitises: a name that has to be repaired to be safe is a name the owner
 * should rename at the other end. */
bool fetch_name_is_safe(const char *name);

/* The absolute path a downloaded file should end up at.
 *
 * `mount` is the card's mount point (ND_PATH_SDCARD_MOUNT on the phone, a
 * scratch directory in the tests). `psx_installed` is passed in rather than
 * looked up so that the whole function stays pure and testable: when it is
 * false a disc image goes to untrusted/ with kind FETCH_DEST_OTHER, because
 * writing games/ into an app directory that does not exist would create a
 * folder nothing will ever read.
 *
 * A disc image gets a directory of its own named after the file's stem,
 * which is the layout PSX reads: games/<Title>/<Title>.bin. The caller is
 * responsible for creating it -- fetch_prepare_dir() does.
 *
 * ND_ERR_INVAL for an unsafe name, ND_ERR_TOOLONG if the path will not fit. */
nd_err fetch_dest_path(const char *mount, const char *name, bool psx_installed, char *out,
                       size_t out_sz, fetch_dest_kind *kind);

/* mkdir -p the directory part of `path`, with mode 0755. */
nd_err fetch_prepare_dir(const char *path);

/* Write a single-track cue sheet beside a raw .bin, if there is not one.
 *
 * A one-to-one port of neodct-pcsxrearmed's tools/mkcue.sh, including its
 * refusal: a file whose length is not a whole number of 2352-byte sectors is
 * not a raw disc image, and guessing a cue for it would produce a disc that
 * fails later and further away. Both discs on that shelf are a single
 * MODE2/2352 data track, so that is what is written; a game with real CD
 * audio tracks needs its own cue uploaded beside it.
 *
 * ND_ERR_UNSUPPORTED when the size check fails, ND_OK when a cue was written
 * AND when one already existed -- the caller only wants to know it is there. */
nd_err fetch_write_cue(const char *bin_path);

/* ------------------------------------------------------------------ *
 * Parsing what the server said
 * ------------------------------------------------------------------ */

/* One line of `LIST` output -- the ls -l shape vsftpd, proftpd and every
 * other unix server emits:
 *
 *     -rw-r--r--    1 1000     1000       4194304 Sep 05 12:00 track.mp3
 *     drwxr-xr-x    2 1000     1000          4096 Sep 05 12:00 music
 *
 * Returns false for a line that is not one: the "total 12" header, a blank
 * line, a symlink or a device (mode letter not '-' or 'd'), a name that is
 * not safe, or too few fields. False means SKIP, never fail -- a server with
 * one odd entry should still list the rest.
 *
 * The name is everything from the ninth field to the end of the line, so
 * spaces in file names survive. The size is field five; a size that will not
 * parse becomes -1 rather than 0, because 0 is a real file length and the
 * progress bar has to tell "empty" from "unknown". */
bool fetch_parse_list_line(const char *line, fetch_entry *out);

/* Split a whole LIST response into entries, directories first and then names
 * ascending, the way every list in this OS is ordered. Returns how many were
 * written, never more than `max`. */
size_t fetch_parse_listing(const char *text, fetch_entry *out, size_t max);

/* A size for the screen: "4.0 MB", "812 kB", "17 B". Sizes on this phone are
 * one decimal place at MB and none below, which is what the update screen
 * already shows. */
void fetch_format_size(int64_t bytes, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * The transport
 * ------------------------------------------------------------------ */

typedef struct {
    char host[ND_FETCH_HOST_MAX];
    char user[ND_FETCH_NAME_MAX];
    char pass[ND_FETCH_PASS_MAX];
} fetch_conn;

/* Build "ftp://<host>/<dir>/" or "ftp://<host>/<dir>/<name>".
 *
 * `dir` is the remote directory with no leading or trailing slash ("" for the
 * root), `name` NULL for a directory URL. Refuses anything that is not
 * already safe rather than escaping it: every path this app builds came from
 * a listing that fetch_parse_list_line() already vetted, so a character that
 * would need escaping here means something upstream let it through. */
nd_err fetch_build_url(const char *host, const char *dir, const char *name, char *out,
                       size_t out_sz);

/* Called during a download, roughly five times a second. `total` is the size
 * from the listing, or -1. Return false to abort the transfer -- the app
 * returns false when nd_app_should_exit() does, so a SIGTERM for an incoming
 * call does not leave curl running. */
typedef bool (*fetch_progress_fn)(void *ctx, int64_t done, int64_t total);

/* List a remote directory. ND_ERR_PERM for a refused login (curl 67), which
 * is worth its own message because it is the one failure the owner can fix
 * by typing more carefully. */
nd_err fetch_list(const fetch_conn *c, const char *dir, fetch_entry *out, size_t max, size_t *n_out,
                  char *why, size_t why_sz);

/* Download one file to `local_path`, which is written through a ".part"
 * sibling and renamed on success -- so an interrupted transfer never leaves
 * something in music/ that the player will try to open. */
nd_err fetch_download(const fetch_conn *c, const char *dir, const char *name,
                      const char *local_path, int64_t total, fetch_progress_fn on_progress,
                      void *ctx, char *why, size_t why_sz);

#ifdef __cplusplus
}
#endif

#endif /* FETCH_APP_H_INCLUDED */
