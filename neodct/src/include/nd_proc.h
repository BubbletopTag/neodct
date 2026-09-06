/* nd_proc.h -- starting other programs, and finding out what happened to them.
 *
 * (PORT-PLAN.md section 2.4 calls this header nd_child.h. nd_child.h exists
 * and includes this one, so either spelling compiles.)
 *
 * ============ THE RULE ============
 *
 *     fork() IS ALWAYS IMMEDIATELY FOLLOWED BY execve().
 *
 * Not "usually". The core runs threads -- the modem and the clock -- and
 * forking a threaded process gives the child only the calling thread. Any
 * mutex another thread held at the instant of the fork stays locked forever in
 * the child, INCLUDING THE ONES INSIDE malloc(). The child then hangs on its
 * first allocation, which will be somewhere that looks nothing like the fork.
 *
 * Between fork() and execve() only async-signal-safe calls are permitted. No
 * malloc, no printf, no fopen. _exit(), never exit() -- exit() runs atexit
 * handlers and flushes the parent's buffers a second time, in the child.
 *
 * Every helper here obeys that by construction: argv, envp and the descriptor
 * plan are all built BEFORE the fork, so the child does nothing but dup2,
 * close and exec.
 *
 * ============ OWNER TAGS ============
 *
 * Four kinds of child exist and they are reaped differently, so each one
 * carries a tag:
 *
 *   ND_OWNER_APP    the one app child. The core waits for it.
 *   ND_OWNER_AUDIO  the modem's PCM bridge. Long-lived, restarted on failure.
 *   ND_OWNER_TONE   a one-shot aplay. Fire and forget; the reaper collects it.
 *   ND_OWNER_SYSTEM poweroff, ssh-keygen, sshd, the relay tunnel.
 *
 * A single SIGCHLD reaper collects them all, so nothing becomes a zombie and
 * no code path has to remember to waitpid().
 */

#ifndef ND_PROC_H_INCLUDED
#define ND_PROC_H_INCLUDED

#include <sys/types.h>

#include "nd_crash.h"
#include "nd_input.h"
#include "nd_priv.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ND_OWNER_APP = 0, ND_OWNER_AUDIO, ND_OWNER_TONE, ND_OWNER_SYSTEM } nd_proc_owner;

/* Descriptors to hand the child. Anything not listed is closed on exec. */
#define ND_PROC_MAX_FDS 8

/* Paths a child may be given an empty view of. See `hide_paths`. */
#define ND_PROC_MAX_HIDE 16

typedef struct {
    int child_fd; /* the number the child will see */
    int our_fd;   /* the descriptor to dup2 onto it */
} nd_fd_map;

