/* nd_panic.h -- the screen for when nd-core itself dies, and the guard that
 * stops it becoming a loop.
 *
 * nd_crash.h covers the crash the OS survives: an app faults, the core reads
 * the report and draws a screen. This header covers the one it does not. When
 * nd-core dies there is nobody left inside the process to draw anything, so
 * the drawing moves OUT of it: /bin/run_neodct.sh runs nd-core in a loop and
 * runs `nd-panic` after each death.
 *
 * ============ WHY A SEPARATE BINARY AND NOT THE OTHER TWO ANSWERS ============
 *
 * Three things could draw this screen. Two of them do not work.
 *
 *   run_neodct.sh, in busybox ash. It cannot. The framebuffer's pixel format
 *   is not knowable until something has run FBIOGET_VSCREENINFO on it: the
 *   panel is 16 or 32 bpp depending on what neodct_displayd negotiated, and
 *   at 32 bpp the channel order comes from vinfo.red.offset -- QEMU's
 *   virtio-gpu says 16 (B G R x) and the kernel vfb on the hardware says 0
 *   (R G B x). See "THE CHANNEL ORDER COMES FROM THE DRIVER" in nd_fb.h.
 *   So a pre-rendered blob `cat`ed onto /dev/fb0 would be correct on exactly
 *   one of the two framebuffers this OS runs on, and the phone with the
 *   wrong one would show a blue Nokia. That is before the shell has to
 *   decode a JPEG and rasterise a TTF, which it also cannot do.
 *
 *   nd_crash_draw_engineering() from a fresh process. It needs an nd_ui,
 *   and nd_ui_init() is the heavy end of the boot: four font sizes, the
 *   wallpaper, settings, the image cache, the input device, the notify
 *   service. That is a large fraction of the code that might have just
 *   killed the core, run again to report that the core died. It also draws
 *   a "Continue" softkey and blocks on a keypress, which is the wrong
 *   contract entirely -- nobody is coming to press it.
 *
 *   A small separate binary. What is here. nd-panic links libneodct but
 *   touches only the bottom of it: nd_fb, nd_image's JPEG decoder, nd_font,
 *   nd_draw, nd_paths, nd_log. No nd_ui, no sqlite, no input, no modem, no
 *   audio, no threads. A fresh process with a fresh heap, doing the smallest
 *   thing that can put a picture on a panel.
 *
 * ============ THE LADDER ============
 *
 * "Unlikely to fail for the same reason" is not "cannot fail", so every layer
 * degrades instead of aborting:
 *
 *   artwork missing or undecodable -> the text runs the full width.
 *   fonts missing                  -> a solid red panel, which at least says
 *                                     "this is not a hang" from across a room.
 *   framebuffer unopenable         -> nd-panic exits non-zero and the shell
 *                                     falls back to its ANSI banner on tty0.
 *
 * And the SHELL, not this binary, owns the restart policy. nd-panic can be
 * missing, broken or killed and the phone still counts crashes, still waits,
 * and still comes back.
 *
 * ============ THE GUARD ============
 *
 * Crash, restart, crash, restart, forever is worse than a frozen screen: it
 * burns battery, it thrashes the flash through every boot of the modem and
 * the databases, and it never sits still long enough to read. So the loop
 * counts CONSECUTIVE crashes and stops at ND_PANIC_MAX_RESTARTS.
 *
 * "Consecutive" is decided by how long the core lived, not by a timer running
 * beside it: a core that ran for ND_PANIC_HEALTHY_SECONDS was a working
 * phone, and whatever killed it afterwards is a new fault rather than the
 * same one repeating. That measurement comes from /proc/uptime deltas inside
 * one shell process, so it needs no wall clock -- which matters, because a
 * phone with no battery-backed RTC boots at the epoch and NTP may be exactly
 * what did not happen.
 *
 * The count lives in a shell VARIABLE, not a file. The loop is one shell
 * process for the life of the boot, so a variable is sufficient, and it
 * cannot be defeated by a full, read-only or unmounted /NeoDCT/User -- which
 * is a plausible cause of the crash it is counting.
 *
 * It deliberately does NOT survive a power cycle. A phone that halted, was
 * switched off and switched on again gets its three attempts back: pulling
 * the battery is a real repair for a real class of fault, and booting
 * straight into "not restarting" without having tried would be answering a
 * question the user did not ask.
 */

#ifndef ND_PANIC_H_INCLUDED
#define ND_PANIC_H_INCLUDED

#include "nd_crash.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The numbers the shell and the C both have to agree on
 * ------------------------------------------------------------------ */

/* Consecutive crashes allowed before the phone stops trying. Three is the
 * number the phone can afford: at roughly six seconds per attempt (crash
 * screen, countdown, boot) the user waits under half a minute to find out
 * whether it is coming back. */
#define ND_PANIC_MAX_RESTARTS 3

/* The countdown, in seconds. Long enough to read two short lines and see the
 * Nokia; short enough that a transient fault costs almost nothing. */
#define ND_PANIC_COUNTDOWN 3

/* A core that lived this long was a working phone, so its death starts a new
 * count rather than continuing the old one. Two minutes is well past every
 * boot-time failure -- modem enumeration, the first database open, the
 * first-boot wizard -- and well short of a session anybody would call short. */
#define ND_PANIC_HEALTHY_SECONDS 120

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

