#!/bin/sh
# new-app.sh <Name> <id> -- scaffold a NeoDCT app.
#
# An app lives in two trees and neither half works alone: the code under
# neodct/src/apps/<Name>/ and the manifest and icon under
# neodct/overlay/NeoDCT/System/apps/<Name>/. Forgetting the overlay half gives
# an app that builds and never appears in the menu, which is a confusing five
# minutes. This writes both, plus the two test skeletons, and then prints the
# edits it CANNOT make for you.
#
#   scripts/new-app.sh Notepad 13
set -eu

NAME="${1:-}"
APPID="${2:-}"
[ -n "$NAME" ] && [ -n "$APPID" ] || { echo "usage: new-app.sh <Name> <id>" >&2; exit 1; }

# lower-case stem for file and symbol names
LOWER=$(printf '%s' "$NAME" | tr '[:upper:]' '[:lower:]')
UPPER=$(printf '%s' "$NAME" | tr '[:lower:]' '[:upper:]')

REPO=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "not in a git repo" >&2; exit 1; }
SRC="$REPO/neodct/src/apps/$NAME"
OVL="$REPO/neodct/overlay/NeoDCT/System/apps/$NAME"
TEST="$REPO/neodct/src/test/unit/test_${LOWER}_app.c"

[ -e "$SRC" ] && { echo "$SRC already exists" >&2; exit 1; }

# The id orders the menu and shows in the breadcrumb; a clash makes the sort
# non-deterministic, so refuse rather than produce something subtly wrong.
if grep -rqs "\"id\":[[:space:]]*\"$APPID\"" "$REPO/neodct/overlay/NeoDCT/System/apps" \
        "$REPO/neodct/overlay/NeoDCT/System/engineering/apps"; then
    echo "app id $APPID is already taken:" >&2
    grep -rls "\"id\":[[:space:]]*\"$APPID\"" "$REPO/neodct/overlay/NeoDCT/System" >&2
    exit 1
fi

mkdir -p "$SRC" "$OVL"

cat > "$OVL/manifest.json" <<JSON
{
	"name":	"$NAME",
	"id":	"$APPID",
	"icon": "icon.png",
	"exec": "main.py"
}
JSON

# 120x120 RGBA, white line art on transparent, like every other stock icon.
# A placeholder box is honest about being one; replace it before shipping.
if python3 -c "import PIL" 2>/dev/null; then
    python3 - "$OVL/icon.png" <<'PY'
import sys
from PIL import Image, ImageDraw
S = 4
img = Image.new("RGBA", (120 * S, 120 * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([14 * S, 14 * S, 106 * S, 106 * S],
                    radius=8 * S, outline=(255, 255, 255, 255), width=7 * S)
d.line([38 * S, 60 * S, 82 * S, 60 * S], fill=(255, 255, 255, 255), width=7 * S)
img.resize((120, 120), Image.LANCZOS).save(sys.argv[1])
PY
    echo "  placeholder icon written -- replace it"
else
    cp "$REPO/neodct/overlay/NeoDCT/System/ui/resources/img/appselector/placeholder_icon.png" \
       "$OVL/icon.png" 2>/dev/null || echo "  NOTE: no icon written, add $OVL/icon.png (120x120 RGBA)"
fi

cat > "$SRC/${LOWER}_app.h" <<HEADER
/* ${LOWER}_app.h -- the shape of the $NAME app, app id $APPID.
 *
 * Everything a test needs to reach is declared here rather than left static,
 * so test/unit/test_${LOWER}_app.c can dlopen() the BUILT app.so and assert on
 * the artefact that ships.
 */

#ifndef ND_${UPPER}_APP_H_INCLUDED
#define ND_${UPPER}_APP_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The id orders the menu and is drawn in the breadcrumb. A string as well as a
 * number because the widgets take the root id as text and the two must not be
 * able to drift apart. */
#define ND_${UPPER}_APP_ID   $APPID
#define ND_${UPPER}_APP_ROOT "$APPID"

extern const char *const nd_${LOWER}_app_title;

#ifdef __cplusplus
}
#endif

#endif
HEADER

cat > "$SRC/main.c" <<MAIN
/* apps/$NAME/main.c -- the $NAME app, app id $APPID.
 *
 * <what this app is for, and what it deliberately does NOT do>
 */

#include <string.h>

#include "${LOWER}_app.h"

#include "nd_app.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

const char *const nd_${LOWER}_app_title = "$NAME";

int app_run(nd_ui *ui)
{
    nd_pagedlist menu;
    static const char *const ROWS[] = {"First screen", "Second screen"};

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    /* A PagedList front menu, one row per screen in big type, which is what
     * the phone this imitates puts here. nd_vlist is the alternative when
     * there are more rows than pages are worth. */
    for (;;) {
        int32_t choice;

        nd_pagedlist_init(&menu, ui, nd_${LOWER}_app_title, ROWS,
                          ND_ARRAY_LEN(ROWS), ND_${UPPER}_APP_ROOT, true);
        choice = nd_pagedlist_show(&menu);
        if (choice == ND_WIDGET_BACK)
            return 0;

        switch (choice) {
        case 0:
            /* ... */
            break;
        default:
            break;
        }

        /* nd_app.h: any loop that outlives a frame polls this, so an incoming
         * call is not left waiting on a user who walked away mid-menu. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing held: no file, no child process, no sound card. The symbol exists
 * because nd_app.h requires every app to export one. */
void app_shutdown(void) {}
MAIN

cat > "$TEST" <<TESTC
/* test_${LOWER}_app.c -- the $NAME app, app id $APPID.
 *
 * dlopen()s the BUILT app.so so the test exercises the artefact that ships
 * rather than a second copy compiled with different flags.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "nd_app.h"

#include "smallapp_test.h"

#include "../../apps/$NAME/${LOWER}_app.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    const char *const *title;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    api.title = dlsym(h, "nd_${LOWER}_app_title");
    return api.run != NULL && api.shutdown != NULL && api.title != NULL;
}

static void test_identity(void)
{
    CHECK_STR(*api.title, "$NAME", "the title the header draws");
    CHECK_INT(ND_${UPPER}_APP_ID, $APPID, "app id");
}

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown();
    sa_checks++;
}

int main(void)
{
    void *h = sa_begin("$NAME", "nd${LOWER}");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_identity);
    RUN(test_null_safety);

    return sa_end(h, "test_${LOWER}_app");
}
TESTC

cat <<DONE

Written:
  $SRC/main.c
  $SRC/${LOWER}_app.h
  $OVL/manifest.json
  $OVL/icon.png
  $TEST

Now edit these BY HAND -- a new app changes counts elsewhere:

  1. neodct/src/Makefile          add $NAME to STUB_STOCK_APPS
  2. test/unit/test_appsel.c      add {$APPID, "$NAME", "/NeoDCT/System/apps/$NAME"}
                                  to EXPECTED[], in id order, and bump the
                                  stock-app count
  3. test/unit/test_appreg.c      bump the stock and total app counts, and the
                                  index-12 notch position -- the scrollbar step
                                  is 99/(n_apps-1) so every app moves it
  4. re-cut the menu-* golden frames (references/testing.md)

Then:  cd neodct/src && make && make test
DONE