typedef struct {
    const char *const *argv; /* argv[0] included; NULL-terminated */
    const char *const *envp; /* NULL means "inherit environ"      */
    nd_fd_map fds[ND_PROC_MAX_FDS];
    size_t n_fds;
    nd_proc_owner owner;

    /* setsid() in the child, between the fork and the execve. Python spells
     * this start_new_session=True, and for anything the core may later
     * signal by PROCESS GROUP it is not optional.
     *
     * The failure it prevents is on the record, in RemoteShell's own
     * _owns() docstring: the phone left sshd.pid=244 and tunnel.pid=246
     * behind, came back up, and Remote Shell killed the process group those
     * numbers had been handed to by then -- its own launcher. The UI never
     * started and the serial log stopped mid-boot.
     *
     * With a new session the child's group contains the child and its own
     * descendants and nothing else, so killing that group cannot reach back
     * into the core. It is off by default because an app SHOULD stay in the
     * core's session: nd_proc_terminate() signals a single pid, and an app
     * that outlives its session leader is exactly the orphan the crash
     * screen exists to catch. */
    bool new_session;

    /* Close every descriptor above stderr in the child, after the fds[] above
     * are in place and before the execve. Off by default because an app is
     * MEANT to inherit what the core hands it -- the crash pipe and the
     * service socket are passed exactly that way. Turn it on for anything
     * long-lived that the core does not own, or it will hold the core's pipes
     * open for as long as it runs. */
    bool close_others;

    /* Run the child as somebody else. SECURITY-PLAN.md section 1.
     *
     * Resolved with nd_priv_lookup() in the parent -- getpwnam allocates and
     * reads a file, and neither is allowed after the fork -- then applied by
     * the child with nd_priv_become(), which is four syscalls on integers
     * that were copied before the fork. A `valid` of false means "no such
     * user in this image", which is a no-op rather than a failure, so a
     * caller needs no branch for a build without the users table.
     *
     * This is the ONE call site the plan asks for, and the reason section 3
     * insists on the order it does: an app denied libneodct writes ten lines
     * of termios and talks to /dev/ttyUSB2 itself, so a library is not a
     * boundary. A uid is, because it is the kernel's answer rather than
     * ours. Zeroed by memset like every other field, so a spec that does not
     * mention it runs the child as the caller, exactly as before. */
    nd_priv_id run_as;

    /* prctl(PR_SET_NO_NEW_PRIVS) in the child even when it keeps its uid.
     * Independent of run_as: a child that stays root gains nothing from it,
     * but a child that is going to load untrusted code should not be able to
     * regain privilege through a setuid binary either way, and it is the
     * precondition for the seccomp filter in Phase 3. */
    bool no_new_privs;

    /* ---- the mount namespace. SECURITY-PLAN.md section 2. --------------
     *
     * unshare(CLONE_NEWNS) in the child, then MS_REC|MS_PRIVATE on / so
     * nothing done afterwards propagates back out. This is the 5.10
     * stand-in for Landlock, which does not exist before 5.13, and it is
     * strictly more thorough than a permission would be: it removes paths
     * from EXISTENCE rather than denying access to them, so there is no
     * `..`, no symlink and no /proc/self/root trick to walk back out.
     *
     * It matters for the world-readable half of the image, which is
     * precisely what DAC cannot help with. /NeoDCT/System has to be readable
     * by every app, so `file:///NeoDCT/System/...` in the browser enumerates
     * the whole system tree no matter who the browser runs as --
     * SECURITY-AUDIT.md finding 5.
     *
     * hide_paths is over-mounted with an empty, read-only, mode-0000 tmpfs:
     * the directory still exists and is empty and unreadable, which is a
     * quieter failure for the program inside than a missing path and just as
     * final. Resolve the list before the fork -- a path that does not exist
     * is dropped there, so that a failure in the child is always a real one.
     *
     * FAIL-OPEN ON unshare, deliberately. CONFIG_MNT_NS is a vendor BSP
     * question the plan's section 6 lists as unverified on the phone, and a
     * kernel without it must still open the browser -- the uid boundary is
     * what carries the confinement, and this is defence on top of it. A hide
     * that fails AFTER the namespace exists is a different matter and kills
     * the child, because that one is a bug rather than a missing feature. */
    bool private_mounts;
    const char *const *hide_paths; /* NULL-terminated, or NULL */
} nd_proc_spec;

/* Whether this kernel can give a child a mount namespace at all.
 *
 * Answered by trying it in a throwaway child, once, and remembering. Call it
 * from the parent and log the answer: a phone whose kernel was built without
 * CONFIG_MNT_NS otherwise loses section 2's confinement silently, which is
 * the failure mode the whole plan is written against. */
bool nd_proc_namespaces_available(void);

/* fork + execve, in that order and nothing in between. *pid_out receives the
 * child's pid. ND_ERR_IO when the fork itself fails; an execve failure is
 * reported by the child exiting 127, which the caller sees at waitpid.
 *
 * The path is NOT ND_ROOT-resolved: it is an executable, and the test harness
 * runs the real binaries. */
nd_err nd_proc_spawn(const char *path, const nd_proc_spec *spec, pid_t *pid_out);

/* Install the SIGCHLD reaper. Idempotent. Call once, from the core, before
 * anything is spawned. */
nd_err nd_proc_reaper_start(void);
void nd_proc_reaper_stop(void);