/* The sick Nokia inside CRASH.jpg, which is a 240x175 picture with the phone
 * drawn right of centre and hand-lettered text down the left. Measured, not
 * guessed: ink runs x 79..160, y 0..149, and the top of the phone is cut off
 * by the frame in the original.
 *
 * The APP crash screen shows the whole picture full-bleed. This one shows
 * only the phone, pinned to the left so the live text can have the right --
 * the same artwork, the same size, the same vertical position, just moved
 * across. It is CROPPED and NOT RESIZED on purpose: a resample would soften
 * every hand-drawn line, and the phone would stop looking like the phone on
 * the other crash screen. */
#define ND_PANIC_ART_X0 79
#define ND_PANIC_ART_Y0 0
#define ND_PANIC_ART_X1 160
#define ND_PANIC_ART_Y1 149

#define ND_PANIC_ART_W (ND_PANIC_ART_X1 - ND_PANIC_ART_X0 + 1) /* 82  */
#define ND_PANIC_ART_H (ND_PANIC_ART_Y1 - ND_PANIC_ART_Y0 + 1) /* 150 */

/* Where the text column starts when the artwork is there, and when it is not,
 * and how much room that leaves. The gap is eight pixels and the column is
 * 150, which is the number every string on this screen was measured against
 * -- see the table in lib/nd_panic.c. */
#define ND_PANIC_TEXT_X      (ND_PANIC_ART_W + 8) /* 90  */
#define ND_PANIC_TEXT_X_BARE 6
#define ND_PANIC_TEXT_W      (240 - ND_PANIC_TEXT_X) /* 150 */

/* ------------------------------------------------------------------ *
 * One frame's worth of state
 * ------------------------------------------------------------------ */

typedef enum {
    ND_PANIC_RESTART = 0, /* counting down to another try            */
    ND_PANIC_HALT         /* out of tries; this screen stays up      */
} nd_panic_mode;

typedef struct {
    nd_panic_mode mode;
    /* The wait status the shell saw: 128+signo for a signal, or a plain exit
     * code. It is `$?` and nothing richer, because a dead process cannot
     * hand over a report the way nd-apprun's signal handler does. */
    int32_t status;
    int32_t crash;     /* this is consecutive crash N; 0 when unknown  */
    int32_t limit;     /* of a maximum of N; 0 when unknown            */
    int32_t remaining; /* seconds still on the clock, RESTART mode only */
} nd_panic_state;

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

/* The one-line cause, from a shell wait status:
 *
 *   139 -> "SIGSEGV (11)"            134 -> "SIGABRT (6)"
 *     1 -> "exit code 1"               0 -> "exited cleanly"
 *
 * Name first and number in brackets, rather than the other way round, purely
 * so it fits: the 150 px text column holds about eighteen characters of the
 * 14 px face and "signal 11 (SIGSEGV)" is nineteen. A signal the table has no
 * name for degrades to "signal 37", which is all there is to say about it.
 *
 * 128+n is read as a signal because that is what busybox ash reports for one,
 * and nd-core's own fatal handler re-raises so the status is the real signal.
 * A program that genuinely exits 139 is indistinguishable from one that
 * segfaulted, from the shell's side -- that ambiguity is in the exit-status
 * convention itself and is not ours to resolve. Returns the wanted length,
 * snprintf-style. */
size_t nd_panic_status_text(int32_t status, char *out, size_t out_sz);

/* The same status as an nd_crash_info, so the core's death lands in
 * /NeoDCT/User/logs/crash.log in the same shape as an app's. si_code and
 * fault_addr stay zero: nobody was alive to record them. */
void nd_panic_status_info(int32_t status, nd_crash_info *out);

/* The digit line under "Restarting in": "3...", "2...", "1...".
 *
 * EMPTY, and a returned length of zero, at or below zero seconds -- there is
 * no "0..." frame. The screen then says "Restarting..." on the lead line
 * instead, because "Restarting in" with an empty line under it reads as a
 * screen that has stopped updating rather than one that is about to act. */
size_t nd_panic_countdown_text(int32_t remaining, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * The screen
 * ------------------------------------------------------------------ */

/* CRASH.jpg cropped to the phone. NULL when the file is missing or will not
 * decode, which nd_panic_draw() handles by widening the text. Owned by the
 * caller; free with nd_image_free(). */
nd_image *nd_panic_load_art(void);

/* The three sizes the screen is set in. All four of the OS's sizes are
 * loadable and nd_font.h forbids a fifth, so this picks from that set rather
 * than inventing one. Any of them may be NULL: a missing size falls back to
 * whichever is present, and only all three missing gives up. */
typedef struct {
    const nd_font *title; /* ND_FONT_PX_MD -- the two headline rows       */
    const nd_font *body;  /* ND_FONT_PX_S  -- cause, attempt, and lead-in */
    const nd_font *count; /* ND_FONT_PX_XL -- the one digit that changes  */
} nd_panic_fonts;

/* Compose one frame. canvas must be ND_UI_W x ND_UI_H RGB888; art may be
 * NULL; fonts may be NULL or hold NULLs. With no usable font at all the
 * canvas comes back solid red, deliberately -- see THE LADDER above. */
nd_err nd_panic_draw(nd_image *canvas, const nd_panic_fonts *fonts, const nd_image *art,
                     const nd_panic_state *st);

#ifdef __cplusplus
}
#endif

#endif /* ND_PANIC_H_INCLUDED */
