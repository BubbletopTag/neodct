/* koki_audio.c -- SoundManager: the external-player backend and the two ways
 * of having no sound at all.
 *
 * ============ WHAT IS HERE AND WHAT IS NOT ============
 *
 * engine.py has TWO backends. The preferred one is _MiniaudioMixer: an
 * in-process mixer that decodes every asset itself, sums up to three sfx over
 * one looping music voice with saturating int16 adds, and hands 22,050 Hz
 * mono s16 to a single ALSA device. THAT ONE IS NOT PORTED. It needs an MP3
 * decoder (seventeen of the fifty-seven assets are MP3, the longest 183 s and
 * therefore something that must be STREAMED, never decoded whole -- 8.1 MB)
 * and an ALSA writer thread. Both are new third-party code in lib/, which is
 * outside this task's scope; asking was the right call rather than vendoring
 * a decoder unreviewed. See README-PORT.md and the session report.
 *
 * What IS here is the fallback engine.py itself falls back to -- aplay,
 * mpg123 and mpv as child processes -- plus the disable paths. On this host
 * and in QEMU today, /dev/snd does not exist, so the path actually taken is
 * _disable(), which is also the path the golden frame was captured through:
 * sound reaches no pixel and no timing (play_until_done waits the manifest's
 * declared duration, not the device's).
 *
 * ============ WHY MPV FOR MUSIC AND APLAY FOR SFX ============
 *
 * engine.py's comment, kept because the reasoning is not recoverable from
 * the code: aplay starts in milliseconds and its RSS is trivial, which
 * matters because mpv's init delay is audible on an emulated CPU; mpv's deep
 * buffering survives sharing the device with aplay bursts, which mpg123 did
 * not -- it stuttered and reset whenever aplay grabbed QEMU's emulated card.
 * And mpv's footprint is per PROCESS, so if sfx ever fall back to mpv on a
 * small-RAM system MAX_SFX drops to one: two concurrent mpvs OOM'd a 72 MB VM.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"

#include "koki.h"

/* Memory trims, probed against the installed mpv at init and dropped
 * wholesale if its build rejects any of them: an unknown option is an
 * instant exit, which is total silence rather than a louder game. */
static const char *const MPV_EXTRA[] = {
    "--no-config", "--load-scripts=no",        "--audio-display=no",
    "--cache=no",  "--demuxer-max-bytes=1MiB", "--demuxer-max-back-bytes=256KiB",
};

/* One de-duplicating log, standing in for reasons_logged. Bounded: the
 * Python's set is unbounded but only ever holds a handful of fixed strings. */
#define KOKI_SND_LOG_MAX 12
static char g_logged[KOKI_SND_LOG_MAX][160];
static size_t g_n_logged;

static bool log_once(const char *msg)
{
    size_t i;

    for (i = 0u; i < g_n_logged; i++) {
        if (strcmp(g_logged[i], msg) == 0)
            return false;
    }
    if (g_n_logged < KOKI_SND_LOG_MAX)
        (void)nd_strlcpy(g_logged[g_n_logged++], msg, sizeof g_logged[0]);
    return true;
}

static void snd_disable(koki_sound_mgr *sm, const char *reason)
{
    if (log_once(reason))
        nd_log(ND_LOG_KOKI, "SOUND DISABLED: %s -- game continues silent", reason);
    (void)nd_strlcpy(sm->disabled_reason, reason, sizeof sm->disabled_reason);
    sm->enabled = false;
}

static void snd_log_once(const char *msg)
{
    if (log_once(msg))
        nd_log(ND_LOG_KOKI, "%s", msg);
}

static bool dir_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* shutil.which(). PATH order, executable bit, no shell. */
static bool which(const char *name, char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    const char *p;

    if (name == NULL || path == NULL)
        return false;
    for (p = path; *p != '\0';) {
        const char *sep = strchr(p, ':');
        size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        char cand[ND_PATH_MAX];

        if (len > 0u && len + strlen(name) + 2u < sizeof cand) {
            (void)memcpy(cand, p, len);
            cand[len] = '/';
            (void)nd_strlcpy(cand + len + 1u, name, sizeof cand - len - 1u);
            if (access(cand, X_OK) == 0) {
                (void)nd_strlcpy(out, cand, out_sz);
                return true;
            }
        }
        if (sep == NULL)
            break;
        p = sep + 1;
    }
    return false;
}

