/* nd_themepicker.c -- choosing a look, by wearing it.
 *
 * ============ THE PREVIEW IS THE SCREEN ============
 *
 * A picker normally shows a swatch: a little picture of what you would get,
 * drawn beside a list of names. This one does not, because it does not have
 * to. nd_theme_apply() replaces the palette of the RUNNING PROCESS, so moving
 * onto a theme and repainting IS the preview -- the title plate, the panel,
 * the softkey and the type all become that theme, full size, in the real
 * widgets, at the real 240x175. Nothing is simulated and nothing can drift
 * from what the phone actually draws, because it is what the phone actually
 * draws.
 *
 * That is the whole argument for making the framework patchable at runtime
 * rather than at build time, and it is worth stating plainly: this screen is
 * not a feature that the theme system happens to allow, it is the theme
 * system being visible.
 *
 * The theme's own preview.png is still shown INSIDE the page, and it is not a
 * duplicate: it is a picture of the app selector, which is the screen a theme
 * changes most and the one screen the owner cannot be standing on while they
 * choose. The chrome around it is live; the picture is of somewhere else.
 *
 * ============ WHY IT IS BUILT ON nd_detailpage ============
 *
 * The page has a title, a picture, several paragraphs and a softkey, and it
 * has to scroll when the description is long. That is nd_detailpage exactly
 * -- the widget the update changelog and the .nap installer already use -- so
 * the picker is that widget PAGED rather than a fourteenth way of laying out
 * a page. A theme is one page; * and # turn to the previous and the next.
 *
 * ============ AND WHY * AND # ============
 *
 * There is no left and no right on this phone (nd_keypadsetup.c has the
 * sixteen keys). Up and Down are already spoken for here -- they scroll the
 * page, which a long description needs -- so the second axis comes off the
 * number pad, which is what MusicPlayer and Messages do for the same reason.
 * * and # sit either side of 0 and read as "back one" and "on one" without a
 * legend.
 */

#include <string.h>

#include "nd_app.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_settings.h"
#include "nd_theme.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* The body of one theme's page: what it is, who made it, and what it brings.
 *
 * Assembled rather than stored so that a theme file does not have to describe
 * itself twice -- the flags were worked out when the directory was read, and
 * an owner deciding between two themes wants to know which one carries icons
 * far more than they want the author's name. */
static void page_body(const nd_theme_info *t, bool active, char *out, size_t out_sz)
{
    char parts[ND_THEME_DESC_MAX + 160];
    size_t n = 0u;

    parts[0] = '\0';
    if (t->desc[0] != '\0')
        n = nd_strlcpy(parts, t->desc, sizeof parts);
    if (n < sizeof parts) {
        char brings[96];
        const char *items[4];
        size_t n_items = 0u;

        if (t->has_icons)
            items[n_items++] = "icons";
        if (t->has_font)
            items[n_items++] = "a typeface";
        if (t->has_wallpaper)
            items[n_items++] = "a wallpaper";
        if (t->has_img)
            items[n_items++] = "status icons";

        brings[0] = '\0';
        if (n_items == 0u) {
            (void)nd_strlcpy(brings, "Colours only.", sizeof brings);
        } else {
            size_t i;

            (void)nd_strlcpy(brings, "Brings ", sizeof brings);
            for (i = 0u; i < n_items; i++) {
                if (i > 0u)
                    (void)nd_strlcat(brings, (i + 1u == n_items) ? " and " : ", ", sizeof brings);
                (void)nd_strlcat(brings, items[i], sizeof brings);
            }
            (void)nd_strlcat(brings, ".", sizeof brings);
        }
        (void)nd_snprintf(out, out_sz, "%s%s%s%s%s%s%s%s", parts, parts[0] != '\0' ? "\n\n" : "",
                          brings, t->author[0] != '\0' ? "\nBy " : "", t->author,
                          t->version[0] != '\0' ? "\nVersion " : "", t->version,
                          active ? "\n\nThis is the look you are using." : "");
        return;
    }
    (void)nd_strlcpy(out, parts, out_sz);
}

/* "2/5", the same badge shape the installer's pages use. */
static void page_badge(size_t i, size_t n, char *out, size_t out_sz)
{
    (void)nd_snprintf(out, out_sz, "%zu/%zu", i + 1u, n);
}