/* ============ AND MAKE IT SAY WHAT IT REAPED ============
 *
 * A child that fails between fork() and execve() has no voice but its exit
 * status, so every failure on that path is an _exit() of a RESERVED code:
 *
 *     121  prctl(PR_SET_NO_NEW_PRIVS) refused
 *     122  setgroups() refused -- it needs CAP_SETGID
 *     123  setgid() refused, or the MS_PRIVATE remount failed
 *     124  setuid() refused, or a hide mount failed
 *     125  the drop did not read back: a PARTIAL drop
 *     126  dup2() of an inherited descriptor failed
 *     127  execve() failed
 *
 * Nothing can legitimately exit with one of those: aplay, mpv, netsurf and
 * nd-apprun exit with their own small numbers or die on a signal. So a status
 * in that range is always one of our own corpses, and always worth printing.
 *
 * Three of the four owner kinds are never waited for -- a tone is fire and
 * forget -- so their statuses sit in the reaper's ring until it wraps and
 * nobody ever looks. That is how every DTMF tone on the phone came to die at
 * exit 122, several a second while dialling, with an empty log. The reaper
 * itself cannot say so: it is a signal handler and nd_log() formats,
 * allocates and writes to a FILE*.
 *
 * This is the drain. It is cheap (sixteen slots, and each is reported once),
 * it is safe to call from anywhere ordinary, and nd_proc_spawn() already
 * calls it on every spawn -- so a core that never calls it still gets the
 * message as soon as it starts anything else. Calling it from the core's own
 * idle loop makes the report prompt on a phone that has stopped spawning. */
void nd_proc_reap_report(void);

/* What waitpid told us. */
typedef struct {
    pid_t pid;
    bool exited;     /* WIFEXITED   */
    int exit_status; /* WEXITSTATUS */
    bool signalled;  /* WIFSIGNALED */
    int signo;       /* WTERMSIG    */
} nd_proc_status;

/* Wait for one child. timeout_s < 0 blocks. ND_ERR_TIMEOUT when it is still
 * running. Safe to call while the reaper is installed -- the reaper skips
 * pids that are being waited on explicitly. */
nd_err nd_proc_wait(pid_t pid, double timeout_s, nd_proc_status *out);

/* SIGTERM, wait up to grace_s, then SIGKILL and wait again. This is the
 * sequence the incoming-call path uses; see the teardown contract in
 * nd_app.h. */
nd_err nd_proc_terminate(pid_t pid, double grace_s, nd_proc_status *out);

/* ------------------------------------------------------------------ *
 * Launching an app -- the whole sequence, in one call
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Which user an app runs as
 * ------------------------------------------------------------------ */

/* True when this app must keep the core's privileges rather than dropping to
 * ndusr. Pure, and separated from the launcher so that the POLICY can be
 * tested without forking anything -- the launcher's own behaviour cannot be,
 * because a test cannot become another user.
 *
 * ============ THE ONE REASON AN APP KEEPS ROOT ============
 *
 * IT IS AN ENGINEERING APP AND ENGINEERING MODE IS ON.
 *
 *    RemoteShell exists to give a developer a root shell; LinuxShell, Modem,
 *    KeypadMapperI2C and FuelGauge exist to poke at hardware. Confining them
 *    would not make the phone safer, it would make them useless, and a
 *    diagnostic that cannot diagnose is worse than none.
 *
 *    "Is it an engineering app" is decided by WHERE IT LIVES:
 *    /NeoDCT/System/engineering/apps/ rather than /NeoDCT/System/apps/. Both
 *    are on the read-only, dm-verity'd rootfs, so an app cannot move itself
 *    into the privileged directory, and nothing in its own manifest -- which
 *    is also read-only, but is at least app-authored in spirit -- is
 *    consulted. The path is not ND_ROOT-resolved either: nd_ui_scan_apps()
 *    stores the virtual prefix it was given, so this comparison means the
 *    same thing under a test root as on a phone.
 *
 * ============ THERE USED TO BE A SECOND, AND THERE IS NOT ANY MORE ============
 *
 * Five STOCK apps kept root because each had one privileged operation and
 * nowhere to send it. The list was written out longhand so that it was
 * uncomfortable to look at, and it is empty now:
 *
 *      Power     poweroff, reboot        -> nd_svc_reboot/poweroff
 *      Update    reboot, to finish       -> nd_svc_reboot
 *      Downgrade reboot                  -> nd_svc_reboot, via Update's dlopen
 *      Clock     settimeofday by hand    -> nd_svc_set_clock
 *      Settings  neodct-sdcard format    -> nd_svc_format_card
 *
 * None of those five operations belonged to the app. Each is a thing the CORE
 * does on the app's behalf, over the nd_svc socket that already existed for
 * sending an SMS -- and the core stays root, so it can. That is what "the
 * abstraction is the only path" means in SECURITY-PLAN.md section 3, and it
 * is now true of every stock app on the phone.
 *
 * ROOT_STOCK_APPS still exists, empty, in nd_proc.c. Read the comment above
 * it before adding anything to it.
 *
 * ============ WHAT THIS IS NOT ============
 *
 * `engineering_mode` comes from system.ui.engineering_mode in settings.prop,
 * on the WRITABLE partition. AGENTS.md is blunt that this is not a security
 * gate -- "it lives in settings.prop, on the partition the attacker just
 * wrote to" -- and it is right. Anything running as ndusr can set that key
 * and get root back on the next engineering app it opens.
 *
 * That is a known and accepted hole while the architecture is being settled,
 * and the shape of the fix is already decided and already proven elsewhere in
 * this tree: env.sh had exactly this problem and is now gated on
 * neodct.devenv=1 on the kernel command line, which the writable partition
 * cannot set. The same second gate belongs here -- both conditions true, the
 * settings key for the menu and the cmdline flag for the privilege -- and
 * when the initramfs recovery lands it is the natural place to set it.
 *
 * Written so that adding it is one && in the line below and nothing else. */