/* The first line of /proc/meminfo, "MemTotal: <kB>". Returns 0 when it
 * cannot be read, which leaves every caller on its default. */
static long meminfo_total_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long kb = 0;

    if (f == NULL)
        return 0;
    if (fgets(line, sizeof line, f) != NULL) {
        char *sp = strchr(line, ' ');

        if (sp != NULL)
            kb = strtol(sp, NULL, 10);
    }
    (void)fclose(f);
    return kb;
}

/* Ask mpv whether it accepts the trim flags. Ten-second cap, as the Python
 * has, because an mpv that hangs at startup would hang the game's launch. */
static bool probe_mpv(const char *mpv_path)
{
    const char *argv[ND_ARRAY_LEN(MPV_EXTRA) + 3u];
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    size_t i;
    size_t n = 0u;
    int devnull;

    argv[n++] = "mpv";
    for (i = 0u; i < ND_ARRAY_LEN(MPV_EXTRA); i++)
        argv[n++] = MPV_EXTRA[i];
    argv[n++] = "--version";
    argv[n] = NULL;

    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_AUDIO;
    if (devnull >= 0) {
        spec.fds[0].child_fd = 1;
        spec.fds[0].our_fd = devnull;
        spec.fds[1].child_fd = 2;
        spec.fds[1].our_fd = devnull;
        spec.n_fds = 2u;
    }
    if (nd_proc_spawn(mpv_path, &spec, &pid) != ND_OK) {
        if (devnull >= 0)
            (void)close(devnull);
        return false;
    }
    if (devnull >= 0)
        (void)close(devnull);
    memset(&st, 0, sizeof st);
    if (nd_proc_wait(pid, 10.0, &st) != ND_OK) {
        (void)nd_proc_terminate(pid, 0.0, NULL);
        return false;
    }
    return st.exited && st.exit_status == 0;
}

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */

void koki_sound_open(koki_sound_mgr *sm, const char *assets_dir)
{
    char aplay[ND_PATH_MAX];
    char mpg123[ND_PATH_MAX];
    char mpv[ND_PATH_MAX];
    bool have_aplay;
    bool have_mpg123;
    bool have_mpv;
    const char *forced;
    const char *env;
    size_t i;

    if (sm == NULL)
        return;
    memset(sm, 0, sizeof *sm);
    g_n_logged = 0u;
    sm->enabled = true;
    sm->max_sfx = KOKI_SND_MAX_SFX;
    sm->music_pid = -1;
    for (i = 0u; i < KOKI_SND_MAX_SFX; i++)
        sm->sfx_pid[i] = -1;
    (void)nd_strlcpy(sm->base_dir, (assets_dir != NULL) ? assets_dir : "", sizeof sm->base_dir);

    if (getenv("NEODCT_KOKI_NOSOUND") != NULL) {
        snd_disable(sm, "NEODCT_KOKI_NOSOUND set");
        return;
    }
    /* A plain stat, NOT nd_path_is_dir(): /dev/snd is a kernel device path
     * and has nothing to do with the ND_ROOT namespace. Resolving it would
     * make a staged test root claim the host has no sound card, which is
     * true here by accident and would be wrong for the wrong reason. */
    if (!dir_exists("/dev/snd")) {
        snd_disable(sm, "/dev/snd missing (no ALSA device; kernel audio not implemented yet?)");
        return;
    }

    /* The in-process mixer's slot. It is never available in this build, so
     * NEODCT_KOKI_AUDIO=miniaudio is a hard disable and anything else falls
     * through -- which is exactly what engine.py does when python-miniaudio
     * is not installed. */
    forced = getenv("NEODCT_KOKI_AUDIO");
    if (forced == NULL || strcmp(forced, "subprocess") != 0) {
        const char *msg = "miniaudio unavailable (not built into the C port)";

        if (forced != NULL && strcmp(forced, "miniaudio") == 0) {
            snd_disable(sm, msg);
            return;
        }
        nd_log(ND_LOG_KOKI, "%s; falling back to external players", msg);
    }

    have_aplay = which("aplay", aplay, sizeof aplay);
    have_mpg123 = which("mpg123", mpg123, sizeof mpg123);
    have_mpv = which("mpv", mpv, sizeof mpv);

    /* pick(): an explicit override wins, then the preference order. */
    env = getenv("NEODCT_KOKI_WAV_PLAYER");
    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(sm->wav_player, env, sizeof sm->wav_player);
        sm->have_wav_player = true;
    } else if (have_aplay) {
        (void)nd_strlcpy(sm->wav_player, "aplay", sizeof sm->wav_player);
        sm->have_wav_player = true;
    } else if (have_mpv) {
        (void)nd_strlcpy(sm->wav_player, "mpv", sizeof sm->wav_player);
        sm->have_wav_player = true;
    }

    env = getenv("NEODCT_KOKI_MP3_PLAYER");
    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(sm->mp3_player, env, sizeof sm->mp3_player);
        sm->have_mp3_player = true;
    } else if (have_mpv) {
        (void)nd_strlcpy(sm->mp3_player, "mpv", sizeof sm->mp3_player);
        sm->have_mp3_player = true;
    } else if (have_mpg123) {
        (void)nd_strlcpy(sm->mp3_player, "mpg123", sizeof sm->mp3_player);
        sm->have_mp3_player = true;
    }

    if (sm->have_wav_player && strcmp(sm->wav_player, "mpv") == 0) {
        long kb = meminfo_total_kb();

        if (kb > 0 && kb < 72L * 1024L)
            sm->max_sfx = 1;
    }
    env = getenv("NEODCT_KOKI_MAX_SFX");
    if (env != NULL) {
        char *end = NULL;
        long v = strtol(env, &end, 10);

        /* Unparseable is ignored, as the Python's except ValueError is. */
        if (end != NULL && end != env && *end == '\0' && v >= 0 && v <= KOKI_SND_MAX_SFX)
            sm->max_sfx = (int)v;
    }

    if (have_mpv) {
        sm->mpv_trim_ok = probe_mpv(mpv);
        if (!sm->mpv_trim_ok)
            nd_log(ND_LOG_KOKI, "this mpv rejects the memory-trim flags; running it plain");
    }

    if (!sm->have_wav_player && !sm->have_mp3_player)
        snd_disable(sm, "no audio player found (aplay/mpg123/mpv)");
    else
        nd_log(ND_LOG_KOKI, "audio players: sfx=%s music=%s",
               sm->have_wav_player ? sm->wav_player : "None",
               sm->have_mp3_player ? sm->mp3_player : "None");
}

