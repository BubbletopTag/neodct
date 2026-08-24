/* test_testsapp.c -- the Tests engineering app, app id 9999.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The three strings are main.py's. Two of them reach pixels: the dialog
 *     body decides where the wrap breaks and therefore where every glyph on
 *     golden/eng-tests.png lands, and "Hello World" decides the centring on
 *     the frame before it.
 *
 *  2. The exit key set is (46, 28, 50) -- C, Enter and Menu. NOT 14: Back is
 *     MessageDialog's cancel key, so pressing it dismisses the dialog and the
 *     app's own `key in (46, 28, 50)` then rejects it and re-shows. That is
 *     the Python's behaviour and it is reproduced rather than tidied, so the
 *     test asserts the odd half too.
 *
 *  3. "Hello World" is centred on the INK box, not on a font metric. At
 *     font_xl the string has an ascender and no descender, so the box is
 *     shorter than the line and the text sits low. Checked against
 *     nd_ui_text_size directly.
 *
 *  4. THE GOLDEN FRAME. eng-tests is the THIRD of three commits, and the
 *     third is the dialog -- which clears the full 0..175 and paints its own
 *     "OK" bar over the "Testing123" softkey the first commit drew. The count
 *     is pinned as well as the digest: it is the only thing that can tell
 *     that the first two screens happened at all.
 *
 *  5. The SIGTERM teardown contract. Back cannot end this app (see 2), so
 *     nd_app_should_exit() is the only bounded way out of the loop, which is
 *     precisely why nd_app.h requires the poll.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "nd_app.h"

#include "smallapp_test.h"

#include "../../apps/TestsApp/testsapp.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    bool (*is_exit_key)(int32_t);
    void (*greeting_pos)(nd_ui *, int32_t *, int32_t *);
    const char *const *softkey;
    const char *const *message;
    const char *const *greeting;
    const int32_t *exit_keys;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.is_exit_key = sa_sym(h, "nd_testsapp_is_exit_key");
    *(void **)&api.greeting_pos = sa_sym(h, "nd_testsapp_greeting_pos");
    api.softkey = dlsym(h, "nd_testsapp_softkey");
    api.message = dlsym(h, "nd_testsapp_message");
    api.greeting = dlsym(h, "nd_testsapp_greeting");
    api.exit_keys = dlsym(h, "nd_testsapp_exit_keys");

    return api.run != NULL && api.shutdown != NULL && api.is_exit_key != NULL &&
           api.greeting_pos != NULL && api.softkey != NULL && api.message != NULL &&
           api.greeting != NULL && api.exit_keys != NULL;
}

/* ------------------------------------------------------------------ *
 * 1 and 2. Strings and keys
 * ------------------------------------------------------------------ */

static void test_strings(void)
{
    CHECK_STR(*api.softkey, "Testing123", "the softkey text");
    CHECK_STR(*api.message, "This is a test of the error screen", "the dialog body");
    CHECK_STR(*api.greeting, "Hello World", "the greeting");
}

static void test_exit_keys(void)
{
    CHECK_INT(api.exit_keys[0], 46, "exit_keys[0] is KEY_C");
    CHECK_INT(api.exit_keys[1], 28, "exit_keys[1] is Enter");
    CHECK_INT(api.exit_keys[2], 50, "exit_keys[2] is Menu");

    CHECK(api.is_exit_key(46), "C leaves");
    CHECK(api.is_exit_key(28), "Enter leaves");
    CHECK(api.is_exit_key(50), "Menu leaves");
    /* The one that looks like it should and does not. */
    CHECK(!api.is_exit_key(14), "Back does NOT leave -- it re-shows the dialog");
    CHECK(!api.is_exit_key(ND_KEY_NONE), "no key is not an exit key");
}

/* ------------------------------------------------------------------ *
 * 3. Centring
 * ------------------------------------------------------------------ */

static void test_greeting_position(void)
{
    sa_fixture fx;
    int32_t x = -1;
    int32_t y = -1;
    int32_t w = 0;
    int32_t h = 0;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }

    nd_ui_text_size(&fx.ui, *api.greeting, fx.ui.font_xl, &w, &h);
    api.greeting_pos(&fx.ui, &x, &y);
    CHECK_INT(x, (240 - w) / 2, "x is (W - ink width) // 2");
    CHECK_INT(y, (145 - h) / 2, "y is (content_bottom - ink height) // 2");
    /* The ink box of a capital-H-to-lowercase-d string is well short of the
     * 24 px face, which is what puts the baseline where the reference has
     * it. If this ever came out equal to the face height, nd_ui_text_size
     * would have stopped returning ink and every centred string on the phone
     * would have moved. */
    CHECK(h > 0 && h < 24, "the ink box is shorter than the 24 px face");

    sa_fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 4. The golden frame
 * ------------------------------------------------------------------ */