bool nd_proc_app_needs_root(const nd_app_entry *app, bool engineering_mode);

/* ------------------------------------------------------------------ *
 * The other direction: apps that run as LESS than ndusr
 * ------------------------------------------------------------------ *
 *
 * ============ WHY THIS HAD TO MOVE INTO THE CORE ============
 *
 * The Browser app is a launcher: its whole job is to start netsurf and pump
 * its stderr. netsurf is the untrusted half of this phone -- it renders
 * whatever the internet sends it -- so apps/Browser used to do the confining
 * itself, calling nd_priv_lookup(ND_PRIV_USER_UT) and asking nd_proc_spawn()
 * for a mount namespace.
 *
 * THAT STOPPED WORKING THE DAY APPS STOPPED BEING ROOT, and it stopped
 * working silently, in the worst possible direction:
 *
 *   setgroups() needs CAP_SETGID. As ndusr it fails, so nd_priv_become()
 *   returns a step code and the child _exit(122)s BEFORE execve -- netsurf
 *   never starts. The Browser app draws nothing and returns to the home
 *   screen, so the phone looks like the browser simply did not open.
 *
 *   unshare(CLONE_NEWNS) needs CAP_SYS_ADMIN. As ndusr it fails too, and
 *   because the code treats that as "this kernel has no namespaces" the
 *   phone printed "no mount namespaces in this kernel" on a kernel that has
 *   them -- nd-selftest reports CONFIG_MNT_NS PASS from the root core on the
 *   very same boot.
 *
 * Both were verified on a booted phone, not reasoned about.
 *
 * The privilege being asked for is "become a LESS privileged user, and drop
 * a namespace" -- which Linux still gates behind CAP_SETGID/CAP_SETUID and
 * CAP_SYS_ADMIN. Only one process on this phone has those and should keep
 * them: the core. So the confinement moves here, where it works, and the app
 * is left with nothing to get wrong.
 *
 * ============ WHAT THE APP GETS INSTEAD ============
 *
 * An app named here is launched as ndusr_ut with private_mounts and the hide
 * list below, AND WITHOUT A SERVICE SOCKET -- see nd_proc_launch_app(). That
 * last part is not tidiness. nd_svc has no peer credential check of any kind
 * (spec-app-services.md 5), so the socket is a straight line from any process
 * holding it to a root thread that will send an SMS on its behalf. An
 * untrusted app must not hold one, and neither must anything it forks.
 *
 * WHOLE PATHS, for the reason ROOT_STOCK_APPS' comment gives at length: a
 * name is not safe to match on. The direction of the grant is different here
 * -- this one only ever takes privilege away -- but a future user-installed
 * directory called Browser must not be able to claim the browser's profile
 * directory or the untrusted half of the SD card, both of which ndusr_ut owns
 * and ndusr does not. */