/* ------------------------------------------------------------------ *
 * Spawning a player
 * ------------------------------------------------------------------ */

static pid_t snd_spawn(koki_sound_mgr *sm, const char *rel, bool loop)
{
    char virt[ND_PATH_MAX];
    char real[ND_PATH_MAX];
    char exe[ND_PATH_MAX];
    const char *argv[16];
    const char *player;
    nd_proc_spec spec;
    pid_t pid = -1;
    size_t n = 0u;
    size_t i;
    bool is_mp3;
    int devnull;

    if (nd_snprintf(virt, sizeof virt, "%s/%s", sm->base_dir, rel) != ND_OK)
        return -1;
    /* The player is a separate program opening a real path, so this is one
     * of the few places that has to leave the ND_ROOT namespace. */
    if (nd_path_resolve(real, sizeof real, virt) != ND_OK)
        return -1;

    is_mp3 = (strlen(rel) > 4u && strcmp(rel + strlen(rel) - 4u, ".mp3") == 0);
    player = is_mp3 ? (sm->have_mp3_player ? sm->mp3_player : NULL)
                    : (sm->have_wav_player ? sm->wav_player : NULL);

    if (loop && player != NULL && strcmp(player, "aplay") == 0) {
        /* aplay has no loop mode. Exactly one asset needs this: "Koki D
         * score" is 14.46 s, under the builder's 15 s music threshold, so it
         * was baked as a WAV and is then used as looping music. */
        char probe[ND_PATH_MAX];

        if (which("mpv", probe, sizeof probe)) {
            player = "mpv";
            snd_log_once("looped wav needs mpv (aplay can't loop)");
        } else {
            player = NULL;
            snd_log_once("no looping wav player; music skipped");
        }
    }
    if (player == NULL) {
        snd_log_once(is_mp3 ? "no mp3 player installed; skipping mp3 audio"
                            : "no wav player installed; skipping wav audio");
        return -1;
    }
    if (!which(player, exe, sizeof exe))
        return -1;

    if (strcmp(player, "aplay") == 0) {
        argv[n++] = "aplay";
        argv[n++] = "-q";
        argv[n++] = real;
    } else if (strcmp(player, "mpg123") == 0) {
        argv[n++] = "mpg123";
        argv[n++] = "-q";
        if (loop) {
            argv[n++] = "--loop";
            argv[n++] = "-1";
        }
        argv[n++] = real;
    } else {
        argv[n++] = "mpv";
        argv[n++] = "--no-video";
        argv[n++] = "--really-quiet";
        argv[n++] = "--no-terminal";
        if (sm->mpv_trim_ok) {
            for (i = 0u; i < ND_ARRAY_LEN(MPV_EXTRA); i++)
                argv[n++] = MPV_EXTRA[i];
        }
        if (loop)
            argv[n++] = "--loop=inf";
        argv[n++] = real;
    }
    argv[n] = NULL;

    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_AUDIO;
    if (devnull >= 0) {
        spec.fds[0].child_fd = 1;
        spec.fds[0].our_fd = devnull;
        spec.fds[1].child_fd = 2;
        spec.fds[1].our_fd = devnull;
        spec.n_fds = (getenv("NEODCT_KOKI_SOUND_DEBUG") != NULL) ? 1u : 2u;
    }
    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        char reason[160];

        (void)snprintf(reason, sizeof reason, "%s spawn failed", player);
        snd_disable(sm, reason);
        pid = -1;
    }
    if (devnull >= 0)
        (void)close(devnull);
    return pid;
}