/* The warning triangle is an absolute /NeoDCT/System/... path, so ND_ROOT has
 * to point at the overlay. READ ONLY: nothing here writes with it set. */
static char saved_root[ND_PATH_MAX];

static bool root_to_overlay(void)
{
    char overlay[ND_PATH_MAX];

    (void)nd_strlcpy(saved_root, nd_path_root(), sizeof saved_root);
    if (!sa_overlay_root(overlay, sizeof overlay))
        return false;
    return nd_path_set_root(overlay) == ND_OK;
}

static void root_restore(void)
{
    (void)nd_path_set_root(saved_root[0] != '\0' ? saved_root : NULL);
}

static void test_golden_frame(void)
{
    sa_fixture fx;
    int rc;

    if (!root_to_overlay()) {
        CHECK(false, "found the overlay for the warning icon");
        return;
    }
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    /* MessageDialog drains the channel before its first draw, so a queued
     * press would be eaten before the dialog is up. A HELD Enter survives the
     * drain and arrives as repeats afterwards -- once to dismiss the dialog,
     * once for the app's own wait_for_key(). Same trick as app-clock. */
    if (!sa_hold(&fx, ND_KEY_ENTER)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    nd_vclock_enable();
    rc = api.run(&fx.ui);

    CHECK_INT(rc, 0, "Enter returns 0");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 3,
              "three commits: the softkey, Hello World, then the dialog");
    CHECK_INT(nd_vclock_frame(), 3, "the virtual clock ticked once per committed frame");
    sa_expect_golden(&fx, nd_capture_recent(fx.cap, 0u), "eng-tests");

    /* Frame 2 is the one the dialog painted over. It is not a reference
     * frame, so it is judged by what has to be true of it rather than by a
     * digest: white ink somewhere in the middle band, and a black row 174
     * that the softkey bar owns. */
    {
        const nd_image *hello = nd_capture_recent(fx.cap, 1u);
        int32_t x;
        int32_t y;
        bool any_ink = false;

        CHECK(hello != NULL, "frame 2 is still in the ring");
        if (hello != NULL) {
            api.greeting_pos(&fx.ui, &x, &y);
            for (int32_t dy = 0; dy < 20 && !any_ink; dy++) {
                for (int32_t dx = 0; dx < 120; dx++) {
                    nd_color p = nd_image_get_px(hello, x + dx, y + dy);

                    if (p.r == 255u && p.g == 255u && p.b == 255u) {
                        any_ink = true;
                        break;
                    }
                }
            }
            CHECK(any_ink, "frame 2 really has Hello World on it");
        }
    }

    nd_vclock_disable();
    sa_fx_free(&fx);
    root_restore();
}

/* ------------------------------------------------------------------ *
 * 5. Back re-shows, and only SIGTERM ends it
 * ------------------------------------------------------------------ */

static void test_sigterm_leaves_the_loop(void)
{
    sa_fixture fx;
    int rc;

    if (!root_to_overlay()) {
        CHECK(false, "found the overlay for the warning icon");
        return;
    }
    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    if (nd_app_install_signal_handlers() != ND_OK) {
        CHECK(false, "signal handlers");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    /* Back: MessageDialog cancels on it and returns, the app's own
     * wait_for_key() gets the next repeat, which is also Back, which is not
     * an exit key -- so it loops and re-draws. The budget bounds the run and
     * the flag is what actually gets us out. */
    if (!sa_hold(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "held key");
        sa_fx_free(&fx);
        root_restore();
        return;
    }

    (void)raise(SIGTERM);
    CHECK(nd_app_should_exit(), "SIGTERM was seen");

    nd_vclock_enable();
    nd_capture_set_budget(fx.cap, 8);
    rc = api.run(&fx.ui);
    nd_capture_clear_budget(fx.cap);

    CHECK_INT(rc, 0, "the app returns rather than spinning on a key it ignores");
    nd_vclock_disable();
    sa_fx_free(&fx);
    root_restore();
}

static void test_null_safety(void)
{
    int32_t x = 7;
    int32_t y = 7;

    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown();
    api.greeting_pos(NULL, &x, &y);
    CHECK_INT(x, 7, "greeting_pos(NULL) writes nothing");
    api.greeting_pos(NULL, NULL, NULL);
    sa_checks++;
}

int main(void)
{
    void *h = sa_begin("TestsApp", "ndtestsapp");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_strings);
    RUN(test_exit_keys);
    RUN(test_greeting_position);
    RUN(test_golden_frame);
    RUN(test_null_safety);
    /* Last: nd_app_should_exit() cannot be lowered again. */
    RUN(test_sigterm_leaves_the_loop);

    return sa_end(h, "test_testsapp");
}