bool nd_proc_app_is_untrusted(const nd_app_entry *app);

/* Emptied over the untrusted app's view of the filesystem: each becomes a
 * read-only, mode-0000 tmpfs, so the directory still exists and is simply
 * unreadable. Moved here from apps/Browser/browser.h, which is where it lived
 * when the app did its own confining.
 *
 * These are the things ndusr_ut has no business reading even though the DAC
 * bits alone might let it: the engineering apps, the release signing keys,
 * the owner's tones, the databases, the SSH keys, the update records and the
 * RNG seed.
 *
 * ============ AND ONE THAT USED TO BE HERE AND IS NOT ============
 *
 * /NeoDCT/System/wallpapers was in this list and has been taken out. It is
 * the STOCK wallpaper art: read-only decoration baked into the dm-verity'd
 * squashfs, shipped identically on every phone, and therefore not the owner's
 * data in any sense -- an attacker who can read it learns which build is
 * running, which the version string on the settings screen already says.
 *
 * Masking it cost something real. Every installed app runs as ndusr_ut, so
 * with the directory emptied an app drew its chrome on black while the rest
 * of the phone drew it on the owner's wallpaper -- the app looked broken, and
 * looked broken in a way whose cause was three files away from anything the
 * author could see.
 *
 * The owner's OWN wallpapers are a different directory and stay exactly as
 * they are: the card's wallpapers/ is 0750 ndusr:ndusr, so ndusr_ut cannot
 * read it and needs no entry here to be kept out. What the app is given
 * instead is the RESOLVED PATH of the wallpaper actually in use, in its
 * environment -- see app_env() -- which lets it draw the stock art it can
 * read and fail gracefully on the art it cannot. */
#define ND_PROC_UNTRUSTED_HIDE_PATHS                                                 \
    {                                                                                \
        "/NeoDCT/System/engineering", "/NeoDCT/System/keys", "/NeoDCT/System/tones", \
            "/NeoDCT/User/db", "/NeoDCT/User/.remote", "/NeoDCT/User/.ndsys",        \
            "/NeoDCT/User/.seedrng", NULL                                            \
    }

/* ============ AND ONE MORE, FOR APPS THE OWNER INSTALLED ============
 *
 * Everything above is hidden from the whole untrusted set. This is hidden
 * from INSTALLED apps only, and the browser keeps it, because it is the
 * browser's own profile: /NeoDCT/User/browser is 0770 ndusr:ndusr_ut, which
 * made it not "the browser's directory" but "the directory of every process
 * that runs as ndusr_ut".
 *
 * That was fine while the browser was the only untrusted thing on the phone.
 * It stopped being fine the moment an app you installed became untrusted too:
 * same uid, same group, so an app under the apps directory could read the
 * browser's cookies and history, and could leave a file there for the browser
 * to pick up later.
 *
 * Masking it is not the real fix and is not pretending to be. The real fix is
 * a uid per installed app, at which point the group stops meaning "anything
 * untrusted" and this entry becomes unnecessary. Until then it costs one
 * tmpfs mount and closes the leak that actually exists. */
#define ND_PROC_INSTALLED_HIDE_EXTRA "/NeoDCT/User/browser"