/* ------------------------------------------------------------------ *
 * The API game.py sees
 * ------------------------------------------------------------------ */

void koki_sound_music(koki_sound_mgr *sm, const char *rel)
{
    if (sm == NULL || !sm->enabled || rel == NULL)
        return;
    koki_sound_stop_music(sm);
    sm->music_pid = snd_spawn(sm, rel, true);
    sm->music_death_logged = false;
}

void koki_sound_sfx(koki_sound_mgr *sm, const char *rel)
{
    size_t i;
    int live = 0;
    int slot = -1;

    if (sm == NULL || !sm->enabled || rel == NULL)
        return;
    /* Prune finished voices first, then refuse rather than queue: a fourth
     * simultaneous sfx is DROPPED, which is what Scratch does too. */
    for (i = 0u; i < KOKI_SND_MAX_SFX; i++) {
        if (sm->sfx_pid[i] > 0) {
            nd_proc_status st;

            memset(&st, 0, sizeof st);
            if (nd_proc_wait(sm->sfx_pid[i], 0.0, &st) == ND_OK)
                sm->sfx_pid[i] = -1;
        }
        if (sm->sfx_pid[i] > 0)
            live++;
        else if (slot < 0)
            slot = (int)i;
    }
    if (live >= sm->max_sfx || slot < 0)
        return;
    sm->sfx_pid[slot] = snd_spawn(sm, rel, false);
}

void koki_sound_stop_music(koki_sound_mgr *sm)
{
    if (sm == NULL || sm->music_pid <= 0)
        return;
    (void)nd_proc_terminate(sm->music_pid, 0.0, NULL);
    sm->music_pid = -1;
}

void koki_sound_stop_all(koki_sound_mgr *sm)
{
    size_t i;

    if (sm == NULL)
        return;
    koki_sound_stop_music(sm);
    for (i = 0u; i < KOKI_SND_MAX_SFX; i++) {
        if (sm->sfx_pid[i] > 0)
            (void)nd_proc_terminate(sm->sfx_pid[i], 0.0, NULL);
        sm->sfx_pid[i] = -1;
    }
}

void koki_sound_check(koki_sound_mgr *sm)
{
    nd_proc_status st;

    /* Looping music should never exit on its own, so an exit here means the
     * player crashed or the OOM killer got it. Called every 30 frames. */
    if (sm == NULL || sm->music_pid <= 0)
        return;
    memset(&st, 0, sizeof st);
    if (nd_proc_wait(sm->music_pid, 0.0, &st) != ND_OK)
        return;
    sm->music_pid = -1;
    if (!sm->music_death_logged) {
        sm->music_death_logged = true;
        snd_log_once("music player died -- bad option or OOM kill? check dmesg / run with "
                     "NEODCT_KOKI_SOUND_DEBUG=1");
    }
}

void koki_sound_shutdown(koki_sound_mgr *sm)
{
    if (sm == NULL)
        return;
    koki_sound_stop_all(sm);
    sm->enabled = false;
}