nd_err nd_themepicker_init(nd_themepicker *p, nd_ui *ui)
{
    size_t i;

    if (p == NULL || ui == NULL)
        return ND_ERR_INVAL;
    memset(p, 0, sizeof *p);
    p->ui = ui;
    p->n = nd_theme_list(p->themes, ND_THEME_MAX_FOUND);
    if (p->n == 0u)
        return ND_ERR_NOTFOUND;

    /* What was on when we walked in, so that backing out restores it. The id
     * and not the index: the list is rebuilt on entry and a card pulled since
     * the last visit would make an index mean a different theme. */
    (void)nd_strlcpy(p->entry_id, nd_theme_active()->id, sizeof p->entry_id);
    p->sel = 0u;
    for (i = 0u; i < p->n; i++) {
        if (strcmp(p->themes[i].id, p->entry_id) == 0) {
            p->sel = i;
            break;
        }
    }
    return ND_OK;
}

/* Wear `i` and lay its page out. The apply is what makes the page a preview:
 * everything nd_detailpage_init() measures and everything it later draws
 * reads the palette that was just installed. */
static nd_err build_page(nd_themepicker *p, size_t i, nd_detailpage *page, char *body,
                         size_t body_sz, char *badge, size_t badge_sz)
{
    const nd_theme_info *t = &p->themes[i];
    char preview[ND_PATH_MAX];
    bool active = strcmp(t->id, p->entry_id) == 0;

    nd_theme_apply(t->builtin ? NULL : t);
    page_body(t, active, body, body_sz);
    page_badge(i, p->n, badge, badge_sz);
    if (!nd_theme_preview_path(t, preview, sizeof preview))
        preview[0] = '\0';

    /* The softkey says what NaviKey will do, and for the theme already in use
     * that is nothing -- so it says so rather than offering to re-apply it and
     * leaving the owner to wonder whether anything happened. */
    return nd_detailpage_init(page, p->ui, t->name, NULL, body,
                              preview[0] != '\0' ? preview : NULL, badge, "Theme",
                              active ? "In use" : "Apply");
}

int32_t nd_themepicker_show(nd_themepicker *p)
{
    if (p == NULL || p->n == 0u)
        return ND_WIDGET_BACK;

    for (;;) {
        nd_detailpage page;
        char body[ND_THEME_DESC_MAX + 256];
        char badge[16];
        int32_t key;

        if (build_page(p, p->sel, &page, body, sizeof body, badge, sizeof badge) != ND_OK) {
            nd_log_err(ND_LOG_UI, "theme picker: could not lay out %s", p->themes[p->sel].name);
            goto restore;
        }

        /* * and # leave the page the way ENTER and C do, so that nd_detailpage
         * hands them back instead of swallowing them as scroll keys. The page
         * is then rebuilt around the new theme -- a rebuild and not a redraw,
         * because the body text and the picture are different. */
        page.accept_keys[page.n_accept++] = ND_KEY_STAR;
        page.accept_keys[page.n_accept++] = ND_KEY_HASH;

        key = nd_detailpage_show(&page);
        nd_detailpage_free(&page);

        if (nd_app_should_exit())
            goto restore;

        if (key == ND_KEY_STAR) {
            p->sel = (p->sel == 0u) ? p->n - 1u : p->sel - 1u;
            continue;
        }
        if (key == ND_KEY_HASH) {
            p->sel = (p->sel + 1u) % p->n;
            continue;
        }
        if (key == ND_KEY_ENTER) {
            const nd_theme_info *t = &p->themes[p->sel];

            if (nd_theme_select(t->id) != ND_OK) {
                nd_log_err(ND_LOG_UI, "theme picker: could not apply %s", t->id);
                goto restore;
            }
            nd_log(ND_LOG_UI, "Theme set to %s (%s).", t->name, t->id);
            return (int32_t)p->sel;
        }
        goto restore;
    }

restore:
    /* Walking away puts back what was on when we walked in. Without this the
     * process keeps whatever the owner last hovered over -- Settings would
     * carry on drawing in a theme it did not apply, which is worse than
     * either outcome the owner was choosing between. */
    {
        nd_theme_info back;

        if (strcmp(p->entry_id, ND_THEME_ID_BUILTIN) == 0 || !nd_theme_find(p->entry_id, &back))
            nd_theme_apply(NULL);
        else
            nd_theme_apply(&back);
    }
    return ND_WIDGET_BACK;
}