/* ------------------------------------------------------------------ *
 * THE KEY DEVICE: how a program that is not an app.so gets the keypad
 * ------------------------------------------------------------------ *
 *
 * ============ THE PROBLEM THIS SOLVES ============
 *
 * An app.so reads keys from ND_ENV_KEYPAD_FD, a pipe the core pumps. That is
 * a PRIVATE PROTOCOL: it answers no ioctl, it appears under no /dev/input
 * path and it has no EVIOCGBIT, so netsurf-fb, mpv and any emulator core --
 * every program written to read a keyboard rather than to be a NeoDCT app --
 * cannot consume it. Those programs scan /dev/input instead, and on the
 * Luckfox /dev/input is EMPTY: the keypad is a PCF8575 matrix on i2c that the
 * core scans in userspace, and a matrix is not an input device.
 *
 * The only thing in the tree that ever made an evdev node for them was
 * nd_uinput_open() called from inside apps/Browser -- and that stopped
 * working the day the browser became ndusr_ut, because 61-neodct-devices.rules
 * grants /dev/uinput to group ndusr and ndusr_ut is deliberately not in it.
 * The browser has had no keys on hardware ever since, mpv has had none either
 * (it finds its keypad by looking for the browser's bridge BY NAME), and
 * every installed app that wraps a real binary would hit the same wall.
 *
 * ============ WHERE THE DEVICE LIVES NOW, AND WHY THERE ============
 *
 * The CORE creates it, before it spawns the app that asked for one. That is
 * what the udev rule's own comment has always said the design was -- "The
 * core is what opens /dev/uinput" -- and the core is the process that is
 * already reading every key, so it has nothing new to be told.
 *
 * The child is handed the RESULTING /dev/input/eventN path in
 * ND_ENV_KEY_EVDEV, and nothing else. That node is group `input` mode 0660
 * from eudev's stock rule, and ndusr_ut is already in group input because
 * that is how it RECEIVES keys today. So an untrusted program can read its
 * keys and still cannot open /dev/uinput, still cannot create a device of its
 * own, and still cannot inject a keystroke into anything. The boundary the
 * audit drew is exactly where it was; only the plumbing moved.
 *
 * DO NOT "simplify" this by passing the uinput WRITE descriptor down in
 * spec.fds. It looks like one less moving part and it is the whole hole: a
 * process holding that descriptor can synthesise any keypress on a phone with
 * no compositor, i.e. it can drive the real UI underneath it. The write end
 * stays in the core, which already decides what keys mean.
 *
 * ============ WHAT IT CARRIES ============
 *
 * By default (ND_APP_KEYDEV_RAW) it carries the sixteen keys the phone has --
 * NaviKey, C, Up, Down, 0-9, * and # -- as themselves, press and release,
 * because NeoDCT keycodes ARE Linux keycodes (nd_keycodes.h). An app that
 * wants them translated into a QWERTY-shaped stream says so in its manifest
 * and the core applies one of nd_t9_bridge's maps on the way through; see
 * nd_app_manifest_key_device().
 *
 * ============ THREE HELPERS THAT ARE NOT IN nd_input.h ============
 *
 * nd_input.h is another work package's contract and is frozen; these exist
 * solely for the process boundary THIS header describes, which is the same
 * argument nd_app.h makes for declaring nd_fb_adopt_fd(). They are
 * implemented in lib/nd_uinput.c beside the rest of the uinput code.
 */

/* One half of a keystroke on a uinput device: EV_KEY <code> <1|0> followed by
 * SYN_REPORT. Unlike nd_uinput_send_key() this does NOT synthesise the other
 * half, so held keys stay held on the far side. */
nd_err nd_uinput_send_raw(nd_uinput_kbd *k, uint16_t code, bool pressed);

/* "/dev/input/eventN" for the device `k` created, via UI_GET_SYSNAME. Only
 * meaningful for a descriptor that owns a device (nd_uinput_open, not
 * nd_uinput_attach). ND_ERR_NOTFOUND when the kernel has not published one. */
nd_err nd_uinput_event_node(const nd_uinput_kbd *k, char *out, size_t out_sz);

/* Poll until `node` can be opened for reading, or `timeout_s` elapses. True
 * means a child spawned now will find it; false means udev has not applied
 * the group yet and the caller should say so rather than carry on. */
bool nd_uinput_wait_readable(const char *node, double timeout_s);

/* ------------------------------------------------------------------ *
 * THE PRESENTATION SUBSET: how an installed app sees the phone's look
 * ------------------------------------------------------------------ *
 *
 * ============ THE PROBLEM ============
 *
 * settings.prop is 0640 ndusr:ndusr. Every app the owner installed runs as
 * ndusr_ut, so the read fails with EACCES and nd_settings_get() hands back
 * the DEFAULT for every key -- silently, because a default is a perfectly
 * good answer and nothing distinguishes it from a real one. The app then
 * draws with framework defaults: no wallpaper, chrome as though
 * wpeverywhere were off, and a dim factor nobody chose. It looks broken, and
 * it looks broken three files away from anything its author can see.
 *
 * ============ WHY THE FILE IS NOT WIDENED ============
 *
 * Because of one key in it. system.ui.engineering_mode gates the engineering
 * apps -- LinuxShell, raw AT, RemoteShell -- and nd_proc_app_needs_root()
 * reads it. It is already the weakest gate on the phone (AGENTS.md: "it
 * lives in settings.prop, on the partition the attacker just wrote to"), and
 * making the file readable by the untrusted user would hand a compromised
 * browser the answer to "is the root path open right now" for free.
 * neodct/tests/test_sdcard_layout.py asserts the mode, deliberately.
 *
 * ============ SO THE CORE PROJECTS THE THREE THAT ARE LOOK ============
 *
 * Exactly the same shape as ND_ENV_KEYPAD_MATRIX above: the core knows, the
 * app cannot ask, so the core says. Set for EVERY app and not only the
 * untrusted ones, because one code path that always runs is worth more than
 * two that agree today.
 *
 * The values are the settings' own strings, VERBATIM and unparsed -- "ON",
 * "0.75", a path -- so the app applies nd_setting_is_enabled() and strtod()
 * exactly as the core would and no parsing policy is duplicated on two sides
 * of a fork.
 *
 * WHAT THIS DOES NOT HAND OVER: the wallpaper VARIABLE is a path, not a
 * picture. The stock art under /NeoDCT/System/wallpapers is world-readable
 * decoration on the squashfs and the app can open it; a picture the owner put
 * on the card lives in a 0750 ndusr:ndusr directory and the app still cannot,
 * which is right. Knowing the name of a file it cannot read tells it how to
 * fail -- draw the plain chrome -- instead of leaving it to guess.
 *
 * ND_ENV_UI_WALLPAPER is always set: "NONE" when the owner has no wallpaper,
 * so that ABSENT unambiguously means "no core told me" and a hand-run
 * nd-apprun still falls back to reading the file for itself. */
#define ND_ENV_UI_WALLPAPER     "NEODCT_UI_WALLPAPER"
#define ND_ENV_UI_WP_EVERYWHERE "NEODCT_UI_WPEVERYWHERE"
#define ND_ENV_UI_WP_DIM        "NEODCT_UI_WPDIM"

/* How long the core waits for the node before giving up on it. Two seconds:
 * long enough to cover a coldplug still running on a loaded single core,
 * short enough that a phone whose udev is genuinely broken still opens the
 * app -- keyless, and saying so -- rather than appearing to hang. */
#define ND_PROC_KEYDEV_WAIT_S 2.0

/* ------------------------------------------------------------------ *
 * manifest.json's "wantsPerformance" -- a way for an app to ASK for the CPU
 * ------------------------------------------------------------------ *
 *
 * ============ THE GRANT THAT IS CORRECT AND THE GAP BESIDE IT ============
 *
 * 61-neodct-devices.rules gives scaling_min_freq, scaling_max_freq and
 * scaling_governor to GROUP=ndusr, and that is right: the core drops the
 * ceiling on the home screen and needs to be able to put it back.
 *
 * Widening it to ndusr_ut would be wrong, and obviously so. Every app the
 * owner installs runs as ndusr_ut; a game that pinned the clock to 1.2 GHz
 * for as long as it was open -- or worse, one that pinned it and exited
 * without putting it back, which nd_cpufreq.h warns is exactly what pinning
 * does -- would flatten a phone battery in an afternoon, and the owner would
 * have no way to tell which app did it.
 *
 * So the permission stays where it is. What was missing was any way to ASK.
 * The owner wants a PlayStation emulator to be playable, and an emulator on a
 * 1103 needs the top operating point; today it has no route to say so and no
 * route to be given it.
 *
 * ============ THE APP ASKS; THE CORE DECIDES AND CLAMPS ============
 *
 * One boolean in the app's own manifest.json, in the style of useWallpaper:
 *
 *     { "name": "PSX", "id": "42", "wantsPerformance": true }
 *
 * ABSENT MEANS FALSE, the opposite direction from useWallpaper and for the
 * same reason a key device is: this costs battery, so an app that has not
 * said anything must not get it.
 *
 * It is a REQUEST and not a setting. What the core does with it is bounded
 * three ways, and none of the three is the app's to change:
 *
 *   THE CEILING ONLY. scaling_max_freq is raised; scaling_min_freq is left
 *   exactly where it was. That is the difference between "you may go fast"
 *   and "you must go fast" -- the governor still idles the chip down between
 *   frames, and a paused emulator costs what a paused emulator should.
 *   nd_cpufreq_set() deliberately does NOT do this: it pins both ends,
 *   because Sleepy wants a held frequency to measure. This wants the
 *   opposite.
 *
 *   THE TOP OF THE KERNEL'S OWN TABLE and nothing else. There is no number in
 *   the manifest and there is not going to be one; the app cannot name a
 *   frequency, only ask for the highest the kernel is already offering.
 *
 *   FOR THE DURATION OF THE APP. The old ceiling is restored when the launch
 *   returns, on every path including a crash, a SIGTERM from an incoming call
 *   and a refused spawn. An app cannot leave the phone fast, which is the
 *   failure mode that would actually cost the owner a battery.
 *
 * A kernel with no cpufreq (QEMU, every time) makes all of this a no-op that
 * logs nothing, because "this machine does not scale its CPU" is not a
 * failure of the app. */
#define ND_APP_KEY_WANTS_PERFORMANCE "wantsPerformance"

/* Does the manifest in `app_dir` ask? Pure, and public for the reason
 * nd_app_manifest_use_wallpaper() is: the POLICY has to be testable without
 * forking anything, because the launcher's own behaviour cannot be -- a test
 * cannot make a host kernel offer a second operating point. */
bool nd_proc_app_wants_performance(const char *app_dir);

/* ============ THE ESCAPE HATCH ============
 *
 * How long ND_KEY_CLEAR has to be held, while an app owns the screen, before
 * the core takes the screen back by terminating it.
 *
 * There is otherwise NO key sequence that ends an app which has stopped
 * responding to keys. The core's launch loop only forwards presses and
 * inspects none of them, so a full-screen program that is not reading its
 * channel -- the keyless browser above, an emulator wedged on a bad disc --
 * leaves the phone with nothing to press. The only ways out were an incoming
 * call and the battery.
 *
 * Three seconds and not two, which is the difference between a safety net and
 * a bug: C is "back" everywhere in this OS, C does not auto-repeat
 * (nd_input.c arms repeat for the four arrows only), and nothing in the tree
 * gives a long C press a meaning of its own. Three seconds cannot be reached
 * by anybody who meant to go back. */
#define ND_PROC_APP_ABORT_HOLD_S 3.0

/* 1. open the key channel and the crash pipe
 * 2. spawn nd-apprun with the app's directory, the entry point and its
 *    argument, and the three inherited descriptors in the environment
 * 3. pump keys to the child while it runs, so the core stays awake and an
 *    incoming call can interrupt it
 * 4. waitpid, classify:
 *       WIFEXITED   && status == 0  -> a clean return; redraw the menu
 *       WIFEXITED   && status != 0  -> a crash
 *       WIFSIGNALED                 -> a crash, and we know which signal
 * 5. on a crash, read the child's report from the crash pipe, log it, and
 *    show the crash screen
 * 6. ALWAYS call nd_ui_refresh_after_app()
 *
 * Returns ND_OK whether or not the app crashed -- a crashed app is a handled
 * condition, not a failure of the launcher. crash_out, when non-NULL,
 * receives the report and .from_signal tells you whether there was one. */
nd_err nd_proc_launch_app(nd_ui *ui, const nd_app_entry *app, const char *entry, const char *arg,
                          nd_crash_info *crash_out);

#ifdef __cplusplus
}
#endif

#endif /* ND_PROC_H_INCLUDED */
